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
//   --characters <n>             (hosting; the harness's) the most characters in the match's multiplayer
//                                games, 0 (the game's own, by default), 12 or 16 (more_bots.cpp)
//   --netplay-address <address>  (hosting) this computer's address as the others reach it: the join code
//                                is made from it and printed (it holds the match's random key)
//   --netplay-join <code>        joins the host by its join code, or by its room code (on --netplay-server)
//   --netplay-host room          hosts through a room on the netplay server instead (--netplay-server): the
//                                server gives a room code to pass on (printed), which has no address in it;
//                                no port to forward, and by default the players only reach each other through
//                                the server's relay, so nobody sees another's address
//   --netplay-server <host:port> the netplay server, for rooms (tools/netplay/server/)
//   --netplay-direct             (through a room) allow direct connections too, when the networks allow it
//                                (the relay if not): quicker, but the players see each other's addresses
//   --netplay-public             (hosting through a room) list it in the server's public room list, where
//                                players of the same build, ROM, settings and mods find it by its name
//   --netplay-lan                (hosting on a port) visible on this network: a beacon each second while the
//                                players join (UDP port 27016), which games there list and join from (no
//                                address needed then); anyone on the network can join it
//   --netplay-player <hex>       this machine's player id (Hello's: the waiting room's names, block lists);
//                                random for the process if not given (the menu keeps one in netplay.json)
//   --netplay-block <hex>        keep this player out (hosting: refused; joining: leave a room they're in),
//                                again for more
//                                The waiting room: a match starts once every port is taken and everyone's
//                                ready; from the command line every machine is ready by itself
//   --netplay-start-with <n>     (hosting) start once <n> players are in (and ready), the host's among them: the
//                                free ports are open, for players joining the match under way (below)
//   --netplay-seat-hold <s>      (hosting) how long a player who left keeps their ports for coming back before a
//                                new player can take them, in seconds (120 by default)
//   --netplay-quick              Quick Match on --netplay-server: joins the first open public room this game
//                                can join (trying up to 5), or hosts one, in public, as --netplay-host room
//                                would with --players and the like. A host still alone that sees an older open
//                                room leaves its own and joins that, so players who started at once meet
//   --netplay-watch              (joining) watch the match, with no players on this machine: a spectator, which gets
//                                every input and sends none, its game a little behind the players' (nobody waits for
//                                it); joining a match under way, it gets the host's game as a new player does
//   --netplay-no-spectators      (hosting) take no spectators (by default the match takes up to 8)
//   --netplay-claim-at <read>:<port>  (a test, watching) asks the host for that free port (2 to 4) at that read (0:
//                                before the start)
//   --netplay-step-out-at <read> (a test, joining) steps out at that read (0: before the start), every player here, to watch
//   --netplay-seat-at <read>:<port>:<n>  (a test, hosting) seats spectator <n> (from 1) in that port at that read, as
//                                the Esc menu's Netplay tab does (its player stepping out first: a swap)
//   --local-players <n>          (hosting or joining) players on this machine, 1 to 3 (1 by default): each
//                                has a port of their own (the lowest free ones), and plays with this
//                                machine's controller of the same number (the first has the keyboard, mouse
//                                and gyro). "My view only" (view.cpp) shows two players' views across
//                                the top and bottom halves of the window; three see the whole split screen
//   --input-delay <reads|auto>   (hosting) how many reads ahead each peer's own pads are sent, 0 to 8, or
//                                auto (the default): each player's own, chosen for how their inputs reach
//                                the others (the round trips as the match starts, then how evenly the
//                                inputs come too), and changed as that changes. The host tells everyone
//                                (Start, then Delay: every peer switches at the same read)
//   --netplay-desync-at <read>   (a test) changes a byte of game memory at that read (in region 64,
//                                0x80400003), which the next hash comparison must catch (again for more)
//   --netplay-no-recovery        (hosting) a desync ends the match, rather than the host sending its game to
//                                the joiners whose game went another way (desync recovery, below)
//   --netplay-kick-at <read>:<port>  (a test, hosting) kicks that player (2 to 4) at that read, as the Esc
//                                menu's Netplay tab does: their port plays on idle
//   --netplay-fake-network <spec>  (a test) fakes a bad network on this peer's side, through the library:
//                                lag=<ms>, jitter=<ms>, loss=<%>, reorder=<%> and dup=<%> on what it sends,
//                                outage=<read>:<s> (nothing sent or received for that long from that read)
//                                lag-at=<read>:<ms> (the lag changes) and spikes=<every reads>:<ms> (that lag
//                                for 60 reads, every so many reads), separated by commas. It only makes
//                                this player's own match worse, so it's in every build, like the above
//   --netplay-stall-at <read>:<ms>  (a test) the game stops at that read for that long, as on a hitch (again for more)
//   --rollback <reads>           (hosting) instead of waiting for the others' inputs, every peer
//                                predicts them (each player's last) up to <reads> reads ahead of the last read
//                                it has them all for, 0 (the default: they wait) to 12; when one comes that
//                                isn't what it predicted, the harness goes back to before that read and plays
//                                on again from there, unpaced, unseen and unheard; time sync slows the machine
//                                ahead of the others a little, till they're level. The match's, the same for
//                                everyone: the joiners get it with Start (a joiner can't choose its own)
//
// Desync recovery: when the host finds a joiner's game isn't its own, it sends that machine its game's state
// (compressed, about 1 or 2 MB), which it loads at a read chosen ahead, and the match goes on; meanwhile the others
// wait for that machine's inputs. The host's game is the one that counts. A machine whose game goes another way
// again soon after, or that can't load the state, ends the match (or leaves it), as a desync did before.
//
// A joiner's machine that leaves (or stops answering) leaves the others playing: the host tells them from which
// read its ports get no input (Gone), and every machine gives them an idle pad from there. The host leaving
// ends the match. A player who left (not kicked) can come back, by joining again with the same player id
// (--netplay-player, or the menu's) and as many players: their game starts at once, the host tells the others from
// which read their ports play again (Back: past any input that can have come), and sends them its game as it is
// before that read, with the match as it is then (Rejoin), which they load once their game has started; meanwhile
// the others wait for their inputs from that read, as for desync recovery. A new player can join the match under
// way the same way, on free ports: those open since the start (--netplay-start-with, the menu's Start now), and
// those of a player who left over --netplay-seat-hold seconds ago and didn't come back (who then can't).
//
// A spectator (--netplay-watch) watches the match from a machine with nobody playing: it gets every input and sends
// none, nobody waits for it, and its game follows the players' a little behind; joining under way, it gets the host's
// game as a new player does, and after a desync the host sends it its game alone. It never ends the match.
//
// A peer that falls behind the others (a hitch: loading, the system) catches up on its own: it runs the
// timeline faster than real time until the others' inputs are no further ahead of it than the delay. One whose
// inputs stop coming holds the others up only a while (5 s, or 2.5 s for one that did lately): the host lets go of
// it, the others play on, and it comes back with the host's game once it can, still connected.
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

#include "net/connection_test.hpp"
#include "net/packets.hpp"

namespace conker::lockstep {
    // A command line option of lockstep's, taking its value too (true if it was one).
    bool parse_option(int argc, char** argv, int& i);
    // Hosting or joining.
    bool active();
    // The same as the options, from the menu (frontend's netplay.cpp).
    // `input_delay`: reads, or -1 for auto; `local`: the players on this machine (fewer than `players`).
    // `rollback`: the reads every peer predicts ahead (--rollback's), 0 (off) to 12.
    // `port` 0: through a room on the netplay server (set_server); -1: not hosting.
    // `spectators`: the match takes them (--netplay-no-spectators: not). set_join's `local` 0: a spectator (--netplay-watch).
    void set_host(int port, int players, bool own_view = true, int input_delay = -1, int local = 1, int rollback = 0,
        bool spectators = true);
    void set_join(const std::string& where, int local = 1);
    // Hosting on a port: visible on this network (--netplay-lan).
    void set_lan(bool visible);
    // Who plays here (the menu's kept player id), whom they keep out, and whether they're ready by themselves
    // (the command line's way) or when set_ready says (the menu's waiting room).
    void set_identity(uint64_t player, const std::vector<uint64_t>& blocked, bool ready_by_itself);
    // Quick Match (--netplay-quick): as set_host's, which it hosts with if it finds no room to join.
    void set_quick(int players, bool own_view = true, int input_delay = -1, int local = 1, int rollback = 0, bool spectators = true);
    // The netplay server ("host:port") for rooms, whether direct connections are allowed through it, and
    // (hosting) whether the room is in its public list.
    void set_server(const std::string& server, bool allow_direct, bool listed = false);
    // Checks the options, before the game starts (false: a message was printed).
    bool prepare();
    // Hosting: the join code for this match at `address` (this computer's, as the others reach it; the
    // port is added if it hasn't one), or empty if it isn't an address or nothing is hosted yet. It holds
    // the match's key, which is new each time a match is hosted.
    std::string join_code(std::string address);
    // Hosting through a room: its code, once the server has made it (empty until then).
    std::string room_code();

    // Connecting, a step at a time, so a menu can show how it goes (and the networking is started the
    // first time): begin hosts or joins (false: `error` says why), then poll_connect until every player
    // is in and the match starts, or it fails. `status` says how it's going, in a sentence for the
    // player. cancel says goodbye and stops.
    enum class Progress { waiting, started, failed };
    bool begin(const netplay::Hello& me, std::string& error);
    Progress poll_connect(std::string& status);
    void cancel();
    // The public room list on the netplay server (set_server's), for the menu, while no match is being
    // connected: browse connects to it for a game whose Hello is `me` (false: `error` says why), and
    // poll_browse says how it goes and gives the rooms that game can join, asked for again every few seconds.
    // begin stops it, as does stop_browsing; browse_again asks for the list at once.
    struct ListedRoom {
        std::string code;       // its room code
        std::string name;       // made from its number (netplay::room_name)
        int players = 2;
        int joined = 1;
        bool playing = false;   // being played: a player who left can come back, and with free ports (joined below
                                // players) anyone can join it
        bool direct = false;    // its host allows direct connections
        int rollback = 0;
        std::string release;    // its release's name (hello.hpp: release_name); empty: not known (an older server)
        bool other_build = false;   // another build of the game than this one's: not to be joined (another release)
        int watching = 0;           // spectators in it
        bool spectators = false;    // it takes spectators
    };
    struct Browse {
        enum class State { connecting, listed, failed };
        State state = State::connecting;
        std::string error;                  // failed: why
        std::vector<ListedRoom> rooms;      // open ones first
        int others = 0;                     // rooms of this build and ROM besides, with other settings or mods
        int older = 0;                      // rooms of older releases besides, which can't say their release
    };
    bool browse(const netplay::Hello& me, std::string& error);
    Browse poll_browse();
    void browse_again();
    void stop_browsing();
    // The matches on this network a game whose Hello is `me` can join (hosts with --netplay-lan; no server):
    // lan_browse listens (false: `error` says why), poll_lan gives those heard from lately (each `code` a
    // join code; `direct`), stop_lan stops.
    bool lan_browse(const netplay::Hello& me, std::string& error);
    std::vector<ListedRoom> poll_lan();
    void stop_lan();
    // The menu's Test connection (net/connection_test.hpp), on the netplay server (set_server's; none: only
    // the LAN port), while no match is being connected: it stops the room list (one connection to the server
    // at a time). poll_connection_test: whether it's done, and then its report; begin stops it, as does
    // stop_connection_test.
    bool start_connection_test(std::string& error);
    bool poll_connection_test(netplay::ConnectionReport& report);
    void stop_connection_test();
    // The waiting room, while connecting: each port's player (by name, made from their player id), whether
    // they're ready, their platform and round trip to the host, which are this machine's, and the match's
    // settings. `in`: there's one to show (hosting, or welcomed). set_ready: this machine's players are ready
    // (the host's: Start). kick: the host removes a port's machine. block: adds a player to the block list
    // (the menu keeps it): a host kicks them, a joiner leaves a room they're in.
    struct Seat {
        uint64_t player = 0;        // 0: free
        std::string name;
        bool ready = false;
        int platform = 0;           // 1 Linux, 2 Windows, 3 macOS
        int round_trip_ms = -1;     // -1: not known (or this machine's)
        bool mine = false;
        bool left = false;          // in a match: they've left (or were removed)
        bool open = false;          // in a match: nobody has had it (open since the start), free for a new player
        bool movable = false;       // on a joiner's machine (not the host's): it can step out, or be swapped
    };
    // A spectator watching: the host's number for it (0 to max_spectators - 1), and who.
    struct Spectator {
        int number = 0;
        uint64_t player = 0;
        std::string name;
        int platform = 0;
        bool mine = false;          // this machine
    };
    struct WaitingRoom {
        bool in = false;
        bool hosting = false;
        int players = 0;
        std::array<Seat, netplay::max_players> seats;
        bool ready = false;         // this machine's players
        bool own_view = false;
        int characters = 0;         // the most in a multiplayer game (0: the game's own)
        int delay = -1;             // -1: auto
        int rollback = 0;
        bool watching = false;      // this machine is a spectator
        bool takes_spectators = true;
        std::vector<Spectator> spectators;
    };
    WaitingRoom waiting_room();
    void set_ready(bool ready);
    // The host's: starts with whoever's in, once they're ready (as set_ready(true)), every free port open for players
    // joining the match under way (--netplay-start-with's way). set_ready(false) takes it back.
    void start_now();
    // `player`: the one shown on `port` (0: whoever's there), so a place that changed hands isn't kicked.
    bool kick(int port, uint64_t player = 0);
    // The host's: removes spectator `number`, if it's still `player`.
    bool kick_spectator(int number, uint64_t player);
    // Seat changes during a match. The host's: seat puts spectator `spectator` (still `player`) in `port` (free, or
    // held for a player who left: they can't come back to it; or a joiner's, whose machine steps out first: a swap);
    // make_spectator asks the joiner's machine on `port` (still `player`) to step out and watch. A spectator's: claim
    // asks the host for a free port. A joiner's: step_out stops playing (every player on this machine) and watches, their
    // places open. Before the match starts, each is done at once; in it, at the next read.
    bool seat(int port, int spectator, uint64_t player);
    bool make_spectator(int port, uint64_t player);
    void claim(int port);
    void step_out();
    void block(uint64_t player);
    // During a match (the Esc menu's Netplay tab): its players as the waiting room last had them, which have
    // left since (`in`: there's a match). kick works then too (the host's: their ports play on idle, as when
    // someone leaves), and block (a host kicks them; a joiner only keeps them out of later matches).
    // leave_match: this machine leaves; the match is over here (the others play on, or end if it hosted).
    WaitingRoom match_room();
    void leave_match();
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
    // The most characters (players and bots) in the match's multiplayer games (more_bots.cpp; 0: the game's own,
    // 12 or 16): the host's choice (set_characters, before it hosts; the harness's --characters), sent with Start,
    // once connected. The game's memory is laid out by it as it starts, so it's the same on every machine.
    void set_characters(int characters);
    int characters();
    // Hosting a match (--netplay-host, by a port or a room; not Quick Match), from the options.
    bool hosts();
    // The ports whose views are shown two to a window in "my view only" (bit n: port n; each machine with two
    // players'), the same for every peer (Start), once connected.
    uint8_t view_pairs();
    // This machine is a spectator, once connected; and the view it watches (bit n: port n; 0: the whole split screen),
    // which it chooses with its first controller's L and R (or the D-pad's left and right) as it watches.
    bool spectating();
    uint8_t watched_ports();
    // This peer's first port, all of them (bit n: port n), how many, and the match's players, once
    // connected.
    int port();
    uint8_t ports();
    int local_players();
    // The port of this machine's player `local` (0 is the first), once connected; -1 if there's none.
    int port_of_local(int local);
    int players();
    // This machine's players' input delay (each player has their own), once connected.
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
    // Rollback (--rollback): its window (0: off); the earliest read the game was given another input than has
    // come since, for the harness to go back to before (UINT64_MAX: none), as it's taken; and how many reads
    // from the first are known and were given as they were (the hash of a read up to there is right). A read
    // the game makes again after going back is given the inputs as they're known then, with nothing sent or
    // waited for.
    int rollback_window();
    uint64_t take_rollback();
    uint64_t verified_reads();
    // Desync recovery: the host's game, sent to a joiner whose game went another way, which loads it; and to
    // a player coming back to a match under way, whose game has just started. Host: the read whose state the harness
    // is to take, at a safe point before it (UINT64_MAX: none), and give_state with it (the runtime's state and the
    // host's own). Joiner: take_state, with the read the game is about to make, gives the host's state once it has
    // come (and is checked), and returns the read it's before (UINT64_MAX: none yet), for the harness to load at the
    // next safe point and go on from that read: the one about to be made, after a desync; coming back, any (the
    // reads made till then are this machine's alone, its game starting). Then state_loaded (it's at that read now),
    // or state_failed (it couldn't be: this machine leaves).
    uint64_t state_wanted();
    void give_state(uint64_t read, const std::vector<uint8_t>& runtime, const std::vector<uint8_t>& host);
    uint64_t take_state(uint64_t next_read, std::vector<uint8_t>& runtime, std::vector<uint8_t>& host);
    void state_loaded(uint64_t read);
    void state_failed(const char* why);
    // A replay's last read (the harness's): each input before it is waited for near it, so it ends right.
    void set_last_read(uint64_t read);
    // How the match goes, for the player to see (netplay/status.cpp draws it), from any thread: lockstep
    // keeps it as the game thread goes, which may be stuck at a read waiting for the others.
    struct Status {
        bool playing = false;           // a match has started and isn't over
        bool hosting = false;
        bool watching = false;          // this machine is a spectator (no ports)
        int players = 0;
        uint8_t own_ports = 0;          // bit n: port n
        int input_delay = 0;
        std::array<int, netplay::max_players> delays{};    // each port's input delay (a player's own)
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
        uint8_t lost = 0;               // the ports let go of as their inputs stopped coming, till they're back
        uint8_t open = 0;               // the ports nobody has (open since the start: free for a new player)
        // Desync recovery, or coming back: this machine is getting the host's game (how much of it has come, 0 to 1).
        bool getting_state = false;
        double state_progress = 0;
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
    // Whether this machine, a joiner, may be able to come back to the match that ended here: it left, or lost
    // the host, rather than the host ending it, removing it, or a desync.
    bool can_come_back();
    // The room code or join code this machine joined its match by, to come back to it later; empty when hosting.
    std::string joined_code();
    // Says goodbye (after the last read of a replay): what's been sent still arrives.
    void leave();
    // Says goodbye if not yet, and stops the networking, as the game quits.
    void finish();
}
