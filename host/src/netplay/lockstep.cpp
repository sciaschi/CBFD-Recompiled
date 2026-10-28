// Netplay's lockstep (lockstep.hpp). It all runs on the game thread: the connecting in harness::start,
// before any of the game's code, then each controller read. The session (net/session.hpp) is polled only
// from here; GameNetworkingSockets keeps the connections alive from its own thread in between.
//
// Each port's pads are kept from read 0 on (5 bytes a read: 1 MB is over 15 hours for 4 players). They
// arrive in order, as they're sent reliably: an Inputs packet that doesn't start at the next read of its
// port ends the match.

#include "lockstep.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/save_states.hpp"
#include "ultramodern/timeline.hpp"

#include "../conker.hpp"
#include "hello.hpp"
#include "net/session.hpp"

// The runtime's flag for the program quitting (ultramodern::quit), set before it waits for the game's
// threads to end. librecomp's pi.cpp reads it the same way.
extern std::atomic_bool exited;

using conker::netplay::Delay;
using conker::netplay::Event;
using conker::netplay::Gone;
using conker::netplay::Hash;
using conker::netplay::Inputs;
using conker::netplay::Motion;
using conker::netplay::Pad;
using conker::lockstep::Input;
using conker::netplay::Reason;

namespace {
    constexpr uint64_t never = UINT64_MAX;
    // Reads between hash comparisons: one a second.
    constexpr uint64_t hash_every = 30;
    // How long a read waits for a peer's pad before the match is given up on. (A peer that's gone is
    // noticed by the library sooner, in about 10 s.)
    constexpr double wait_seconds = 20.0;
    // How long the host waits for every player to join.
    constexpr double join_seconds = 600.0;

    int host_port = -1;
    std::string host_address;       // hosting from the command line: the address the others reach, for the code
    std::string join_where;
    int wanted_players = 2;
    int local_option = 1;           // the players on this machine
    bool host_own_view = true;      // hosting: the rule this peer will start the match with
    bool match_own_view = false;    // the rule of the match under way
    uint8_t match_pairs = 0;        // ...and the ports shown two to a window (Start)
    // --input-delay's, or the menu's: the reads ahead, or auto (the host chooses from the round trips).
    constexpr int auto_delay = -1;
    int delay_option = auto_delay;
    // The match's, from the host (Start), at the read the game is at.
    int delay = 2;
    // ...and its last change (#18): `before` up to read `from`, `after` from it. With auto, the host changes it
    // as the round trips do (Delay); every peer switches at the same read, which none has reached as it hears.
    // At that read each peer sends its own pads up to the read plus the new delay: a raised one's reads in
    // between get the pad the controllers give then too (held), and a lowered one's live pads aren't sent
    // until the reads already sent are played.
    struct DelayChange {
        uint64_t from = 0;
        int before = 2;
        int after = 2;
    } delays;
    int delay_at(uint64_t read) { return read < delays.from ? delays.before : delays.after; }
    // The host's checks (every hash_every reads) in a row that wanted a higher delay, and a lower one. Higher:
    // the round trips need more, or at least late_reads_to_raise reads came late as they waited for the others
    // since the last check (a lossy network: the pads that are sent again come late, which a higher delay
    // hides; the round trips don't show it), or most of a peer's reads since its last hash waited over
    // long_wait_ms (its own, or a joiner's, its Hash says: two machines half a frame out of step on a LAN, one
    // waiting so at every read, keep 30 frames a second but show them unevenly, which frame interpolation
    // makes judder), then one more, up to most_raised_for_waiting more than the round
    // trips need (a peer that can't keep up, rather than the network, makes reads late whatever the delay). It's
    // raised after raise_checks (every read waits until then), and lowered only after lower_checks with
    // lower_spare_ms to spare and no read late from waiting, so it doesn't go back and forth: a read at a time
    // while it's above what the round trips need for waiting, else to what they need. Nor is it changed again
    // within settle_reads of the last change (Delay's from read relies on that: a joiner is at most the delay
    // ahead only once the pads a lowering already sent are played).
    int wanted_higher = 0;
    int wanted_lower = 0;
    int raised_for_waiting = 0;
    constexpr int most_raised_for_waiting = 2;
    uint64_t late_from_waiting = 0;
    // Reads since this peer's last hash that waited over long_wait_ms, and the most a joiner's last Hash said.
    uint64_t long_waits = 0;
    uint8_t joiners_long_waits = 0;
    constexpr double long_wait_ms = 8.0;    // delay_needed's wait_ms: more than a read can wait unseen
    constexpr uint64_t long_waits_to_raise = hash_every / 2;
    constexpr int raise_checks = 2;
    constexpr int lower_checks = 5;
    constexpr uint64_t late_reads_to_raise = 3;
    constexpr double lower_spare_ms = 12.0;
    constexpr uint64_t settle_reads = 90;
    uint64_t desync_at = never;
    // --netplay-fake-network's: the network faked from the start, and changes at reads (an outage of
    // `outage_seconds` from read `outage_at`; the lag `lag_then` from read `lag_at`).
    conker::netplay::FakeNetwork fake;
    bool faking = false;
    uint64_t outage_at = never;
    double outage_seconds = 0;
    double outage_until = 0;
    uint64_t lag_at = never;
    int lag_then = 0;
    // --netplay-stall-at's: the game stops at that read for that long, as on a hitch (loading, the system).
    uint64_t stall_at = never;
    long stall_ms = 0;
    bool options_valid = true;

    bool library_started = false;
    conker::netplay::Session* session = nullptr;
    uint8_t own_ports = 0;          // bit n: port n
    int match_players = 0;
    conker::netplay::Hello sent;    // what this peer said in the handshake (begin's)
    bool initial_sent = false;
    bool ended = false;         // the match is over for this peer: nothing more is sent
    bool left = false;
    std::string why_over;       // why the match ended, for the player
    // How long the game waited for the others' input, for tests (printed as the match ends): the reads that
    // waited over a millisecond, the time in all and the longest wait.
    struct Waits {
        uint64_t reads = 0;
        uint64_t waited = 0;
        double seconds = 0;
        double longest = 0;
        uint64_t longest_read = 0;
        uint64_t catch_ups = 0;
        uint64_t delay_changes = 0;
        // What a player feels: reads that came late after the one before (a frame is 33 ms at 30 a second;
        // a read waiting where the game would have slept until its time anyway costs nothing).
        double last_read_at = 0;
        uint64_t late = 0;
        double latest = 0;
        uint64_t latest_read = 0;
        bool printed = false;
    } waits;
    constexpr double late_seconds = 0.050;
    // Catching up (#16): when the others' inputs reach this many reads further ahead than they would if they
    // were at this peer's read, this peer has fallen behind them (a hitch here: the others ran on until they
    // needed its inputs, then waited, and would wait on it at every read from then on). It runs the timeline
    // faster than real time until they're no further ahead than that again. The input delay is the furthest
    // anyone can get ahead, so with a delay of 0 or 1 it never needs to.
    constexpr int64_t behind_to_catch_up = 2;
    bool catching_up = false;
    uint64_t caught_up_from = 0;
    double catching_up_since = 0;

    void set_catching_up(bool on) {
        catching_up = on;
        ultramodern::timeline::set_catching_up(on);
    }

    std::array<std::vector<Input>, conker::netplay::max_players> received;
    std::array<bool, conker::netplay::max_players> gone{};
    // A player that left the match while it went on (#17): from this read their port gets an idle pad (never:
    // they're in, or the match ends without them). The host decides it, the read after the last of their
    // inputs it had, and tells the others (Gone); nobody can be past it, as everyone needed those inputs.
    std::array<uint64_t, conker::netplay::max_players> idle_from;
    // Whether a peer that's gone stopped answering, rather than saying goodbye (a crash, a lost connection),
    // so it's told as maybe having quit.
    bool gone_silently = false;
    // What status() and take_news() give the main thread, from the game thread (and the connecting one):
    // set by publish, under their own lock, as the session's is held while a read waits. A port's round trip
    // is asked of the library every round_trips_every seconds at most.
    std::mutex status_mutex;
    conker::lockstep::Status shown;
    std::vector<std::string> news;
    double round_trips_at = 0;
    constexpr double round_trips_every = 0.5;
    // How many reads the others are ahead of this peer at the last read (INT64_MIN: nobody else plays).
    int64_t last_behind = INT64_MIN;
    void publish(uint8_t waiting_for = 0, double waiting_since = 0);
    void tell(const std::string& sentence) {
        std::printf("[netplay] (on screen) %s\n", sentence.c_str());
        std::lock_guard lock(status_mutex);
        news.push_back(sentence);
    }
    // The read the game is at, and how far ahead of it the others' Inputs and hashes may be: a peer runs at
    // most the input delay ahead of the slowest (it waits for their pads), and sends its own pads the delay
    // ahead again, so twice the delay, an Inputs packet's reads and some spare. Anything further is a broken
    // or hostile peer, and refused, so a peer can't make this one keep pads or hashes without end.
    uint64_t current_read = 0;
    uint64_t horizon() {
        return current_read + 2 * (uint64_t)std::max(delays.before, delays.after) + conker::netplay::input_window + 16;
    }

    // The session is used by the thread connecting (the menu's, or the game's), then the game's, and by the
    // main thread as the program quits (finish): one at a time. `stopping` is set by finish, after which
    // the game thread leaves the session alone.
    std::recursive_mutex session_mutex;
    std::atomic<bool> stopping = false;
    // Watches for the program quitting, to say goodbye at once: finish only runs once the runtime has
    // ended every game thread, and by then the others have long given up on this peer (the game thread
    // may be waiting in on_read for pads that won't come, as they're waiting for this one's).
    std::thread quit_watcher;
    // This peer's hashes (the memory's, and each region's), and those of the others (the host's for a
    // joiner, every joiner's for the host), until they're compared.
    struct OwnHash {
        uint64_t hash = 0;
        std::array<uint64_t, conker::netplay::memory_regions> regions{};
    };
    std::map<uint32_t, OwnHash> own_hashes;
    std::map<std::pair<int, uint32_t>, Hash> their_hashes;
    // on_read's, while it runs: the memory kept for a desync report.
    const conker::lockstep::Memory* memory_now = nullptr;
    // A desync report holds each player's inputs for this many reads up to the desync's.
    constexpr uint32_t report_reads = 120;

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // "port 2", or "ports 2 and 3", for the log.
    std::string ports_text(uint8_t ports) {
        std::string numbers;
        const int count = std::popcount(ports);
        for (int port = 0, n = 0; port < conker::netplay::max_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            numbers += (n == 0 ? "" : n == count - 1 ? " and " : ", ") + std::to_string(port + 1);
            n++;
        }
        return (count == 1 ? "port " : "ports ") + numbers;
    }

    // "Player 2", or "Players 2 and 3", for the player.
    std::string players_text(uint8_t ports) {
        std::string text = ports_text(ports);
        text.replace(0, 4, "Player");
        return text;
    }

    void print_waits() {
        if (waits.printed || waits.reads == 0) return;
        waits.printed = true;
        std::printf("[netplay] waits: %llu of %llu reads waited for the others, %.0f ms in all, the longest %.0f ms (read %llu); "
            "%llu reads came over %.0f ms after the one before, the latest %.0f ms (read %llu); input delay %d, changed %llu times; "
            "caught up %llu times\n",
            (unsigned long long)waits.waited, (unsigned long long)waits.reads, waits.seconds * 1000, waits.longest * 1000,
            (unsigned long long)waits.longest_read, (unsigned long long)waits.late, late_seconds * 1000, waits.latest * 1000,
            (unsigned long long)waits.latest_read, delay, (unsigned long long)waits.delay_changes, (unsigned long long)waits.catch_ups);
    }

    void end_match(Reason reason, const char* why) {
        print_waits();
        // The game goes on without the match (the menu's): on real time.
        if (catching_up) set_catching_up(false);
        std::printf("[netplay] the match is over: %s\n", why);
        why_over = why;
        if (!left && session != nullptr) {
            session->leave(reason);
            left = true;
        }
        ended = true;
        publish();
    }

    // Leaves the match with a goodbye, and polls the library a moment for it to go out (it's sent as the
    // library is polled), or the others only hear the match is over when their connection times out.
    void say_goodbye() {
        std::scoped_lock lock(session_mutex);
        if (catching_up) set_catching_up(false);
        if (session != nullptr && !left) {
            session->leave(Reason::quit);
            left = true;
            for (int i = 0; i < 30; i++) {
                session->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        ended = true;
        publish();
    }

    std::string hex(uint64_t value, int digits = 16) {
        char text[17];
        std::snprintf(text, sizeof(text), "%0*llx", digits, (unsigned long long)value);
        return text;
    }

    // The regions of memory whose hashes differ (by the 16 bits a Hash carries).
    std::vector<int> differing_regions(const OwnHash& own, const Hash& theirs) {
        std::vector<int> regions;
        for (int i = 0; i < conker::netplay::memory_regions; i++) {
            if ((uint16_t)own.regions[i] != theirs.regions[i]) {
                regions.push_back(i);
            }
        }
        return regions;
    }

    // The game's addresses a region covers, as "0x80400000-0x8040FFFF".
    std::string region_addresses(int region) {
        const uint32_t start = 0x80000000u + (uint32_t)region * conker::netplay::memory_region_size;
        return "0x" + hex(start, 8) + "-0x" + hex(start + conker::netplay::memory_region_size - 1, 8);
    }

    // A path as UTF-8, for the log and the player (what string() gives can't hold every character on Windows).
    std::string path_text(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    Input idle_input(int port);

    // Writes this peer's desync report (lockstep.hpp) for a difference from `port`'s at `read`, and the memory
    // kept at that read beside it. Its path, or empty if it couldn't.
    std::string write_report(int port, uint32_t read, const OwnHash& own, const Hash& theirs, const std::vector<int>& regions) {
        std::error_code error;
        const std::filesystem::path folder = recomp::get_config_path() / "netplay-desyncs";
        std::filesystem::create_directories(folder, error);
        char stamp[32];
        const std::time_t time = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&time));
        const std::string name = std::string("desync-") + stamp + "-read" + std::to_string(read) + "-port" +
            std::to_string(std::countr_zero(own_ports) + 1);
        const std::filesystem::path path = folder / (name + ".json");
        const bool dumped = memory_now != nullptr && memory_now->dump(read, folder / (name + ".bin"));
        FILE* file = conker::lockstep::create_file(path);
        if (file == nullptr) {
            std::fprintf(stderr, "[netplay] %s: couldn't create the desync report\n", path_text(path).c_str());
            return {};
        }
        std::fprintf(file, "{\n  \"report\": \"Conker netplay desync\",\n  \"version\": 1,\n");
        std::fprintf(file, "  \"read\": %u,\n  \"ports\": [", read);
        for (int p = 0, n = 0; p < conker::netplay::max_players; p++) {
            if ((own_ports & (1u << p)) != 0) std::fprintf(file, "%s%d", n++ == 0 ? "" : ", ", p + 1);
        }
        std::fprintf(file, "],\n  \"compared_with_port\": %d,\n", port + 1);
        std::fprintf(file, "  \"build_id\": \"%s\",\n  \"build\": \"%s\",\n  \"rom\": \"%s\",\n  \"profile\": \"%s\",\n  \"mods\": \"%s\",\n",
            conker::netplay::build_id(), hex(sent.build).c_str(), hex(sent.rom).c_str(), hex(sent.profile).c_str(), hex(sent.mods).c_str());
        std::fprintf(file, "  \"players\": %d,\n  \"input_delay\": %d,\n  \"own_view\": %s,\n  \"view_pairs\": %u,\n",
            match_players, delay_at(read), match_own_view ? "true" : "false", match_pairs);
        std::fprintf(file, "  \"hash\": \"%s\",\n  \"their_hash\": \"%s\",\n  \"differing_regions\": [",
            hex(own.hash).c_str(), hex(theirs.hash).c_str());
        for (size_t i = 0; i < regions.size(); i++) std::fprintf(file, "%s%d", i == 0 ? "" : ", ", regions[i]);
        std::fprintf(file, "],\n  \"region_size\": %u,\n  \"regions\": [", conker::netplay::memory_region_size);
        for (int i = 0; i < conker::netplay::memory_regions; i++) {
            std::fprintf(file, "%s\"%s\"", i == 0 ? "" : i % 4 == 0 ? ",\n    " : ", ", hex(own.regions[i]).c_str());
        }
        std::fprintf(file, "],\n  \"their_regions\": [");
        for (int i = 0; i < conker::netplay::memory_regions; i++) {
            std::fprintf(file, "%s\"%s\"", i == 0 ? "" : i % 8 == 0 ? ",\n    " : ", ", hex(theirs.regions[i], 4).c_str());
        }
        std::fprintf(file, "],\n  \"memory\": %s,\n", dumped ? ("\"" + name + ".bin\"").c_str() : "null");
        // Each player's input for the reads up to the desync's (the reads before it made the memory compared;
        // the read's own is given after): [buttons, stick x, stick y, mouse x, y, gyro x, y, wheel, Controls].
        const uint32_t first = read > report_reads ? read - report_reads : 0;
        // Players that left: the read their port was idle from (the inputs then are the idle ones), or null.
        std::fprintf(file, "  \"idle_from\": [");
        for (int p = 0; p < match_players; p++) {
            std::fprintf(file, "%s%s", p == 0 ? "" : ", ", idle_from[p] == never ? "null" : std::to_string(idle_from[p]).c_str());
        }
        std::fprintf(file, "],\n  \"inputs_from_read\": %u,\n  \"inputs\": [", first);
        for (int p = 0; p < match_players; p++) {
            std::fprintf(file, "%s\n    [", p == 0 ? "" : ",");
            for (uint32_t r = first; r <= read && (r < received[p].size() || r >= idle_from[p]); r++) {
                const Input in = r < idle_from[p] ? received[p][r] : idle_input(p);
                std::fprintf(file, "%s[%u, %d, %d, %d, %d, %d, %d, %d, %u]", r == first ? "" : (r - first) % 4 == 0 ? ",\n     " : ", ",
                    in.pad.buttons, in.pad.stick_x, in.pad.stick_y, in.motion.mouse_x, in.motion.mouse_y,
                    in.motion.gyro_x, in.motion.gyro_y, in.motion.wheel, in.motion.controls);
            }
            std::fprintf(file, "]");
        }
        std::fprintf(file, "\n  ]\n}\n");
        if (std::fclose(file) != 0) {
            std::fprintf(stderr, "[netplay] %s: couldn't write the desync report\n", path_text(path).c_str());
            return {};
        }
        std::printf("[netplay] desync report written to %s%s\n", path_text(path).c_str(), dumped ? " (and its memory)" : "");
        return path_text(path);
    }

    // The game's memory at `read` isn't `port`'s: says where, writes the report, and ends the match.
    void desync(int port, uint32_t read, const OwnHash& own, const Hash& theirs) {
        const std::vector<int> regions = differing_regions(own, theirs);
        std::string where;
        for (size_t i = 0; i < regions.size() && i < 8; i++) {
            where += (i == 0 ? "" : ", ") + std::to_string(regions[i]) + " (" + region_addresses(regions[i]) + ")";
        }
        if (regions.size() > 8) where += " and " + std::to_string(regions.size() - 8) + " more";
        std::printf("[netplay] desync at read %u: port %d's game memory isn't this one's (%016llx, here %016llx), in %s\n",
            read, port + 1, (unsigned long long)theirs.hash, (unsigned long long)own.hash,
            regions.empty() ? "no region the region hashes tell apart" : ((regions.size() == 1 ? "region " : "regions ") + where).c_str());
        const std::string report = write_report(port, read, own, theirs, regions);
        std::string why = "the games went different ways (a desync)";
        if (!report.empty()) {
            why += ". A report is in " + report + ": please send it, with the other players' reports, when you report the problem";
        }
        end_match(Reason::desync, why.c_str());
    }

    // Compares every hash that both sides have for the same read. Peers are never more than the input
    // delay apart, so this peer's own are kept only for the last few comparisons.
    void compare_hashes() {
        for (auto it = their_hashes.begin(); it != their_hashes.end();) {
            const auto own = own_hashes.find(it->first.second);
            if (own == own_hashes.end()) {
                ++it;
                continue;
            }
            if (own->second.hash != it->second.hash) {
                desync(it->first.first, own->first, own->second, it->second);
                return;
            }
            it = their_hashes.erase(it);
        }
        while (own_hashes.size() > 8) {
            own_hashes.erase(own_hashes.begin());
        }
    }

    // A peer found a desync. Its hash for that read came before its goodbye, so if this peer's game is the
    // one it differs from, the comparison finds it here too (and writes this peer's report).
    void other_desynced() {
        compare_hashes();
        if (!ended) {
            end_match(Reason::desync, "a player's game went a different way (a desync)");
        }
    }

    // `port` plays no more from read `from`: an idle pad, with their last Controls (their camera's settings).
    void let_go(int port, uint64_t from) {
        idle_from[port] = from;
        std::printf("[netplay] port %d plays no more: no input from read %llu, the others play on\n", port + 1, (unsigned long long)from);
    }

    // Tells the player that `ports` left (`silently`: stopped answering), and when nobody else is left, that
    // this machine's players play on.
    void tell_left(uint8_t ports, bool silently) {
        {
            std::lock_guard lock(status_mutex);
            shown.left |= ports;
            if (silently) shown.left_silently |= ports;
        }
        tell(players_text(ports) + (silently ? " stopped answering (they may have quit)." : " left."));
        for (int port = 0; port < match_players; port++) {
            if ((own_ports & (1u << port)) == 0 && idle_from[port] == never) return;
        }
        tell("Everyone else has left: the match goes on here.");
    }

    // The delay becomes `to` from read `from`.
    void change_delay(int to, uint64_t from) {
        std::printf("[netplay] read %llu: the input delay becomes %d from read %llu (it's %d)\n", (unsigned long long)current_read, to,
            (unsigned long long)from, delays.after);
        delays = { from, delays.after, to };
        waits.delay_changes++;
    }

    Input idle_input(int port) {
        Input idle;
        idle.motion.controls = received[port].empty() ? conker::motion::default_controls : received[port].back().motion.controls;
        return idle;
    }

    void handle(const Event& event) {
        // Once the match is over (a poll's events go on after the one that ended it), nothing more: the first
        // reason stays the one told, and a desync's report with it.
        if (ended) {
            return;
        }
        switch (event.type) {
            case Event::Type::joined:
            case Event::Type::welcomed:
            case Event::Type::started:
                break;
            case Event::Type::left:
                // The host's: a joiner's machine left (all its ports), saying goodbye or not answering.
                std::printf("[netplay] %s left: %s\n", ports_text(event.ports).c_str(), conker::netplay::describe(event.reason));
                for (int port = 0; port < conker::netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) gone[port] = true;
                }
                gone_silently = gone_silently || event.reason == Reason::timeout;
                if (event.reason == Reason::desync) {
                    other_desynced();
                }
                else if (!ended) {
                    // The others play on: every input of theirs this host had was passed on as it came, so
                    // the read after the last is the same for everyone, and none of them is past it.
                    uint8_t let_go_ports = 0;
                    for (int port = 0; port < conker::netplay::max_players; port++) {
                        if ((event.ports & (1u << port)) != 0 && idle_from[port] == never) {
                            let_go(port, received[port].size());
                            session->send(Gone{ (uint8_t)port, (uint32_t)received[port].size(), (uint8_t)(event.reason == Reason::timeout) });
                            let_go_ports |= (uint8_t)(1u << port);
                        }
                    }
                    if (let_go_ports != 0) {
                        tell_left(let_go_ports, event.reason == Reason::timeout);
                    }
                }
                break;
            case Event::Type::ended:
                if (event.reason == Reason::timeout) {
                    std::printf("[netplay] the host stopped answering (it may have quit)\n");
                }
                else {
                    std::printf("[netplay] the host ended the match: %s\n", conker::netplay::describe(event.reason));
                }
                // Every pad comes through the host.
                for (bool& g : gone) g = true;
                gone_silently = gone_silently || event.reason == Reason::timeout;
                left = true;
                if (event.reason == Reason::desync) {
                    other_desynced();
                }
                break;
            case Event::Type::packet:
                if (const Inputs* inputs = std::get_if<Inputs>(&event.packet)) {
                    std::vector<Input>& got = received[inputs->port];
                    if ((own_ports & (1u << inputs->port)) != 0 || inputs->port >= match_players || inputs->first_read != got.size() ||
                        (uint64_t)inputs->first_read + inputs->count > horizon()) {
                        end_match(Reason::protocol, "a player's inputs came out of order");
                        return;
                    }
                    for (int i = 0; i < inputs->count; i++) {
                        got.push_back({ inputs->pads[i], inputs->motions[i] });
                    }
                }
                else if (const Gone* g = std::get_if<Gone>(&event.packet)) {
                    // The host's word that a player left: from the read after the last of their inputs it passed on,
                    // all of which came first.
                    if (idle_from[g->port] != never || g->from_read != received[g->port].size()) {
                        end_match(Reason::protocol, "the host said a player left at a read that doesn't fit");
                        return;
                    }
                    std::printf("[netplay] %s left%s\n", ports_text((uint8_t)(1u << g->port)).c_str(), g->silent ? " (stopped answering)" : "");
                    gone[g->port] = true;
                    let_go(g->port, g->from_read);
                    tell_left((uint8_t)(1u << g->port), g->silent != 0);
                }
                else if (const Delay* d = std::get_if<Delay>(&event.packet)) {
                    // The host's: from a read this peer hasn't sent its pads at yet, once the last change is made.
                    if (d->from_read <= current_read || current_read < delays.from) {
                        end_match(Reason::protocol, "the host changed the input delay at a read that doesn't fit");
                        return;
                    }
                    change_delay(d->delay, d->from_read);
                }
                else if (const Hash* hash = std::get_if<Hash>(&event.packet)) {
                    // Hashes come every hash_every reads, within the horizon; one for a read this peer no
                    // longer has its own of can't be compared, and is dropped.
                    if (hash->read % hash_every != 0 || hash->read > horizon()) {
                        end_match(Reason::protocol, "a player sent a broken hash");
                        return;
                    }
                    if (own_hashes.empty() || hash->read >= own_hashes.begin()->first) {
                        their_hashes[{ event.port, hash->read }] = *hash;
                    }
                    joiners_long_waits = std::max(joiners_long_waits, hash->long_waits);
                }
                break;
        }
    }

    // This machine's player `local`'s port: the `local`th of its own (-1 if there's none).
    int nth_own_port(int local) {
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((own_ports & (1u << port)) != 0 && local-- == 0) {
                return port;
            }
        }
        return -1;
    }

    void send_own(uint64_t read, int port, Input input) {
        Pad& pad = input.pad;
        pad.buttons &= (uint16_t)~conker::netplay::reserved_buttons;
        pad.err_no = 0;     // a player's port always has a controller
        // CONKER_NETPLAY_LOG_PADS: each change of this peer's buttons, for debugging.
        static const bool log_pads = std::getenv("CONKER_NETPLAY_LOG_PADS") != nullptr;
        if (log_pads && (received[port].empty() || received[port].back().pad.buttons != pad.buttons)) {
            std::printf("[netplay] read %llu: port %d's buttons %04x\n", (unsigned long long)read, port + 1, pad.buttons);
        }
        Inputs inputs;
        inputs.port = (uint8_t)port;
        inputs.first_read = (uint32_t)read;
        inputs.count = 1;
        inputs.pads[0] = pad;
        inputs.motions[0] = input.motion;
        session->send(inputs);
        received[port].push_back(input);
    }

    // The input delay auto chooses, from the round trips to the joiners (ms): enough reads that a pad sent
    // the delay ahead arrives before it's needed, along the slowest way a pad goes. The host passes the
    // joiners' on, so between two joiners that's half of each of their round trips, the two slowest; with
    // one joiner, half of its round trip. The game can wait a few ms at a read and still keep up (a read
    // is a frame, 33 ms at 30 frames a second, most of it spare), so a LAN's sub-millisecond trips give 0:
    // the others' inputs reach the game as soon as this one's own. Each read past that hides a frame's
    // worth of trip, with a few ms for the trip to vary.
    constexpr double spare_ms = 4.0;    // for the trip varying
    int delay_needed(std::vector<int> round_trips, double spare) {
        constexpr double frame_ms = 1000.0 / 30.0;
        constexpr double wait_ms = 8.0;     // what a read can wait without the game falling behind
        std::sort(round_trips.rbegin(), round_trips.rend());
        double one_way = 0;
        for (size_t i = 0; i < round_trips.size() && i < 2; i++) {
            one_way += round_trips[i] / 2.0;
        }
        const double ahead = one_way + spare - wait_ms;
        return std::min(ahead <= 0 ? 0 : (int)std::ceil(ahead / frame_ms), conker::netplay::max_delay);
    }

    std::string trips_text(const std::vector<int>& round_trips) {
        std::string list;
        for (int t : round_trips) list += (list.empty() ? "" : ", ") + std::to_string(t);
        return list.empty() ? std::string("unknown") : list;
    }

    int delay_for(const std::vector<int>& round_trips, size_t joiners) {
        // A round trip the library doesn't know yet: the delay a match had before it was chosen.
        if (round_trips.size() < joiners) {
            std::printf("[netplay] the round trips to the players aren't all known: input delay 2\n");
            return 2;
        }
        const int chosen = delay_needed(round_trips, spare_ms);
        std::printf("[netplay] round trips to the players: %s ms: input delay %d\n", trips_text(round_trips).c_str(), chosen);
        return chosen;
    }

    // The host, with auto, at read `read` (every hash_every reads): changes the delay if the round trips have
    // wanted another for long enough. From the read after the furthest a joiner can send its pads at before
    // it hears: a joiner is at most the delay ahead (it needs the host's pads, sent the delay ahead), and
    // can make the read after its last of the host's pads before the Delay (sent after them) arrives.
    void adapt_delay(uint64_t read) {
        const std::vector<int> trips = session->round_trips_ms();
        if (trips.empty() || trips.size() < (size_t)session->joiners() || read < delays.from + settle_reads) {
            late_from_waiting = 0;
            joiners_long_waits = 0;
            wanted_higher = 0;
            wanted_lower = 0;
            return;
        }
        const uint64_t late = late_from_waiting;
        late_from_waiting = 0;
        const uint64_t most_long_waits = std::max<uint64_t>(long_waits, joiners_long_waits);
        joiners_long_waits = 0;
        const int needed = delay_needed(trips, spare_ms);
        const int roomy = delay_needed(trips, spare_ms + lower_spare_ms);
        const bool waiting = (late >= late_reads_to_raise || most_long_waits >= long_waits_to_raise) &&
            delay < conker::netplay::max_delay && raised_for_waiting < most_raised_for_waiting;
        wanted_higher = needed > delay || waiting ? wanted_higher + 1 : 0;
        wanted_lower = roomy < delay && late == 0 && most_long_waits < long_waits_to_raise ? wanted_lower + 1 : 0;
        if (wanted_higher < raise_checks && wanted_lower < lower_checks) {
            return;
        }
        int to = roomy;
        if (wanted_higher >= raise_checks && needed > delay) {
            to = needed;
            raised_for_waiting = 0;
        }
        else if (wanted_higher >= raise_checks) {
            to = delay + 1;
            raised_for_waiting++;
        }
        else if (raised_for_waiting > 0) {
            to = delay - 1;
            raised_for_waiting--;
        }
        std::printf("[netplay] round trips to the players: %s ms; %llu reads late from waiting, at most %llu of %llu waited over "
            "%.0f ms, since the last check\n", trips_text(trips).c_str(), (unsigned long long)late, (unsigned long long)most_long_waits,
            (unsigned long long)hash_every, long_wait_ms);
        change_delay(to, read + delay + 2);
        session->send(Delay{ (uint8_t)to, (uint32_t)delays.from });
        wanted_higher = 0;
        wanted_lower = 0;
    }

    // The read a peer is at whose pads reach `last`: the earliest it can be, as the delay changes.
    int64_t read_sending(int64_t last) {
        const int64_t from = (int64_t)delays.from;
        if (last <= from - 1 + delays.before) return last - delays.before;
        if (last >= from + delays.after) return last - delays.after;
        return from - 1;
    }

    // The ports whose pad for `read` isn't here yet (and who haven't left), bit n for port n.
    uint8_t missing(uint64_t read) {
        uint8_t ports = 0;
        for (int port = 0; port < match_players; port++) {
            if (received[port].size() <= read && read < idle_from[port]) ports |= (uint8_t)(1u << port);
        }
        return ports;
    }
}

bool conker::lockstep::parse_option(int argc, char** argv, int& i) {
    const char* option = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    auto number = [&](long low, long high, long& out) {
        char* end = nullptr;
        out = value != nullptr ? std::strtol(value, &end, 10) : 0;
        if (value == nullptr || end == value || *end != '\0' || out < low || out > high) {
            std::fprintf(stderr, "[netplay] %s needs a number from %ld to %ld\n", option, low, high);
            options_valid = false;
        }
        if (value != nullptr) i++;
    };
    long n = 0;
    if (std::strcmp(option, "--netplay-host") == 0) {
        number(1, 65535, n);
        host_port = (int)n;
        return true;
    }
    if (std::strcmp(option, "--players") == 0) {
        number(2, netplay::max_players, n);
        wanted_players = (int)n;
        return true;
    }
    if (std::strcmp(option, "--local-players") == 0) {
        number(1, netplay::max_players - 1, n);
        local_option = (int)n;
        return true;
    }
    if (std::strcmp(option, "--split-screen") == 0) {
        host_own_view = false;
        return true;
    }
    if (std::strcmp(option, "--input-delay") == 0) {
        if (value != nullptr && std::strcmp(value, "auto") == 0) {
            delay_option = auto_delay;
            i++;
            return true;
        }
        number(0, netplay::max_delay, n);
        delay_option = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-fake-network") == 0) {
        // e.g. lag=100,jitter=10,loss=5,reorder=2,dup=1,outage=300:3,lag-at=600:150
        std::string spec = value != nullptr ? value : "";
        if (value != nullptr) i++;
        faking = true;
        bool good = !spec.empty();
        for (size_t at = 0; good && at < spec.size();) {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(at, end - at);
            at = end + 1;
            const size_t equals = item.find('=');
            const std::string key = item.substr(0, equals);
            const char* text = equals == std::string::npos ? "" : item.c_str() + equals + 1;
            char* rest = nullptr;
            const double a = std::strtod(text, &rest);
            double b = 0;
            const bool pair = *rest == ':';
            if (pair) b = std::strtod(rest + 1, &rest);
            good = rest != text && *rest == '\0' && a >= 0 && b >= 0;
            if (!good) break;
            if (key == "lag" && !pair && a <= 2000) fake.lag_ms = (int)a;
            else if (key == "jitter" && !pair && a <= 500) fake.jitter_ms = (float)a;
            else if (key == "loss" && !pair && a <= 50) fake.loss = (float)a;
            else if (key == "reorder" && !pair && a <= 50) fake.reorder = (float)a;
            else if (key == "dup" && !pair && a <= 50) fake.dup = (float)a;
            else if (key == "outage" && pair && b > 0 && b <= 8) { outage_at = (uint64_t)a; outage_seconds = b; }
            else if (key == "lag-at" && pair && b <= 2000) { lag_at = (uint64_t)a; lag_then = (int)b; }
            else good = false;
        }
        if (!good) {
            std::fprintf(stderr, "[netplay] --netplay-fake-network takes lag=<ms>, jitter=<ms>, loss=<%%>, reorder=<%%>, dup=<%%>, "
                "outage=<read>:<s> (8 s at most) and lag-at=<read>:<ms>, separated by commas\n");
            options_valid = false;
        }
        return true;
    }
    if (std::strcmp(option, "--netplay-stall-at") == 0) {
        unsigned long long at = 0;
        long ms = 0;
        char end = 0;
        if (value == nullptr || std::sscanf(value, "%llu:%ld%c", &at, &ms, &end) != 2 || ms <= 0 || ms > 8000) {
            std::fprintf(stderr, "[netplay] --netplay-stall-at takes <read>:<ms>, up to 8000 ms\n");
            options_valid = false;
        }
        stall_at = at;
        stall_ms = ms;
        if (value != nullptr) i++;
        return true;
    }
    if (std::strcmp(option, "--netplay-desync-at") == 0) {
        number(0, LONG_MAX, n);
        desync_at = (uint64_t)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-address") == 0) {
        if (value == nullptr) {
            std::fprintf(stderr, "[netplay] --netplay-address needs this computer's address, as the others reach it\n");
            options_valid = false;
            return true;
        }
        host_address = argv[++i];
        return true;
    }
    if (std::strcmp(option, "--netplay-join") == 0) {
        if (value == nullptr) {
            std::fprintf(stderr, "[netplay] --netplay-join needs the host's join code\n");
            options_valid = false;
            return true;
        }
        join_where = argv[++i];
        return true;
    }
    return false;
}

bool conker::lockstep::active() {
    return host_port > 0 || !join_where.empty();
}

void conker::lockstep::set_host(int port, int players, bool own_view, int input_delay, int local) {
    host_address.clear();
    host_port = port;
    wanted_players = players;
    local_option = local;
    host_own_view = own_view;
    delay_option = input_delay < 0 ? auto_delay : std::min(input_delay, netplay::max_delay);
    join_where.clear();
}

void conker::lockstep::set_join(const std::string& where, int local) {
    host_port = -1;
    join_where = where;
    local_option = local;
}

bool conker::lockstep::prepare() {
    if (!options_valid) {
        return false;
    }
    if (host_port > 0 && !join_where.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-host and --netplay-join can't be used together\n");
        return false;
    }
    if (host_port > 0 && local_option >= wanted_players) {
        std::fprintf(stderr, "[netplay] --local-players %d needs a match of more players than that (--players)\n", local_option);
        return false;
    }
    // The others join by the code, which holds the match's key: without the address there's none to give.
    if (host_port > 0 && host_address.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-host needs --netplay-address <this computer's address>, for the join code\n");
        return false;
    }
    return true;
}

std::string conker::lockstep::join_code(std::string address) {
    std::scoped_lock lock(session_mutex);
    while (!address.empty() && address.back() == ' ') address.pop_back();
    while (!address.empty() && address.front() == ' ') address.erase(address.begin());
    if (session == nullptr || !session->hosting() || address.empty()) {
        return {};
    }
    std::string code = session->join_code(address);
    if (code.empty()) {
        // Without its port: this match's. An IPv6 address has colons of its own, so brackets go around it.
        const std::string port = std::to_string(host_port);
        code = session->join_code(address.find(':') != std::string::npos && address.front() != '['
            ? "[" + address + "]:" + port : address + ":" + port);
    }
    return code;
}

namespace {
    int joined = 0;             // host: the players in so far, the host too
    double connecting_since = 0;
    bool match_started = false;

    // Updates what status() gives, with the ports the read waits for since `waiting_since` (none: it's not
    // waiting). With the session's lock held, or the session alone.
    void publish(uint8_t waiting_for, double waiting_since) {
        const bool playing = match_started && !ended && session != nullptr;
        std::array<int, conker::netplay::max_players> trips{ -1, -1, -1, -1 };
        const double t = now();
        const bool ask = playing && t - round_trips_at >= round_trips_every;
        if (ask) {
            trips = session->round_trips_by_port();
            round_trips_at = t;
        }
        std::lock_guard lock(status_mutex);
        shown.playing = playing;
        shown.hosting = session != nullptr && session->hosting();
        shown.players = match_players;
        shown.own_ports = own_ports;
        shown.input_delay = delay;
        shown.others_playing = last_behind != INT64_MIN;
        shown.ahead = shown.others_playing ? -last_behind : 0;
        shown.waiting_for = waiting_for;
        shown.waiting_since = waiting_since;
        shown.catching_up = catching_up;
        shown.catching_up_since = catching_up_since;
        if (ask) shown.round_trip_ms = trips;
    }
}

conker::lockstep::Status conker::lockstep::status() {
    std::lock_guard lock(status_mutex);
    return shown;
}

double conker::lockstep::now() {
    return ::now();
}

std::vector<std::string> conker::lockstep::take_news() {
    std::lock_guard lock(status_mutex);
    std::vector<std::string> out;
    out.swap(news);
    return out;
}

bool conker::lockstep::begin(const netplay::Hello& me, std::string& error) {
    std::scoped_lock lock(session_mutex);
    // No mods in a match (texture packs aside): a mod is native code the others would have to trust. The
    // host refuses a joiner with any (the handshake's mods hash); this says so before connecting.
    if (me.mods != 0) {
        error = "netplay allows no mods but texture packs: turn them off in the Mods menu.";
        return false;
    }
    if (!library_started) {
        if (!netplay::startup(error)) {
            error = "couldn't start the networking: " + error;
            return false;
        }
        library_started = true;
    }
    if (faking) {
        fake.outage = false;
        netplay::set_fake_network(fake);
        std::printf("[netplay] faking a bad network: lag %d ms, jitter %.0f ms, loss %.1f%%, reorder %.1f%%, dup %.1f%%\n",
            fake.lag_ms, fake.jitter_ms, fake.loss, fake.reorder, fake.dup);
    }
    cancel();
    session = new netplay::Session();
    const bool hosting = host_port > 0;
    netplay::Hello mine = me;
    mine.players = (uint8_t)local_option;
    if (hosting ? !session->host((uint16_t)host_port, wanted_players, mine, error, local_option) : !session->join(join_where, mine, error)) {
        delete session;
        session = nullptr;
        return false;
    }
    sent = me;
    own_ports = hosting ? session->ports() : 0;
    match_players = hosting ? wanted_players : 0;
    joined = hosting ? local_option : 0;
    connecting_since = now();
    match_started = false;
    ended = false;
    left = false;
    current_read = 0;
    match_own_view = false;
    match_pairs = 0;
    why_over.clear();
    waits = {};
    delays = {};
    wanted_higher = 0;
    wanted_lower = 0;
    raised_for_waiting = 0;
    late_from_waiting = 0;
    long_waits = 0;
    joiners_long_waits = 0;
    outage_until = 0;
    if (catching_up) set_catching_up(false);
    initial_sent = false;
    for (auto& pads : received) pads.clear();
    gone = {};
    idle_from.fill(never);
    gone_silently = false;
    last_behind = INT64_MIN;
    round_trips_at = 0;
    {
        std::lock_guard status_lock(status_mutex);
        shown = {};
        news.clear();
    }
    own_hashes.clear();
    their_hashes.clear();
    if (hosting) {
        std::printf("[netplay] hosting a match of %d on UDP port %d, %d of them here: waiting for the players\n",
            wanted_players, host_port, local_option);
        if (!host_address.empty()) {
            const std::string code = join_code(host_address);
            if (code.empty()) {
                std::fprintf(stderr, "[netplay] %s isn't an address: no join code\n", host_address.c_str());
            }
            else {
                std::printf("[netplay] join code %s (the others join with it: it holds the match's key)\n", code.c_str());
                std::fflush(stdout);
            }
        }
    }
    else {
        std::printf("[netplay] joining by code\n");
    }
    return true;
}

conker::lockstep::Progress conker::lockstep::poll_connect(std::string& status) {
    std::scoped_lock lock(session_mutex);
    if (session == nullptr) {
        status = "Not connecting.";
        return Progress::failed;
    }
    const bool hosting = session->hosting();
    for (const Event& event : session->poll()) {
        switch (event.type) {
            case Event::Type::joined:
                joined += std::popcount(event.ports);
                std::printf("[netplay] %s joined (%d of %d)\n", ports_text(event.ports).c_str(), joined, match_players);
                if (joined == match_players) {
                    const int chosen = delay_option == auto_delay ? delay_for(session->round_trips_ms(), (size_t)session->joiners()) : delay_option;
                    if (session->start(host_own_view, chosen)) {
                        match_started = true;
                        match_own_view = host_own_view;
                        match_pairs = session->pairs();
                        delay = chosen;
                        delays = { 0, chosen, chosen };
                    }
                }
                break;
            case Event::Type::left:
                // Once started (a joiner leaving in the poll the match started in), as in the match: Gone.
                if (match_started) {
                    handle(event);
                    break;
                }
                // Before the start, a joiner leaving frees its ports, for the next to start afresh.
                joined -= std::popcount(event.ports);
                for (int port = 0; port < netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) {
                        received[port].clear();
                        gone[port] = false;
                    }
                }
                std::printf("[netplay] %s left: %s\n", ports_text(event.ports).c_str(), netplay::describe(event.reason));
                break;
            case Event::Type::welcomed:
                own_ports = event.ports;
                match_players = event.players;
                std::printf("[netplay] in, on %s of %d: waiting for the others\n", ports_text(own_ports).c_str(), match_players);
                break;
            case Event::Type::started:
                match_started = true;
                match_own_view = event.own_view;
                match_pairs = event.pairs;
                delay = event.delay;
                delays = { 0, event.delay, event.delay };
                break;
            case Event::Type::ended:
                std::printf("[netplay] couldn't join: %s (%s)\n", netplay::describe(event.reason), event.detail.c_str());
                status = std::string("Couldn't join: ") + netplay::describe(event.reason) + ".";
                // Full for this machine's players, not for one: fewer may fit.
                if (event.reason == Reason::full && local_option > 1) {
                    status += " Fewer players on this computer may fit.";
                }
                left = true;
                cancel();
                return Progress::failed;
            case Event::Type::packet:
                handle(event);
                break;
        }
    }
    if (match_started) {
        std::printf("[netplay] the match starts: %d players, this is %s, input delay %d reads%s\n", match_players,
            ports_text(own_ports).c_str(), delay, match_own_view ? ", everyone sees only their own view" : "");
        publish();
        status = "The match starts.";
        return Progress::started;
    }
    if (hosting && now() - connecting_since > join_seconds) {
        std::printf("[netplay] not every player joined in %.0f minutes\n", join_seconds / 60);
        status = "Not every player joined in " + std::to_string((int)(join_seconds / 60)) + " minutes.";
        cancel();
        return Progress::failed;
    }
    if (hosting) {
        status = "Waiting for players: " + std::to_string(joined) + " of " + std::to_string(match_players) + " in.";
    }
    else if (own_ports == 0) {
        status = "Connecting...";
    }
    else {
        // "player 2", "players 2 and 3".
        std::string as = ports_text(own_ports);
        as.replace(0, 4, "player");
        status = "In, as " + as + " of " + std::to_string(match_players) + ": waiting for the others.";
    }
    return Progress::waiting;
}

void conker::lockstep::cancel() {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr) {
        say_goodbye();
        delete session;
        session = nullptr;
    }
    match_started = false;
    publish();
}

bool conker::lockstep::connected() {
    return session != nullptr && match_started;
}

const conker::netplay::Hello& conker::lockstep::sent_hello() {
    return sent;
}

bool conker::lockstep::connect(const netplay::Hello& me) {
    std::string error;
    if (!begin(me, error)) {
        std::fprintf(stderr, "[netplay] %s\n", error.c_str());
        return false;
    }
    std::string status;
    while (true) {
        switch (poll_connect(status)) {
            case Progress::started:
                return true;
            case Progress::failed:
                std::fprintf(stderr, "[netplay] %s\n", status.c_str());
                return false;
            case Progress::waiting:
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int conker::lockstep::port() {
    return own_ports == 0 ? -1 : std::countr_zero(own_ports);
}

uint8_t conker::lockstep::ports() {
    return own_ports;
}

int conker::lockstep::local_players() {
    return std::popcount(own_ports);
}

int conker::lockstep::port_of_local(int local) {
    return nth_own_port(local);
}

int conker::lockstep::players() {
    return match_players;
}

int conker::lockstep::input_delay() {
    return delay;
}

bool conker::lockstep::own_view_for_everyone() {
    return match_started && match_own_view;
}

uint8_t conker::lockstep::view_pairs() {
    return match_started ? match_pairs : 0;
}

bool conker::lockstep::on_read(uint64_t read, ultramodern::input::Pad* pads, netplay::Motion* motions, const InputSource& source,
    const Memory& memory, uint8_t* rdram) {
    std::unique_lock lock(session_mutex);
    struct MemoryNow {
        MemoryNow(const Memory& memory) { memory_now = &memory; }
        ~MemoryNow() { memory_now = nullptr; }
    } memory_while_here(memory);
    if (ended || stopping) {
        return false;
    }
    current_read = read;
    delay = delay_at(read);
    session->set_horizon(horizon());
    if (!initial_sent) {
        if (!quit_watcher.joinable()) {
            quit_watcher = std::thread([] {
                while (!stopping && !exited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (exited) {
                    std::printf("[netplay] quitting: leaving the match\n");
                    stopping = true;
                    say_goodbye();
                }
            });
        }
    }
    // This machine's players' input for read + delay, and for any read before it not sent yet: at the first read,
    // the reads before it (nobody has played yet), and as the delay goes up, those it skips (the controllers'
    // pads now again: the movement goes with the first). As it goes down, the reads up to it are sent already.
    if (read + delay < UINT32_MAX - netplay::input_window) {
        for (int local = 0; local < std::popcount(own_ports); local++) {
            const int port = nth_own_port(local);
            for (uint64_t r = received[port].size(); r <= read + delay; r++) {
                send_own(r, port, source(r, initial_sent || r == read + delay ? pads : nullptr, local));
            }
        }
    }
    initial_sent = true;

    if (faking && read == outage_at) {
        fake.outage = true;
        netplay::set_fake_network(fake);
        outage_until = now() + outage_seconds;
        std::printf("[netplay] read %llu: the network is out for %.1f s (--netplay-fake-network)\n", (unsigned long long)read, outage_seconds);
    }
    if (faking && read == lag_at) {
        fake.lag_ms = lag_then;
        netplay::set_fake_network(fake);
        std::printf("[netplay] read %llu: the lag is %d ms now (--netplay-fake-network)\n", (unsigned long long)read, lag_then);
    }
    if (read == stall_at) {
        std::printf("[netplay] read %llu: the game stops for %ld ms (--netplay-stall-at)\n", (unsigned long long)read, stall_ms);
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        lock.lock();
        if (stopping) {
            return false;
        }
    }
    if (read == desync_at) {
        rdram[recomp::save_states::game_memory_size / 2] ^= 0x5A;
        std::printf("[netplay] read %llu: game memory changed, as --netplay-desync-at asked\n", (unsigned long long)read);
    }
    if (read % hash_every == 0) {
        OwnHash& own = own_hashes[(uint32_t)read];
        own.hash = memory.hash();
        own.regions = memory.keep(read);
        Hash sent_hash{ (uint32_t)read, own.hash };
        for (int i = 0; i < netplay::memory_regions; i++) {
            sent_hash.regions[i] = (uint16_t)own.regions[i];
        }
        sent_hash.long_waits = (uint8_t)std::min<uint64_t>(long_waits, 255);
        session->send(sent_hash);
    }

    const double since = now();
    double last_note = since;
    uint8_t waiting_for = 0;
    while (true) {
        if (outage_until != 0 && now() >= outage_until) {
            outage_until = 0;
            fake.outage = false;
            netplay::set_fake_network(fake);
            std::printf("[netplay] read %llu: the network is back (--netplay-fake-network)\n", (unsigned long long)read);
        }
        for (const Event& event : session->poll()) {
            handle(event);
        }
        compare_hashes();
        if (ended) {
            return false;
        }
        const uint8_t missing_ports = missing(read);
        if (missing_ports == 0) {
            break;
        }
        if (missing_ports != waiting_for) {
            waiting_for = missing_ports;
            publish(waiting_for, since);
        }
        for (int port = 0; port < match_players; port++) {
            if (received[port].size() <= read && gone[port] && read < idle_from[port]) {
                end_match(Reason::quit, gone_silently ? "a player stopped answering (they may have quit)" : "a player left");
                return false;
            }
        }
        const double t = now();
        if (t - since > wait_seconds) {
            end_match(Reason::timeout, "a player's inputs stopped coming");
            return false;
        }
        if (t - last_note > 2.0) {
            std::printf("[netplay] read %llu: waiting for the other players\n", (unsigned long long)read);
            last_note = t;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        lock.lock();
        if (stopping) {
            return false;
        }
    }

    // How far behind the others this peer is: how many reads ahead of it they are, by how far their inputs reach.
    int64_t behind = INT64_MIN;
    for (int port = 0; port < match_players; port++) {
        if ((own_ports & (1u << port)) == 0 && idle_from[port] == never) {
            behind = std::max(behind, read_sending((int64_t)received[port].size() - 1) - (int64_t)read);
        }
    }
    last_behind = behind;
    if (!catching_up && behind >= behind_to_catch_up) {
        std::printf("[netplay] read %llu: %lld reads behind the others: catching up\n", (unsigned long long)read, (long long)behind);
        set_catching_up(true);
        caught_up_from = read;
        catching_up_since = now();
        waits.catch_ups++;
    }
    else if (catching_up && behind <= 0) {
        std::printf("[netplay] read %llu: caught up (%llu reads in %.0f ms)\n", (unsigned long long)read,
            (unsigned long long)(read - caught_up_from), (now() - catching_up_since) * 1000);
        set_catching_up(false);
    }

    const double done = now();
    const double waited = done - since;
    if (waits.reads > 0 && done - waits.last_read_at > late_seconds) {
        waits.late++;
        if (waited > 0.001) late_from_waiting++;
        if (done - waits.last_read_at > waits.latest) {
            waits.latest = done - waits.last_read_at;
            waits.latest_read = read;
        }
    }
    waits.last_read_at = done;
    waits.reads++;
    if (waited > 0.001) {
        waits.waited++;
        waits.seconds += waited;
        if (waited > waits.longest) {
            waits.longest = waited;
            waits.longest_read = read;
        }
    }
    if (waited * 1000 > long_wait_ms) long_waits++;
    if (delay_option == auto_delay && session->hosting() && read % hash_every == 0) {
        adapt_delay(read);
    }
    if (read % hash_every == 0) long_waits = 0;
    publish();

    for (int port = 0; port < match_players; port++) {
        const Input input = read < idle_from[port] ? received[port][read] : idle_input(port);
        pads[port] = { input.pad.buttons, input.pad.stick_x, input.pad.stick_y, input.pad.err_no };
        motions[port] = input.motion;
    }
    return true;
}

FILE* conker::lockstep::create_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

void conker::lockstep::end(netplay::Reason reason, const char* why) {
    std::scoped_lock lock(session_mutex);
    end_match(reason, why);
}

const std::string& conker::lockstep::why_ended() {
    return why_over;
}

void conker::lockstep::leave() {
    std::scoped_lock lock(session_mutex);
    print_waits();
    if (catching_up) set_catching_up(false);
    if (session != nullptr && !left) {
        session->leave(Reason::quit);
        left = true;
    }
    ended = true;
    publish();
}

void conker::lockstep::finish() {
    // On the main thread as the program quits: the game thread may still be in on_read, so it's told to
    // stop first, and the session is only touched under the lock. The session isn't deleted (the game
    // thread may still look at it), and the library is stopped next anyway.
    stopping = true;
    if (quit_watcher.joinable()) {
        quit_watcher.join();
    }
    say_goodbye();
    std::scoped_lock lock(session_mutex);
    if (library_started) {
        netplay::shutdown();
        library_started = false;
    }
}
