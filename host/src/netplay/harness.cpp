// The determinism harness (netplay, issue #126): before two machines can play in lockstep, the same
// inputs have to give the same game on each. This records the game's controller inputs and replays
// them, and logs a hash of the game's memory at each controller read, so two runs can be compared
// read by read (tools/netplay/compare_hashes.py finds the first read they differ at, and where).
// Built only with CONKER_NETPLAY.
//
//   --record <file>     writes the pads the game reads, at each controller read, to <file>
//   --replay <file>     gives the game the pads from <file> instead of the controllers' (it starts the
//                       game directly), and quits after the file's last read
//   --hash-log <file>   one line per controller read: its number and the XXH3-64 of the game's memory as
//                       the read is made (so it's the result of the reads before it)
//   --dump-at <n>       also writes the game's memory at read <n> to <hash-log>.<n>.bin, big-endian (the
//                       N64's byte order, as tools/netplay reads it); it can be given more than once
//   --ignore <a>:<n>    leaves <n> bytes from address <a> (both in hex, e.g. 800BE6F0:8) out of the hash
//                       and zeroes them in dumps, for differences already known (it can be given more
//                       than once; ranges are widened to whole 32-bit words)
//   --stop-after <n>    quits after read <n>
//   --ports <n>         the controllers the game sees: ports 1 to <n> connected, no paks (1 by default; a
//                       replay uses its recording's)
//   --real-time         runs the game on the time, as normally, not the virtual timeline (below), to compare
//   --fast              doesn't wait for real time on the timeline: as fast as the host can
//   --state-test <s>:<l>  takes a save state after read <s> and loads it at read <l> (later), so the
//                       game goes back to <s> and on again from there: the hash log then holds the
//                       reads after the load in place of the first ones (they should match a run
//                       without it). The keys' save states (F5, F7) don't work in that run
//   --rollback-test <every>:<back>  rollback's measurement (step 1): takes a save state at every
//                       read (the first safe point before it), and every <every> reads goes back <back> reads
//                       and plays them again; each read played again must hash as it did the first time. The
//                       hash log has each read once (so it matches a run without it); at the end it prints
//                       how long taking, loading and playing again took
//   --rollback-late <late>[:<seed>]  rollback's local test (step 3), with --replay: ports 2 on are
//                       other machines' players, whose inputs arrive up to <late> reads late (at random, from
//                       <seed>); till then each is predicted (their last known input), and once the real one
//                       differs the game goes back to that read and plays on again. A read's hash is logged
//                       once every input before it is known and was the real one, so the log matches a plain
//                       replay's
//   --rollback <reads>  in a netplay match (lockstep.hpp): the same going back, with the inputs predicted and
//                       arriving as they do; the hash log has each read once it's known to be right, as above
//   --jitter <us>       sleeps at random, up to <us> microseconds, at the game's OS calls and in the
//                       graphics thread: a test that the host's speed doesn't change the game
//   --own-view          records or hashes as a match whose rule is that every player sees only their own
//                       view (its culls, netplay/view.cpp), recorded in the recording; a replay goes by its
//                       recording's rule instead (not with --replay)
//   --view-pairs <ports>  ...with these ports' views shown two to a window (e.g. 12: ports 1 and 2, as a
//                       machine with two players), recorded too (not with --replay; within --ports)
//   --characters <n>    the most characters in a multiplayer game, 0 (the game's own), 12 or 16
//                       (more_bots.cpp): records or hashes so, recorded in the recording (not with --replay);
//                       hosting a match (lockstep.hpp), the match's rule
//   --netplay-host, --netplay-join, ...  a netplay match in lockstep (lockstep.hpp)
//
// Controller reads are counted from the first, so read <n> of one run is read <n> of another. The pads
// are the N64's own values (after the sticks are converted), as osContGetReadData gives them to the
// game: what netplay will send.
//
// While the harness is used, the game runs on the runtime's virtual timeline (deterministic mode,
// ultramodern/timeline.hpp): its clock, VIs, timers and task completions come at fixed points in its
// progress, not when the host gets to them, so the same inputs give the same game. And the netplay
// settings profile (profile.cpp) fixes the settings that change the game. A recording, a replay or a
// netplay match can't have mods on but texture packs.
//
// The recording: "CBFDPADS", then a little-endian u32 version (4) and u32 port count, u64 XXH3-64 of the
// ROM, u64 hash of the settings profile, u32 flags (1: every player sees only their own view, the match's
// rule: own_view_rule; bits 8-15: the most characters in a multiplayer game, 0 the game's own, 12 or 16:
// characters_rule) and u32 view pairs (the ports shown two to a window, bit n port n: view_pairs), then
// for each read the number of controllers the game reads (u8), four
// pads of 5 bytes each (u16 buttons, s8 stick x, s8 stick y, u8 err_no) and four players' movement of 13
// bytes each (s16 mouse x, y, s16 gyro x, y, s8 wheel, u32 Controls: netplay::Motion, motion.cpp). A replay
// is refused if its ROM or profile isn't this run's; a hash of 0 isn't checked (a made-up recording,
// tools/netplay/make_inputs.py). Version 3 had no flags and 16-bit Controls (11 bytes of movement), version 2
// no movement (none, with the default Controls), version 1 no hashes either (its header ends at the port
// count): it's replayed unchecked.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../conker.hpp"
#include "hello.hpp"
#include "lockstep.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/save_states.hpp"
#include "recomp.h"
#include "ultramodern/save_states.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/timeline.hpp"
#include "ultramodern/ultramodern.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace {
    constexpr char magic[8] = { 'C', 'B', 'F', 'D', 'P', 'A', 'D', 'S' };
    constexpr uint32_t version = 4;
    constexpr size_t header_size = 40;
    constexpr size_t header_size_v3 = 32;
    constexpr size_t header_size_v1 = 16;
    constexpr int max_pads = 4;
    constexpr size_t pad_size = 5;
    constexpr size_t motion_size = 13;
    constexpr size_t motion_size_v3 = 11;
    constexpr size_t read_size = 1 + max_pads * (pad_size + motion_size);
    constexpr size_t read_size_v3 = 1 + max_pads * (pad_size + motion_size_v3);
    constexpr size_t read_size_v2 = 1 + max_pads * pad_size;
    constexpr uint32_t own_view_flag = 1;
    constexpr int characters_shift = 8;
    constexpr uint32_t characters_flags = 0xFFu << characters_shift;
    constexpr uint64_t no_limit = UINT64_MAX;

    std::string record_path;
    std::string replay_path;
    std::string hash_log_path;
    std::vector<uint64_t> dump_at;
    uint64_t stop_after = no_limit;
    int ports = 1;
    // --state-test's.
    uint64_t state_save_at = no_limit;
    uint64_t state_load_at = no_limit;
    enum class StateRequest { None, Save, Load };
    StateRequest state_request = StateRequest::None;
    std::vector<uint8_t> state;
    std::vector<uint8_t> state_host;
    uint64_t state_next_read = 0;
    long state_log_position = 0;
    // --rollback-test's: a ring of states, by the read they're before, and what it took.
    uint64_t rollback_every = 0;
    uint64_t rollback_back = 0;
    struct Snapshot {
        uint64_t read = UINT64_MAX;
        std::vector<uint8_t> state;
        std::vector<uint8_t> host;
    };
    std::vector<Snapshot> snapshots;
    uint64_t rollback_to = UINT64_MAX;      // a load wanted: back to before this read
    uint64_t rollback_from = 0;             // the read the game was at, to play up to again
    uint64_t rerun_reads = 0;               // how many reads that is
    std::chrono::steady_clock::time_point rerun_started;
    bool rerunning = false;
    uint64_t rollback_reads_ahead = 0;      // reads made at least once (the hash log has these)
    std::vector<uint64_t> first_hashes;     // by read
    // --rollback-late's: the remote ports' inputs, when each arrives, what the game was given, and how far
    // it's known to be right.
    uint64_t late_max = 0;
    uint64_t late_seed = 1;
    struct ReadInputs {
        conker::netplay::Pad pads[4];
        conker::netplay::Motion motions[4];
    };
    std::vector<ReadInputs> late_used;                      // by read, in the game as it is now
    std::vector<uint64_t> late_arrival[4];                  // by port and read: the read (first time) it arrives at
    uint64_t late_known[4] = {};                            // by port: its reads known (they arrive in order)
    uint64_t late_verified = 0;                             // reads from 0 known and given as they were
    uint64_t late_logged = 0;                               // reads whose hash is logged
    std::vector<uint64_t> late_hash;                        // by read: as it was last made
    bool late_broken = false;                               // a rollback couldn't be done: the game is wrong (also --rollback's)
    uint64_t late_mispredicted = 0;
    constexpr uint64_t late_boot_reads = 30;

    // Desync recovery and coming back (lockstep.hpp): a joiner's state from the host, to load at the next safe point
    // that can (the game is given no read until then: `resync_holds` counts the ones it held), and the read it's before
    // (the game goes on from there: coming back, from a read of the match, past its own as it started).
    std::vector<uint8_t> resync_state;
    std::vector<uint8_t> resync_host;
    uint64_t resync_read = UINT64_MAX;
    uint64_t resync_holds = 0;
    std::string resync_error;
    // The reads a load waits for its threads to be where the state's were before it gives up (as a save state's 5 s).
    constexpr uint64_t resync_most_holds = 150;

    // --rollback's: lockstep predicts the others' inputs, and says when to go back (take_rollback).
    bool rollback_match() { return conker::lockstep::active() && conker::lockstep::rollback_window() > 0; }
    bool rollback_on() { return rollback_every != 0 || late_max != 0 || rollback_match(); }
    // A replay's last read is done with (it was made with every input right): it's quit.
    bool last_read_done = false;
    struct Timings {
        std::vector<double> us;
        void add(double value) { us.push_back(value); }
        std::string summary(const char* unit = "us") const {
            if (us.empty()) return "none";
            std::vector<double> sorted = us;
            std::sort(sorted.begin(), sorted.end());
            double total = 0;
            for (double v : sorted) total += v;
            char line[200];
            std::snprintf(line, sizeof(line), "%zu, mean %.1f %s, median %.0f, p99 %.0f, max %.0f", sorted.size(),
                total / sorted.size(), unit, sorted[sorted.size() / 2], sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)],
                sorted.back());
            return line;
        }
    };
    struct RollbackStats {
        Timings capture, host_save, restore, host_load, rerun_per_read, read_gap, load_wait_reads, late_depth;
        uint64_t safe_index = 0;                // safe points since the last read
        std::array<uint64_t, 16> taken_at{}, loaded_at{};  // by safe_index
        uint64_t asked_at_read = 0;
        uint64_t safe_points = 0, busy = 0, capture_failed = 0, not_now = 0, no_snapshot = 0, mismatches = 0;
        uint64_t given_up = 0, earlier_state = 0, later_load = 0;
        uint64_t reads_without_snapshot = 0, rollbacks = 0, reruns_done = 0;
        uint64_t first_mismatch = UINT64_MAX;
        std::chrono::steady_clock::time_point started, last_read_at;
        bool started_set = false;
    } rb;
    bool real_time = false;
    bool fast = false;
    // --own-view's: recording or hashing alone, as in a match whose rule is that everyone sees only their own view.
    bool own_view_option = false;
    uint8_t view_pairs_option = 0;  // --view-pairs'
    // --characters': recording or hashing alone, or hosting (0: the game's own; -1: not given).
    int characters_option = -1;
    uint64_t jitter_us = 0;
    bool options_valid = true;

    uint8_t* rdram = nullptr;
    FILE* record_file = nullptr;
    // The reads written to it: with rollback, the end, after which reads made the first time may follow (a match
    // that ended as reads were made again), cut off at the end.
    uint64_t recorded_to = 0;
    FILE* hash_log = nullptr;
    std::vector<uint8_t> replay;
    uint64_t replay_reads = 0;
    // Where the replay's reads start, their size, and its hashes (0: not checked); whether they hold movement.
    size_t replay_header = header_size;
    size_t replay_read_size = read_size;
    size_t replay_motion_size = motion_size;
    bool replay_motion = true;
    uint32_t replay_flags = 0;
    uint8_t replay_pairs = 0;

    // No movement, with these Controls.
    conker::netplay::Motion still(uint32_t controls = conker::motion::default_controls) {
        conker::netplay::Motion m;
        m.controls = controls;
        return m;
    }
    uint64_t replay_rom_hash = 0;
    uint64_t replay_profile_hash = 0;
    // Reads past this aren't logged, and the game is quit at the first.
    uint64_t last_read = no_limit;
    uint64_t reads = 0;
    // A netplay match that has ended: the game gets no more input.
    bool match_over = false;
    // A match started from the menu says it's over through this (frontend's netplay.cpp) instead of
    // quitting.
    std::function<void(const std::string&)> match_over_callback;
    bool activated = false;

    // The game is quit from a thread of its own, not the game thread that reads the controllers.
    std::thread quitter;
    std::mutex quit_mutex;
    std::condition_variable quit_wanted;
    bool quit_requested = false;
    bool quit_finished = false;

    bool active() {
        return !record_path.empty() || !replay_path.empty() || !hash_log_path.empty() || conker::lockstep::active();
    }

    bool parse_count(const char* text, uint64_t& out) {
        char* end = nullptr;
        unsigned long long value = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0') {
            return false;
        }
        out = value;
        return true;
    }

    void put_u32(uint8_t* out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out[i] = (uint8_t)(value >> (8 * i));
        }
    }

    uint32_t get_u32(const uint8_t* in) {
        return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    }

    void put_u64(uint8_t* out, uint64_t value) {
        put_u32(out, (uint32_t)value);
        put_u32(out + 4, (uint32_t)(value >> 32));
    }

    uint64_t get_u64(const uint8_t* in) {
        return (uint64_t)get_u32(in) | ((uint64_t)get_u32(in + 4) << 32);
    }

    // A replay's movement's Controls (at +9 of its movement): 32 bits, or 16 before version 4.
    uint32_t replay_controls(const uint8_t* m) {
        return replay_motion_size == motion_size ? get_u32(m + 9) : (uint32_t)(m[9] | (m[10] << 8));
    }

    // The whole recording, checked before the game starts: its header, its size and every read's
    // controller count, so a read during the game can't go wrong.
    bool load_replay() {
        FILE* file = std::fopen(replay_path.c_str(), "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't open it\n", replay_path.c_str());
            return false;
        }
        uint8_t buffer[65536];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            replay.insert(replay.end(), buffer, buffer + got);
        }
        std::fclose(file);
        if (replay.size() < header_size_v1 || std::memcmp(replay.data(), magic, sizeof(magic)) != 0) {
            std::fprintf(stderr, "[harness] %s isn't a recording of inputs\n", replay_path.c_str());
            return false;
        }
        const uint32_t recorded_version = get_u32(replay.data() + 8);
        if (recorded_version == 1) {
            replay_header = header_size_v1;
            replay_read_size = read_size_v2;
            replay_motion = false;
            std::printf("[harness] %s is a version 1 recording: its ROM and settings aren't checked\n", replay_path.c_str());
        }
        else if (recorded_version < 2 || recorded_version > version || replay.size() < (recorded_version == version ? header_size : header_size_v3)) {
            std::fprintf(stderr, "[harness] %s is a version %u recording; this build reads versions 1 to %u\n",
                replay_path.c_str(), recorded_version, version);
            return false;
        }
        else {
            replay_rom_hash = get_u64(replay.data() + 16);
            replay_profile_hash = get_u64(replay.data() + 24);
            if (recorded_version == version) {
                replay_flags = get_u32(replay.data() + 32);
                const uint32_t pairs = get_u32(replay.data() + 36);
                const uint32_t pair_ports = std::min<uint32_t>(get_u32(replay.data() + 12), max_pads);
                // Its view pairs: ports it has, an even number of them.
                if ((replay_flags & ~(own_view_flag | characters_flags)) != 0 ||
                    !conker::netplay::valid_characters((uint8_t)(replay_flags >> characters_shift)) ||
                    (pairs >> pair_ports) != 0 || std::popcount(pairs) % 2 != 0) {
                    std::fprintf(stderr, "[harness] %s has flags or view pairs this build doesn't know\n", replay_path.c_str());
                    return false;
                }
                replay_pairs = (uint8_t)pairs;
            }
            else {
                replay_header = header_size_v3;
                replay_read_size = recorded_version == 2 ? read_size_v2 : read_size_v3;
                replay_motion_size = motion_size_v3;
                replay_motion = recorded_version == 3;
            }
        }
        const uint32_t recorded_ports = get_u32(replay.data() + 12);
        if (recorded_ports < 1 || recorded_ports > max_pads) {
            std::fprintf(stderr, "[harness] %s has %u ports\n", replay_path.c_str(), recorded_ports);
            return false;
        }
        if ((replay.size() - replay_header) % replay_read_size != 0) {
            std::fprintf(stderr, "[harness] %s ends partway through a read\n", replay_path.c_str());
            return false;
        }
        replay_reads = (replay.size() - replay_header) / replay_read_size;
        for (uint64_t i = 0; i < replay_reads; i++) {
            if (replay[replay_header + i * replay_read_size] > max_pads) {
                std::fprintf(stderr, "[harness] %s: read %llu has %u controllers\n", replay_path.c_str(),
                    (unsigned long long)i, replay[replay_header + i * replay_read_size]);
                return false;
            }
            for (int port = 0; replay_motion && port < max_pads; port++) {
                const uint8_t* m = replay.data() + replay_header + i * replay_read_size + 1 + max_pads * pad_size + port * replay_motion_size;
                if (!conker::netplay::valid_controls(replay_controls(m))) {
                    std::fprintf(stderr, "[harness] %s: read %llu: port %d's controls aren't any\n", replay_path.c_str(),
                        (unsigned long long)i, port + 1);
                    return false;
                }
            }
        }
        ports = (int)recorded_ports;
        std::printf("[harness] replaying %llu reads from %s (%d ports)\n", (unsigned long long)replay_reads,
            replay_path.c_str(), ports);
        return true;
    }

    // What's left out of the hash and zeroed in dumps, as offsets into game memory: whole 32-bit words,
    // so they're the same bytes in the runtime's order and the N64's.
    struct Range {
        uint32_t offset;
        uint32_t size;
    };
    // --ignore's.
    std::vector<Range> ignored;

    // Where the runtime keeps a host pointer in game memory, which differs from run to run: each game
    // thread's OSThread holds its native context (8 bytes at 0x20, ultramodern's ultra64.h).
    constexpr uint32_t thread_context_offset = 0x20;
    constexpr uint32_t thread_context_size = 8;

    // The runtime's pointers and --ignore's ranges, in order.
    std::vector<Range> left_out() {
        std::vector<Range> out = ignored;
        for (const auto& thread : ultramodern::save_states::get_threads(rdram)) {
            const uint32_t offset = (uint32_t)thread.thread - 0x80000000u + thread_context_offset;
            if (offset + thread_context_size <= recomp::save_states::game_memory_size) {
                out.push_back({ offset, thread_context_size });
            }
        }
        std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
        return out;
    }

    uint64_t hash_memory() {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        uint32_t at = 0;
        for (const Range& skip : left_out()) {
            if (skip.offset > at) {
                XXH3_64bits_update(&state, rdram + at, skip.offset - at);
            }
            at = std::max(at, skip.offset + skip.size);
        }
        XXH3_64bits_update(&state, rdram + at, recomp::save_states::game_memory_size - at);
        return XXH3_64bits_digest(&state);
    }

    // Writes `memory` (game memory in the runtime's order) to `path`, with what's left out zeroed in `zero`.
    bool write_memory(const std::filesystem::path& path, const uint8_t* memory, const std::vector<Range>& zero) {
        FILE* file = conker::lockstep::create_file(path);
        if (file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", (const char*)path.u8string().c_str());
            return false;
        }
        // The runtime keeps each 32-bit word of game memory in the host's order; the dump has the N64's.
        std::vector<uint8_t> image(recomp::save_states::game_memory_size);
        for (size_t i = 0; i < image.size(); i += 4) {
            image[i + 0] = memory[i + 3];
            image[i + 1] = memory[i + 2];
            image[i + 2] = memory[i + 1];
            image[i + 3] = memory[i + 0];
        }
        for (const Range& skip : zero) {
            std::fill_n(image.begin() + skip.offset, skip.size, 0);
        }
        const bool written = std::fwrite(image.data(), 1, image.size(), file) == image.size();
        return std::fclose(file) == 0 && written;
    }

    void dump_memory(uint64_t read) {
        const std::string path = hash_log_path + "." + std::to_string(read) + ".bin";
        if (write_memory(path, rdram, left_out())) {
            std::printf("[harness] read %llu: memory written to %s\n", (unsigned long long)read, path.c_str());
        }
    }

    // The game's memory as lockstep last compared it (keep_memory), for a desync report: a desync is found
    // a few reads after the read it's at, once the others' hashes arrive, so the last two are kept. In the
    // runtime's order, with what's left out zeroed.
    struct Kept {
        uint64_t read = UINT64_MAX;
        std::vector<uint8_t> memory;
    };
    std::array<Kept, 2> kept;

    std::array<uint64_t, conker::netplay::memory_regions> keep_memory(uint64_t read) {
        static_assert((uint64_t)conker::netplay::memory_regions * conker::netplay::memory_region_size ==
            recomp::save_states::game_memory_size);
        // Reads start again in another match, or are made again (rollback): copies from then are no longer the
        // game's. The copy goes where one is free, else over the oldest.
        for (Kept& old : kept) {
            if (old.read != UINT64_MAX && old.read >= read) old.read = UINT64_MAX;
        }
        Kept* free = &kept[0];
        for (Kept& old : kept) {
            if (old.read == UINT64_MAX || (free->read != UINT64_MAX && old.read < free->read)) free = &old;
        }
        Kept& copy = *free;
        copy.read = read;
        copy.memory.assign(rdram, rdram + recomp::save_states::game_memory_size);
        for (const Range& skip : left_out()) {
            std::fill_n(copy.memory.begin() + skip.offset, skip.size, 0);
        }
        std::array<uint64_t, conker::netplay::memory_regions> regions;
        for (int i = 0; i < conker::netplay::memory_regions; i++) {
            regions[i] = XXH3_64bits(copy.memory.data() + (size_t)i * conker::netplay::memory_region_size,
                conker::netplay::memory_region_size);
        }
        return regions;
    }

    bool dump_kept(uint64_t read, const std::filesystem::path& path) {
        for (const Kept& copy : kept) {
            if (copy.read == read && !copy.memory.empty()) {
                return write_memory(path, copy.memory.data(), {});
            }
        }
        return false;
    }

    double micros_since(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }

    // The game functions a thread waits in, innermost first (as the runtime's game signature walks them), for
    // --rollback-test's log of where the game's threads change where they wait.
    std::string game_calls(const ultramodern::save_states::ThreadInfo& thread) {
        static const std::vector<uint8_t> rom = recomp::get_code_rom();
        if (thread.game_context == nullptr) return "-";
        const recomp_context* ctx = static_cast<const recomp_context*>(thread.game_context);
        uint32_t ra = (uint32_t)ctx->r31, sp = (uint32_t)ctx->r29;
        std::string out;
        for (int depth = 0; depth < 64; depth++) {
            uint32_t func_vram, func_rom, func_size;
            if (!recomp::overlays::find_loaded_function(ra - 8, func_vram, func_rom, func_size)) break;
            char name[16];
            std::snprintf(name, sizeof(name), "%s%08X", out.empty() ? "" : " < ", func_vram);
            out += name;
            int32_t frame_size = 0, ra_offset = -1;
            const uint32_t count = std::min<uint32_t>((ra - 8 - func_vram) / 4, 64);
            for (uint32_t i = 0; i < count && func_rom + i * 4 + 4 <= rom.size(); i++) {
                const uint8_t* w = rom.data() + func_rom + i * 4;
                const uint32_t word = ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) | ((uint32_t)w[2] << 8) | w[3];
                const int16_t immediate = (int16_t)(word & 0xFFFF);
                if ((word >> 16) == 0x27BD && immediate < 0 && frame_size == 0) frame_size = -immediate;
                else if ((word >> 16) == 0xAFBF && ra_offset < 0) ra_offset = immediate;
                else if ((word >> 16) == 0xFFBF && ra_offset < 0) ra_offset = immediate + 4;
            }
            if (frame_size == 0 || ra_offset < 0) break;
            const uint32_t saved_at = sp + (uint32_t)ra_offset;
            if (saved_at < 0x80000000u || saved_at + 4 > 0x80000000u + recomp::save_states::game_memory_size) break;
            ra = (uint32_t)MEM_W(0, (gpr)(int32_t)saved_at);
            sp += (uint32_t)frame_size;
            if (ra == 0) break;
        }
        return out;
    }
    std::string last_places;

    // --rollback-test's, at each safe point: a wanted load (back <back> reads), or else a state before the
    // next read, if there's none yet.
    void rollback_safe_point(uint8_t* game_rdram) {
        rb.safe_points++;
        const uint64_t index = std::min<uint64_t>(rb.safe_index++, 15);
        if (!ultramodern::save_states::is_quiet()) {
            rb.busy++;
            return;
        }
        std::string error;
        if (rollback_to != UINT64_MAX) {
            // The state before the read to go back to, or else the nearest earlier one there is (more reads to play
            // again): a state isn't taken where a safe point isn't quiet, and can't be loaded where a thread waits
            // elsewhere than in it, as the game's main thread does for one read at a scene change. If none can now,
            // it's tried again at the next safe point.
            uint64_t target = UINT64_MAX;
            bool any_state = false;
            recomp::save_states::LoadResult result = recomp::save_states::LoadResult::NotNow;
            double restore_us = 0;
            // Replaying from the load up to where the game is (timeline.hpp; further on, if it's replaying already): it
            // keeps the pacing as it was, so it's on before it.
            const uint64_t replay_was = ultramodern::timeline::replay_until();
            ultramodern::timeline::set_replay_until(std::max(replay_was, ultramodern::timeline::delivery_time()));
            for (uint64_t back = 0; back < snapshots.size() && back <= rollback_to; back++) {
                const uint64_t read = rollback_to - back;
                if (snapshots[read % snapshots.size()].read != read) continue;
                any_state = true;
                auto start = std::chrono::steady_clock::now();
                result = recomp::save_states::restore(game_rdram, snapshots[read % snapshots.size()].state, error);
                restore_us = micros_since(start);
                if (result != recomp::save_states::LoadResult::NotNow) {
                    target = read;
                    break;
                }
                rb.not_now++;
            }
            if (result != recomp::save_states::LoadResult::Loaded) {
                ultramodern::timeline::set_replay_until(replay_was);
            }
            if (!any_state) {
                rb.no_snapshot++;
                if (late_max != 0 || rollback_match()) {
                    late_broken = true;
                    std::printf("[harness] rollback to read %llu: no state for it\n", (unsigned long long)rollback_to);
                }
                rollback_to = UINT64_MAX;
                return;
            }
            if (result == recomp::save_states::LoadResult::NotNow) {
                // As rollback would, it gives up after 2 reads (and the reads would have waited for the inputs).
                if (reads >= rb.asked_at_read + 2) {
                    rb.given_up++;
                    if (late_max != 0 || rollback_match()) {
                        late_broken = true;
                    }
                    std::printf("[harness] rollback to read %llu given up at read %llu: %s\n", (unsigned long long)rollback_to,
                        (unsigned long long)reads, error.c_str());
                    rollback_to = UINT64_MAX;
                }
                return;
            }
            if (result == recomp::save_states::LoadResult::Invalid) {
                std::fprintf(stderr, "[harness] rollback: the state couldn't be loaded: %s\n", error.c_str());
                if (late_max != 0 || rollback_match()) {
                    late_broken = true;
                }
                rollback_to = UINT64_MAX;
                return;
            }
            if (target != rollback_to) {
                rb.earlier_state++;
            }
            if (reads != rb.asked_at_read) {
                rb.later_load++;
            }
            rollback_to = target;
            Snapshot& snapshot = snapshots[target % snapshots.size()];
            rb.restore.add(restore_us);
            rb.loaded_at[index]++;
            rb.load_wait_reads.add((double)(reads - rb.asked_at_read));
            auto start = std::chrono::steady_clock::now();
            conker::host_state::load(snapshot.host);
            rb.host_load.add(micros_since(start));
            // (Going back again partway through playing again: still up to where the game was.)
            rollback_from = rerunning ? std::max(rollback_from, reads) : reads;
            rerun_reads = rollback_from - rollback_to;
            // The states after it are of the game that's being replaced: they're taken again as it's played again.
            for (Snapshot& later : snapshots) {
                if (later.read != UINT64_MAX && later.read > rollback_to) {
                    later.read = UINT64_MAX;
                }
            }
            reads = rollback_to;
            rollback_to = UINT64_MAX;
            rb.rollbacks++;
            rerunning = true;
            rerun_started = std::chrono::steady_clock::now();
            return;
        }
        Snapshot& snapshot = snapshots[reads % snapshots.size()];
        if (snapshot.read == reads) {
            return;
        }
        auto start = std::chrono::steady_clock::now();
        if (!recomp::save_states::capture(game_rdram, snapshot.state, error)) {
            snapshot.read = UINT64_MAX;
            if (rb.capture_failed++ < 5) {
                std::printf("[harness] rollback: no state before read %llu: %s\n", (unsigned long long)reads, error.c_str());
            }
            return;
        }
        rb.capture.add(micros_since(start));
        {
            std::string places;
            for (const auto& thread : ultramodern::save_states::get_threads(game_rdram)) {
                places += "  thread " + std::to_string(thread.id) + ": " + game_calls(thread) + "\n";
            }
            if (places != last_places) {
                std::printf("[harness] before read %llu the threads wait in:\n%s", (unsigned long long)reads, places.c_str());
                last_places = places;
            }
        }
        rb.taken_at[index]++;
        start = std::chrono::steady_clock::now();
        snapshot.host = conker::host_state::save();
        rb.host_save.add(micros_since(start));
        snapshot.read = reads;
    }

    // --rollback-test's, at each read: checks a read played again, and asks for the next rollback.
    void rollback_read(uint64_t read, uint64_t hash) {
        const auto now = std::chrono::steady_clock::now();
        if (!rb.started_set) {
            rb.started = now;
            rb.started_set = true;
        }
        else if (!rerunning) {
            rb.read_gap.add(std::chrono::duration<double, std::micro>(now - rb.last_read_at).count());
        }
        rb.last_read_at = now;
        rb.safe_index = 0;
        if (snapshots[read % snapshots.size()].read != read) {
            rb.reads_without_snapshot++;
        }
        if (late_max != 0 || rollback_match()) {
        }
        else if (read < first_hashes.size()) {
            if (first_hashes[read] != hash) {
                rb.mismatches++;
                if (rb.first_mismatch == UINT64_MAX) {
                    rb.first_mismatch = read;
                    std::printf("[harness] rollback: read %llu played again hashes %016llx, the first time %016llx\n",
                        (unsigned long long)read, (unsigned long long)hash, (unsigned long long)first_hashes[read]);
                }
            }
        }
        else {
            first_hashes.push_back(hash);
        }
        if (rerunning && read + 1 >= rollback_from) {
            rerunning = false;
            rb.reruns_done++;
            rb.rerun_per_read.add(micros_since(rerun_started) / (double)rerun_reads);
        }
        // Back <back> reads after this one: to before read + 1 - back, to play this one and those before it again.
        if (rollback_every != 0 && !rerunning && rollback_to == UINT64_MAX && read + 1 >= rollback_back && read >= rollback_reads_ahead &&
            read % rollback_every == rollback_every - 1) {
            rollback_to = read + 1 - rollback_back;
            rb.asked_at_read = read + 1;
        }
        rollback_reads_ahead = std::max(rollback_reads_ahead, read + 1);
    }

    void rollback_report() {
        if (!rollback_on() || !rb.started_set) {
            return;
        }
        if (rollback_match()) {
            std::printf("[harness] rollback in the match: window %d; %llu reads verified and logged of %llu%s\n",
                conker::lockstep::rollback_window(), (unsigned long long)late_logged, (unsigned long long)rollback_reads_ahead,
                late_broken ? " (A ROLLBACK COULDN'T BE DONE: the game went wrong there)" : "");
            std::printf("[harness]   rollbacks asked %llu; reads gone back: %s\n", (unsigned long long)rb.late_depth.us.size(),
                rb.late_depth.summary("reads").c_str());
        }
        if (late_max != 0) {
            std::printf("[harness] rollback late test: up to %llu reads late (seed %llu), %d ports; %llu reads verified and logged of %llu%s\n",
                (unsigned long long)late_max, (unsigned long long)late_seed, ports, (unsigned long long)late_logged,
                (unsigned long long)rollback_reads_ahead, late_broken ? " (A ROLLBACK COULDN'T BE DONE: the game went wrong there)" : "");
            std::printf("[harness]   reads mispredicted when found %llu; reads gone back: %s\n", (unsigned long long)late_mispredicted,
                rb.late_depth.summary("reads").c_str());
        }
        const double seconds = micros_since(rb.started) / 1e6;
        std::printf("[harness] rollback test: every %llu reads back %llu; %llu reads made, %llu rollbacks in %.1f s\n",
            (unsigned long long)rollback_every, (unsigned long long)rollback_back, (unsigned long long)rollback_reads_ahead,
            (unsigned long long)rb.rollbacks, seconds);
        std::printf("[harness]   safe points %llu (busy %llu), states not taken %llu, reads without a state %llu, "
            "loads not now %llu (rollbacks given up %llu), rollbacks with no state %llu\n",
            (unsigned long long)rb.safe_points, (unsigned long long)rb.busy, (unsigned long long)rb.capture_failed,
            (unsigned long long)rb.reads_without_snapshot, (unsigned long long)rb.not_now, (unsigned long long)rb.given_up, (unsigned long long)rb.no_snapshot);
        std::printf("[harness]   loads from an earlier state %llu, loads a read or more later %llu\n",
            (unsigned long long)rb.earlier_state, (unsigned long long)rb.later_load);
        std::printf("[harness]   take:      %s\n", rb.capture.summary().c_str());
        std::printf("[harness]   host save: %s\n", rb.host_save.summary().c_str());
        std::printf("[harness]   load:      %s\n", rb.restore.summary().c_str());
        std::printf("[harness]   host load: %s\n", rb.host_load.summary().c_str());
        std::printf("[harness]   read to read (first time): %s\n", rb.read_gap.summary().c_str());
        std::printf("[harness]   played again, a read: %s\n", rb.rerun_per_read.summary().c_str());
        std::printf("[harness]   reads a load waited: %s\n", rb.load_wait_reads.summary("reads").c_str());
        std::string at = "[harness]   safe point after a read (taken/loaded):";
        for (size_t i = 0; i < rb.taken_at.size(); i++) {
            if (rb.taken_at[i] + rb.loaded_at[i] != 0) at += " " + std::to_string(i) + ":" + std::to_string(rb.taken_at[i]) + "/" + std::to_string(rb.loaded_at[i]);
        }
        std::printf("%s\n", at.c_str());
        std::printf("[harness]   reads played again that hashed otherwise: %llu%s\n", (unsigned long long)rb.mismatches,
            rb.mismatches == 0 ? " (all the same)" : "");
        std::fflush(stdout);
    }

    // Desync recovery, at a safe point: the host's state taken before the read lockstep wants, or a joiner's from the
    // host loaded. True if it loaded one (nothing else is done at that safe point).
    bool resync_safe_point(uint8_t* game_rdram) {
        if (!conker::lockstep::active() || match_over) {
            return false;
        }
        std::string error;
        if (resync_read != UINT64_MAX) {
            if (!ultramodern::save_states::is_quiet()) {
                return false;
            }
            switch (recomp::save_states::restore(game_rdram, resync_state, error)) {
                case recomp::save_states::LoadResult::Loaded:
                    conker::host_state::load(resync_host);
                    std::printf("[harness] the host's game loaded before read %llu (after %llu reads held)\n",
                        (unsigned long long)resync_read, (unsigned long long)resync_holds);
                    reads = resync_read;
                    // (Nor any hash of it logged: coming back, its reads were this machine's game starting.)
                    late_logged = std::max(late_logged, reads);
                    // Nothing from the game it replaced: no state to go back to, nothing being played again, but this
                    // one (with rollback, a prediction from here may need it).
                    for (Snapshot& snapshot : snapshots) snapshot.read = UINT64_MAX;
                    if (!snapshots.empty()) {
                        snapshots[resync_read % snapshots.size()] = { resync_read, resync_state, resync_host };
                    }
                    for (Kept& copy : kept) copy.read = UINT64_MAX;
                    rollback_to = UINT64_MAX;
                    rerunning = false;
                    rollback_reads_ahead = std::max(rollback_reads_ahead, reads);
                    conker::lockstep::state_loaded(resync_read);
                    resync_read = UINT64_MAX;
                    resync_state.clear();
                    resync_state.shrink_to_fit();
                    resync_host.clear();
                    return true;
                case recomp::save_states::LoadResult::NotNow:
                    resync_error = error;
                    return false;
                case recomp::save_states::LoadResult::Invalid:
                    std::printf("[harness] the host's game can't be loaded: %s\n", error.c_str());
                    resync_read = UINT64_MAX;
                    conker::lockstep::state_failed(error.c_str());
                    return false;
            }
            return false;
        }
        // (Not with a rollback to load: the game here is about to be replaced, and is taken as it's played again.)
        const uint64_t wanted = conker::lockstep::state_wanted();
        if (wanted == reads && rollback_to == UINT64_MAX && ultramodern::save_states::is_quiet()) {
            std::vector<uint8_t> taken;
            if (recomp::save_states::capture(game_rdram, taken, error)) {
                conker::lockstep::give_state(wanted, taken, conker::host_state::save());
            }
        }
        return false;
    }

    // At a safe point (a delivery point, on the idle thread), with a --state-test save or load asked for.
    void on_safe_point(uint8_t* game_rdram) {
        if (resync_safe_point(game_rdram)) {
            return;
        }
        if (rollback_on()) {
            // (Once the match is over the game may play on, in a menu match: nothing to go back to. Nor with the host's
            // game to load.)
            if (!match_over && resync_read == UINT64_MAX) {
                rollback_safe_point(game_rdram);
            }
            return;
        }
        if (state_request == StateRequest::None || !ultramodern::save_states::is_quiet()) {
            return;
        }
        std::string error;
        if (state_request == StateRequest::Save) {
            if (!recomp::save_states::capture(game_rdram, state, error)) {
                return;
            }
            state_host = conker::host_state::save();
            // The next read is the state's, and the hash log goes on from there.
            state_next_read = reads;
            if (hash_log != nullptr) {
                std::fflush(hash_log);
                state_log_position = std::ftell(hash_log);
            }
            std::printf("[harness] state taken before read %llu (%zu bytes)\n", (unsigned long long)state_next_read, state.size());
        }
        else {
            switch (recomp::save_states::restore(game_rdram, state, error)) {
                case recomp::save_states::LoadResult::Loaded:
                    conker::host_state::load(state_host);
                    std::printf("[harness] state loaded before read %llu: back to read %llu\n", (unsigned long long)reads,
                        (unsigned long long)state_next_read);
                    reads = state_next_read;
                    if (hash_log != nullptr) {
                        std::fflush(hash_log);
                        std::fseek(hash_log, state_log_position, SEEK_SET);
                    }
                    break;
                case recomp::save_states::LoadResult::NotNow:
                    return;
                case recomp::save_states::LoadResult::Invalid:
                    std::fprintf(stderr, "[harness] the state couldn't be loaded: %s\n", error.c_str());
                    break;
            }
        }
        state_request = StateRequest::None;
        ultramodern::save_states::set_safe_points_wanted(false);
    }

    void request_quit() {
        std::lock_guard lock(quit_mutex);
        quit_requested = true;
        quit_wanted.notify_one();
    }

    // A replay's read `read`: its pad and movement for `port`.
    conker::netplay::Pad replay_pad(uint64_t read, int port) {
        const uint8_t* pad = replay.data() + replay_header + read * replay_read_size + 1 + port * pad_size;
        return { (uint16_t)(pad[0] | (pad[1] << 8)), (int8_t)pad[2], (int8_t)pad[3], pad[4] };
    }
    conker::netplay::Motion replay_motion_of(uint64_t read, int port) {
        if (!replay_motion) {
            return still();
        }
        const uint8_t* m = replay.data() + replay_header + read * replay_read_size + 1 + max_pads * pad_size + port * replay_motion_size;
        return { (int16_t)(m[0] | (m[1] << 8)), (int16_t)(m[2] | (m[3] << 8)), (int16_t)(m[4] | (m[5] << 8)),
                 (int16_t)(m[6] | (m[7] << 8)), (int8_t)m[8], replay_controls(m) };
    }

    // Lockstep's source of the input of this machine's player `local` (lockstep.hpp): a replay's for their
    // port, or this machine's controller of the same number (the frontend gives the controllers ports 1 to 4
    // here in the order they first press a button); the first player has the mouse and gyro too. Nobody's
    // playing before the first read, or past a replay's end.
    conker::lockstep::Input local_input(uint64_t for_read, const ultramodern::input::Pad* live, int local) {
        if (!replay.empty()) {
            if (for_read >= replay_reads) {
                return { {}, still() };
            }
            const int port = conker::lockstep::port_of_local(local);
            conker::netplay::Pad pad = replay_pad(for_read, port);
            pad.err_no = 0;
            return { pad, replay_motion_of(for_read, port) };
        }
        if (live == nullptr) {
            return { {}, still(conker::motion::local_controls()) };
        }
        conker::lockstep::Input input{ { live[local].button, live[local].stick_x, live[local].stick_y, 0 },
            still(conker::motion::local_controls()) };
        if (local == 0) {
            conker::motion::capture(input.motion);
        }
        return input;
    }

    // With rollback (--rollback-late's or --rollback's), at each read: its hash `hash`, logged once it's known to
    // be right. A read's hash is the result of the reads before it: right once those (the first `verified`) are.
    void log_right(uint64_t read, uint64_t hash, uint64_t verified) {
        if (late_hash.size() <= read) {
            late_hash.resize(read + 1);
        }
        late_hash[read] = hash;
        // The last time a read is made is the right one: a dump is written each time.
        if (hash_log != nullptr && std::find(dump_at.begin(), dump_at.end(), read) != dump_at.end()) {
            dump_memory(read);
        }
        while (hash_log != nullptr && late_logged <= std::min(verified, read)) {
            std::fprintf(hash_log, "%llu %016llx\n", (unsigned long long)late_logged, (unsigned long long)late_hash[late_logged]);
            late_logged++;
        }
        if (hash_log == nullptr) {
            late_logged = std::max(late_logged, std::min(verified, read) + 1);
        }
    }

    // A replay's (or --stop-after's) last read made, right: written out and quit.
    void finish_last_read(uint64_t read) {
        last_read_done = true;
        // Written out now, should the program end before finish() closes them.
        if (record_file != nullptr) {
            std::fflush(record_file);
        }
        if (hash_log != nullptr) {
            std::fflush(hash_log);
        }
        conker::lockstep::leave();
        rollback_report();
        std::printf("[harness] read %llu was the last: quitting\n", (unsigned long long)read);
        request_quit();
    }

    // --rollback-late's, at each read, with the replay's pads in `pads` and `motions` (the real ones): finds
    // the inputs that arrived to be other than predicted (and asks to go back), gives the game the remote
    // ports' predictions, and logs the hashes known to be right.
    void late_read(uint64_t read, ultramodern::input::Pad* pads, conker::netplay::Motion* motions, uint64_t hash) {
        auto real_of = [](uint64_t r, int port) {
            conker::netplay::Pad pad = replay_pad(r, port);
            return std::pair{ pad, replay_motion_of(r, port) };
        };
        if (read >= rollback_reads_ahead) {
            // The first time: what has arrived by now.
            for (int port = 1; port < ports; port++) {
                while (late_known[port] < replay_reads && late_arrival[port][late_known[port]] <= read) {
                    late_known[port]++;
                }
            }
            // The first read made that was given another input than what arrived.
            uint64_t wrong = UINT64_MAX;
            for (uint64_t r = late_verified; r < read && wrong == UINT64_MAX; r++) {
                for (int port = 1; port < ports; port++) {
                    if (r >= late_known[port]) continue;
                    const auto [pad, motion] = real_of(r, port);
                    if (!(late_used[r].pads[port] == pad) || !(late_used[r].motions[port] == motion)) {
                        wrong = r;
                        break;
                    }
                }
            }
            if (wrong != UINT64_MAX && !late_broken && (rollback_to == UINT64_MAX || wrong < rollback_to)) {
                rollback_to = wrong;
                rb.asked_at_read = read + 1;
                late_mispredicted++;
                rb.late_depth.add((double)(read + 1 - wrong));
            }
        }
        // The remote ports: as they arrived, or predicted (the last known).
        ReadInputs& used = late_used[read];
        for (int port = 0; port < max_pads; port++) {
            conker::netplay::Pad pad{ pads[port].button, pads[port].stick_x, pads[port].stick_y, pads[port].err_no };
            conker::netplay::Motion motion = motions[port];
            if (port >= 1 && port < ports && read >= late_known[port]) {
                if (late_known[port] == 0) {
                    pad = { 0, 0, 0, pad.err_no };
                    motion = still();
                }
                else {
                    const auto [last_pad, last_motion] = real_of(late_known[port] - 1, port);
                    pad = { last_pad.buttons, last_pad.stick_x, last_pad.stick_y, pad.err_no };
                    motion = last_motion;
                }
                pads[port] = { pad.buttons, pad.stick_x, pad.stick_y, pad.err_no };
                motions[port] = motion;
            }
            used.pads[port] = pad;
            used.motions[port] = motion;
        }
        // Right so far: the reads before this one, each known and given as it was.
        while (!late_broken && late_verified < read) {
            bool right = true;
            for (int port = 1; port < ports && right; port++) {
                if (late_verified >= late_known[port]) {
                    right = false;
                    break;
                }
                const auto [pad, motion] = real_of(late_verified, port);
                right = late_used[late_verified].pads[port] == pad && late_used[late_verified].motions[port] == motion;
            }
            if (!right) break;
            late_verified++;
        }
        log_right(read, hash, late_verified);
    }

    // On the game thread, at each controller read, with the pads the game is about to get.
    void on_read_data(ultramodern::input::Pad* pads, int count) {
        const uint64_t read = reads++;
        // The ports past --ports (or the match's players) have no controller, whatever is plugged into this
        // machine: the game polls all four, and a pad there would reach this game only (a recording's, or a
        // match's, would then differ from what the others or a replay give it).
        constexpr uint8_t no_controller = 0x08; // CONT_NO_RESPONSE_ERROR
        for (int i = ports; i < max_pads; i++) {
            pads[i] = { 0, 0, 0, no_controller };
        }
        // Each once (after the load the game passes read <l> again).
        if (read == state_save_at || read == state_load_at) {
            state_request = read == state_save_at ? StateRequest::Save : StateRequest::Load;
            (read == state_save_at ? state_save_at : state_load_at) = no_limit;
            ultramodern::save_states::set_safe_points_wanted(true);
        }
        // Each player's mouse and gyro movement for this read (motion.cpp): none, with the default Controls,
        // unless one of the below gives it.
        conker::netplay::Motion motions[max_pads] = { still(), still(), still(), still() };
        // Waiting to load the host's game (desync recovery): this read isn't the game's; it's made again once loaded.
        if (resync_read != UINT64_MAX && !match_over) {
            reads--;
            if (++resync_holds > resync_most_holds) {
                std::printf("[harness] the host's game couldn't be loaded in %llu reads: %s\n", (unsigned long long)resync_most_holds,
                    resync_error.c_str());
                resync_read = UINT64_MAX;
                conker::lockstep::state_failed(("the game's threads didn't get where the state's were (" + resync_error + ")").c_str());
            }
            for (int i = 0; i < max_pads; i++) {
                pads[i] = { 0, 0, 0, pads[i].err_no };
            }
            conker::motion::set_read(motions);
            conker::motion::clear_local();
            return;
        }
        if (read > last_read || match_over) {
            // Past a replay's last read with a rollback still wanted at it, which couldn't be done (given up).
            if (!last_read_done && !match_over && rollback_match() && rollback_to == UINT64_MAX) {
                finish_last_read(last_read);
            }
            // Waiting for the quit: the game gets no input.
            for (int i = 0; i < max_pads; i++) {
                pads[i] = { 0, 0, 0, pads[i].err_no };
            }
            conker::motion::set_read(motions);
            conker::motion::clear_local();
            return;
        }

        // The memory as the read is made, hashed once for the log and the lockstep.
        uint64_t hash = 0;
        bool hashed = false;
        auto memory_hash = [&] {
            if (!hashed) {
                hash = hash_memory();
                hashed = true;
            }
            return hash;
        };
        // With --rollback-test, each read once (the first time); with --rollback-late or --rollback, once it's right.
        if (hash_log != nullptr && late_max == 0 && !rollback_match() && (rollback_every == 0 || read >= rollback_reads_ahead)) {
            std::fprintf(hash_log, "%llu %016llx\n", (unsigned long long)read, (unsigned long long)memory_hash());
            if (std::find(dump_at.begin(), dump_at.end(), read) != dump_at.end()) {
                dump_memory(read);
            }
        }

        if (!replay.empty()) {
            const uint8_t* in = replay.data() + replay_header + read * replay_read_size;
            for (int i = 0; i < max_pads; i++) {
                const conker::netplay::Pad pad = replay_pad(read, i);
                pads[i] = { pad.buttons, pad.stick_x, pad.stick_y, pad.err_no };
                motions[i] = replay_motion_of(read, i);
            }
            if (in[0] != count) {
                std::fprintf(stderr, "[harness] read %llu: the game reads %d controllers, the recording %u\n",
                    (unsigned long long)read, count, in[0]);
            }
        }

        if (late_max != 0) {
            late_read(read, pads, motions, memory_hash());
        }
        if (rollback_on()) {
            rollback_read(read, rollback_every != 0 ? memory_hash() : 0);
        }
        // Before lockstep, which may change the memory (--netplay-desync-at).
        const uint64_t log_hash = rollback_match() && hash_log != nullptr ? memory_hash() : 0;

        const conker::lockstep::Memory memory{ memory_hash, keep_memory, dump_kept };
        bool playing = true;
        if (conker::lockstep::active()) {
            if (late_broken && rollback_match()) {
                // This machine's game is wrong from there, the others' aren't: it leaves (a joiner's ports go idle
                // and the others play on; the host leaving ends the match), rather than ending it for everyone as
                // a desync.
                conker::lockstep::end(conker::netplay::Reason::quit, "the game couldn't go back to correct a prediction (rollback)");
                playing = false;
            }
            else {
                playing = conker::lockstep::on_read(read, pads, motions, local_input, memory, rdram);
            }
        }
        if (!playing) {
            match_over = true;
            // No rollback after it (the game plays on, in a menu match). Partway through one, the replaying ends by
            // itself where the game had got to, so nothing is shown or heard twice.
            rollback_to = UINT64_MAX;
            rerunning = false;
            for (int i = 0; i < max_pads; i++) {
                pads[i] = { 0, 0, 0, pads[i].err_no };
                motions[i] = still();
            }
            conker::motion::set_read(motions);
            if (hash_log != nullptr) {
                std::fflush(hash_log);
            }
            if (match_over_callback) {
                std::printf("[harness] read %llu: the netplay match is over\n", (unsigned long long)read);
                match_over_callback(conker::lockstep::why_ended());
            }
            else {
                std::printf("[harness] read %llu: the netplay match is over: quitting\n", (unsigned long long)read);
                request_quit();
            }
            return;
        }
        if (const uint64_t state_read = conker::lockstep::take_state(read + 1, resync_state, resync_host); state_read != UINT64_MAX) {
            resync_read = state_read;
            resync_holds = 0;
            resync_error = "not tried yet";
        }
        if (conker::lockstep::active()) {
            ultramodern::save_states::set_safe_points_wanted(rollback_on() || resync_read != UINT64_MAX ||
                conker::lockstep::state_wanted() != UINT64_MAX);
        }
        if (rollback_match()) {
            const uint64_t wrong = conker::lockstep::take_rollback();
            if (wrong != UINT64_MAX && !late_broken && (rollback_to == UINT64_MAX || wrong < rollback_to)) {
                rollback_to = wrong;
                rb.asked_at_read = read + 1;
                rb.late_depth.add((double)(read + 1 - wrong));
            }
            log_right(read, log_hash, conker::lockstep::verified_reads());
        }

        // Playing on this machine alone (recording, or hashing): player 1's movement is this machine's. In a
        // match lockstep took it (local_input); in a replay it's the recording's.
        if (!conker::lockstep::active()) {
            if (replay.empty()) {
                conker::motion::capture(motions[0]);
            }
            else {
                conker::motion::clear_local();
            }
        }
        conker::motion::set_read(motions);

        if (record_file != nullptr) {
            // With rollback a read can be made again: it's written over.
            if (rollback_match()) {
                std::fseek(record_file, (long)(header_size + read * read_size), SEEK_SET);
            }
            uint8_t out[read_size];
            out[0] = (uint8_t)count;
            for (int i = 0; i < max_pads; i++) {
                uint8_t* pad = out + 1 + i * pad_size;
                pad[0] = (uint8_t)pads[i].button;
                pad[1] = (uint8_t)(pads[i].button >> 8);
                pad[2] = (uint8_t)pads[i].stick_x;
                pad[3] = (uint8_t)pads[i].stick_y;
                pad[4] = pads[i].err_no;
                uint8_t* m = out + 1 + max_pads * pad_size + i * motion_size;
                const conker::netplay::Motion& motion = motions[i];
                for (int k = 0; k < 4; k++) {
                    const int16_t v = k == 0 ? motion.mouse_x : k == 1 ? motion.mouse_y : k == 2 ? motion.gyro_x : motion.gyro_y;
                    m[2 * k] = (uint8_t)v;
                    m[2 * k + 1] = (uint8_t)((uint16_t)v >> 8);
                }
                m[8] = (uint8_t)motion.wheel;
                put_u32(m + 9, motion.controls);
            }
            std::fwrite(out, 1, sizeof(out), record_file);
            recorded_to = read + 1;
            // Written out every 2 s of game or so, should the program end before finish() closes it.
            if (read % 60 == 59) {
                std::fflush(record_file);
            }
        }

        // With a rollback wanted, the game goes back and makes it again first.
        if (read == last_read && !(rollback_match() && rollback_to != UINT64_MAX)) {
            finish_last_read(read);
        }
    }
}

static void activate();

bool conker::harness::parse_option(int argc, char** argv, int& i) {
    if (conker::lockstep::parse_option(argc, argv, i)) {
        return true;
    }
    const char* option = argv[i];
    const bool has_value = i + 1 < argc;
    auto count_option = [&](uint64_t& out) {
        if (!has_value || !parse_count(argv[i + 1], out)) {
            std::fprintf(stderr, "[harness] %s needs a number\n", option);
            options_valid = false;
        }
        if (has_value) {
            i++;
        }
    };
    std::string* path = nullptr;
    if (std::strcmp(option, "--record") == 0) {
        path = &record_path;
    }
    else if (std::strcmp(option, "--replay") == 0) {
        path = &replay_path;
    }
    else if (std::strcmp(option, "--hash-log") == 0) {
        path = &hash_log_path;
    }
    if (path != nullptr) {
        if (!has_value) {
            std::fprintf(stderr, "[harness] %s needs a file\n", option);
            options_valid = false;
            return true;
        }
        *path = argv[++i];
        return true;
    }
    if (std::strcmp(option, "--dump-at") == 0) {
        uint64_t value = 0;
        count_option(value);
        dump_at.push_back(value);
        return true;
    }
    if (std::strcmp(option, "--ignore") == 0) {
        unsigned long long address = 0, size = 0;
        char end = 0;
        if (!has_value || std::sscanf(argv[i + 1], "%llx:%llx%c", &address, &size, &end) != 2 ||
            address < 0x80000000ull || size == 0 || address + size > 0x80000000ull + recomp::save_states::game_memory_size) {
            std::fprintf(stderr, "[harness] --ignore needs <address>:<size>, in hex, inside game memory\n");
            options_valid = false;
        }
        else {
            // Out to whole words.
            const uint32_t first = (uint32_t)(address - 0x80000000ull) & ~3u;
            const uint32_t last = ((uint32_t)(address + size - 0x80000000ull) + 3u) & ~3u;
            ignored.push_back({ first, last - first });
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--stop-after") == 0) {
        count_option(stop_after);
        return true;
    }
    if (std::strcmp(option, "--real-time") == 0) {
        real_time = true;
        return true;
    }
    if (std::strcmp(option, "--own-view") == 0) {
        own_view_option = true;
        return true;
    }
    if (std::strcmp(option, "--characters") == 0) {
        // The most characters in a multiplayer game: 0 (the game's own), 12 or 16 (more_bots.cpp).
        const std::string value = has_value ? argv[i + 1] : "";
        const int characters = value == "0" ? 0 : value == "12" ? 12 : value == "16" ? 16 : -1;
        if (characters < 0) {
            std::fprintf(stderr, "[harness] --characters needs 0 (the game's own), 12 or 16\n");
            options_valid = false;
        }
        else {
            characters_option = characters;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--view-pairs") == 0) {
        // Port numbers from 1, e.g. 12 or 34.
        view_pairs_option = 0;
        for (const char* c = has_value ? argv[i + 1] : ""; *c != '\0'; c++) {
            view_pairs_option |= (*c >= '1' && *c <= '4') ? (uint8_t)(1u << (*c - '1')) : 0x80;
        }
        if (!has_value || (view_pairs_option & 0x80) != 0 || std::popcount(view_pairs_option) % 2 != 0) {
            std::fprintf(stderr, "[harness] --view-pairs needs ports from 1 to 4, an even number of them (e.g. 12)\n");
            options_valid = false;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--state-test") == 0) {
        unsigned long long save_at = 0, load_at = 0;
        char end = 0;
        if (!has_value || std::sscanf(argv[i + 1], "%llu:%llu%c", &save_at, &load_at, &end) != 2 || load_at <= save_at) {
            std::fprintf(stderr, "[harness] --state-test needs <save read>:<load read>, the load after the save\n");
            options_valid = false;
        }
        else {
            state_save_at = save_at;
            state_load_at = load_at;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--rollback-test") == 0) {
        unsigned long long every = 0, back = 0;
        char end = 0;
        if (!has_value || std::sscanf(argv[i + 1], "%llu:%llu%c", &every, &back, &end) != 2 || every == 0 || back == 0) {
            std::fprintf(stderr, "[harness] --rollback-test needs <every>:<back>, both above 0\n");
            options_valid = false;
        }
        else {
            rollback_every = every;
            rollback_back = back;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--rollback-late") == 0) {
        unsigned long long late = 0, seed = 1;
        char end = 0;
        const int got = has_value ? std::sscanf(argv[i + 1], "%llu:%llu%c", &late, &seed, &end) : 0;
        if ((got != 1 && got != 2) || late == 0) {
            std::fprintf(stderr, "[harness] --rollback-late needs <late>[:<seed>], late above 0\n");
            options_valid = false;
        }
        else {
            late_max = late;
            late_seed = seed;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--fast") == 0) {
        fast = true;
        return true;
    }
    if (std::strcmp(option, "--jitter") == 0) {
        count_option(jitter_us);
        return true;
    }
    if (std::strcmp(option, "--ports") == 0) {
        uint64_t value = 1;
        count_option(value);
        if (value < 1 || value > max_pads) {
            std::fprintf(stderr, "[harness] --ports is 1 to 4\n");
            options_valid = false;
            value = 1;
        }
        ports = (int)value;
        return true;
    }
    return false;
}

bool conker::harness::prepare() {
    if (!options_valid) {
        return false;
    }
    if (!active()) {
        return true;
    }
    if (!record_path.empty() && !replay_path.empty()) {
        std::fprintf(stderr, "[harness] --record and --replay can't be used together\n");
        return false;
    }
    if (characters_option >= 0) {
        if (conker::lockstep::active() && !conker::lockstep::hosts()) {
            // Joining (or Quick Match, the game's own): the host's rule.
            std::fprintf(stderr, "[harness] --characters is the host's choice\n");
            return false;
        }
        conker::lockstep::set_characters(characters_option);
    }
    if (!conker::lockstep::prepare()) {
        return false;
    }
    if (conker::lockstep::active() && state_save_at != no_limit) {
        std::fprintf(stderr, "[harness] --state-test can't be used in a netplay match\n");
        return false;
    }
    if (rollback_every != 0 && late_max != 0) {
        std::fprintf(stderr, "[harness] --rollback-test and --rollback-late can't be used together\n");
        return false;
    }
    if (late_max != 0 && replay_path.empty()) {
        std::fprintf(stderr, "[harness] --rollback-late needs --replay\n");
        return false;
    }
    if ((rollback_every != 0 || late_max != 0) && (conker::lockstep::active() || state_save_at != no_limit || !record_path.empty())) {
        std::fprintf(stderr, "[harness] --rollback-test is for a replay or playing alone, without --state-test or --record\n");
        return false;
    }
    if (!dump_at.empty() && hash_log_path.empty()) {
        std::fprintf(stderr, "[harness] --dump-at needs --hash-log (the dump is named after it)\n");
        return false;
    }
    if ((view_pairs_option >> ports) != 0 && !conker::lockstep::active()) {
        std::fprintf(stderr, "[harness] --view-pairs has a port past --ports (%d)\n", ports);
        return false;
    }
    if ((own_view_option || view_pairs_option != 0 || (characters_option >= 0 && !conker::lockstep::active())) &&
        !replay_path.empty()) {
        // A replay goes by its recording's rule, so it gives the game its recording did (in a match, by the match's:
        // a host's --characters is its rule).
        std::fprintf(stderr, "[harness] --own-view, --view-pairs and --characters are for recording or hashing: a replay has its recording's\n");
        return false;
    }
    if (!replay_path.empty() && !load_replay()) {
        return false;
    }
    if (!record_path.empty()) {
        record_file = std::fopen(record_path.c_str(), "wb");
        if (record_file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", record_path.c_str());
            return false;
        }
        // Its header is written as the game starts, with the ROM's hash.
        std::printf("[harness] recording to %s (%d ports)\n", record_path.c_str(), ports);
    }
    if (!hash_log_path.empty()) {
        hash_log = std::fopen(hash_log_path.c_str(), "w");
        if (hash_log == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", hash_log_path.c_str());
            return false;
        }
    }
    last_read = stop_after;
    if (!replay.empty() && replay_reads - 1 < last_read) {
        last_read = replay_reads - 1;
    }
    else if (!replay_path.empty() && replay_reads == 0) {
        std::fprintf(stderr, "[harness] %s has no reads\n", replay_path.c_str());
        return false;
    }
    conker::lockstep::set_last_read(last_read);

    activate();
    return true;
}

// The settings profile and the quitting thread, once the harness is to be used.
static void activate() {
    if (activated) {
        return;
    }
    activated = true;
    conker::netplay_profile::set_active(true);
    quitter = std::thread([] {
        std::unique_lock lock(quit_mutex);
        quit_wanted.wait(lock, [] { return quit_requested || quit_finished; });
        if (quit_requested && !quit_finished) {
            lock.unlock();
            ultramodern::quit();
        }
    });
}

void conker::harness::begin_menu_match(std::function<void(const std::string&)> on_match_over) {
    match_over_callback = std::move(on_match_over);
    activate();
}

bool conker::harness::replaying() {
    return !replay_path.empty();
}

uint8_t conker::harness::view_pairs() {
    if (!conker::netplay_profile::active()) {
        return 0;
    }
    if (conker::lockstep::connected()) {
        return conker::lockstep::view_pairs();
    }
    return !replay.empty() ? replay_pairs : view_pairs_option;
}

bool conker::harness::own_view_rule() {
    if (!conker::netplay_profile::active()) {
        return false;
    }
    if (conker::lockstep::connected()) {
        return conker::lockstep::own_view_for_everyone();
    }
    return !replay.empty() ? (replay_flags & own_view_flag) != 0 : own_view_option;
}

int conker::harness::characters_rule() {
    if (!conker::netplay_profile::active()) {
        return 0;
    }
    if (conker::lockstep::connected()) {
        return conker::lockstep::characters();
    }
    return !replay.empty() ? (int)((replay_flags & characters_flags) >> characters_shift) : std::max(characters_option, 0);
}

bool conker::harness::fixes_ports() {
    return active();
}

ultramodern::input::connected_device_info_t conker::harness::connected_device_info(int controller_num) {
    if (controller_num < ports) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Every run starts from an empty save, in a folder of the harness's own (saves/harness, emptied here):
// the player's own save is left alone. The save file is switched once saving has started, before any
// of the game's code runs.
void conker::harness::entrypoint(uint8_t* game_rdram, recomp_context* ctx) {
    if (!::active()) {
        recomp_entrypoint(game_rdram, ctx);
        return;
    }
    const std::filesystem::path folder = ultramodern::get_save_file_path().parent_path() / "harness";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        std::fprintf(stderr, "[harness] couldn't empty %s: the game may start from its save\n", folder.string().c_str());
    }
    ultramodern::change_save_file(u8"harness", u8"empty");
    recomp_entrypoint(game_rdram, ctx);
}

bool conker::harness::active() {
    return ::active();
}

// A recording or replay that can't go on: before any of the game's code has run, so it's left at once
// (before the runtime has anything to quit).
[[noreturn]] static void refuse() {
    if (record_file != nullptr) {
        std::fclose(record_file);
    }
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

void conker::harness::start(uint8_t* game_rdram) {
    if (!active()) {
        return;
    }
    rdram = game_rdram;

    // The ROM in play and the settings profile, checked against a replay's and written into a recording.
    // Mods are loaded after this, so they're checked here too.
    // They're netplay's handshake's (hello.cpp), printed for comparing two machines' builds.
    const conker::netplay::Hello hello = conker::netplay::local_hello();
    const uint64_t rom_hash = hello.rom;
    const uint64_t profile_hash = hello.profile;
    std::printf("[harness] build %s: build %016llx, ROM %016llx, profile %016llx\n", conker::netplay::build_id(),
        (unsigned long long)hello.build, (unsigned long long)rom_hash, (unsigned long long)profile_hash);
    // Neither a match nor a recording or replay can have mods (texture packs aside).
    bool can_go_on = conker::netplay_profile::check_mods();
    if (replay_rom_hash != 0 && replay_rom_hash != rom_hash) {
        std::fprintf(stderr, "[harness] %s was recorded with another ROM (%016llx; this one is %016llx)\n",
            replay_path.c_str(), (unsigned long long)replay_rom_hash, (unsigned long long)rom_hash);
        can_go_on = false;
    }
    if (replay_profile_hash != 0 && replay_profile_hash != profile_hash) {
        std::fprintf(stderr, "[harness] %s was recorded with another settings profile (%016llx; this build's is %016llx)\n",
            replay_path.c_str(), (unsigned long long)replay_profile_hash, (unsigned long long)profile_hash);
        can_go_on = false;
    }
    if (!can_go_on) {
        refuse();
    }
    // A netplay match: every player in before the game starts, with as many ports as players.
    if (conker::lockstep::active()) {
        if (conker::lockstep::connected()) {
            // Connected from the menu, with the stored ROM's hash: it's the one loaded, unless something
            // changed it since.
            const conker::netplay::Hello sent = conker::lockstep::sent_hello();
            if (!conker::netplay::same_game(sent, hello)) {
                std::fprintf(stderr, "[harness] the game loaded isn't the one the match was joined with\n");
                conker::lockstep::end(conker::netplay::Reason::rom, "the ROM loaded isn't the one the match was joined with");
                match_over = true;
                if (match_over_callback) {
                    match_over_callback(conker::lockstep::why_ended());
                }
            }
        }
        else if (!conker::lockstep::connect(hello)) {
            conker::lockstep::finish();
            refuse();
        }
        if (!replay.empty() && ports != conker::lockstep::players()) {
            std::fprintf(stderr, "[harness] %s has %d ports, the match %d players\n", replay_path.c_str(), ports,
                conker::lockstep::players());
            conker::lockstep::finish();
            refuse();
        }
        ports = conker::lockstep::players();
    }
    if (record_file != nullptr) {
        uint8_t header[header_size];
        std::memcpy(header, magic, sizeof(magic));
        put_u32(header + 8, version);
        put_u32(header + 12, (uint32_t)ports);
        put_u64(header + 16, rom_hash);
        put_u64(header + 24, profile_hash);
        put_u32(header + 32, (conker::harness::own_view_rule() ? own_view_flag : 0) |
            ((uint32_t)conker::harness::characters_rule() << characters_shift));
        put_u32(header + 36, conker::harness::view_pairs());
        std::fwrite(header, 1, sizeof(header), record_file);
    }

    if (!real_time) {
        ultramodern::timeline::enable(fast);
    }
    ultramodern::timeline::set_jitter((uint32_t)std::min<uint64_t>(jitter_us, 1'000'000));
    ultramodern::input::set_read_data_callback(on_read_data);
    if (state_load_at != no_limit) {
        ultramodern::save_states::set_safe_point_callback(on_safe_point);
    }
    if (late_max != 0) {
        // Each remote input arrives 0 to <late> reads after its read, in order.
        uint64_t random = late_seed * 0x9E3779B97F4A7C15ull + 1;
        for (int port = 1; port < ports; port++) {
            late_arrival[port].assign(replay_reads, 0);
            uint64_t previous = 0;
            for (uint64_t r = 0; r < replay_reads; r++) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                // Not in the boot's first reads (the game changes its main loop there, so they can't be gone back to):
                // a match would wait for everyone's inputs there.
                previous = std::max(previous, r < late_boot_reads ? r : r + random % (late_max + 1));
                late_arrival[port][r] = previous;
            }
        }
        late_used.assign(replay_reads, ReadInputs{});
        late_hash.assign(replay_reads, 0);
        rollback_back = late_max + 6;
    }
    if (rollback_match()) {
        // Back as far as the window, a read or two more as a load waits for a safe point, and earlier states.
        rollback_back = (uint64_t)conker::lockstep::rollback_window() + 8;
    }
    if (rollback_on()) {
        snapshots.assign(rollback_back + 2, Snapshot{});
        ultramodern::save_states::set_safe_point_callback(on_safe_point);
        ultramodern::save_states::set_safe_points_wanted(true);
    }
    else if (conker::lockstep::active()) {
        // Desync recovery's (wanted only while there's a state to take or load).
        ultramodern::save_states::set_safe_point_callback(on_safe_point);
    }
}

void conker::harness::finish() {
    conker::lockstep::finish();
    if (quitter.joinable()) {
        {
            std::lock_guard lock(quit_mutex);
            quit_finished = true;
            quit_wanted.notify_one();
        }
        quitter.join();
    }
    if (record_file != nullptr) {
        std::fclose(record_file);
        record_file = nullptr;
        if (rollback_match()) {
            std::error_code error;
            std::filesystem::resize_file(record_path, header_size + recorded_to * read_size, error);
        }
        std::printf("[harness] recorded %llu reads\n", (unsigned long long)(rollback_match() ? recorded_to :
            reads <= last_read ? reads : last_read + 1));
    }
    if (hash_log != nullptr) {
        std::fclose(hash_log);
        hash_log = nullptr;
    }
}
