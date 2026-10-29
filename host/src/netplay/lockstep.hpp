// Netplay's lockstep: every peer runs the whole game, and at each controller read gives
// it every player's pad for that read, so the same inputs give each the same game. A peer's own pad is
// sent a few reads ahead (the input delay) to hide the time it takes to arrive; the game waits at a read
// until every pad for it is there. Every so often the peers compare hashes of the game's memory, and a
// difference ends the match (a desync). Each peer that finds one writes a report, to find its cause: in
// netplay-desyncs/ in the game's data folder, desync-<date>-<time>-read<n>-port<p>.json (the read, the
// regions of memory that differ, the build, settings and the last inputs; tools/netplay/compare_desyncs.py
// compares two peers') and its memory at that read beside it (.bin, as the harness's --dump-at writes it).
//
// Part of the harness (harness.cpp), which gives it the deterministic timeline, the settings profile
// and an empty save on each peer. For now it's started from the command line (the menu is #9):
//
//   --netplay-host <port>        hosts a match on UDP <port> (the game's first ports are the host's players')
//   --players <n>                ...of <n> players, 2 to 4 (2 by default)
//   --split-screen               (hosting) let each player choose what they see; by default every player
//                                sees only their own view of the split screen (the match's rule, for all)
//   --netplay-address <address>  (hosting) this computer's address as the others reach it: the join code
//                                is made from it and printed (it holds the match's random key)
//   --netplay-join <code>        joins the host by its join code
//   --local-players <n>          (hosting or joining) players on this machine, 1 to 3 (1 by default): each
//                                has a port of their own (the lowest free ones), and plays with this
//                                machine's controller of the same number (the first has the keyboard, mouse
//                                and gyro). "My view only" (view.cpp) shows two players' views across
//                                the top and bottom halves of the window; three see the whole split screen
//   --input-delay <reads|auto>   (hosting) how many reads ahead each peer's own pads are sent, 0 to 8, or
//                                auto (the default): chosen from the round trips to the players as the
//                                match starts, and changed as they change. The host tells everyone (Start,
//                                then Delay: every peer switches at the same read)
//   --netplay-desync-at <read>   (a test) changes a byte of game memory at that read (in region 64,
//                                0x80400003), which the next hash comparison must catch
//   --netplay-fake-network <spec>  (a test) fakes a bad network on this peer's side, through the library:
//                                lag=<ms>, jitter=<ms>, loss=<%>, reorder=<%> and dup=<%> on what it sends,
//                                outage=<read>:<s> (nothing sent or received for that long from that read)
//                                and lag-at=<read>:<ms> (the lag changes), separated by commas. It only makes
//                                this player's own match worse, so it's in every build, like the above
//   --netplay-stall-at <read>:<ms>  (a test) the game stops at that read for that long, as on a hitch
//
// A joiner's machine that leaves (or stops answering) leaves the others playing: the host tells them from which
// read its ports get no input (Gone), and every machine gives them an idle pad from there. The host leaving
// ends the match.
//
// A peer that falls behind the others (a hitch: loading, the system) catches up on its own: it runs the
// timeline faster than real time until the others' inputs are no further ahead of it than the delay.
//
// What the match is doing (waiting for a player, catching up, a player leaving, the delay and round trips) is
// kept for the screen (status, take_news: netplay/status.cpp draws it).
//
// As a match ends, it prints how many reads waited for the others' input and for how long (for the tests:
// tools/netplay/network.sh).
//
// The game waits at its start for every player to join. With --replay, each peer's own pads come from
// its ports in the recording instead of its controllers: peers replaying the same recording give the
// game exactly what a replay alone would (tools/netplay/lockstep.sh tests that).

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ultramodern/input.hpp"

#include "net/packets.hpp"

namespace conker::lockstep {
    // A command line option of lockstep's, taking its value too (true if it was one).
    bool parse_option(int argc, char** argv, int& i);
    // Hosting or joining.
    bool active();
    // The same as the options, from the menu (frontend's netplay.cpp).
    // `input_delay`: reads, or -1 for auto; `local`: the players on this machine (fewer than `players`).
    void set_host(int port, int players, bool own_view = true, int input_delay = -1, int local = 1);
    void set_join(const std::string& where, int local = 1);
    // Checks the options, before the game starts (false: a message was printed).
    bool prepare();
    // Hosting: the join code for this match at `address` (this computer's, as the others reach it; the
    // port is added if it hasn't one), or empty if it isn't an address or nothing is hosted yet. It holds
    // the match's key, which is new each time a match is hosted.
    std::string join_code(std::string address);

    // Connecting, a step at a time, so a menu can show how it goes (and the networking is started the
    // first time): begin hosts or joins (false: `error` says why), then poll_connect until every player
    // is in and the match starts, or it fails. `status` says how it's going, in a sentence for the
    // player. cancel says goodbye and stops.
    enum class Progress { waiting, started, failed };
    bool begin(const netplay::Hello& me, std::string& error);
    Progress poll_connect(std::string& status);
    void cancel();
    // The match has started (from the menu or connect).
    bool connected();
    // The Hello begin was given.
    const netplay::Hello& sent_hello();
    // begin, then poll_connect until the match starts, printing how it goes (the command line's way, in
    // harness::start; false: it couldn't, and a message was printed).
    bool connect(const netplay::Hello& me);
    // Whether the match's rule is that every player sees only their own view of the split screen (the
    // host's choice, sent with Start), once connected. Not enforceable: every machine draws every view,
    // so a modified build could show them; it keeps honest players on the same footing.
    bool own_view_for_everyone();
    // The ports whose views are shown two to a window in "my view only" (bit n: port n; each machine with two
    // players'), the same for every peer (Start), once connected.
    uint8_t view_pairs();
    // This peer's first port, all of them (bit n: port n), how many, and the match's players, once
    // connected.
    int port();
    uint8_t ports();
    int local_players();
    // The port of this machine's player `local` (0 is the first), once connected; -1 if there's none.
    int port_of_local(int local);
    int players();
    // The match's input delay, once connected.
    int input_delay();

    // A player's input for one read: their pad, and their mouse and gyro movement with their Controls.
    struct Input {
        netplay::Pad pad;
        netplay::Motion motion;
    };
    // The input of this machine's player `local` (0 is the first) for read `for_read`: `live` holds the pads
    // the controllers give now, or is null for the reads before the first (sent at the start, as nobody has
    // played yet). As the input delay goes up, it's asked for several reads at once (the same pads).
    using InputSource = std::function<Input(uint64_t for_read, const ultramodern::input::Pad* live, int local)>;
    // The game's memory as a read is made (the harness's), for the desync check.
    struct Memory {
        // Its XXH3-64.
        std::function<uint64_t()> hash;
        // Each region's XXH3-64 (netplay::memory_region_size each), keeping a copy for a report (the last two).
        std::function<std::array<uint64_t, netplay::memory_regions>(uint64_t read)> keep;
        // Writes the copy kept at `read` to `path`, as --dump-at does (false: none, or it couldn't).
        std::function<bool(uint64_t read, const std::filesystem::path& path)> dump;
    };
    // Creates `path` to write (binary), whatever characters it has (on Windows too); null if it can't.
    FILE* create_file(const std::filesystem::path& path);
    // At controller read `read`, on the game thread: sends this peer's players' input for read + delay, waits for
    // every player's for `read` and puts them in `pads` and `motions` (4 each), and compares the memory's
    // hashes (`memory` gives them every so often). False if the match has ended (a message was printed): the
    // game can't go on.
    bool on_read(uint64_t read, ultramodern::input::Pad* pads, netplay::Motion* motions, const InputSource& source,
        const Memory& memory, uint8_t* rdram);
    // How the match goes, for the player to see (netplay/status.cpp draws it), from any thread: lockstep
    // keeps it as the game thread goes, which may be stuck at a read waiting for the others.
    struct Status {
        bool playing = false;           // a match has started and isn't over
        bool hosting = false;
        int players = 0;
        uint8_t own_ports = 0;          // bit n: port n
        int input_delay = 0;
        // How many reads ahead of the furthest of the others this machine is (behind: below 0); none while
        // nobody else plays.
        int64_t ahead = 0;
        bool others_playing = false;
        // The ports a read waits for (bit n: port n), and since when (seconds, lockstep::now).
        uint8_t waiting_for = 0;
        double waiting_since = 0;
        // Catching up with the others (#16), since when.
        bool catching_up = false;
        double catching_up_since = 0;
        // Each port's round trip (ms; -1 not known: Session::round_trips_by_port), and the ports that left,
        // and of those the ones that stopped answering.
        std::array<int, netplay::max_players> round_trip_ms{ -1, -1, -1, -1 };
        uint8_t left = 0;
        uint8_t left_silently = 0;
    };
    Status status();
    // The steady clock lockstep's times are on, in seconds.
    double now();
    // What has happened since the last call, a sentence each for the player ("Player 3 left."), oldest first.
    std::vector<std::string> take_news();
    // Ends the match for `reason`, saying `why` (the reason is the peers').
    void end(netplay::Reason reason, const char* why);
    // Why the match ended, in a sentence for the player (after on_read returned false).
    const std::string& why_ended();
    // Says goodbye (after the last read of a replay): what's been sent still arrives.
    void leave();
    // Says goodbye if not yet, and stops the networking, as the game quits.
    void finish();
}
