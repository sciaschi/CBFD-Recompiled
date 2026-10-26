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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/save_states.hpp"
#include "ultramodern/timeline.hpp"

#include <zstd.h>
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "../conker.hpp"
#include "hello.hpp"
#include "net/join_code.hpp"
#include "net/session.hpp"
#include "net/lan.hpp"
#include "net/signalling.hpp"

// The runtime's flag for the program quitting (ultramodern::quit), set before it waits for the game's
// threads to end. librecomp's pi.cpp reads it the same way.
extern std::atomic_bool exited;

using conker::netplay::Back;
using conker::netplay::Claim;
using conker::netplay::Unseat;
using conker::netplay::Delay;
using conker::netplay::Event;
using conker::netplay::Gone;
using conker::netplay::Hash;
using conker::netplay::Inputs;
using conker::netplay::Motion;
using conker::netplay::Pad;
using conker::lockstep::Input;
using conker::netplay::Reason;
using conker::netplay::Rejoin;
using conker::netplay::Resync;
using conker::netplay::StatePart;

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
    bool host_room = false;         // hosting through a room on the netplay server (--netplay-host room)
    std::string host_address;       // hosting from the command line: the address the others reach, for the code
    std::string join_where;         // a join code, or a room code
    std::string server_option;      // the netplay server's "host:port", for rooms
    bool direct_option = false;     // through the server: direct connections allowed, not only the relay
    bool public_option = false;     // hosting through a room: it's in the server's public list (--netplay-public)
    // Quick Match (--netplay-quick): join the first open public room that fits, or host one. Each try is a
    // room joined or hosted as by code, on one connection to the server; a host still alone that sees an
    // older open room in the list leaves its own (a new connection: a room lasts as long as its host's)
    // and joins that, so two players who started at once end up in one room.
    bool quick_option = false;
    // Hosting on a port: visible on this network (--netplay-lan), by a beacon each second while the players
    // join (lan.hpp).
    bool lan_option = false;
    // Who plays here (Hello's player id: --netplay-player, or the menu's kept one; random if neither), and whom
    // they keep out (the block list: a host refuses them, a joiner leaves a room they're in).
    uint64_t player_option = 0;
    std::vector<uint64_t> blocked_players;
    // The waiting room: whether this machine's players are ready by themselves (the command line's way), or
    // when the menu says (set_ready); and whether they are.
    bool auto_ready = true;
    bool ready_now = false;
    conker::netplay::LanBeacon* lan_beacon = nullptr;
    double next_beacon = 0;
    // The matches on this network, for the menu (lan_browse).
    conker::netplay::LanListener* lan_listener = nullptr;
    conker::netplay::Hello lan_hello;
    std::set<uint64_t> quick_tried;     // the rooms tried, not to try again
    int quick_tries = 0;                // joins tried; then it hosts
    uint64_t quick_target = 0;          // a room to join first (the older one a host left its own for)
    bool quick_asked = false;           // hosting alone: the list was asked for, to look for an older room
    double next_quick_list = 0;
    bool quick_restarting = false;      // begin, from Quick Match itself: its state stays
    constexpr int quick_most_tries = 5;
    constexpr double quick_list_seconds = 5.0;
    int wanted_players = 2;
    // Hosting: start once this many players are in (--netplay-start-with; 0: every port taken), or at once when the
    // menu says (start_now), the free ports open for players joining later; and how long a player who left keeps
    // their ports before a new one can take them (--netplay-seat-hold).
    int start_with = 0;
    bool start_open = false;
    double seat_hold_option = 120.0;
    int local_option = 1;           // the players on this machine (0: none, a spectator: --netplay-watch)
    // Hosting: whether the match takes spectators (--netplay-no-spectators: not).
    bool spectators_option = true;
    bool host_own_view = true;      // hosting: the rule this peer will start the match with
    bool match_own_view = false;    // the rule of the match under way
    uint8_t match_pairs = 0;        // ...and the ports shown two to a window (Start)
    // --input-delay's, or the menu's: the reads ahead, or auto (the host chooses from the round trips).
    constexpr int auto_delay = -1;
    int delay_option = auto_delay;
    // This machine's players' input delay at the read the game is at (each port has its own, from the host: Start,
    // Delay; a machine's ports always the same). A spectator's: port 0's, the host's.
    int delay = 2;
    // A spectator: this machine watches (no ports); its game follows the players' a little behind, and never predicts
    // (it waits for every input, the match's rollback or not). The match's rollback, for the player.
    bool watching = false;
    int match_rollback = 0;
    // A spectator may be well behind the players, so further from the host's Delay changes than they are: one that
    // comes before it has reached the last change of its ports waits here (from_read order) until it has.
    std::vector<Delay> later_delays;
    // ...and the view it watches (bit n: port n; 0: the whole split screen, view.cpp), chosen with its first controller's
    // L and R (or the D-pad's left and right): everyone's, then each player's in turn. The buttons at its last read, for
    // a press.
    std::atomic<uint8_t> watched{ 0 };
    uint16_t watch_buttons = 0;
    constexpr uint16_t next_view_buttons = 0x0010 | 0x0100;     // R, D-pad right
    constexpr uint16_t last_view_buttons = 0x0020 | 0x0200;     // L, D-pad left
    // Each port's input delay (a player's own, sized for how their inputs reach the others), by its last change
    // (#18): `before` up to read `from`, `after` from it. With auto, the host changes a machine's as its connection
    // changes (Delay); every peer switches at the same read, which none has reached as it hears. At that read the
    // machine sends its own pads up to the read plus the new delay: a raised one's reads in between get the pad the
    // controllers give then too (held), and a lowered one's live pads aren't sent until the reads already sent are
    // played.
    struct DelayChange {
        uint64_t from = 0;
        int before = 2;
        int after = 2;
    };
    std::array<DelayChange, conker::netplay::max_players> delays;
    int delay_at(int port, uint64_t read) { return read < delays[port].from ? delays[port].before : delays[port].after; }
    // The most any port's delay is, or can be from its last change on: how far ahead any input can reach.
    int most_delay() {
        int most = 0;
        for (const DelayChange& d : delays) most = std::max({ most, d.before, d.after });
        return most;
    }
    // This machine's players' delay at `read` (a spectator: port 0's).
    int own_delay_at(uint64_t read);
    // The host's choice of each machine's delay, at its checks (every hash_every reads), kept by the machine's first
    // port. Without rollback (lockstep), a read waits whenever an input comes late: the delay is what the round trips
    // need, along the slowest way a pad goes, plus what this machine's inputs arriving unevenly needs; checks in a row
    // that wanted a higher delay, and a lower one. Higher: the trips need more, or at least late_reads_to_raise reads
    // waited for the machine since the last check (a lossy network: the pads that are sent again come late, which a
    // higher delay hides; the round trips don't show it), or at least long_waits_to_raise reads waited over
    // long_wait_ms for it (two machines half a frame out of step on a LAN, one waiting so at every read for the other:
    // 30 frames a second still, but shown unevenly, which frame interpolation makes judder), then one more, up to most_raised_for_waiting more than the
    // round trips need (a peer that can't keep up, rather than the network, makes reads late whatever the delay).
    // It's raised after raise_checks (every read waits until then), and lowered only after lower_checks with
    // lower_spare_ms to spare and no read waiting for it (late, or long), so it doesn't go back and forth: a read at a time while it's
    // above what the round trips need for waiting, else to what they need. Nor is it changed again within
    // settle_reads of the last change (Delay's from read relies on that: a joiner is at most the delay ahead only once
    // the pads a lowering already sent are played).
    struct MachineDelay {
        int wanted_higher = 0;
        int wanted_lower = 0;
        int raised_for_waiting = 0;
        std::deque<int> wanted;         // rollback: each check's (rollback_trip_checks)
        std::deque<uint64_t> late;      // rollback: each check's reads that waited for it (rollback_late_checks)
        int calm = 0;
    };
    std::array<MachineDelay, conker::netplay::max_players> machine_delays;
    constexpr int most_raised_for_waiting = 2;
    constexpr int raise_checks = 2;
    constexpr int lower_checks = 10;
    constexpr uint64_t late_reads_to_raise = 3;
    constexpr double long_wait_ms = 8.0;    // reads_for's wait_ms: more than a read can wait unseen
    constexpr uint64_t long_waits_to_raise = hash_every / 2;
    constexpr double lower_spare_ms = 12.0;
    constexpr uint64_t settle_reads = 90;
    // With rollback, the delay is for rollback, and steady: a change of delay is felt (the own inputs'
    // timing shifts), while rollback covers a round trip's spikes. It's chosen at the start from the round trips, and
    // then goes by the median of the last rollback_trip_checks checks' (each one what the trip then needed, less
    // rollback_cover reads that predicting covers: shallow rollbacks, rather than none; but at least 1 unless it needs
    // none, a LAN: the others' inputs come as soon as the own; and what the machine's inputs' unevenness needs on top).
    // It goes a read at a time,
    // at most once in rollback_settle_reads: up as the median needs it, or one more for reads waiting for the machine
    // (past the window) at least rollback_late_to_raise times in the last rollback_late_checks checks, up to
    // most_raised_for_waiting more; and down only after rollback_calm_checks checks in a row with no read waiting for it
    // (a long calm: a delay going back and forth is the hitch).
    constexpr size_t rollback_trip_checks = 30;
    constexpr int rollback_cover = 3;
    constexpr uint64_t rollback_settle_reads = 300;
    constexpr size_t rollback_late_checks = 10;
    constexpr uint64_t rollback_late_to_raise = 10;
    constexpr int rollback_calm_checks = 90;
    // How unevenly each port's inputs reach this machine: for each, how much later than this machine made the
    // read they were sent at (its own clock) they came, against the median of the last baseline_samples of that
    // (about a second: a trip that changes for good moves it, rather than looking uneven), the last spread_samples of
    // them (ms). Their spread, how much later than usual the latest tenth came, is the part of a player's connection
    // that's theirs (a mobile uplink sending in bursts), which their own delay covers: what a read's timing varies
    // anyway (free_spread_ms) aside. Machines tell the host theirs with their hashes, and the
    // reads they waited for each port; the host goes by the receiver that has them the most evenly (the others' own
    // connections aside: a receiver with a bad link gets everyone's late), and the fewest waits.
    constexpr size_t spread_samples = 300;
    constexpr size_t baseline_samples = 31;
    std::array<std::deque<float>, conker::netplay::max_players> recent_lateness;
    constexpr size_t spread_least_samples = 30;
    constexpr double free_spread_ms = 33.0;
    constexpr size_t read_times_kept = 512;
    std::array<double, read_times_kept> read_made_at{};
    std::array<uint64_t, read_times_kept> read_made_number{};
    std::array<std::deque<float>, conker::netplay::max_players> lateness;
    // ...inputs that came before this machine made the read they were sent at (it's behind): the read and when.
    std::array<std::deque<std::pair<uint64_t, double>>, conker::netplay::max_players> early_inputs;
    // ...and the reads this machine's game waited for each port's inputs (Hash's `waited`, wrapping), and those that
    // waited over long_wait_ms (`waited_long`).
    std::array<uint64_t, conker::netplay::max_players> waited_for_port{};
    std::array<uint64_t, conker::netplay::max_players> waited_long_for_port{};
    // The host: each machine's latest word (by its first port: the spread it has each port's inputs at, -1 not known;
    // its reads waited for each, and those already counted at the last check), and when it came.
    struct Report {
        std::array<int, conker::netplay::max_players> spread_ms{ -1, -1, -1, -1 };
        std::array<uint8_t, conker::netplay::max_players> waited{};
        std::array<uint8_t, conker::netplay::max_players> waited_counted{};
        std::array<uint8_t, conker::netplay::max_players> waited_long{};
        std::array<uint8_t, conker::netplay::max_players> waited_long_counted{};
        bool heard = false;
        double at = 0;
    };
    std::array<Report, conker::netplay::max_players> reports;
    std::array<uint64_t, conker::netplay::max_players> own_waited_counted{};
    std::array<uint64_t, conker::netplay::max_players> own_waited_long_counted{};
    constexpr double report_fresh_seconds = 10.0;
    std::vector<uint64_t> desync_at;    // --netplay-desync-at's reads
    // Desync recovery: hosting, whether a desync is set right by sending the host's game to the joiners whose
    // game went another way (--netplay-no-recovery: the match ends, as before).
    bool recovery_option = true;
    // (a test) the host kicks this port (from 0) at that read (--netplay-kick-at), as the Esc menu's Kick does.
    uint64_t kick_at = never;
    int kick_port = -1;
    // --netplay-fake-network's: the network faked from the start, and changes at reads (an outage of
    // `outage_seconds` from read `outage_at`; the lag `lag_then` from read `lag_at`).
    conker::netplay::FakeNetwork fake;
    bool faking = false;
    uint64_t outage_at = never;
    double outage_seconds = 0;
    double outage_until = 0;
    uint64_t lag_at = never;
    int lag_then = 0;
    // And a spike every `spike_every` reads, of `spike_lag` ms for spike_reads (a mobile link's, as in a
    // playtest), the lag going back to what it was after.
    uint64_t spike_every = 0;
    int spike_lag = 0;
    int lag_between_spikes = -1;   // (-1: the lag from the start, not yet taken)
    constexpr uint64_t spike_reads = 60;
    // --netplay-stall-at's: the game stops at that read for that long, as on a hitch (loading, the system), by read.
    std::map<uint64_t, long> stalls;
    bool options_valid = true;

    bool library_started = false;
    conker::netplay::Session* session = nullptr;
    // Through a room: the connection to the netplay server, from begin until the session goes (which polls
    // it: a host stays there, with its room, for the whole match), and how far connecting through it is.
    conker::netplay::Signaller* signaller = nullptr;
    enum class Rendezvous { none, registering, listing, creating, finding };
    Rendezvous rendezvous = Rendezvous::none;
    double rendezvous_since = 0;
    std::string room_text;          // hosting: the room's code, once the server has made it
    // The public room list (browse): a connection to the netplay server of its own, while no match is being
    // connected, which asks for the list again every few seconds.
    conker::netplay::Signaller* browser = nullptr;
    // The menu's connection test, while it runs (it has a signaller of its own).
    conker::netplay::ConnectionTest* connection_test = nullptr;
    conker::netplay::Hello browse_hello;
    double next_list = 0;
    double browse_since = 0;
    double asked_at = 0;            // the list was last asked for
    bool browse_heard = false;      // a list has come
    uint8_t own_ports = 0;          // bit n: port n
    int match_players = 0;
    int joined = 0;                 // host: the players in so far, the host too (in the match: those still in)
    conker::netplay::Hello sent;    // what this peer said in the handshake (begin's)
    bool initial_sent = false;
    bool match_started = false; // the match has started (connected)
    bool ended = false;         // the match is over for this peer: nothing more is sent
    bool left = false;
    std::string why_over;       // why the match ended, for the player
    Reason end_reason = Reason::quit;
    bool host_closed = false;   // a joiner: the host ended its connection (rather than it timing out)
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
    // A spectator is always behind the players by the time their inputs take to reach it: it catches up only once
    // it's this much further behind than that (the reads its game was given every input late), and stops once it's
    // within `spectator_caught_up` of them.
    constexpr int64_t spectator_catch_up = 8;
    constexpr int64_t spectator_caught_up = 4;
    bool catching_up = false;
    uint64_t caught_up_from = 0;
    double catching_up_since = 0;

    // Time sync (with rollback): two machines drift apart a read or two, and the one ahead then predicts every
    // read while the other never needs to. Each says, with its inputs, how many reads its game is past each port's
    // latest input it has (its gap: the latency plus how far ahead it is), as measured at its last read once that
    // read's inputs were in (the same moment on every machine: a gap measured before them is a read too big);
    // half the difference between this machine's gap to a player and theirs to it is how far ahead of them it
    // really is, the latency cancelling. A joiner goes by the host alone: the host passes the joiners' inputs on
    // once a read, which holds them up to a read either way between joiners (so a gap between them says little),
    // and with each level with the host they're level with each other. The host goes by every joiner.
    // Averaged over about a second, while it's over half a read the game is paced a little later at each read
    // (up to 2 ms of 33: it's not seen), till they're level. Only when frames are shown changes, never the game.
    struct TimeSync {
        std::array<int, conker::netplay::max_players> my_gap{};      // by port: this machine's, at its last read
        std::array<int, conker::netplay::max_players> their_gap{};   // by port: their latest gap to this machine
        std::array<bool, conker::netplay::max_players> heard{};
        double lead = 0;            // how far ahead of the furthest behind, averaged (reads)
        double most_lead = 0;
        uint64_t reads_slowed = 0;
        uint64_t us_later = 0;
    } time_sync;
    constexpr double time_sync_average_reads = 30;
    constexpr double time_sync_from = 0.1;          // reads ahead before it slows
    constexpr double time_sync_us_per_read = 4000;  // a read later for each read ahead past that, up to:
    constexpr uint32_t time_sync_most_us = 2000;
    // One player can't drag everyone down: the host keeps how much it has paced later for each port (the one
    // furthest behind, it went by) over about the last half minute (decaying by time_sync_budget_reads), and goes by a
    // port no more while that's past time_sync_budget_us (the others slowed about 1% for it on average), till it's
    // half that again. A player kept behind for good (a slow computer) then catches up on its own, as any does.
    constexpr double time_sync_budget_reads = 900;
    constexpr double time_sync_budget_us = 300000;
    std::array<double, conker::netplay::max_players> slowed_for{};
    uint8_t time_sync_skipped = 0;

    // This machine's gap to `port`: how many reads its game is past the read `port` sent its latest input at.
    int gap_to(int port);
    double own_spread_ms(int port);
    void note_arrival(int port, uint64_t read, double arrived);

    void set_catching_up(bool on) {
        catching_up = on;
        ultramodern::timeline::set_catching_up(on);
    }

    // Rollback (--rollback <reads>): instead of waiting at a read for the others' inputs, this peer predicts
    // them (each player's last input it has) up to `rollback_option` reads past the last read it has every
    // input for, and when one arrives that isn't what it predicted, the harness loads the state before that
    // read and the game plays on again from there (harness.cpp) with the inputs as they are now known. Reads
    // made again (below `frontier`) send nothing and wait for nothing. A peer's hash of a read is only sent
    // once every input before that read is known and was given as it was (`verified`), so peers compare the
    // game as it really went. It's the match's, the same on every peer: the host's (--rollback), which the joiners
    // get with Start.
    int rollback_option = 0;
    // With rollback, falling a few reads behind makes nobody wait (they predict this peer's inputs), and running
    // fast for them is a hitch here (a playtest: dozens of them): time sync slows the others for it
    // instead. How far behind the inputs that have come here say it is (`behind`) is short by the one-way trip,
    // which on a bad link swings, and inputs held then sent in a burst (a mobile link's) make it jump; so it goes
    // by the others' own word: their gaps to it (time sync's, with their inputs: how far each is past this peer's
    // latest input it has). It catches up at once when one is 2 reads short of its window and the delay (it waits
    // past them), or once one has been half the window past it for catch_up_after_reads reads in a row (counted
    // afresh after each catch-up); and only if `behind` says it's behind at all (a gap that's the link's, not the
    // time's, isn't). (So on a link whose round trip is longer than the window and the delay, over 400 ms at 12, a
    // peer behind is left to time sync, as `behind` never gets above 0.) (Not in the boot's first reads, nor near a
    // replay's end: everyone waits for every input there.)
    bool rollback_catch_up(uint64_t read);
    int widest_gap_to_me();
    constexpr int catch_up_after_reads = 15;
    int wide_gap_for = 0;
    // Its most (--rollback's), which the horizon and the host's delay changes leave room for.
    constexpr int most_rollback_window = conker::netplay::max_rollback;
    // The boot changes the game's main loop at once (a state from before can't be loaded after): no
    // predicting in the first reads.
    constexpr uint64_t rollback_from_read = 30;
    uint64_t frontier = 0;                  // the reads made at least once
    uint64_t last_read = never;             // a replay's last (harness): every input for it is waited for
    std::vector<std::array<Input, conker::netplay::max_players>> given;   // by read: as the game was given them
    std::array<uint64_t, conker::netplay::max_players> checked{};         // by port: reads compared with what came
    uint64_t verified = 0;                  // reads from 0 known and given as they were
    uint64_t rollback_to = never;           // the earliest read given another input than came, for the harness
    uint64_t last_made = never;             // the read made last (a read after another than the one before: a rollback)
    struct Rollbacks {
        uint64_t count = 0;
        uint64_t predicted = 0;             // reads made the first time with a prediction
        uint64_t deepest = 0;
        uint64_t reads_again = 0;
    } rollbacks;

    // Desync recovery: when the host finds a joiner's game isn't its own, it chooses a read none has made yet (as
    // for a Delay) and tells everyone (Resync): that joiner's machine stops after the read before it until the host's
    // game as it is before that read has come (the harness takes it at a safe point there, state_wanted and
    // give_state; it's compressed and sent in StateParts), then loads it (the harness: take_state, state_loaded) and
    // plays on from there. Everyone's inputs go on as they were: the reads before it had their inputs already, and
    // those after are sent as ever. Only the host's game counts: a joiner that finds a desync waits for the host's
    // Resync, and the match ends as before if none comes (the host doesn't recover), if a machine's game goes another
    // way again soon after it was set right (something keeps them apart), or if a state can't be loaded.
    constexpr uint64_t resync_again_reads = 300;   // 10 s: a desync this soon after the last of that port's ends the match
    constexpr uint64_t resync_wait_reads = 150;    // a joiner that found a desync waits this long for the Resync
    // ...a spectator, this long (its reads may go by faster than real time, catching up).
    constexpr double spectator_resync_wait_seconds = 15.0;
    constexpr double state_wait_seconds = 60.0;    // a joiner waits this long at the read before for the state to come
    // The decompressed state's most: the runtime's (the game's 8 MB and the rest) and the host's own.
    constexpr size_t most_state_bytes = conker::netplay::max_state_size;
    constexpr size_t most_host_state_bytes = 256 * 1024;
    struct Recovery {
        // The Resync under way: the read it's before (never: none) and the ports that load it.
        uint64_t read = never;
        uint8_t ports = 0;
        // Host: the ports found to differ for the next one; the state taken (compressed) and its hash; by port, the
        // read of its last Resync (hashes of reads before it are of the game that was replaced).
        uint8_t wanted = 0;
        std::vector<uint8_t> state;
        uint64_t hash = 0;
        std::array<uint64_t, conker::netplay::max_players> last_read{ never, never, never, never };
        double sent_at = -1e9;          // a Resync (or its state) was last sent or heard: the others wait longer
        int announcements = 0;
        uint64_t count = 0;
        // The state under way is for players coming back (Back), not a desync's (Resync); host: and how many came back.
        bool rejoin = false;
        uint8_t late = 0;               // ...of its ports, the new players'
        uint64_t rejoins = 0;
        // Host: the spectators it goes to too (bit n: spectator n): those coming in, and of them `spectators_resync`, those
        // watching whose game went another way (told by a Resync of their own).
        uint8_t spectators = 0;
        uint8_t spectators_resync = 0;
        // Joiner: a desync found here (the read compared, never: none) and the read this machine was at then; the
        // state's parts so far; the state once it's all here and checked (the runtime's and the host's own); the read
        // of the last state loaded (hashes before it are of the replaced game).
        uint64_t desync_read = never;
        uint64_t desync_found_at = 0;
        double desync_found_time = 0;
        std::vector<uint8_t> parts;
        uint32_t size = 0;
        uint64_t part_hash = 0;
        bool ready = false;
        std::vector<uint8_t> runtime;
        std::vector<uint8_t> host;
        uint64_t loaded_read = 0;
    } recovery;

    // Coming back. Host: the ports of players welcomed back, for the next Back (after any state under way). A
    // joiner coming back: its game's reads are its own (it's starting) till it has loaded the host's, from
    // `rejoining_since`; it waits for that at its game's rejoin_boot_reads'th read (its game has started by then).
    uint8_t rejoin_wanted = 0;
    uint8_t late_wanted = 0;        // ...of them, the new players (joining under way)
    // Host: the spectators for the next state (bit n: spectator n): coming in to the match under way, and watching with a
    // game that went another way; by spectator, the read of the last state it got (hashes before it are of the game it
    // replaced; never: none). Spectators are never waited for: theirs go with whatever state is next, or one of their own.
    uint8_t spectators_late_wanted = 0;
    uint8_t spectators_resync_wanted = 0;
    std::array<uint64_t, conker::netplay::max_spectators> spectator_last_read;
    // Each state costs the host a moment and an upload, and holds up players coming back while it goes: a state for
    // spectators alone at most every spectator_state_seconds, and a spectator set right at most most_spectator_resyncs
    // times (then it's removed).
    constexpr double spectator_state_seconds = 10.0;
    double spectator_state_at = -1e9;
    constexpr int most_spectator_resyncs = 3;
    std::array<int, conker::netplay::max_spectators> spectator_resyncs{};

    // Seat changes. Host: the spectators to seat, in turn (their Claims, the host's moves: one a read), and by port,
    // the spectator to seat once its player has stepped out (the host's swap: -1 none). A spectator's game is the
    // match's already: it plays from a read past any input sent (a Back naming it), with no state sent. A player
    // stepping out (asked, or of their own accord: step_out_wanted, at their next read) sends no more inputs; the host
    // tells everyone their port is idle (open) from the read after the last (Gone's 2), and they watch.
    struct SeatMove {
        int spectator = -1;
        uint64_t player = 0;
        int port = 0;
    };
    std::vector<SeatMove> seat_moves;
    std::array<SeatMove, conker::netplay::max_players> swaps;
    // Host: the ports asked to step out, until they do (or leave); by spectator, the read of its latest hash (how far
    // its game has got) and since when a move of it has waited. A spectator is seated only once its game is near the
    // host's (it loaded any state it was sent, and is within 10 s of it): the others then wait a little at most for its
    // first inputs, as it catches up (well within wait_seconds).
    uint8_t unseat_asked = 0;
    std::array<uint64_t, conker::netplay::max_spectators> spectator_at{};
    constexpr uint64_t seat_near_reads = 300;
    constexpr double seat_wait_seconds = 15.0;
    double seat_waiting_since = 0;
    bool step_out_wanted = false;
    // (Tests) a spectator claims a port at a read (--netplay-claim-at), a player steps out at one
    // (--netplay-step-out-at), the host seats a spectator in a port at one (--netplay-seat-at).
    uint64_t claim_at = never;
    int claim_port = -1;
    uint64_t step_out_at = never;
    uint64_t seat_at = never;
    int seat_port = -1;
    int seat_spectator = -1;
    double rejoin_wanted_at = 0;
    // (Another connection on its way in may be another player coming back: up to this long, the host waits for it,
    // so they're brought back together, the others waiting for one state rather than two.)
    constexpr double rejoin_gather_seconds = 3.0;
    bool rejoining = false;
    bool joining_late = false;      // ...as a new player
    double rejoining_since = 0;
    constexpr uint64_t rejoin_boot_reads = 30;
    // The most reads a match coming back to can be at: six hours' (the inputs before are kept, as in any match: a
    // Rejoin from a broken or hostile host saying more would have this machine hold that many for each port at once).
    constexpr uint64_t most_rejoin_read = 30ull * 60 * 60 * 6;

    std::array<std::vector<Input>, conker::netplay::max_players> received;
    std::array<bool, conker::netplay::max_players> gone{};
    // A player that left the match while it went on (#17): from this read their port gets an idle pad (never:
    // they're in, or the match ends without them). The host decides it, the read after the last of their
    // inputs it had, and tells the others (Gone); nobody can be past it, as everyone needed those inputs.
    std::array<uint64_t, conker::netplay::max_players> idle_from;
    // By port, the read a player who came back plays again from (never: none). Their inputs before it are idle pads
    // put there as they came back: till their own come, they say nothing of how far ahead they are.
    std::array<uint64_t, conker::netplay::max_players> back_at;
    // The ports nobody has had since the start, or since their player left as someone joined (bit n: port n): idle,
    // and free for a new player (rather than a player who left, who may come back).
    uint8_t open_ports = 0;
    bool only_idle_since_back(int port) {
        return back_at[port] != never && received[port].size() <= back_at[port];
    }
    // Whether a peer that's gone stopped answering, rather than saying goodbye (a crash, a lost connection),
    // so it's told as maybe having quit.
    bool gone_silently = false;
    // A player whose inputs stop coming holds the others up only for a while: once their machine has kept a read
    // waiting `stall_grace` (a stall: reads waiting for it, each over stall_reset_seconds, one after the other), the host
    // lets go of it (Gone's 3: idle from the read after the last of its inputs, as for a player who left) and the others
    // play on. It stays connected, and asks for the host's game at once (or once its connection is back), as a player
    // coming back does (Rejoin). A stall that ends before then lets go of nobody. The grace is first_stall_seconds, for
    // one bad moment to set itself right, but long_stall_seconds for a machine that had a stall that long within
    // repeat_stall_seconds (its connection keeps failing: the others shouldn't keep waiting for it). Short waits, often,
    // are its delay's to cover (adapt_delay), not this. (Not for ports getting a state, or whose first inputs since
    // coming back haven't come: they're waited for as ever; nor in the boot's first reads.)
    constexpr double first_stall_seconds = 5.0;
    constexpr double long_stall_seconds = 2.5;
    constexpr double repeat_stall_seconds = 180.0;
    constexpr double stall_reset_seconds = 0.25;
    std::array<double, conker::netplay::max_players> stall_since{};    // 0: none under way
    std::array<double, conker::netplay::max_players> stall_grace{};
    std::array<double, conker::netplay::max_players> long_stall_at{};  // the last time a stall had gone on that long (0: never)
    uint8_t lost_ports = 0;         // let go of, not back yet
    // By port, the read of the last state sent to it (0: none): till an input of its own for that read comes, it's loading.
    std::array<uint64_t, conker::netplay::max_players> state_read_for{};
    uint64_t lost_count = 0;
    bool lost_here = false;         // a joiner: this machine was let go of, and is coming back
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
    // A spectator, behind the players by as much as its network and machine make it, takes them up to a minute ahead.
    uint64_t current_read = 0;
    constexpr uint64_t spectator_slack = 30 * 60;
    // A spectator seated, till it reaches the read it plays from: still behind, as a spectator may be.
    uint64_t seated_from = 0;
    uint64_t horizon() {
        return current_read + 2 * (uint64_t)most_delay() + conker::netplay::input_window + 16 +
            (watching || current_read < seated_from ? spectator_slack : 0);
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
    // Rollback: this peer's hashes of reads not yet known to be right, until they are (verify).
    std::map<uint32_t, OwnHash> unverified_hashes;
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
        if (rollback_option > 0) {
            std::printf("[netplay] rollback: %llu reads predicted, %llu rollbacks, %llu reads made again, the deepest %llu reads\n",
                (unsigned long long)rollbacks.predicted, (unsigned long long)rollbacks.count,
                (unsigned long long)rollbacks.reads_again, (unsigned long long)rollbacks.deepest);
            std::printf("[netplay] time sync: %llu reads paced later, %.0f ms in all; ahead of the others by %.2f reads at the end, "
                "%.2f at most\n", (unsigned long long)time_sync.reads_slowed, time_sync.us_later / 1000.0, time_sync.lead,
                time_sync.most_lead);
        }
        if (recovery.count > 0) {
            std::printf("[netplay] desync recovery: %llu, each set right by the host's game\n", (unsigned long long)recovery.count);
        }
        if (recovery.rejoins > 0) {
            std::printf("[netplay] players came back %llu times, each sent the host's game\n", (unsigned long long)recovery.rejoins);
        }
        if (lost_count > 0) {
            std::printf("[netplay] players let go of %llu times as their inputs stopped coming\n", (unsigned long long)lost_count);
        }
        std::string by_port;
        for (int port = 0; port < match_players; port++) {
            by_port += (port == 0 ? "" : ", ") + std::to_string(delays[port].after);
        }
        std::printf("[netplay] input delays by port: %s\n", by_port.c_str());
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
        end_reason = reason;
        if (!left && session != nullptr) {
            session->leave(reason);
            left = true;
        }
        ended = true;
        publish();
    }

    // Leaves the match with a goodbye, and polls the library a moment for it to go out (it's sent as the
    // library is polled), or the others only hear the match is over when their connection times out.
    // A new session's waiting room: the block list, and as a host the match's settings and whether it's
    // ready by itself.
    void prepare_room() {
        session->set_blocked(blocked_players);
        session->set_release(conker::netplay::release_name());
        if (session->hosting()) {
            session->set_room_settings(host_own_view, delay_option, rollback_option, spectators_option);
            session->set_seat_hold(seat_hold_option);
            ready_now = auto_ready;
            session->set_ready(ready_now);
        }
    }

    void say_goodbye() {
        std::scoped_lock lock(session_mutex);
        print_waits();
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
        const std::string name = std::string("desync-") + stamp + "-read" + std::to_string(read) +
            (own_ports == 0 ? std::string("-spectator") : "-port" + std::to_string(std::countr_zero(own_ports) + 1));
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
        std::fprintf(file, "  \"players\": %d,\n  \"input_delay\": %d,\n  \"input_delays\": [", match_players, own_delay_at(read));
        for (int p = 0; p < match_players; p++) std::fprintf(file, "%s%d", p == 0 ? "" : ", ", delay_at(p, read));
        std::fprintf(file, "],\n  \"own_view\": %s,\n  \"view_pairs\": %u,\n  \"rollback\": %d,\n",
            match_own_view ? "true" : "false", match_pairs, match_rollback);
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

    // Through a room: tells the netplay server this match desynced, in a summary for its operator (the build,
    // this machine's platform, the read and the regions; no address, memory or inputs). The full report stays
    // here.
    void report_to_server(int port, uint32_t read, const std::vector<int>& regions) {
        if (signaller == nullptr || session == nullptr || own_ports == 0) {
            return;
        }
        // The match's key, which every player has, made into a tag that tells this match's reports from
        // another's (and can't be turned back into the key).
        uint64_t key = session->hosting() ? session->key() : signaller->found().key;
        key = (key ^ (key >> 33)) * 0xFF51AFD7ED558CCDull;
        key = (key ^ (key >> 33)) * 0xC4CEB9FE1A85EC53ull;
        conker::netplay::signal::DesyncReport summary;
        summary.match = (uint32_t)(key ^ (key >> 32)) | 1;
        summary.build = sent.build != 0 ? sent.build : 1;
        summary.platform = conker::netplay::this_platform();
        summary.players = (uint8_t)match_players;
        summary.port = (uint8_t)std::countr_zero(own_ports);
        summary.other = (uint8_t)port;
        summary.read = read;
        summary.rollback = (uint8_t)match_rollback;
        summary.delay = (uint8_t)delay;
        for (const int region : regions) {
            summary.regions[region / 8] |= (uint8_t)(1u << (region % 8));
        }
        if (signaller->report_desync(summary)) {
            std::printf("[netplay] a summary of the desync was sent to the netplay server (no addresses, memory or inputs)\n");
        }
    }

    // The game's memory at `read` isn't `port`'s: says where and writes the report (its path, or empty).
    std::string report_desync(int port, uint32_t read, const OwnHash& own, const Hash& theirs) {
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
        report_to_server(port, read, regions);
        return report;
    }

    // ...and ends the match.
    void end_for_desync(const std::string& report) {
        std::string why = "the games went different ways (a desync)";
        if (!report.empty()) {
            why += ". A report is in " + report + ": please send it, with the other players' reports, when you report the problem";
        }
        end_match(Reason::desync, why.c_str());
    }

    void desync(int port, uint32_t read, const OwnHash& own, const Hash& theirs) {
        end_for_desync(report_desync(port, read, own, theirs));
    }

    // The host's key for spectator `n`'s hashes (their_hashes'), past the ports'.
    constexpr int spectator_key(int n) { return conker::netplay::max_players + n; }

    // A desync with `port` at `read`, found as the hashes are compared: set right if it can be (true), or else
    // the match ends. The host recovers by sending its game (the next Resync); a joiner waits for the host's. A
    // spectator's (`port` its key) never ends the match: it's sent the host's game alone, or removed should it go
    // another way again soon after.
    bool recover(int port, uint32_t read, const OwnHash& own, const Hash& theirs) {
        if (session->hosting() && port >= conker::netplay::max_players) {
            const int n = port - conker::netplay::max_players;
            const uint8_t bit = (uint8_t)(1u << n);
            const uint64_t last = spectator_last_read[n];
            // (A hash of the game a state replaced or replaces, or one already found; without recovery the spectator,
            // which finds it too, leaves when no state comes.)
            if ((last != never && read < last) || (recovery.read != never && (recovery.spectators & bit) != 0 && read < recovery.read) ||
                ((spectators_resync_wanted | spectators_late_wanted) & bit) != 0 || !recovery_option) {
                return true;
            }
            std::printf("[netplay] desync at read %u: spectator %d's game memory isn't this one's (%zu regions differ)\n", read, n + 1,
                differing_regions(own, theirs).size());
            if (last != never && read < last + resync_again_reads) {
                std::printf("[netplay] spectator %d's game went another way again %llu reads after it was set right: it's removed\n",
                    n + 1, (unsigned long long)(read - last));
                session->kick_spectator(n, 0, false);
                return true;
            }
            // (Nor is it sent the host's game without end: each costs the host a state.)
            if (++spectator_resyncs[n] > most_spectator_resyncs) {
                std::printf("[netplay] spectator %d's game went another way %d times: it's removed\n", n + 1, spectator_resyncs[n]);
                session->kick_spectator(n, 0, false);
                return true;
            }
            spectators_resync_wanted |= bit;
            return true;
        }
        if (session->hosting()) {
            const uint64_t last = recovery.last_read[port];
            // A hash of the game a Resync replaced (or replaces, once it's sent): of no account.
            if ((last != never && read < last) || (recovery.read != never && (recovery.ports & (1u << port)) != 0 && read < recovery.read)) {
                return true;
            }
            if (!recovery_option || idle_from[port] != never) {
                return false;
            }
            // (One report a desync: it's set right with the next Resync.)
            if ((recovery.wanted & (1u << port)) != 0) {
                return true;
            }
            report_desync(port, read, own, theirs);
            if (last != never && read < last + resync_again_reads) {
                std::printf("[netplay] port %d's game went another way again %llu reads after it was set right: the match ends\n",
                    port + 1, (unsigned long long)(read - last));
                end_match(Reason::desync, "the games kept going different ways (a desync), even after the host's game was sent");
                return true;
            }
            recovery.wanted |= (uint8_t)(1u << port);
            return true;
        }
        // (The game a state is on its way to replace, or was, or one already reported.)
        if (read < recovery.loaded_read || recovery.desync_read != never || (recovery.read != never && read < recovery.read)) {
            return true;
        }
        report_desync(port, read, own, theirs);
        recovery.desync_read = read;
        recovery.desync_found_at = current_read;
        recovery.desync_found_time = now();
        std::printf("[netplay] waiting for the host to send its game (desync recovery)\n");
        return true;
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
            if (own->second.hash != it->second.hash && !recover(it->first.first, own->first, own->second, it->second)) {
                desync(it->first.first, own->first, own->second, it->second);
                return;
            }
            if (ended) {
                return;
            }
            it = their_hashes.erase(it);
        }
        // (A spectator may be well behind: while any watches, the host keeps its own as far back as one can be.)
        const size_t kept = session->hosting() && session->spectators() != 0 ? spectator_slack / hash_every + 8 : 8;
        while (own_hashes.size() > kept) {
            own_hashes.erase(own_hashes.begin());
        }
    }

    // Sends this peer's hash of `read`, keeping it to compare with the others'.
    void send_hash(uint32_t read, const OwnHash& own) {
        own_hashes[read] = own;
        Hash sent_hash{ read, own.hash };
        for (int i = 0; i < conker::netplay::memory_regions; i++) {
            sent_hash.regions[i] = (uint16_t)own.regions[i];
        }
        // How each port's inputs reach this machine, for the host's delays.
        for (int port = 0; port < conker::netplay::max_players; port++) {
            const double spread = (own_ports & (1u << port)) != 0 || watching ? -1 : own_spread_ms(port);
            sent_hash.spread[port] = spread < 0 ? conker::netplay::no_spread : (uint8_t)std::min(254.0, std::round(spread / 2));
            sent_hash.waited[port] = (uint8_t)waited_for_port[port];
            sent_hash.waited_long[port] = (uint8_t)waited_long_for_port[port];
        }
        session->send(sent_hash);
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

    // Tells the player that `ports` lost connection (let go of for now: the others play on).
    void tell_lost(uint8_t ports) {
        lost_ports |= ports;
        {
            std::lock_guard lock(status_mutex);
            shown.lost |= ports;
        }
        tell(players_text(ports) + " lost connection: the others play on.");
    }

    // The host: `port`'s machine has held everyone up past its grace (stall_since's comment): let go of, the others play on.
    void let_go_of_machine(int port, double stalled) {
        const uint8_t machine = session->machine_of(port);
        if (machine == 0) {
            return;
        }
        std::printf("[netplay] read %llu: %s held everyone up for %.1f s: let go of, the others play on\n", (unsigned long long)current_read,
            ports_text(machine).c_str(), stalled);
        for (int p = 0; p < match_players; p++) {
            if ((machine & (1u << p)) == 0) continue;
            const uint64_t from = received[p].size();
            let_go(p, from);
            session->send(Gone{ (uint8_t)p, (uint32_t)from, 3 });
            stall_since[p] = 0;
        }
        session->let_go_of(machine);
        // (Nothing under way for it any more: it comes back with a state of its own.)
        rejoin_wanted &= (uint8_t)~machine;
        late_wanted &= (uint8_t)~machine;
        recovery.wanted &= (uint8_t)~machine;
        unseat_asked &= (uint8_t)~machine;
        if (recovery.read != never && (recovery.ports & machine) != 0) {
            recovery.ports &= (uint8_t)~machine;
            recovery.late &= (uint8_t)~machine;
            if (recovery.ports == 0 && recovery.spectators == 0) {
                recovery.read = never;
                recovery.state.clear();
            }
        }
        std::erase_if(their_hashes, [&](const auto& h) { return h.first.first < conker::netplay::max_players && (machine & (1u << h.first.first)) != 0; });
        lost_count++;
        tell_lost(machine);
    }

    // The host, as read `read` waits at `t` for the ports `missing`: how long each machine has held everyone up, and
    // those past their grace let go of.
    void let_go_of_stalled(uint64_t read, uint8_t missing, double t, const std::array<double, conker::netplay::max_players>& missing_at) {
        if (read < rejoin_boot_reads) {
            return;
        }
        for (int port = 1; port < match_players; port++) {
            const uint8_t bit = (uint8_t)(1u << port);
            const bool getting_state = (recovery.read != never && (recovery.ports & bit) != 0) ||
                (state_read_for[port] != 0 && received[port].size() <= state_read_for[port] && t - recovery.sent_at < state_wait_seconds);
            if ((missing & bit) == 0 || (own_ports & bit) != 0 || idle_from[port] != never || only_idle_since_back(port) || getting_state) {
                continue;
            }
            // (From when this read began waiting for it: an exemption ending partway through a wait mustn't lose the
            // time already waited to the wait's own end.)
            if (stall_since[port] == 0) {
                stall_since[port] = missing_at[port] != 0 ? missing_at[port] : t;
                stall_grace[port] = long_stall_at[port] != 0 && t - long_stall_at[port] < repeat_stall_seconds ? long_stall_seconds
                    : first_stall_seconds;
            }
            const double stalled = t - stall_since[port];
            if (stalled >= long_stall_seconds) long_stall_at[port] = t;
            if (stalled >= stall_grace[port]) {
                let_go_of_machine(port, stalled);
            }
        }
    }

    // A joiner: the host let go of this machine, its inputs having stopped coming for a while (Gone's 3): the others
    // play on. It's still connected: it asks for the host's game at once, and comes back with it as a player coming back
    // does (rejoining_read, Rejoin, the state).
    void lost_here_now() {
        // (A machine with several players hears it for each.)
        if (lost_here) {
            return;
        }
        std::printf("[netplay] read %llu: the host let go of this machine (its inputs stopped coming for a while): coming back\n",
            (unsigned long long)current_read);
        rejoining = true;
        rejoining_since = now();
        lost_here = true;
        recovery.read = never;
        recovery.ports = 0;
        recovery.parts.clear();
        recovery.size = 0;
        recovery.ready = false;
        recovery.desync_read = never;
        rollback_to = never;
        if (catching_up) set_catching_up(false);
        session->come_back();
        tell("Your connection dropped: the others played on. Getting the host's game...");
    }

    // The delay of `ports` becomes `to` from read `from`.
    void change_delay(uint8_t ports, int to, uint64_t from) {
        std::printf("[netplay] read %llu: the input delay of %s becomes %d from read %llu (it's %d)\n", (unsigned long long)current_read,
            ports_text(ports).c_str(), to, (unsigned long long)from, delays[std::countr_zero(ports)].after);
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((ports & (1u << port)) != 0) delays[port] = { from, delays[port].after, to };
        }
        if ((ports & own_ports) != 0 || (own_ports == 0 && (ports & 1) != 0)) waits.delay_changes++;
    }

    int own_delay_at(uint64_t read) {
        return delay_at(own_ports == 0 ? 0 : std::countr_zero(own_ports), read);
    }

    void take_resync(const Resync& r);

    bool rollback_catch_up(uint64_t read) {
        return rollback_option > 0 && !watching && read >= rollback_from_read && read + rollback_option < last_read;
    }
    int widest_gap_to_me() {
        int widest = INT_MIN;
        for (int port = 0; port < match_players; port++) {
            if ((own_ports & (1u << port)) == 0 && idle_from[port] == never && time_sync.heard[port] && !only_idle_since_back(port)) {
                widest = std::max(widest, time_sync.their_gap[port]);
            }
        }
        return widest;
    }
    void list_room();
    void play_again(uint8_t ports, uint64_t from);
    bool can_play_again(int port, uint64_t from);
    void beacon_match();
    void take_state_part(const StatePart& part);
    bool take_back(const Back& b);
    bool take_rejoin(const Rejoin& r);

    // `port` stepped out to watch from its idle read: open from now on. `stepped_here`: this machine's ports that did,
    // till each is told.
    uint8_t stepped_here = 0;
    void stepped_out(int port) {
        open_ports |= (uint8_t)(1u << port);
        {
            std::lock_guard lock(status_mutex);
            shown.open = open_ports;
        }
        if ((stepped_here & (1u << port)) != 0) {
            stepped_here &= (uint8_t)~(1u << port);
            if (stepped_here == 0) tell("You're watching now: L and R switch between everyone and each player.");
            return;
        }
        tell(players_text((uint8_t)(1u << port)) + " stepped out to watch: their place is open.");
    }

    // The host: the joiner's machine on `ports` stepped out (its Unseat, after its last inputs): it watches from now on.
    // Before the start, nothing's been played: it's a spectator at once.
    void host_unseated(uint8_t ports) {
        unseat_asked &= (uint8_t)~ports;
        const int number = session->unseat(ports);
        if (!match_started) {
            if (number >= 0) {
                joined -= std::popcount(ports);
                std::printf("[netplay] %s stepped out to watch (spectator %d)\n", ports_text(ports).c_str(), number + 1);
                spectator_last_read[number] = never;
                spectator_resyncs[number] = 0;
                spectator_at[number] = 0;
            }
            return;
        }
        if (number < 0) {
            // (No spectator number free, or none taken: they leave, as players who quit.)
            for (int port = 0; port < conker::netplay::max_players; port++) {
                if ((ports & (1u << port)) != 0) swaps[port] = {};
            }
            session->kick(std::countr_zero(ports), false);
            return;
        }
        // (A state on its way to those ports is for nobody now.)
        if (recovery.read != never && (recovery.ports & ports) != 0) {
            recovery.ports &= (uint8_t)~ports;
            recovery.late &= (uint8_t)~ports;
            if (recovery.ports == 0 && recovery.spectators == 0) {
                recovery.read = never;
                recovery.state.clear();
            }
        }
        spectator_last_read[number] = never;
        spectator_resyncs[number] = 0;
        spectator_at[number] = 0;
        joined -= std::popcount(ports);
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            const uint64_t from = received[port].size();
            std::printf("[netplay] port %d stepped out to watch (spectator %d), idle from read %llu\n", port + 1, number + 1,
                (unsigned long long)from);
            gone[port] = true;
            let_go(port, from);
            session->send(conker::netplay::Gone{ (uint8_t)port, (uint32_t)from, 2 });
            stepped_out(port);
            // The host's swap: the spectator it chose takes the place.
            if (swaps[port].spectator >= 0) {
                seat_moves.push_back(swaps[port]);
                swaps[port] = {};
            }
        }
        list_room();
    }

    // Before the start: spectator `n` (still `player`) takes `port`, at once, if it's free (the roster tells everyone).
    bool seat_before_start(int n, uint64_t player, int port) {
        if (session->roster().watchers[n].player != player || !session->seat_spectator(n, port)) {
            return false;
        }
        joined++;
        std::printf("[netplay] spectator %d takes %s\n", n + 1, ports_text((uint8_t)(1u << port)).c_str());
        return true;
    }

    // The host, at a read made the first time: the next spectator to seat, if it still can be (a spectator in the match,
    // its game not going another way, the port free or held), plays from a read past any input sent.
    void seat_next(uint64_t read) {
        if (seat_moves.empty() || read < rejoin_boot_reads) {
            return;
        }
        const SeatMove move = seat_moves.front();
        const uint8_t bit = (uint8_t)(1u << move.port);
        const uint8_t watcher = (uint8_t)(1u << move.spectator);
        // (Not till its game is near this one's, having loaded any state it was sent: else the others would wait for its
        // first inputs, and it would be behind for good. It's waited for a while, then given up.)
        const uint64_t loaded = spectator_last_read[move.spectator] == never ? 0 : spectator_last_read[move.spectator];
        const uint64_t at = spectator_at[move.spectator];
        if (session->roster().watchers[move.spectator].player == move.player && (at < loaded || at + seat_near_reads < read)) {
            if (seat_waiting_since == 0) {
                seat_waiting_since = now();
                std::printf("[netplay] read %llu: spectator %d is at read %llu: it takes %s once it's nearer\n", (unsigned long long)read,
                    move.spectator + 1, (unsigned long long)at, ports_text(bit).c_str());
            }
            if (now() - seat_waiting_since < seat_wait_seconds) {
                return;
            }
            std::printf("[netplay] read %llu: spectator %d is still too far behind to take %s\n", (unsigned long long)read,
                move.spectator + 1, ports_text(bit).c_str());
            seat_moves.erase(seat_moves.begin());
            seat_waiting_since = 0;
            return;
        }
        seat_moves.erase(seat_moves.begin());
        seat_waiting_since = 0;
        if (session->roster().watchers[move.spectator].player != move.player ||
            ((spectators_late_wanted | spectators_resync_wanted | recovery.spectators) & watcher) != 0 ||
            (rejoin_wanted & bit) != 0 || (recovery.read != never && (recovery.ports & bit) != 0) ||
            !can_play_again(move.port, horizon() + 1) || !session->seat_spectator(move.spectator, move.port)) {
            std::printf("[netplay] read %llu: spectator %d can't take %s now\n", (unsigned long long)read, move.spectator + 1,
                ports_text(bit).c_str());
            return;
        }
        std::erase_if(their_hashes, [&](const auto& h) { return h.first.first == spectator_key(move.spectator); });
        const uint64_t from = horizon() + 1;
        play_again(bit, from);
        std::printf("[netplay] read %llu: spectator %d takes %s from read %llu\n", (unsigned long long)read, move.spectator + 1,
            ports_text(bit).c_str(), (unsigned long long)from);
        session->back(bit, (uint32_t)from, bit, move.player);
        tell(conker::netplay::player_name(move.player) + " takes " + players_text(bit) + "'s place.");
        list_room();
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
                // The host's, in a match: a spectator coming in, sent the host's game with the next state (or one of its own).
                if (event.spectator >= 0) {
                    std::printf("[netplay] spectator %d is watching (%s)\n", event.spectator + 1,
                        conker::netplay::player_name(session->roster().watchers[event.spectator].player).c_str());
                    spectator_last_read[event.spectator] = never;
                    spectator_resyncs[event.spectator] = 0;
                    spectator_at[event.spectator] = 0;
                    if (event.rejoin) {
                        if (rejoin_wanted == 0 && spectators_late_wanted == 0) rejoin_wanted_at = now();
                        spectators_late_wanted |= (uint8_t)(1u << event.spectator);
                    }
                    break;
                }
                // ...or a machine let go of, here again on its connection (the next Back, as for one coming back).
                if (event.returned) {
                    std::printf("[netplay] %s here again: they come back\n", ports_text(event.ports).c_str());
                    if (rejoin_wanted == 0) rejoin_wanted_at = now();
                    rejoin_wanted |= event.ports;
                    break;
                }
                // ...or a player who left, coming back, or a new one on free ports (the next Back).
                if (event.rejoin) {
                    std::printf("[netplay] %s %s (%s)\n", ports_text(event.ports).c_str(), event.late ? "joined the match under way" : "came back",
                        conker::netplay::player_name(session->roster().seats[event.port].player).c_str());
                    if (rejoin_wanted == 0) rejoin_wanted_at = now();
                    rejoin_wanted |= event.ports;
                    if (event.late) late_wanted |= event.ports;
                    joined += std::popcount(event.ports);
                    list_room();
                }
                break;
            case Event::Type::welcomed:
            case Event::Type::started:
            case Event::Type::moved:
                break;
            case Event::Type::left:
                // The host's: a spectator stopped watching (nobody waits for it).
                if (event.spectator >= 0) {
                    const uint8_t bit = (uint8_t)(1u << event.spectator);
                    std::printf("[netplay] spectator %d left: %s\n", event.spectator + 1, conker::netplay::describe(event.reason));
                    spectators_late_wanted &= (uint8_t)~bit;
                    spectators_resync_wanted &= (uint8_t)~bit;
                    std::erase_if(seat_moves, [&](const SeatMove& m) { return m.spectator == event.spectator; });
                    for (SeatMove& swap : swaps) {
                        if (swap.spectator == event.spectator) swap = {};
                    }
                    std::erase_if(their_hashes, [&](const auto& h) { return h.first.first == spectator_key(event.spectator); });
                    if (recovery.read != never && (recovery.spectators & bit) != 0) {
                        recovery.spectators &= (uint8_t)~bit;
                        recovery.spectators_resync &= (uint8_t)~bit;
                        if (recovery.ports == 0 && recovery.spectators == 0) {
                            recovery.read = never;
                            recovery.state.clear();
                        }
                    }
                    break;
                }
                // ...or a joiner's machine left (all its ports), saying goodbye or not answering.
                std::printf("[netplay] %s left: %s\n", ports_text(event.ports).c_str(), conker::netplay::describe(event.reason));
                for (int port = 0; port < conker::netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) gone[port] = true;
                }
                gone_silently = gone_silently || event.reason == Reason::timeout;
                // (Coming back, and gone again before its Back: its ports stay idle; after it, before its state went:
                // nobody to send it to, nor to say they're back again.)
                rejoin_wanted &= (uint8_t)~event.ports;
                late_wanted &= (uint8_t)~event.ports;
                unseat_asked &= (uint8_t)~event.ports;
                for (int port = 0; port < conker::netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) swaps[port] = {};
                }
                joined -= std::popcount(event.ports);
                list_room();
                if (recovery.rejoin && recovery.read != never) {
                    recovery.ports &= (uint8_t)~event.ports;
                    recovery.late &= (uint8_t)~event.ports;
                    if (recovery.ports == 0 && recovery.spectators == 0) {
                        recovery.read = never;
                        recovery.state.clear();
                    }
                }
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
                    // (A machine let go of, gone for good now: idle already from the read it was let go of at; everyone is
                    // told it left, by a Gone of that read again.)
                    const uint8_t was_lost = (uint8_t)(lost_ports & event.ports);
                    if (was_lost != 0) {
                        for (int port = 0; port < conker::netplay::max_players; port++) {
                            if ((was_lost & (1u << port)) != 0) {
                                session->send(Gone{ (uint8_t)port, (uint32_t)idle_from[port], (uint8_t)(event.reason == Reason::timeout) });
                            }
                        }
                        lost_ports &= (uint8_t)~was_lost;
                        std::lock_guard lock(status_mutex);
                        shown.lost &= (uint8_t)~was_lost;
                    }
                    if ((let_go_ports | was_lost) != 0) {
                        tell_left((uint8_t)(let_go_ports | was_lost), event.reason == Reason::timeout);
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
                host_closed = event.reason != Reason::timeout;
                left = true;
                if (event.reason == Reason::desync) {
                    other_desynced();
                }
                break;
            case Event::Type::packet:
                if (const Inputs* inputs = std::get_if<Inputs>(&event.packet)) {
                    std::vector<Input>& got = received[inputs->port];
                    // An early copy (sent unreliably, so a lost packet doesn't hold up the inputs after it) may repeat
                    // reads that came, as the reliable Inputs after it do; one that starts past them, or for a port
                    // that's idle now, isn't taken (it's late, or the reliable ones fill in).
                    if (event.copy && (inputs->port >= match_players || (own_ports & (1u << inputs->port)) != 0 ||
                        inputs->first_read > got.size() || idle_from[inputs->port] != never)) {
                        break;
                    }
                    if ((own_ports & (1u << inputs->port)) != 0 || inputs->port >= match_players || inputs->first_read > got.size() ||
                        (uint64_t)inputs->first_read + inputs->count > horizon()) {
                        std::printf("[netplay] port %d's inputs from read %u (%d) don't fit: %zu came, up to read %llu can\n", inputs->port + 1,
                            inputs->first_read, inputs->count, got.size(), (unsigned long long)horizon());
                        end_match(Reason::protocol, "a player's inputs came out of order");
                        return;
                    }
                    const size_t skip = got.size() - inputs->first_read;
                    if (skip >= inputs->count) {
                        break;
                    }
                    for (int i = (int)skip; i < inputs->count; i++) {
                        got.push_back({ inputs->pads[i], inputs->motions[i] });
                        note_arrival(inputs->port, got.size() - 1, event.arrived);
                    }
                    // Their gap to this machine (to its first port: its players are at the same read).
                    if (own_ports != 0) {
                        time_sync.their_gap[inputs->port] = inputs->gaps[std::countr_zero(own_ports)];
                        time_sync.heard[inputs->port] = true;
                    }
                }
                else if (const Gone* g = std::get_if<Gone>(&event.packet)) {
                    // This machine let go of (its inputs stopped coming for a while): it comes back.
                    if ((own_ports & (1u << g->port)) != 0) {
                        lost_here_now();
                        break;
                    }
                    // ...as it stepped out to watch (its step-out crossed it: the host never took it): it can only leave.
                    if (g->silent == 3 && (stepped_here & (1u << g->port)) != 0) {
                        end_match(Reason::quit, "the connection dropped as you stepped out to watch: rejoin to come back");
                        return;
                    }
                    // A player let go of has left for good: idle already (from that read; for a machine that came in since,
                    // from the read it came in at, or later).
                    if (g->silent <= 1 && idle_from[g->port] != never && g->from_read <= idle_from[g->port]) {
                        std::printf("[netplay] %s left%s\n", ports_text((uint8_t)(1u << g->port)).c_str(), g->silent ? " (stopped answering)" : "");
                        gone[g->port] = true;
                        lost_ports &= (uint8_t)~(1u << g->port);
                        {
                            std::lock_guard lock(status_mutex);
                            shown.lost &= (uint8_t)~(1u << g->port);
                        }
                        tell_left((uint8_t)(1u << g->port), g->silent != 0);
                        break;
                    }
                    // The host's word that a player left: from the read after the last of their inputs it passed on,
                    // all of which came first.
                    if (idle_from[g->port] != never || g->from_read != received[g->port].size()) {
                        end_match(Reason::protocol, "the host said a player left at a read that doesn't fit");
                        return;
                    }
                    gone[g->port] = true;
                    if (g->silent == 2) {
                        // Stepped out to watch: the port is open.
                        std::printf("[netplay] %s stepped out to watch\n", ports_text((uint8_t)(1u << g->port)).c_str());
                        let_go(g->port, g->from_read);
                        stepped_out(g->port);
                        break;
                    }
                    if (g->silent == 3) {
                        // Let go of for now (their inputs stopped coming): they come back by themselves.
                        std::printf("[netplay] %s lost connection: the others play on\n", ports_text((uint8_t)(1u << g->port)).c_str());
                        gone[g->port] = false;
                        let_go(g->port, g->from_read);
                        tell_lost((uint8_t)(1u << g->port));
                        break;
                    }
                    std::printf("[netplay] %s left%s\n", ports_text((uint8_t)(1u << g->port)).c_str(), g->silent ? " (stopped answering)" : "");
                    let_go(g->port, g->from_read);
                    tell_left((uint8_t)(1u << g->port), g->silent != 0);
                }
                else if (const Delay* d = std::get_if<Delay>(&event.packet)) {
                    // (Of the match's ports, each after its last change: one queued, or made.)
                    bool fits = (d->ports >> match_players) == 0;
                    bool reached = true;
                    for (int port = 0; port < match_players && fits; port++) {
                        if ((d->ports & (1u << port)) == 0) continue;
                        uint64_t last = delays[port].from;
                        for (const Delay& queued : later_delays) {
                            if ((queued.ports & (1u << port)) != 0) last = queued.from_read;
                        }
                        fits = d->from_read > last;
                        reached = reached && current_read >= delays[port].from;
                    }
                    // A spectator (or one just seated, still behind) not yet at the last change: this one waits for it
                    // (on_read), as does any after one waiting.
                    if (fits && (((watching || current_read < seated_from) && !reached) || !later_delays.empty())) {
                        if (later_delays.size() >= 64) {
                            end_match(Reason::protocol, "the host changed the input delay at a read that doesn't fit");
                            return;
                        }
                        later_delays.push_back(*d);
                        break;
                    }
                    // The host's: from a read this peer hasn't sent its pads at yet, once the last change is made.
                    if (!fits || d->from_read <= current_read || !reached) {
                        end_match(Reason::protocol, "the host changed the input delay at a read that doesn't fit");
                        return;
                    }
                    change_delay(d->ports, d->delay, d->from_read);
                }
                else if (const Resync* r = std::get_if<Resync>(&event.packet)) {
                    take_resync(*r);
                }
                else if (const Claim* c = std::get_if<Claim>(&event.packet)) {
                    // The host's: a spectator would take a free port (seat_next, at the next read; before the start, at once).
                    if (!match_started && event.spectator >= 0) {
                        seat_before_start(event.spectator, event.player, c->port);
                    }
                    else if (event.spectator >= 0 && (session->free_ports() & (1u << c->port)) != 0 &&
                        std::none_of(seat_moves.begin(), seat_moves.end(), [&](const SeatMove& m) { return m.spectator == event.spectator; })) {
                        std::printf("[netplay] spectator %d would take %s\n", event.spectator + 1, ports_text((uint8_t)(1u << c->port)).c_str());
                        seat_moves.push_back({ event.spectator, event.player, c->port });
                    }
                }
                else if (const Unseat* u = std::get_if<Unseat>(&event.packet)) {
                    // The host's: its player stepped out (their machine's last inputs came first). A joiner's: asked to.
                    if (session->hosting()) {
                        host_unseated(u->ports);
                    }
                    else {
                        step_out_wanted = true;
                    }
                }
                else if (const Back* b = std::get_if<Back>(&event.packet)) {
                    if (!take_back(*b)) {
                        end_match(Reason::protocol, "the host said a player came back at a read that doesn't fit");
                        return;
                    }
                }
                else if (const Rejoin* r = std::get_if<Rejoin>(&event.packet)) {
                    if (!take_rejoin(*r)) {
                        end_match(Reason::protocol, "the host's match to come back to doesn't fit");
                        return;
                    }
                }
                else if (const StatePart* part = std::get_if<StatePart>(&event.packet)) {
                    take_state_part(*part);
                }
                else if (const Hash* hash = std::get_if<Hash>(&event.packet)) {
                    // Hashes come every hash_every reads, within the horizon; one for a read this peer no
                    // longer has its own of can't be compared, and is dropped.
                    if (hash->read % hash_every != 0 || hash->read > horizon()) {
                        // (A spectator's: it's removed, the match goes on.)
                        if (event.spectator >= 0) {
                            std::printf("[netplay] spectator %d sent a broken hash: it's removed\n", event.spectator + 1);
                            session->kick_spectator(event.spectator, 0, false);
                            break;
                        }
                        end_match(Reason::protocol, "a player sent a broken hash");
                        return;
                    }
                    // (Nor one of a game a state replaced: coming back, the reads before it were never made here.)
                    if ((own_hashes.empty() || hash->read >= own_hashes.begin()->first) && hash->read >= recovery.loaded_read &&
                        !(rejoining && recovery.read != never && hash->read < recovery.read)) {
                        their_hashes[{ event.spectator >= 0 ? spectator_key(event.spectator) : event.port, hash->read }] = *hash;
                    }
                    if (event.spectator >= 0) spectator_at[event.spectator] = hash->read;
                    // A player's machine's word on how everyone's inputs reach it (the host's delays: adapt_delay).
                    if (session->hosting() && event.spectator < 0 && event.port > 0 && event.port < conker::netplay::max_players) {
                        Report& report = reports[event.port];
                        for (int port = 0; port < conker::netplay::max_players; port++) {
                            report.spread_ms[port] = hash->spread[port] == conker::netplay::no_spread ? -1 : hash->spread[port] * 2;
                        }
                        if (!report.heard) {
                            report.waited_counted = hash->waited;
                            report.waited_long_counted = hash->waited_long;
                        }
                        report.waited = hash->waited;
                        report.waited_long = hash->waited_long;
                        report.heard = true;
                        report.at = now();
                    }
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
        for (int other = 0; other < match_players; other++) {
            inputs.gaps[other] = (int8_t)std::clamp(time_sync.my_gap[other], -128, 127);
        }
        session->send(inputs);
        received[port].push_back(input);
    }

    // The input delay auto chooses, from the round trips (ms): enough reads that a pad sent the delay ahead arrives
    // before it's needed. The game can wait a few ms at a read and still keep up (a read is a frame, 33 ms at 30
    // frames a second, most of it spare), so a LAN's sub-millisecond trips give 0: the others' inputs reach the game
    // as soon as this one's own. Each read past that hides a frame's worth of trip, with a few ms for the trip to vary.
    constexpr double spare_ms = 4.0;    // for the trip varying
    constexpr double frame_ms = 1000.0 / 30.0;
    int reads_for(double one_way_ms, double spare) {
        constexpr double wait_ms = 8.0;     // what a read can wait without the game falling behind
        const double ahead = one_way_ms + spare - wait_ms;
        return std::min(ahead <= 0 ? 0 : (int)std::ceil(ahead / frame_ms), conker::netplay::max_delay);
    }

    // Without rollback, every pad must arrive in time along the slowest way one goes: the host passes the joiners' on,
    // so between two joiners that's half of each of their round trips, the two slowest; with one joiner, half of its
    // round trip.
    int delay_needed(std::vector<int> round_trips, double spare) {
        std::sort(round_trips.rbegin(), round_trips.rend());
        double one_way = 0;
        for (size_t i = 0; i < round_trips.size() && i < 2; i++) {
            one_way += round_trips[i] / 2.0;
        }
        return reads_for(one_way, spare);
    }

    // The reads a machine's inputs arriving `spread_ms` unevenly need on top (what a read's timing varies anyway aside).
    int uneven_reads(double spread_ms) {
        const double uneven = spread_ms - free_spread_ms;
        return uneven <= 0 ? 0 : (int)std::ceil(uneven / frame_ms);
    }

    // With rollback: a machine's delay for its one-way trip (rollback_trip_checks's comment), and all its inputs'
    // unevenness needs on top: that's its own link's, which the others would otherwise correct for (theirs are even).
    int rollback_wanted(double one_way_ms, double spread_ms) {
        const int needed = reads_for(one_way_ms, spare_ms);
        const int for_trip = needed == 0 ? 0 : std::max(1, needed - rollback_cover);
        return std::min(for_trip + uneven_reads(spread_ms), conker::netplay::max_delay);
    }

    std::string trips_text(const std::vector<int>& round_trips) {
        std::string list;
        for (int t : round_trips) list += (list.empty() ? "" : ", ") + std::to_string(t);
        return list.empty() ? std::string("unknown") : list;
    }

    // The host: the machines that play, each its ports (its own first; a joiner's still in, by its first port).
    std::vector<uint8_t> playing_machines() {
        std::vector<uint8_t> machines;
        uint8_t seen = 0;
        for (int port = 0; port < match_players; port++) {
            if ((seen & (1u << port)) != 0 || idle_from[port] != never) continue;
            const uint8_t ports = (own_ports & (1u << port)) != 0 ? own_ports : session->machine_of(port);
            if (ports == 0) continue;
            seen |= ports;
            machines.push_back(ports);
        }
        return machines;
    }

    // With rollback, a machine's one-way trip (ms): half the round trip to the host (a joiner's), or to the nearest joiner
    // (the host's own). Its inputs need to reach the host, which passes them on; the others' predicting covers the rest,
    // so one player far away doesn't make everyone's delay theirs. -1 if not known.
    double machine_one_way(uint8_t machine, const std::array<int, conker::netplay::max_players>& trips,
        const std::vector<uint8_t>& machines) {
        if ((machine & own_ports) == 0) {
            const int trip = trips[std::countr_zero(machine)];
            return trip < 0 ? -1 : trip / 2.0;
        }
        int nearest = -1;
        bool others = false;
        for (const uint8_t other : machines) {
            if (other == machine) continue;
            others = true;
            const int trip = trips[std::countr_zero(other)];
            if (trip >= 0) nearest = nearest < 0 ? trip : std::min(nearest, trip);
        }
        return !others ? 0 : nearest < 0 ? -1 : nearest / 2.0;
    }

    // How unevenly `port`'s inputs reach this machine (ms: spread_samples's comment), -1 if not known yet.
    double own_spread_ms(int port) {
        const std::deque<float>& samples = lateness[port];
        if (samples.size() < spread_least_samples) {
            return -1;
        }
        std::vector<float> sorted(samples.begin(), samples.end());
        auto at = [&](double fraction) {
            const auto nth = sorted.begin() + (ptrdiff_t)(fraction * (double)(sorted.size() - 1));
            std::nth_element(sorted.begin(), nth, sorted.end());
            return *nth;
        };
        const float median = at(0.5);
        return std::max(0.0f, at(0.9) - median);
    }

    // The host: how unevenly a machine's inputs reach the others (ms): for each of its ports, as the receiver that has them
    // the most evenly says (this machine, and each other's latest word), the most of its ports. 0 if nobody knows yet.
    double machine_spread(uint8_t machine, const std::vector<uint8_t>& machines) {
        double most = 0;
        const double t = now();
        for (int port = 0; port < match_players; port++) {
            if ((machine & (1u << port)) == 0) continue;
            double least = -1;
            auto take = [&](double ms) {
                if (ms >= 0) least = least < 0 ? ms : std::min(least, ms);
            };
            if ((machine & own_ports) == 0) take(own_spread_ms(port));
            for (const uint8_t other : machines) {
                if (other == machine || (other & own_ports) != 0) continue;
                const Report& report = reports[std::countr_zero(other)];
                if (report.heard && t - report.at < report_fresh_seconds) take(report.spread_ms[port]);
            }
            most = std::max(most, least);
        }
        return most;
    }

    // The host, at a check: the reads each machine (by its first port) made the others wait for its inputs since the last
    // (`long_waits`: those that waited over long_wait_ms): for each of its ports, the fewest any receiver waited (this
    // machine's own count, and each other's latest word: a receiver with a bad link waits for everyone), the most of its ports.
    std::array<uint64_t, conker::netplay::max_players> machines_waited(const std::vector<uint8_t>& machines, bool long_waits) {
        std::array<std::array<int64_t, conker::netplay::max_players>, conker::netplay::max_players> counted;
        for (auto& by_port : counted) by_port.fill(-1);
        for (const uint8_t receiver : machines) {
            const int r = std::countr_zero(receiver);
            for (int port = 0; port < match_players; port++) {
                if ((receiver & own_ports) != 0) {
                    uint64_t& seen = long_waits ? own_waited_long_counted[port] : own_waited_counted[port];
                    const uint64_t total = long_waits ? waited_long_for_port[port] : waited_for_port[port];
                    counted[r][port] = (int64_t)(total - seen);
                    seen = total;
                }
                else if (reports[r].heard) {
                    Report& report = reports[r];
                    uint8_t& seen = long_waits ? report.waited_long_counted[port] : report.waited_counted[port];
                    const uint8_t total = long_waits ? report.waited_long[port] : report.waited[port];
                    counted[r][port] = (uint8_t)(total - seen);
                    seen = total;
                }
            }
        }
        std::array<uint64_t, conker::netplay::max_players> waited{};
        for (const uint8_t machine : machines) {
            uint64_t most = 0;
            for (int port = 0; port < match_players; port++) {
                if ((machine & (1u << port)) == 0) continue;
                int64_t least = -1;
                for (const uint8_t receiver : machines) {
                    const int64_t n = counted[std::countr_zero(receiver)][port];
                    if (receiver != machine && n >= 0) least = least < 0 ? n : std::min(least, n);
                }
                most = std::max<uint64_t>(most, least < 0 ? 0 : (uint64_t)least);
            }
            waited[std::countr_zero(machine)] = most;
        }
        return waited;
    }

    // The host: each port's delay as the match starts (auto: each machine's from the round trips; nothing has come yet to
    // say how evenly its inputs arrive). The ports nobody has get the host's.
    std::array<uint8_t, conker::netplay::max_players> start_delays() {
        std::array<uint8_t, conker::netplay::max_players> chosen{};
        if (delay_option != auto_delay) {
            chosen.fill((uint8_t)delay_option);
            return chosen;
        }
        const std::vector<int> all = session->round_trips_ms();
        // A round trip the library doesn't know yet: the delay a match had before it was chosen.
        if (all.size() < (size_t)session->joiners()) {
            const int fallback = rollback_option > 0 ? 1 : 2;
            std::printf("[netplay] the round trips to the players aren't all known: input delay %d\n", fallback);
            chosen.fill((uint8_t)fallback);
            return chosen;
        }
        const std::array<int, conker::netplay::max_players> trips = session->round_trips_by_port();
        const std::vector<uint8_t> machines = playing_machines();
        int host_delay = 0;
        for (const uint8_t machine : machines) {
            const double one_way = machine_one_way(machine, trips, machines);
            const int d = rollback_option == 0 ? delay_needed(all, spare_ms) : one_way < 0 ? 1 : rollback_wanted(one_way, 0);
            for (int port = 0; port < conker::netplay::max_players; port++) {
                if ((machine & (1u << port)) != 0) chosen[port] = (uint8_t)d;
            }
            if ((machine & own_ports) != 0) host_delay = d;
        }
        std::string by_port;
        for (int port = 0; port < match_players; port++) {
            if (session->machine_of(port) == 0 && (own_ports & (1u << port)) == 0) chosen[port] = (uint8_t)host_delay;
            by_port += (port == 0 ? "" : ", ") + std::to_string(chosen[port]);
        }
        std::printf("[netplay] round trips to the players: %s ms: input delays by port %s\n", trips_text(all).c_str(), by_port.c_str());
        return chosen;
    }

    // The host, with auto, at read `read` (every hash_every reads): changes a machine's delay if its connection has
    // wanted another for long enough. From the read after the furthest a joiner can send its pads at before it hears:
    // a joiner is at most the delay ahead (it needs the host's pads, sent the delay ahead), and can make the read after
    // its last of the host's pads before the Delay (sent after them) arrives; with rollback (the match's, the same for
    // everyone), a joiner predicts up to its window further.
    void adapt_delay(uint64_t read) {
        const std::vector<uint8_t> machines = playing_machines();
        const std::array<uint64_t, conker::netplay::max_players> waited = machines_waited(machines, false);
        const std::array<uint64_t, conker::netplay::max_players> waited_long = machines_waited(machines, true);
        const std::array<int, conker::netplay::max_players> trips = session->round_trips_by_port();
        // (Of the machines in the match only: one let go of is still connected, its trip no one's concern.)
        std::vector<int> all;
        bool all_known = true;
        for (const uint8_t machine : machines) {
            if ((machine & own_ports) != 0) continue;
            const int trip = trips[std::countr_zero(machine)];
            if (trip < 0) all_known = false;
            else all.push_back(trip);
        }
        if (machines.size() < 2 || all.empty() || !all_known) {
            for (MachineDelay& m : machine_delays) {
                m.wanted_higher = 0;
                m.wanted_lower = 0;
            }
            return;
        }
        const uint64_t from = read + (uint64_t)most_delay() + 2 + (uint64_t)rollback_option;
        for (const uint8_t machine : machines) {
            const int first = std::countr_zero(machine);
            MachineDelay& m = machine_delays[first];
            const int now_delay = delays[first].after;
            const uint64_t late = waited[first];
            const uint64_t long_waits = waited_long[first];
            // (A machine's ports go by one delay: a new player on two ports with different ones gets its first's.)
            bool uneven_ports = false;
            for (int port = 0; port < match_players; port++) {
                if ((machine & (1u << port)) != 0 && (delays[port].after != now_delay || read < delays[port].from)) uneven_ports = true;
            }
            if (uneven_ports && read >= delays[first].from) {
                bool settled = true;
                for (int port = 0; port < match_players; port++) {
                    if ((machine & (1u << port)) != 0 && read < delays[port].from + settle_reads) settled = false;
                }
                if (settled) {
                    change_delay(machine, now_delay, from);
                    session->send(Delay{ machine, (uint8_t)now_delay, (uint32_t)from });
                }
                continue;
            }
            const double spread = machine_spread(machine, machines);
            int to = now_delay;
            if (rollback_option > 0) {
                const double one_way = machine_one_way(machine, trips, machines);
                if (one_way >= 0) {
                    m.wanted.push_back(rollback_wanted(one_way, spread));
                    if (m.wanted.size() > rollback_trip_checks) m.wanted.pop_front();
                }
                m.late.push_back(late);
                if (m.late.size() > rollback_late_checks) m.late.pop_front();
                m.calm = late == 0 ? m.calm + 1 : 0;
                if (read < delays[first].from + rollback_settle_reads || m.wanted.empty()) {
                    continue;
                }
                std::vector<int> sorted(m.wanted.begin(), m.wanted.end());
                std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)(sorted.size() / 2), sorted.end());
                const int median = sorted[sorted.size() / 2];
                uint64_t late_lately = 0;
                for (const uint64_t n : m.late) late_lately += n;
                if (late_lately >= rollback_late_to_raise && m.raised_for_waiting < most_raised_for_waiting &&
                    now_delay < conker::netplay::max_delay) {
                    to = now_delay + 1;
                    m.raised_for_waiting++;
                }
                else if (median + m.raised_for_waiting > now_delay && now_delay < conker::netplay::max_delay) {
                    to = now_delay + 1;
                }
                else if (m.calm >= rollback_calm_checks && (m.raised_for_waiting > 0 || median < now_delay)) {
                    if (m.raised_for_waiting > 0) m.raised_for_waiting--;
                    if (median + m.raised_for_waiting < now_delay) to = now_delay - 1;
                    m.calm = 0;
                }
                if (to == now_delay) {
                    continue;
                }
                std::printf("[netplay] %s: a one-way trip of %.0f ms, inputs %.0f ms uneven: %d reads wanted at the median of %zu checks; "
                    "%llu reads waited for them in the last %zu\n", ports_text(machine).c_str(), one_way, spread, median, m.wanted.size(),
                    (unsigned long long)late_lately, m.late.size());
                m.late.clear();
                m.calm = 0;
            }
            else {
                if (read < delays[first].from + settle_reads) {
                    m.wanted_higher = 0;
                    m.wanted_lower = 0;
                    continue;
                }
                const int uneven = uneven_reads(spread);
                const int needed = std::min(delay_needed(all, spare_ms) + uneven, conker::netplay::max_delay);
                const int roomy = std::min(delay_needed(all, spare_ms + lower_spare_ms) + uneven, conker::netplay::max_delay);
                const bool waiting = (late >= late_reads_to_raise || long_waits >= long_waits_to_raise) &&
                    now_delay < conker::netplay::max_delay && m.raised_for_waiting < most_raised_for_waiting;
                m.wanted_higher = needed > now_delay || waiting ? m.wanted_higher + 1 : 0;
                m.wanted_lower = roomy < now_delay && late == 0 && long_waits < long_waits_to_raise ? m.wanted_lower + 1 : 0;
                if (m.wanted_higher < raise_checks && m.wanted_lower < lower_checks) {
                    continue;
                }
                to = roomy;
                if (m.wanted_higher >= raise_checks && needed > now_delay) {
                    to = needed;
                    m.raised_for_waiting = 0;
                }
                else if (m.wanted_higher >= raise_checks) {
                    to = now_delay + 1;
                    m.raised_for_waiting++;
                }
                else if (m.raised_for_waiting > 0) {
                    to = now_delay - 1;
                    m.raised_for_waiting--;
                }
                m.wanted_higher = 0;
                m.wanted_lower = 0;
                if (to == now_delay) {
                    continue;
                }
                std::printf("[netplay] round trips to the players: %s ms; %s's inputs %.0f ms uneven; %llu reads waited for them since "
                    "the last check, %llu over %.0f ms\n", trips_text(all).c_str(), ports_text(machine).c_str(), spread,
                    (unsigned long long)late, (unsigned long long)long_waits, long_wait_ms);
            }
            change_delay(machine, to, from);
            session->send(Delay{ machine, (uint8_t)to, (uint32_t)from });
        }
    }

    // The read a peer is at whose pads for `port` reach `last`: the earliest it can be, as the port's delay changes.
    int64_t read_sending(int port, int64_t last) {
        const DelayChange& d = delays[port];
        const int64_t from = (int64_t)d.from;
        if (last <= from - 1 + d.before) return last - d.before;
        if (last >= from + d.after) return last - d.after;
        return from - 1;
    }

    // An input of `port`'s for `read` came (the first time): how late, by when this machine made the read it was sent at
    // (spread_samples's comment). One sent at a read this machine hasn't made yet (it's behind) is timed once it has.
    void add_lateness(int port, double ms) {
        std::deque<float>& recent = recent_lateness[port];
        recent.push_back((float)ms);
        if (recent.size() > baseline_samples) recent.pop_front();
        std::vector<float> sorted(recent.begin(), recent.end());
        std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)(sorted.size() / 2), sorted.end());
        lateness[port].push_back((float)ms - sorted[sorted.size() / 2]);
        if (lateness[port].size() > spread_samples) lateness[port].pop_front();
    }
    void note_arrival(int port, uint64_t read, double arrived) {
        if (watching || catching_up || rejoining || (own_ports & (1u << port)) != 0) {
            return;
        }
        const int64_t sent_at = read_sending(port, (int64_t)read);
        if (sent_at < (int64_t)rollback_from_read) {
            return;
        }
        const uint64_t at = (uint64_t)sent_at;
        const size_t slot = at % read_times_kept;
        // (When the library got it: the game thread only polls at its reads, so `now` would be up to a read late.)
        const double came = arrived > 0 ? arrived : now();
        if (at <= current_read && read_made_number[slot] == at) {
            add_lateness(port, (came - read_made_at[slot]) * 1000);
        }
        else if (at > current_read && early_inputs[port].size() < 64) {
            early_inputs[port].push_back({ at, came });
        }
    }
    // ...and this machine makes read `read` the first time.
    void note_read_made(uint64_t read) {
        const size_t slot = read % read_times_kept;
        read_made_at[slot] = now();
        read_made_number[slot] = read;
        for (int port = 0; port < conker::netplay::max_players; port++) {
            std::deque<std::pair<uint64_t, double>>& early = early_inputs[port];
            while (!early.empty() && early.front().first <= read) {
                const size_t at = early.front().first % read_times_kept;
                if (read_made_number[at] == early.front().first && !catching_up) {
                    add_lateness(port, (early.front().second - read_made_at[at]) * 1000);
                }
                early.pop_front();
            }
        }
    }

    int gap_to(int port) {
        if ((own_ports & (1u << port)) != 0 || idle_from[port] != never || received[port].empty() || only_idle_since_back(port)) {
            return 0;
        }
        return (int)((int64_t)current_read - read_sending(port, (int64_t)received[port].size() - 1));
    }

    // Time sync, at a read made for the first time, once its inputs are in: this machine's gaps (sent with its next
    // inputs), and with rollback, how far ahead of the others it is, and the game paced a little later while
    // it's too far.
    void sync_time() {
        double lead = -1e9;
        int lead_port = -1;
        const bool hosting = session->hosting();
        for (double& us : slowed_for) us -= us / time_sync_budget_reads;
        for (int port = 0; port < match_players; port++) {
            time_sync.my_gap[port] = gap_to(port);
            // The host: a port it has slowed everyone too much for lately is left to catch up on its own, for now.
            const uint8_t bit = (uint8_t)(1u << port);
            if (hosting && rollback_option > 0) {
                if ((time_sync_skipped & bit) == 0 && slowed_for[port] > time_sync_budget_us) {
                    time_sync_skipped |= bit;
                    std::printf("[netplay] read %llu: time sync goes by %s no more for now (everyone slowed %.0f ms for it lately): it "
                        "catches up on its own\n", (unsigned long long)current_read, ports_text(bit).c_str(), slowed_for[port] / 1000);
                }
                else if ((time_sync_skipped & bit) != 0 && slowed_for[port] < time_sync_budget_us / 2) {
                    time_sync_skipped &= (uint8_t)~bit;
                    std::printf("[netplay] read %llu: time sync goes by %s again\n", (unsigned long long)current_read, ports_text(bit).c_str());
                }
            }
            // (Not in the boot's first reads, which wait for everyone, and whose first inputs carry no gaps yet.)
            if ((hosting || port == 0) && current_read >= rollback_from_read && (own_ports & bit) == 0 &&
                idle_from[port] == never && time_sync.heard[port] && !received[port].empty() && (time_sync_skipped & bit) == 0) {
                const double port_lead = (time_sync.my_gap[port] - time_sync.their_gap[port]) / 2.0;
                if (port_lead > lead) {
                    lead = port_lead;
                    lead_port = port;
                }
            }
        }
        if (rollback_option == 0 || lead == -1e9) {
            return;
        }
        time_sync.lead += (lead - time_sync.lead) / time_sync_average_reads;
        time_sync.most_lead = std::max(time_sync.most_lead, time_sync.lead);
        if (catching_up || time_sync.lead <= time_sync_from) {
            return;
        }
        const uint32_t us = std::min(time_sync_most_us, (uint32_t)((time_sync.lead - time_sync_from) * time_sync_us_per_read));
        ultramodern::timeline::pace_later(us);
        time_sync.reads_slowed++;
        time_sync.us_later += us;
        if (hosting && lead_port >= 0) slowed_for[lead_port] += us;
    }

    bool same_input(const Input& a, const Input& b) {
        return a.pad == b.pad && a.motion == b.motion;
    }

    // Whether `port`'s input for `read` is known (it came, or they'd left by then), and what it is.
    bool known(int port, uint64_t read) {
        return read >= idle_from[port] || read < received[port].size();
    }
    Input known_input(int port, uint64_t read) {
        return read < idle_from[port] ? received[port][read] : idle_input(port);
    }

    // The input the game is given for `port` at `read`: as it came, or else predicted (their last).
    Input input_for(int port, uint64_t read) {
        if (known(port, read)) return known_input(port, read);
        return received[port].empty() ? idle_input(port) : received[port].back();
    }

    // Rollback, at read `read` (the first time it's made): the earliest read the game was given another input
    // than has come since, if any (the harness takes it: take_rollback).
    void find_mispredictions(uint64_t read) {
        for (int port = 0; port < match_players; port++) {
            if ((own_ports & (1u << port)) != 0) continue;
            const uint64_t until = std::min<uint64_t>(read, idle_from[port] != never ? read : received[port].size());
            for (uint64_t r = checked[port]; r < until; r++) {
                if (!same_input(given[r][port], known_input(port, r))) {
                    if (rollback_to == never || r < rollback_to) {
                        rollback_to = r;
                    }
                    break;
                }
            }
            checked[port] = std::max(checked[port], until);
        }
    }

    // Rollback: the reads before `read` known and given as they were, and the hashes of reads now known to be
    // right sent (and kept to compare).
    void verify(uint64_t read) {
        while (verified < read && verified < given.size()) {
            bool right = true;
            for (int port = 0; port < match_players && right; port++) {
                right = known(port, verified) && same_input(given[verified][port], known_input(port, verified));
            }
            if (!right) break;
            verified++;
        }
        for (auto it = unverified_hashes.begin(); it != unverified_hashes.end() && it->first <= verified && it->first <= read;) {
            send_hash(it->first, it->second);
            it = unverified_hashes.erase(it);
        }
    }

    // Puts every player's input for `read` in `pads` and `motions` (4 each): as it came, or predicted (rollback,
    // which keeps what the game was given and sends the hashes now known to be right).
    bool give_inputs(uint64_t read, ultramodern::input::Pad* pads, Motion* motions) {
        for (int port = 0; port < match_players; port++) {
            const Input input = input_for(port, read);
            pads[port] = { input.pad.buttons, input.pad.stick_x, input.pad.stick_y, input.pad.err_no };
            motions[port] = input.motion;
            if (rollback_option > 0) given[read][port] = input;
        }
        if (rollback_option > 0) verify(read);
        return true;
    }

    // --- Desync recovery ---

    // The host, at a read made the first time: a Resync for the ports found to differ, from a read no joiner can
    // have made (as a Delay's: a joiner is at most the delay ahead, and with rollback its window further).
    // A Resync whose state can't be taken is announced again this many times at most, then the match ends.
    constexpr int most_announcements = 10;

    // The host, announcing a state: the spectators waiting for one get it too (again: those already to get it stay), and
    // those watching whose game went another way are told to load it before its read (recovery.read).
    void take_spectators(bool again) {
        if (!again) {
            recovery.spectators = 0;
            recovery.spectators_resync = 0;
        }
        recovery.spectators |= (uint8_t)(spectators_late_wanted | spectators_resync_wanted);
        if (recovery.spectators != 0) spectator_state_at = now();
        recovery.spectators_resync |= spectators_resync_wanted;
        spectators_late_wanted = 0;
        spectators_resync_wanted = 0;
        for (int n = 0; n < conker::netplay::max_spectators; n++) {
            if ((recovery.spectators & (1u << n)) != 0) spectator_last_read[n] = recovery.read;
        }
        if (recovery.spectators != 0) {
            std::printf("[netplay] spectators %u (bits) will load this game as it is before read %llu\n", recovery.spectators,
                (unsigned long long)recovery.read);
        }
        if (recovery.spectators_resync != 0) {
            session->resync_spectators(recovery.spectators_resync, (uint32_t)recovery.read);
        }
    }

    void announce_resync(uint64_t read, uint8_t ports, bool again = false) {
        recovery.announcements = again ? recovery.announcements + 1 : 1;
        if (recovery.announcements > most_announcements) {
            end_match(Reason::desync, "the games went different ways (a desync), and the host's game couldn't be taken to set it right");
            return;
        }
        // The others wait for that machine meanwhile (longer than for a player who stopped answering).
        recovery.sent_at = now();
        recovery.read = read + (uint64_t)most_delay() + 2 + (uint64_t)rollback_option;
        recovery.ports = ports;
        recovery.rejoin = false;
        recovery.state.clear();
        for (int port = 0; port < match_players; port++) {
            if ((ports & (1u << port)) != 0) recovery.last_read[port] = recovery.read;
        }
        std::printf("[netplay] read %llu: %s will load this game as it is before read %llu (desync recovery)\n",
            (unsigned long long)read, ports_text(ports).c_str(), (unsigned long long)recovery.read);
        session->send(Resync{ (uint32_t)recovery.read, ports });
        take_spectators(again);
    }

    // --- Coming back ---

    // `ports` play again from read `from`: idle pads till then (as they've had since they left; or, the host's Back
    // again, since the last), and their own inputs from it. Every peer does the same, as the host says (Back).
    void play_again(uint8_t ports, uint64_t from) {
        for (int port = 0; port < match_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            const Input idle = idle_input(port);
            received[port].resize(from, idle);
            idle_from[port] = never;
            back_at[port] = from;
            gone[port] = false;
            time_sync.heard[port] = false;
            // (A connection of its own now: how its inputs come, and its delay's history, start afresh.)
            lateness[port].clear();
            recent_lateness[port].clear();
            early_inputs[port].clear();
            machine_delays[port] = {};
            reports[port] = {};
            slowed_for[port] = 0;
            time_sync_skipped &= (uint8_t)~(1u << port);
        }
        open_ports &= (uint8_t)~ports;
        lost_ports &= (uint8_t)~ports;
        std::lock_guard lock(status_mutex);
        shown.lost &= (uint8_t)~ports;
        shown.left &= (uint8_t)~ports;
        shown.left_silently &= (uint8_t)~ports;
        shown.open = open_ports;
    }

    // The match starts with `ports` open: nobody's, idle from read 0.
    void open_from_start(uint8_t ports) {
        open_ports = ports;
        for (int port = 0; port < match_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            idle_from[port] = 0;
            gone[port] = true;
        }
        std::lock_guard lock(status_mutex);
        shown.open = open_ports;
    }

    // Whether `port` can be told to play again from `from`: it's gone (its last input passed on), or it was told so
    // already and none of its own has come; and `from` is past both.
    bool can_play_again(int port, uint64_t from) {
        const uint64_t have = received[port].size();
        return port > 0 && port < match_players && (own_ports & (1u << port)) == 0 &&
            (idle_from[port] != never ? have == idle_from[port] : back_at[port] == have) && from > have;
    }

    // The host, at a read made the first time: the players welcomed back on `ports` play again from a read no input
    // can have come for yet (past the furthest any can reach, horizon), so none of theirs is missed and everyone's
    // from it can go to them; the others are told (Back). The state the host takes before it goes to them, with the
    // match then (send_resync_state). Again (`again`) if the state couldn't be taken before the last one.
    void announce_back(uint64_t read, uint8_t ports, bool again = false, uint8_t late = 0) {
        recovery.announcements = again ? recovery.announcements + 1 : 1;
        const uint64_t from = horizon() + 1;
        if (recovery.announcements > most_announcements) {
            // Not for everyone's match: they're sent away (kicked), their ports idle again; spectators too.
            std::printf("[netplay] read %llu: this game couldn't be taken for %s: they're left out\n", (unsigned long long)read,
                ports != 0 ? ports_text(ports).c_str() : "the spectators");
            for (int port = 0; port < match_players; port++) {
                if ((ports & (1u << port)) != 0) session->kick(port);
            }
            for (int n = 0; n < conker::netplay::max_spectators; n++) {
                if ((recovery.spectators & (1u << n)) != 0) session->kick_spectator(n, 0);
            }
            recovery.read = never;
            recovery.state.clear();
            return;
        }
        recovery.sent_at = now();
        recovery.read = from;
        recovery.ports = ports;
        recovery.rejoin = true;
        recovery.late = late;
        recovery.state.clear();
        for (int port = 0; port < match_players; port++) {
            if ((ports & (1u << port)) != 0) recovery.last_read[port] = from;
        }
        if (ports != 0) {
            play_again(ports, from);
            std::printf("[netplay] read %llu: %s to play %s from read %llu, loading this game as it is before it\n",
                (unsigned long long)read, ports_text(ports).c_str(), late == ports ? "in the match" : "again", (unsigned long long)from);
            session->back(ports, (uint32_t)from, late);
        }
        take_spectators(again);
    }

    // A joiner: the host's word that `b.ports` play again from `b.read`.
    bool take_back(const Back& b) {
        // (Past any read this machine has made: the host chose it past every input's; and not so far past that it's
        // a broken or hostile host's, filling memory.)
        if (b.read <= current_read || (rollback_option > 0 && b.read < frontier) ||
            b.read > horizon() + conker::netplay::max_delay + conker::netplay::max_rollback + 1) {
            return false;
        }
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((b.ports & (1u << port)) != 0 && !can_play_again(port, b.read)) return false;
        }
        // (A spectator taking a port: no state comes, nobody waits but for its inputs.)
        if (b.player == 0) recovery.sent_at = now();
        const uint8_t was_lost = (uint8_t)(lost_ports & b.ports);
        play_again(b.ports, b.read);
        if (b.player != 0) {
            const bool mine = watching && b.player == player_option;
            std::printf("[netplay] %s: %s takes it from read %u\n", ports_text(b.ports).c_str(), mine ? "this machine" : "a spectator", b.read);
            if (mine) {
                own_ports = b.ports;
                watching = false;
                seated_from = b.read;
                watched = 0;
                session->take_seat(b.ports);
                for (int port = 0; port < conker::netplay::max_players; port++) {
                    if ((b.ports & (1u << port)) != 0) checked[port] = std::max<uint64_t>(checked[port], b.read);
                }
                tell("You're " + players_text(b.ports) + " now.");
            }
            else {
                tell(conker::netplay::player_name(b.player) + " takes " + players_text(b.ports) + "'s place.");
            }
            return true;
        }
        const uint8_t back_ports = (uint8_t)(b.ports & ~b.late);
        if (b.late != 0) {
            std::printf("[netplay] %s joined: they play from read %u\n", ports_text(b.late).c_str(), b.read);
            tell(players_text(b.late) + " joined: they're getting the host's game...");
        }
        if (back_ports != 0) {
            std::printf("[netplay] %s came back: they play again from read %u\n", ports_text(back_ports).c_str(), b.read);
            tell(players_text(back_ports) + ((back_ports & was_lost) == back_ports ? " is back" : " came back") +
                ": they're getting the host's game...");
        }
        return true;
    }

    // Coming back: the match as it is at the read this machine plays from, before the host's game (its StateParts)
    // and the inputs since. Every input before it is of no use here but the last (what's predicted from, and an
    // idle player's settings).
    bool take_rejoin(const Rejoin& r) {
        // (The host is at the read or a little past it, predicting this machine's inputs at most: no further, or a
        // broken or hostile host could have it keep inputs without end.)
        if (!rejoining || recovery.read != never || r.read == 0 || r.read > most_rejoin_read ||
            r.at > (uint64_t)r.read + conker::netplay::max_rollback + 1) {
            return false;
        }
        for (int port = 0; port < match_players; port++) {
            const bool idle = r.idle_from[port] != conker::netplay::no_read;
            if (idle && (own_ports & (1u << port)) != 0) return false;
            received[port].assign(r.read, Input{});
            received[port].back() = { r.pads[port], r.motions[port] };
            // (An idle port's inputs up to the read it's idle from, if past this one, come next, with the others'.)
            idle_from[port] = idle ? r.idle_from[port] : never;
            gone[port] = idle;
            back_at[port] = never;
        }
        open_ports = r.open;
        for (int port = 0; port < conker::netplay::max_players; port++) {
            delays[port] = { r.delay_from[port], r.delay_before[port], r.delay_after[port] };
        }
        current_read = r.at;
        delay = own_delay_at(r.read);
        session->set_horizon(horizon());
        recovery.read = r.read;
        recovery.ports = own_ports;
        recovery.rejoin = true;
        recovery.sent_at = now();
        recovery.parts.clear();
        recovery.size = 0;
        recovery.ready = false;
        // (Who's in, as the match is now: coming back mid-match, what was shown before is out of date.)
        lost_ports = 0;
        step_out_wanted = false;
        wide_gap_for = 0;
        {
            std::lock_guard lock(status_mutex);
            shown.lost = 0;
            shown.left = 0;
            shown.left_silently = 0;
            for (int port = 0; port < match_players; port++) {
                if (r.idle_from[port] != conker::netplay::no_read && (r.open & (1u << port)) == 0) shown.left |= (uint8_t)(1u << port);
            }
            shown.open = open_ports;
        }
        std::printf("[netplay] coming back from read %u (the host is at read %u): getting the host's game\n", r.read, r.at);
        return true;
    }

    // The host's game before `read` (the runtime's state, then the host's own) as it's sent: the two, each after its
    // size (u32), compressed.
    bool pack_state(const std::vector<uint8_t>& runtime, const std::vector<uint8_t>& host, std::vector<uint8_t>& out, uint64_t& hash) {
        std::vector<uint8_t> whole;
        whole.reserve(runtime.size() + host.size() + 8);
        for (const std::vector<uint8_t>* part : { &runtime, &host }) {
            const uint32_t size = (uint32_t)part->size();
            for (int i = 0; i < 4; i++) whole.push_back((uint8_t)(size >> (8 * i)));
            whole.insert(whole.end(), part->begin(), part->end());
        }
        hash = XXH3_64bits(whole.data(), whole.size());
        out.resize(ZSTD_compressBound(whole.size()));
        const size_t size = ZSTD_compress(out.data(), out.size(), whole.data(), whole.size(), 1);
        if (ZSTD_isError(size) || size > conker::netplay::max_state_size) {
            out.clear();
            return false;
        }
        out.resize(size);
        return true;
    }

    // The joiner's: the state that came, decompressed and checked, into recovery.runtime and .host. False, with why,
    // if it isn't one this machine can load.
    bool unpack_state(std::string& error) {
        const unsigned long long size = ZSTD_getFrameContentSize(recovery.parts.data(), recovery.parts.size());
        if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR || size > most_state_bytes + most_host_state_bytes + 8) {
            error = "its size isn't a state's";
            return false;
        }
        std::vector<uint8_t> whole((size_t)size);
        const size_t got = ZSTD_decompress(whole.data(), whole.size(), recovery.parts.data(), recovery.parts.size());
        if (ZSTD_isError(got) || got != whole.size()) {
            error = "it doesn't decompress";
            return false;
        }
        if (XXH3_64bits(whole.data(), whole.size()) != recovery.part_hash) {
            error = "its hash isn't the one the host sent";
            return false;
        }
        size_t at = 0;
        auto take = [&](std::vector<uint8_t>& out, size_t most) {
            if (whole.size() - at < 4) return false;
            const uint32_t length = (uint32_t)whole[at] | ((uint32_t)whole[at + 1] << 8) | ((uint32_t)whole[at + 2] << 16) |
                ((uint32_t)whole[at + 3] << 24);
            at += 4;
            if (length > most || length > whole.size() - at) return false;
            out.assign(whole.begin() + at, whole.begin() + at + length);
            at += length;
            return true;
        };
        if (!take(recovery.runtime, most_state_bytes) || !take(recovery.host, most_host_state_bytes) || at != whole.size()) {
            error = "it isn't laid out as a state";
            return false;
        }
        return recomp::save_states::check_untrusted(recovery.runtime, error);
    }

    // The host, at read `read` (past the one the players coming back play again from, every input before it known and
    // given as it was): brings them into the match (Rejoin), with every input since that read it has; from then on
    // they get what everyone does. Their state comes next.
    void welcome_back(uint64_t read) {
        const uint64_t from = recovery.read;
        Rejoin rejoin;
        rejoin.read = (uint32_t)from;
        rejoin.at = (uint32_t)read;
        for (int port = 0; port < conker::netplay::max_players; port++) {
            rejoin.delay_from[port] = (uint32_t)std::min<uint64_t>(delays[port].from, UINT32_MAX);
            rejoin.delay_before[port] = (uint8_t)delays[port].before;
            rejoin.delay_after[port] = (uint8_t)delays[port].after;
        }
        std::vector<Inputs> since;
        std::array<uint32_t, conker::netplay::max_players> inputs_from{};
        for (int port = 0; port < match_players; port++) {
            const Input last = known_input(port, from - 1);
            rejoin.pads[port] = last.pad;
            rejoin.motions[port] = last.motion;
            if (idle_from[port] != never) {
                rejoin.idle_from[port] = (uint32_t)std::max(idle_from[port], from);
                if ((open_ports & (1u << port)) != 0) rejoin.open |= (uint8_t)(1u << port);
            }
            inputs_from[port] = (uint32_t)received[port].size();
            // (Theirs, from the read: none yet.)
            for (uint64_t r = from; r < received[port].size(); r += conker::netplay::input_window) {
                Inputs inputs;
                inputs.port = (uint8_t)port;
                inputs.first_read = (uint32_t)r;
                inputs.count = (uint8_t)std::min<uint64_t>(conker::netplay::input_window, received[port].size() - r);
                for (int i = 0; i < inputs.count; i++) {
                    inputs.pads[i] = received[port][r + i].pad;
                    inputs.motions[i] = received[port][r + i].motion;
                }
                since.push_back(inputs);
            }
        }
        const uint8_t spectators = (uint8_t)(recovery.spectators & ~recovery.spectators_resync);
        session->welcome_back(recovery.rejoin ? recovery.ports : 0, spectators, rejoin, since, inputs_from);
        std::printf("[netplay] read %llu: %s%s in the match from read %llu (%zu inputs since sent)\n", (unsigned long long)read,
            recovery.rejoin && recovery.ports != 0 ? (ports_text(recovery.ports) + " back").c_str() : "",
            spectators == 0 ? "" : ((recovery.rejoin && recovery.ports != 0 ? ", spectators " : "spectators ") + std::to_string(spectators) +
            " (bits)").c_str(), (unsigned long long)from, since.size());
    }

    // The host, at read `read` (made the first time, as it waits for the others): the state for the Resync under way,
    // once it's taken and right (with rollback, every input before its read known and given as it was).
    void send_resync_state(uint64_t read) {
        if (!session->hosting() || recovery.read == never || read < recovery.read || recovery.state.empty()) {
            return;
        }
        if (rollback_option > 0) {
            verify(read);
            if (verified < recovery.read) return;
        }
        // (Players coming back, and spectators coming in, first get the match as it is then.)
        if ((recovery.rejoin && recovery.ports != 0) || (recovery.spectators & ~recovery.spectators_resync) != 0) {
            welcome_back(read);
        }
        session->send_state(recovery.ports, recovery.spectators, (uint32_t)recovery.read, recovery.state, recovery.hash);
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((recovery.ports & (1u << port)) != 0) state_read_for[port] = recovery.read;
        }
        std::printf("[netplay] read %llu: this game before read %llu sent to %s%s (%zu bytes)\n", (unsigned long long)read,
            (unsigned long long)recovery.read, recovery.ports != 0 ? ports_text(recovery.ports).c_str() : "",
            recovery.spectators == 0 ? "" : ((recovery.ports != 0 ? " and spectators " : "spectators ") +
            std::to_string(recovery.spectators) + " (bits)").c_str(), recovery.state.size());
        recovery.read = never;
        recovery.state.clear();
        recovery.sent_at = now();
        if (recovery.ports != 0) (recovery.rejoin ? recovery.rejoins : recovery.count)++;
        recovery.spectators = 0;
        recovery.spectators_resync = 0;
    }

    // A joiner: the host's Resync. Its read is past any this machine has made (the host chose it so).
    void take_resync(const Resync& r) {
        // (One to nobody's ports is a spectator's own.)
        if (r.read <= current_read || (rollback_option > 0 && r.read < frontier) || (r.ports == 0 && !watching)) {
            end_match(Reason::protocol, "the host asked for a game state at a read that doesn't fit");
            return;
        }
        recovery.sent_at = now();
        if (watching ? r.ports != 0 : (r.ports & own_ports) == 0) {
            std::printf("[netplay] %s will load the host's game before read %u (desync recovery)\n", ports_text(r.ports).c_str(), r.read);
            return;
        }
        // (A later one replaces the last: the host couldn't take its game there.)
        recovery.read = r.read;
        recovery.ports = r.ports;
        recovery.parts.clear();
        recovery.size = 0;
        recovery.ready = false;
        recovery.desync_read = never;
        std::printf("[netplay] read %llu: the host will send its game as it is before read %u (desync recovery)\n",
            (unsigned long long)current_read, r.read);
        tell("This game went a different way from the host's: getting the host's...");
    }

    // A joiner: a part of the host's state, after its Resync and in order.
    void take_state_part(const StatePart& part) {
        if (recovery.read == never || part.read != recovery.read || recovery.ready ||
            part.offset != recovery.parts.size() || (part.offset != 0 && (part.size != recovery.size || part.hash != recovery.part_hash))) {
            end_match(Reason::protocol, "the host's game state came out of order");
            return;
        }
        if (part.offset == 0) {
            recovery.size = part.size;
            recovery.part_hash = part.hash;
            recovery.parts.reserve(part.size);
        }
        recovery.parts.insert(recovery.parts.end(), part.bytes.begin(), part.bytes.end());
        publish();
        if (recovery.parts.size() < recovery.size) {
            return;
        }
        std::string error;
        if (!unpack_state(error)) {
            std::printf("[netplay] the host's game state can't be loaded: %s\n", error.c_str());
            end_match(Reason::protocol, ("the host's game state was broken (" + error + ")").c_str());
            return;
        }
        recovery.parts.clear();
        recovery.parts.shrink_to_fit();
        recovery.ready = true;
        std::printf("[netplay] the host's game before read %llu has come (%u bytes)\n", (unsigned long long)recovery.read, recovery.size);
    }

    // A spectator, at a read: its first controller's `buttons` (as the game would have had them) pressed L or R, the
    // view watched goes back or on.
    void choose_view(uint16_t buttons) {
        const uint16_t pressed = (uint16_t)(buttons & ~watch_buttons);
        watch_buttons = buttons;
        const int step = (pressed & next_view_buttons) != 0 ? 1 : (pressed & last_view_buttons) != 0 ? -1 : 0;
        if (step == 0 || match_players < 1) {
            return;
        }
        // 0 everyone's, 1 to players each player's.
        const int now_view = watched == 0 ? 0 : std::countr_zero(watched.load()) + 1;
        const int next = (now_view + step + match_players + 1) % (match_players + 1);
        watched = next == 0 ? 0 : (uint8_t)(1u << (next - 1));
        std::printf("[netplay] watching %s\n", next == 0 ? "everyone" : ports_text(watched).c_str());
        tell(next == 0 ? "Watching everyone." : "Watching " + players_text(watched) + ".");
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
        if (value != nullptr && std::strcmp(value, "room") == 0) {
            host_room = true;
            i++;
            return true;
        }
        number(1, 65535, n);
        host_port = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-server") == 0) {
        if (value == nullptr) {
            std::fprintf(stderr, "[netplay] --netplay-server needs the server's address and port\n");
            options_valid = false;
            return true;
        }
        server_option = argv[++i];
        return true;
    }
    if (std::strcmp(option, "--netplay-direct") == 0) {
        direct_option = true;
        return true;
    }
    if (std::strcmp(option, "--netplay-public") == 0) {
        public_option = true;
        return true;
    }
    if (std::strcmp(option, "--netplay-quick") == 0) {
        quick_option = true;
        return true;
    }
    if (std::strcmp(option, "--netplay-lan") == 0) {
        lan_option = true;
        return true;
    }
    if (std::strcmp(option, "--netplay-watch") == 0) {
        local_option = 0;
        return true;
    }
    if (std::strcmp(option, "--netplay-no-spectators") == 0) {
        spectators_option = false;
        return true;
    }
    if (std::strcmp(option, "--netplay-claim-at") == 0 || std::strcmp(option, "--netplay-seat-at") == 0) {
        const bool seating = option[10] == 's';
        unsigned long long read = 0;
        int port = 0, spectator = 1;
        char end = 0;
        const int got = value == nullptr ? 0 : seating ? std::sscanf(value, "%llu:%d:%d%c", &read, &port, &spectator, &end)
                                                       : std::sscanf(value, "%llu:%d%c", &read, &port, &end);
        if (got != (seating ? 3 : 2) || port < 2 || port > netplay::max_players || spectator < 1 || spectator > netplay::max_spectators) {
            std::fprintf(stderr, "[netplay] %s needs <read>:<port>%s (a joiner's port, 2 to %d)\n", option, seating ? ":<spectator>" : "",
                netplay::max_players);
            options_valid = false;
        }
        (seating ? seat_at : claim_at) = read;
        (seating ? seat_port : claim_port) = port - 1;
        seat_spectator = spectator - 1;
        if (value != nullptr) i++;
        return true;
    }
    if (std::strcmp(option, "--netplay-step-out-at") == 0) {
        number(0, LONG_MAX, n);
        step_out_at = (uint64_t)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-player") == 0) {
        player_option = value != nullptr ? std::strtoull(value, nullptr, 16) : 0;
        if (player_option == 0) {
            std::fprintf(stderr, "[netplay] --netplay-player needs a player id (hex, not 0)\n");
            options_valid = false;
        }
        if (value != nullptr) i++;
        return true;
    }
    if (std::strcmp(option, "--netplay-block") == 0) {
        const uint64_t player = value != nullptr ? std::strtoull(value, nullptr, 16) : 0;
        if (player == 0) {
            std::fprintf(stderr, "[netplay] --netplay-block needs a player id (hex, not 0)\n");
            options_valid = false;
        }
        blocked_players.push_back(player);
        if (value != nullptr) i++;
        return true;
    }
    if (std::strcmp(option, "--players") == 0) {
        number(2, netplay::max_players, n);
        wanted_players = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-start-with") == 0) {
        number(1, netplay::max_players - 1, n);
        start_with = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-seat-hold") == 0) {
        number(0, 24 * 60 * 60, n);
        seat_hold_option = (double)n;
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
            else if (key == "spikes" && pair && a > spike_reads && b <= 2000) { spike_every = (uint64_t)a; spike_lag = (int)b; }
            else good = false;
        }
        if (!good) {
            std::fprintf(stderr, "[netplay] --netplay-fake-network takes lag=<ms>, jitter=<ms>, loss=<%%>, reorder=<%%>, dup=<%%>, "
                "outage=<read>:<s> (8 s at most), lag-at=<read>:<ms> and spikes=<every reads>:<ms> (each 60 reads long), "
                "separated by commas\n");
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
        stalls[at] = ms;
        if (value != nullptr) i++;
        return true;
    }
    if (std::strcmp(option, "--rollback") == 0) {
        number(0, most_rollback_window, n);
        rollback_option = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-no-recovery") == 0) {
        recovery_option = false;
        return true;
    }
    if (std::strcmp(option, "--netplay-desync-at") == 0) {
        number(0, LONG_MAX, n);
        desync_at.push_back((uint64_t)n);
        return true;
    }
    if (std::strcmp(option, "--netplay-kick-at") == 0) {
        unsigned long long read = 0;
        int port = 0;
        if (value == nullptr || std::sscanf(value, "%llu:%d", &read, &port) != 2 || port < 2 || port > netplay::max_players) {
            std::fprintf(stderr, "[netplay] --netplay-kick-at needs <read>:<port> (a joiner's port, 2 to %d)\n", netplay::max_players);
            options_valid = false;
        }
        kick_at = read;
        kick_port = port - 1;
        if (value != nullptr) i++;
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
    return host_port > 0 || host_room || !join_where.empty() || quick_option;
}

void conker::lockstep::set_identity(uint64_t player, const std::vector<uint64_t>& blocked, bool ready_by_itself) {
    std::scoped_lock lock(session_mutex);
    player_option = player;
    blocked_players = blocked;
    auto_ready = ready_by_itself;
    if (session != nullptr) {
        session->set_blocked(blocked_players);
        session->set_release(netplay::release_name());
    }
}

void conker::lockstep::set_lan(bool visible) {
    lan_option = visible;
}

void conker::lockstep::set_server(const std::string& server, bool allow_direct, bool listed) {
    server_option = server;
    direct_option = allow_direct;
    public_option = listed;
}

void conker::lockstep::set_quick(int players, bool own_view, int input_delay, int local, int rollback, bool spectators) {
    set_host(-1, players, own_view, input_delay, local, rollback, spectators);
    quick_option = true;
}

void conker::lockstep::set_host(int port, int players, bool own_view, int input_delay, int local, int rollback, bool spectators) {
    spectators_option = spectators;
    quick_option = false;
    host_address.clear();
    host_port = port;
    host_room = port == 0;
    wanted_players = players;
    local_option = local;
    host_own_view = own_view;
    delay_option = input_delay < 0 ? auto_delay : std::min(input_delay, netplay::max_delay);
    rollback_option = std::clamp(rollback, 0, most_rollback_window);
    join_where.clear();
}

void conker::lockstep::set_join(const std::string& where, int local) {
    quick_option = false;
    host_port = -1;
    host_room = false;
    join_where = where;
    local_option = local;
    rollback_option = 0;    // the host's, with Start
    spectators_option = true;
}

bool conker::lockstep::prepare() {
    if (!options_valid) {
        return false;
    }
    const bool hosting = host_port > 0 || host_room;
    if (hosting && !join_where.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-host and --netplay-join can't be used together\n");
        return false;
    }
    if ((host_room || netplay::decode_room_code(join_where)) && server_option.empty()) {
        std::fprintf(stderr, "[netplay] a room is on a netplay server: give its address with --netplay-server <host:port>\n");
        return false;
    }
    if (public_option && !host_room) {
        std::fprintf(stderr, "[netplay] --netplay-public lists a room: it needs --netplay-host room\n");
        return false;
    }
    if (quick_option && (hosting || !join_where.empty() || server_option.empty())) {
        std::fprintf(stderr, "[netplay] --netplay-quick finds or hosts a room on --netplay-server: not with --netplay-host or --netplay-join\n");
        return false;
    }
    if (local_option == 0 && join_where.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-watch watches a match: it needs --netplay-join (not hosting, nor Quick Match)\n");
        return false;
    }
    if (!spectators_option && !hosting && !quick_option) {
        std::fprintf(stderr, "[netplay] --netplay-no-spectators is the host's choice\n");
        return false;
    }
    if (quick_option && local_option >= wanted_players) {
        std::fprintf(stderr, "[netplay] --local-players %d needs a match of more players than that (--players), should it host\n", local_option);
        return false;
    }
    if (!join_where.empty() && !recovery_option) {
        std::fprintf(stderr, "[netplay] --netplay-no-recovery is the host's choice: only the host's game is sent\n");
        return false;
    }
    if (!join_where.empty() && rollback_option > 0) {
        std::fprintf(stderr, "[netplay] --rollback is the host's choice: a joiner predicts as the host's match does\n");
        return false;
    }
    if (hosting && local_option >= wanted_players) {
        std::fprintf(stderr, "[netplay] --local-players %d needs a match of more players than that (--players)\n", local_option);
        return false;
    }
    if (start_with > 0 && (!hosting || start_with < local_option || start_with >= wanted_players)) {
        std::fprintf(stderr, "[netplay] --netplay-start-with is the host's: from its own players (--local-players) to fewer than --players\n");
        return false;
    }
    // The others join by the code, which holds the match's key: without the address there's none to give.
    if (host_port > 0 && host_address.empty() && !lan_option) {
        std::fprintf(stderr, "[netplay] --netplay-host needs --netplay-address <this computer's address>, for the join code (or --netplay-lan)\n");
        return false;
    }
    if (lan_option && host_port <= 0) {
        std::fprintf(stderr, "[netplay] --netplay-lan makes a match on a port visible on this network: it needs --netplay-host <port>\n");
        return false;
    }
    return true;
}

std::string conker::lockstep::room_code() {
    std::scoped_lock lock(session_mutex);
    return room_text;
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
    double connecting_since = 0;

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
        shown.watching = watching;
        shown.players = match_players;
        shown.own_ports = own_ports;
        shown.input_delay = delay;
        for (int port = 0; port < conker::netplay::max_players; port++) {
            shown.delays[port] = delay_at(port, current_read);
        }
        shown.others_playing = last_behind != INT64_MIN;
        shown.ahead = shown.others_playing ? -last_behind : 0;
        shown.waiting_for = waiting_for;
        shown.waiting_since = waiting_since;
        shown.catching_up = catching_up;
        shown.catching_up_since = catching_up_since;
        if (ask) shown.round_trip_ms = trips;
        shown.getting_state = playing && !shown.hosting && (recovery.read != never || rejoining);
        shown.state_progress = recovery.size == 0 ? 0 : (double)recovery.parts.size() / recovery.size;
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

namespace {
    // Starts the networking the first time (false: `error` says why).
    bool start_library(std::string& error) {
        if (!library_started) {
            if (!conker::netplay::startup(error)) {
                error = "couldn't start the networking: " + error;
                return false;
            }
            library_started = true;
        }
        return true;
    }
}

bool conker::lockstep::begin(const netplay::Hello& me, std::string& error) {
    std::scoped_lock lock(session_mutex);
    if (!start_library(error)) {
        return false;
    }
    // No mods in a match (texture packs aside): a mod is native code the others would have to trust. The
    // host refuses a joiner with any (the handshake's mods hash); this says so before connecting.
    if (me.mods != 0) {
        error = "netplay allows no mods but texture packs: turn them off in the Mods menu.";
        return false;
    }
    // One connection to the server at a time (the signaller's).
    stop_browsing();
    stop_connection_test();
    if (faking) {
        fake.outage = false;
        netplay::set_fake_network(fake);
        std::printf("[netplay] faking a bad network: lag %d ms, jitter %.0f ms, loss %.1f%%, reorder %.1f%%, dup %.1f%%\n",
            fake.lag_ms, fake.jitter_ms, fake.loss, fake.reorder, fake.dup);
    }
    cancel();
    if (player_option == 0) {
        // Neither the menu's nor --netplay-player: one for this process.
        if (!netplay::random_key(player_option)) {
            error = "couldn't make a player id (no random source)";
            return false;
        }
    }
    netplay::Hello mine = me;
    mine.players = (uint8_t)local_option;
    mine.player = player_option;
    mine.platform = (uint8_t)netplay::this_platform();
    ready_now = false;
    if (quick_option && !quick_restarting) {
        // A new Quick Match: it starts listing, hosting nothing yet.
        host_room = false;
        public_option = false;
        join_where.clear();
        quick_tried.clear();
        quick_tries = 0;
        quick_target = 0;
    }
    quick_asked = false;
    next_quick_list = 0;
    const bool hosting = host_port > 0 || host_room;
    if (host_room || netplay::decode_room_code(join_where) || quick_option) {
        // Through the server: the session starts once it's there (poll_connect). A new id each time (the
        // last one's may still be on the server, or be kept from reuse there).
        if (!netplay::new_id(error)) {
            return false;
        }
        signaller = new netplay::Signaller();
        if (server_option.empty() || !signaller->connect(server_option, error)) {
            error = server_option.empty() ? "no netplay server is set" : "couldn't reach the netplay server: " + error;
            delete signaller;
            signaller = nullptr;
            return false;
        }
        rendezvous = Rendezvous::registering;
        rendezvous_since = now();
    }
    else {
        session = new netplay::Session();
        if (hosting ? !session->host((uint16_t)host_port, wanted_players, mine, error, local_option) : !session->join(join_where, mine, error)) {
            delete session;
            session = nullptr;
            return false;
        }
    }
    sent = me;
    sent.player = mine.player;
    sent.platform = mine.platform;
    if (session != nullptr) {
        prepare_room();
    }
    own_ports = hosting && session != nullptr ? session->ports() : 0;
    match_players = hosting ? wanted_players : 0;
    joined = hosting ? local_option : 0;
    connecting_since = now();
    match_started = false;
    ended = false;
    left = false;
    current_read = 0;
    match_own_view = false;
    match_pairs = 0;
    watching = false;
    watched = 0;
    watch_buttons = 0;
    match_rollback = rollback_option;
    later_delays.clear();
    why_over.clear();
    end_reason = Reason::quit;
    host_closed = false;
    waits = {};
    delays.fill({});
    machine_delays.fill({});
    reports.fill({});
    for (auto& samples : lateness) samples.clear();
    for (auto& samples : recent_lateness) samples.clear();
    for (auto& early : early_inputs) early.clear();
    read_made_number.fill(never);
    waited_for_port = {};
    waited_long_for_port = {};
    own_waited_counted = {};
    own_waited_long_counted = {};
    stall_since = {};
    long_stall_at = {};
    slowed_for = {};
    time_sync_skipped = 0;
    lost_ports = 0;
    state_read_for = {};
    lost_count = 0;
    lost_here = false;
    wide_gap_for = 0;
    outage_until = 0;
    if (catching_up) set_catching_up(false);
    initial_sent = false;
    for (auto& pads : received) pads.clear();
    gone = {};
    idle_from.fill(never);
    back_at.fill(never);
    rejoin_wanted = 0;
    late_wanted = 0;
    spectators_late_wanted = 0;
    spectators_resync_wanted = 0;
    spectator_last_read.fill(never);
    spectator_resyncs = {};
    spectator_state_at = -1e9;
    seat_moves.clear();
    swaps.fill({});
    unseat_asked = 0;
    spectator_at = {};
    seat_waiting_since = 0;
    seated_from = 0;
    step_out_wanted = false;
    stepped_here = 0;
    open_ports = 0;
    start_open = false;
    rejoining = false;
    joining_late = false;
    gone_silently = false;
    last_behind = INT64_MIN;
    time_sync = {};
    round_trips_at = 0;
    {
        std::lock_guard status_lock(status_mutex);
        shown = {};
        news.clear();
    }
    own_hashes.clear();
    their_hashes.clear();
    frontier = 0;
    given.clear();
    checked = {};
    verified = 0;
    rollback_to = never;
    last_made = never;
    unverified_hashes.clear();
    rollbacks = {};
    recovery = {};
    if (quick_option) {
        if (!quick_restarting) {
            std::printf("[netplay] Quick Match on %s: looking for a room, %d players here\n", server_option.c_str(), local_option);
        }
    }
    else if (host_room) {
        std::printf("[netplay] hosting a match of %d through a room on %s, %d of them here\n", wanted_players,
            server_option.c_str(), local_option);
    }
    else if (!join_where.empty() && signaller != nullptr) {
        std::printf("[netplay] joining room %s on %s\n", join_where.c_str(), server_option.c_str());
    }
    else if (hosting) {
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

namespace {
    // How long the netplay server has to answer each step.
    constexpr double server_seconds = 15.0;

    // Hosting a public room: listed as it is now (the signaller sends that only when it changes).
    // How many the room's listing says are in: in the match, every port but those a new player could take (one held
    // for a player who left counts as theirs).
    int listed_joined() {
        const int in = match_started && session != nullptr ? match_players - std::popcount(session->free_ports()) : joined;
        return std::clamp(in, 1, match_players);
    }

    void list_room() {
        if (public_option && host_room && signaller != nullptr && signaller->room() != 0 && match_players >= 2) {
            conker::netplay::signal::ListedRoom room{ 0, (uint8_t)match_players, (uint8_t)listed_joined(), match_started,
                direct_option, (uint8_t)rollback_option };
            room.release = conker::netplay::release_name();
            room.watching = (uint8_t)std::popcount(session->spectators());
            room.spectators = session->takes_spectators();
            signaller->publish(sent, room);
        }
    }

    // Hosting on this network, in the match: the beacon each second, playing, with how many are in (fewer than its
    // players: the free ports a new player can join).
    void beacon_match() {
        if (lan_beacon == nullptr || !session->hosting() || now() < next_beacon) {
            return;
        }
        lan_beacon->send({ conker::netplay::fit_of(sent), conker::netplay::this_platform(), (uint16_t)host_port, session->key(),
            (uint8_t)match_players, (uint8_t)listed_joined(), true, (uint8_t)rollback_option,
            (uint8_t)std::popcount(session->spectators()), session->takes_spectators() });
        next_beacon = now() + 1.0;
    }

    // Quick Match: asks for the room list (rendezvous listing).
    void quick_list() {
        signaller->list_rooms(sent);
        rendezvous = Rendezvous::listing;
        rendezvous_since = now();
    }

    // Quick Match: the first room in the list this one could join that isn't `own` nor tried (open, with
    // room for this machine's players), or null.
    const conker::netplay::signal::ListedRoom* quick_pick(uint64_t own) {
        for (const conker::netplay::signal::ListedRoom& room : signaller->rooms()) {
            if (room.room != own && !room.other_build && !room.playing && room.players - room.joined >= local_option && !quick_tried.contains(room.room) &&
                std::find(blocked_players.begin(), blocked_players.end(), room.host) == blocked_players.end()) {
                return &room;
            }
        }
        return nullptr;
    }

    // Through the server: this one's session hosts, and makes its room (rendezvous creating).
    conker::lockstep::Progress host_here(std::string& status) {
        conker::netplay::Hello mine = sent;
        mine.players = (uint8_t)local_option;
        std::string error;
        session = new conker::netplay::Session();
        if (!session->host(*signaller, signaller->ice(!direct_option), wanted_players, mine, error, local_option)) {
            delete session;
            session = nullptr;
            status = "Couldn't host: " + error + ".";
            std::printf("[netplay] %s\n", status.c_str());
            conker::lockstep::cancel();
            return conker::lockstep::Progress::failed;
        }
        own_ports = session->ports();
        match_players = wanted_players;
        joined = local_option;
        connecting_since = now();
        prepare_room();
        signaller->create_room(session->key());
        rendezvous = Rendezvous::creating;
        rendezvous_since = now();
        // Quick Match: the first look for an older room in a while (the lists so far count against the
        // server's limit on them).
        next_quick_list = now() + quick_list_seconds;
        status = quick_option ? "Quick Match: making a room..." : "Making a room...";
        return conker::lockstep::Progress::waiting;
    }

    // Quick Match: joins `room` by its code, as a joiner would (rendezvous finding).
    conker::lockstep::Progress quick_join(uint64_t room, const std::string& name, std::string& status) {
        quick_tries++;
        quick_tried.insert(room);
        quick_target = 0;
        join_where = conker::netplay::encode_room_code(room);
        std::printf("[netplay] Quick Match: joining %s (%s)\n", name.c_str(), join_where.c_str());
        signaller->find_room(room);
        rendezvous = Rendezvous::finding;
        rendezvous_since = now();
        status = "Quick Match: joining " + name + "...";
        return conker::lockstep::Progress::waiting;
    }

    // Connecting through the netplay server (poll_connect's, with the lock held): registering, then the
    // host's session and room, or the joiner's lookup and then its session. A Progress when that's all there
    // is this time; nothing when the session's to be polled.
    std::optional<conker::lockstep::Progress> through_server(std::string& status) {
        using conker::lockstep::Progress;
        using conker::netplay::Signaller;
        const auto fail = [&status](const std::string& why) {
            std::printf("[netplay] %s\n", why.c_str());
            status = why;
            conker::lockstep::cancel();
            return Progress::failed;
        };
        if (session == nullptr) {
            signaller->poll();
        }
        if (signaller->state() == Signaller::State::failed) {
            return fail((rendezvous == Rendezvous::registering ? "Couldn't reach the netplay server: " : "Lost the netplay server: ") +
                signaller->error() + ".");
        }
        if (now() - rendezvous_since > server_seconds) {
            return fail("The netplay server didn't answer.");
        }
        conker::netplay::Hello mine = sent;
        mine.players = (uint8_t)local_option;
        std::string error;
        switch (rendezvous) {
            case Rendezvous::registering: {
                if (signaller->state() != Signaller::State::ready) {
                    status = "Connecting to the netplay server...";
                    return Progress::waiting;
                }
                if (!direct_option && !signaller->turn()) {
                    return fail("The netplay server has no relay: allow direct connections, or use another server.");
                }
                std::printf("[netplay] on the netplay server%s\n", signaller->turn() ? ", with its relay" : "");
                if (public_option && host_room && !signaller->has_public_rooms()) {
                    std::printf("[netplay] the netplay server is older and has no public rooms: this one is private\n");
                }
                rendezvous_since = now();
                if (host_room) {
                    return host_here(status);
                }
                if (quick_option && quick_target != 0) {
                    return quick_join(quick_target, conker::netplay::room_name(quick_target), status);
                }
                if (quick_option) {
                    if (!signaller->has_public_rooms()) {
                        return fail("Quick Match needs public rooms, which this netplay server is too old for: join or host by a room code.");
                    }
                    quick_list();
                    status = "Quick Match: looking for a room...";
                    return Progress::waiting;
                }
                signaller->find_room(*conker::netplay::decode_room_code(join_where));
                rendezvous = Rendezvous::finding;
                status = "Looking up the room...";
                return Progress::waiting;
            }
            case Rendezvous::listing: {
                if (signaller->listing() != Signaller::Lookup::found) {
                    status = "Quick Match: looking for a room...";
                    return Progress::waiting;
                }
                if (quick_tries < quick_most_tries) {
                    if (const conker::netplay::signal::ListedRoom* room = quick_pick(0)) {
                        return quick_join(room->room, conker::netplay::room_name(room->room), status);
                    }
                }
                // None to join (or too many tried): this one hosts, in public.
                std::printf("[netplay] Quick Match: no room to join, so hosting one\n");
                host_room = true;
                public_option = true;
                return host_here(status);
            }
            case Rendezvous::finding:
                switch (signaller->lookup()) {
                    case Signaller::Lookup::found:
                        break;
                    case Signaller::Lookup::missing:
                        if (quick_option) {
                            std::printf("[netplay] Quick Match: room %s has gone\n", join_where.c_str());
                            quick_list();
                            status = "Quick Match: looking for a room...";
                            return Progress::waiting;
                        }
                        return fail("There's no room " + join_where + " on the netplay server: check the code (a room closes when its host leaves).");
                    default:
                        status = "Looking up the room...";
                        return Progress::waiting;
                }
                session = new conker::netplay::Session();
                if (!session->join(*signaller, signaller->ice(!direct_option), signaller->found().host, signaller->found().key, mine, error)) {
                    delete session;
                    session = nullptr;
                    if (quick_option) {
                        std::printf("[netplay] Quick Match: couldn't join %s: %s\n", join_where.c_str(), error.c_str());
                        quick_list();
                        status = "Quick Match: looking for another room...";
                        return Progress::waiting;
                    }
                    return fail("Couldn't join: " + error + ".");
                }
                prepare_room();
                rendezvous = Rendezvous::none;
                return std::nullopt;
            default:
                // Creating: the session polls the signaller, and the code comes with it (poll_connect).
                return std::nullopt;
        }
    }
}

conker::lockstep::Progress conker::lockstep::poll_connect(std::string& status) {
    std::scoped_lock lock(session_mutex);
    if (session == nullptr && signaller == nullptr) {
        status = "Not connecting.";
        return Progress::failed;
    }
    if (rendezvous != Rendezvous::none) {
        if (const std::optional<Progress> step = through_server(status)) {
            return *step;
        }
    }
    const bool hosting = session->hosting();
    for (const Event& event : session->poll()) {
        switch (event.type) {
            case Event::Type::joined:
                if (event.spectator >= 0) {
                    std::printf("[netplay] spectator %d is watching\n", event.spectator + 1);
                    break;
                }
                joined += std::popcount(event.ports);
                std::printf("[netplay] %s joined (%d of %d)\n", ports_text(event.ports).c_str(), joined, match_players);
                break;
            case Event::Type::left:
                // Once started (a joiner leaving in the poll the match started in), as in the match: Gone.
                if (match_started) {
                    handle(event);
                    break;
                }
                if (event.spectator >= 0) {
                    std::printf("[netplay] spectator %d left: %s\n", event.spectator + 1, netplay::describe(event.reason));
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
                watching = own_ports == 0;
                // (Nobody waits for a spectator, which may be slow to start: the others' inputs may be well ahead of it
                // before its first read sets the horizon.)
                if (watching) session->set_horizon(horizon());
                if (watching && event.rejoin) {
                    std::printf("[netplay] watching, a match of %d under way\n", match_players);
                    joining_late = true;
                    match_started = true;
                    match_own_view = event.own_view;
                    match_pairs = event.pairs;
                    match_rollback = event.rollback;
                    rollback_option = event.rollback;
                    rejoining = true;
                    rejoining_since = now();
                    break;
                }
                if (watching) {
                    std::printf("[netplay] watching, a match of %d: waiting for the players\n", match_players);
                    break;
                }
                if (event.rejoin) {
                    // Coming back to the match under way (or joining it, `late`), as the host's (its rules: the rest
                    // comes with Rejoin). The game starts at once, and loads the host's as it comes (rejoining_read).
                    std::printf("[netplay] %s, on %s of %d: the match is under way\n", event.late ? "in" : "back",
                        ports_text(own_ports).c_str(), match_players);
                    joining_late = event.late;
                    match_started = true;
                    match_own_view = event.own_view;
                    match_pairs = event.pairs;
                    rollback_option = event.rollback;
                    match_rollback = event.rollback;
                    rejoining = true;
                    rejoining_since = now();
                    break;
                }
                std::printf("[netplay] in, on %s of %d: waiting for the others\n", ports_text(own_ports).c_str(), match_players);
                // Not ready in a new room until it says so (the menu's Ready), unless it's by itself.
                ready_now = auto_ready;
                if (auto_ready) {
                    session->set_ready(true);
                }
                break;
            case Event::Type::moved:
                // Before the start, the host moved this machine: to `ports`, or out of its places to watch (its step-out
                // crossing the Start: the host's Gone for them follows).
                if (event.late) stepped_here = (uint8_t)event.port;
                own_ports = event.ports;
                watching = own_ports == 0;
                std::printf("[netplay] the host moved this machine: %s\n", watching ? "watching" : ("on " + ports_text(own_ports)).c_str());
                if (watching) {
                    session->set_horizon(horizon());
                }
                ready_now = auto_ready && !watching;
                if (ready_now) session->set_ready(true);
                break;
            case Event::Type::started:
                match_started = true;
                match_own_view = event.own_view;
                match_pairs = event.pairs;
                match_rollback = event.rollback;
                rollback_option = event.rollback;
                for (int port = 0; port < netplay::max_players; port++) {
                    delays[port] = { 0, event.delays[port], event.delays[port] };
                }
                delay = own_delay_at(0);
                open_from_start(event.open);
                break;
            case Event::Type::ended:
                std::printf("[netplay] couldn't join: %s (%s)%s%s\n", netplay::describe(event.reason), event.detail.c_str(),
                    event.release.empty() ? "" : ", host's release ", event.release.c_str());
                // Quick Match: another room, on the same connection to the server.
                if (quick_option && signaller != nullptr && signaller->state() == netplay::Signaller::State::ready) {
                    delete session;
                    session = nullptr;
                    own_ports = 0;
                    match_players = 0;
                    ready_now = false;
                    quick_list();
                    status = "Quick Match: looking for another room...";
                    return Progress::waiting;
                }
                // Through the server, why it couldn't find the host says more than "timed out".
                status = "Couldn't join: " + (event.by_signalling ? event.detail : std::string(netplay::describe(event.reason))) + ".";
                // Another release: which one the host has (a host from before releases were told: not known).
                if (!event.by_signalling && (event.reason == Reason::version || event.reason == Reason::build)) {
                    const std::string own = netplay::release_name();
                    status = !event.release.empty() && event.release != own ?
                        "Couldn't join: the host plays release " + event.release + " and you have " + own + ". Everyone needs the same release." :
                        "Couldn't join: the host plays another release of the game than yours (" + own + "). Everyone needs the same release.";
                }
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
    // The host starts the match once every port is taken and everyone's ready (the host too: its Start); or with
    // whoever's in (--netplay-start-with, the menu's start_now), the free ports open for players joining later: once
    // a room has its code (a host alone would otherwise start before the server made it, and nobody could join).
    const bool open_now = rendezvous == Rendezvous::none && (start_open || (start_with > 0 && joined >= start_with));
    if (hosting && !match_started && (joined == match_players || open_now) && session->everyone_ready(!open_now)) {
        const std::array<uint8_t, netplay::max_players> chosen = start_delays();
        if (session->start(host_own_view, chosen, rollback_option, open_now)) {
            match_started = true;
            match_own_view = host_own_view;
            match_pairs = session->pairs();
            for (int port = 0; port < netplay::max_players; port++) {
                delays[port] = { 0, chosen[port], chosen[port] };
            }
            delay = own_delay_at(0);
            open_from_start(session->open_at_start());
        }
    }
    // (Tests) a claim or a step-out at read 0: before the start, once welcomed.
    if (!hosting && !match_started && session->welcomed()) {
        if (claim_at == 0 && session->watching()) {
            claim_at = never;
            std::printf("[netplay] claiming %s before the start, as --netplay-claim-at asked\n", ports_text((uint8_t)(1u << claim_port)).c_str());
            session->claim(claim_port);
        }
        if (step_out_at == 0 && own_ports != 0) {
            step_out_at = never;
            std::printf("[netplay] stepping out before the start, as --netplay-step-out-at asked\n");
            session->step_out();
        }
    }
    // A joiner in a room with someone on its block list leaves (Quick Match: for another room).
    if (!hosting && !match_started && session->welcomed()) {
        for (const netplay::Seat& seat : session->roster().seats) {
            if (seat.player == 0 || seat.player == player_option ||
                std::find(blocked_players.begin(), blocked_players.end(), seat.player) == blocked_players.end()) {
                continue;
            }
            std::printf("[netplay] %s is on the block list: leaving\n", netplay::player_name(seat.player).c_str());
            if (quick_option && signaller != nullptr && signaller->state() == netplay::Signaller::State::ready) {
                // Goodbye to this room only (not say_goodbye: this machine's match isn't over).
                session->leave(Reason::quit);
                for (int i = 0; i < 30; i++) {
                    session->poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                delete session;
                session = nullptr;
                own_ports = 0;
                match_players = 0;
                ready_now = false;
                quick_list();
                status = "Quick Match: looking for another room...";
                return Progress::waiting;
            }
            status = "Left: " + netplay::player_name(seat.player) + " is on your block list.";
            cancel();
            return Progress::failed;
        }
    }
    list_room();
    // (Started: the beacon goes on from the match's reads, beacon_match, for players joining it or coming back.)
    if (match_started && lan_beacon != nullptr) {
        next_beacon = 0;
        beacon_match();
    }
    if (match_started) {
        if (rejoining) {
            publish();
            status = watching ? "Watching the match." : joining_late ? "In the match." : "Back in the match.";
            return Progress::started;
        }
        std::string by_port;
        for (int port = 0; port < match_players; port++) {
            if (delays[port].after != delay) by_port = " (by port: ";
        }
        for (int port = 0; !by_port.empty() && port < match_players; port++) {
            by_port += (port == 0 ? "" : ", ") + std::to_string(delays[port].after) + (port == match_players - 1 ? ")" : "");
        }
        std::printf("[netplay] the match starts: %d players, this is %s, input delay %d reads%s%s%s%s\n", match_players,
            watching ? "a spectator" : ports_text(own_ports).c_str(), delay, by_port.c_str(),
            match_own_view ? ", everyone sees only their own view" : "",
            match_rollback > 0 ? (", rollback " + std::to_string(match_rollback) + " reads").c_str() : "",
            open_ports != 0 ? (", " + ports_text(open_ports) + " open").c_str() : "");
        publish();
        status = "The match starts.";
        return Progress::started;
    }
    if (rendezvous == Rendezvous::creating && signaller->room() != 0) {
        rendezvous = Rendezvous::none;
        room_text = netplay::encode_room_code(signaller->room());
        std::printf("[netplay] room code %s (the others join with it%s): waiting for the players\n", room_text.c_str(),
            public_option ? (", or find it as " + netplay::room_name(signaller->room()) + " in the public list").c_str() : "");
        std::fflush(stdout);
        list_room();
    }
    // Quick Match, hosting alone: an older open room in the list is joined instead (this one left: its room
    // goes with its connection to the server), so two who started at once end up in one room.
    if (quick_option && hosting && joined != local_option) {
        // Someone's in: a list asked for before is out of date.
        quick_asked = false;
    }
    if (quick_option && hosting && rendezvous == Rendezvous::none && joined == local_option && session->arriving() == 0 && signaller != nullptr &&
        signaller->state() == netplay::Signaller::State::ready && signaller->room() != 0) {
        if (quick_asked && signaller->listing() == netplay::Signaller::Lookup::found) {
            quick_asked = false;
            const uint64_t own = signaller->room();
            const auto& rooms = signaller->rooms();
            const auto mine = std::find_if(rooms.begin(), rooms.end(), [own](const auto& room) { return room.room == own; });
            const netplay::signal::ListedRoom* older = quick_pick(own);
            // The server lists open rooms fullest, then oldest, first: one before this one is the one to be in.
            if (older != nullptr && (mine == rooms.end() || older < &*mine)) {
                const uint64_t target = older->room;
                std::printf("[netplay] Quick Match: %s is older, so leaving this room for it\n", netplay::room_name(target).c_str());
                quick_target = target;
                quick_restarting = true;
                host_room = false;
                public_option = false;
                std::string error;
                const bool restarted = begin(sent, error);
                quick_restarting = false;
                if (!restarted) {
                    status = "Quick Match: couldn't join the other room: " + error + ".";
                    std::printf("[netplay] %s\n", status.c_str());
                    cancel();
                    return Progress::failed;
                }
                status = "Quick Match: joining " + netplay::room_name(target) + "...";
                return Progress::waiting;
            }
        }
        else if (!quick_asked && now() >= next_quick_list) {
            signaller->list_rooms(sent);
            quick_asked = true;
            next_quick_list = now() + quick_list_seconds;
        }
    }
    // Visible on this network: a beacon each second while the players join.
    if (lan_option && hosting && host_port > 0) {
        if (lan_beacon == nullptr) {
            std::string error;
            lan_beacon = new netplay::LanBeacon();
            if (!lan_beacon->open(netplay::lan::default_port, error)) {
                std::printf("[netplay] can't be seen on this network: %s\n", error.c_str());
            }
            else {
                std::printf("[netplay] visible on this network (UDP port %u)\n", (unsigned)netplay::lan::default_port);
            }
        }
        if (now() >= next_beacon) {
            lan_beacon->send({ netplay::fit_of(sent), netplay::this_platform(), (uint16_t)host_port, session->key(), (uint8_t)match_players,
                (uint8_t)std::clamp(joined, 1, match_players), false, (uint8_t)rollback_option, (uint8_t)std::popcount(session->spectators()),
                session->takes_spectators() });
            next_beacon = now() + 1.0;
        }
    }
    // A host that lost the server can't be found by anyone else.
    if (hosting && signaller != nullptr && signaller->state() == netplay::Signaller::State::failed) {
        std::printf("[netplay] lost the netplay server: %s\n", signaller->error().c_str());
        status = "Lost the netplay server: " + signaller->error() + ".";
        cancel();
        return Progress::failed;
    }
    if (hosting && now() - connecting_since > join_seconds) {
        const std::string what = joined == match_players ? "not everyone was ready" : "not every player joined";
        std::printf("[netplay] %s in %.0f minutes\n", what.c_str(), join_seconds / 60);
        status = (char)std::toupper((unsigned char)what[0]) + what.substr(1) + " in " + std::to_string((int)(join_seconds / 60)) + " minutes.";
        cancel();
        return Progress::failed;
    }
    if (rendezvous == Rendezvous::creating) {
        status = quick_option ? "Quick Match: making a room..." : "Making a room...";
    }
    else if (hosting && quick_option && signaller != nullptr && signaller->room() != 0) {
        status = "Quick Match: waiting for players in " + netplay::room_name(signaller->room()) + ": " + std::to_string(joined) +
            " of " + std::to_string(match_players) + " in.";
    }
    else if (hosting && joined == match_players) {
        status = "Everyone's in: waiting for everyone to be ready.";
    }
    else if (hosting) {
        status = "Waiting for players: " + std::to_string(joined) + " of " + std::to_string(match_players) + " in.";
    }
    else if (!session->welcomed()) {
        status = "Connecting...";
    }
    else if (watching) {
        status = "Watching a match of " + std::to_string(match_players) + ": waiting for the players.";
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
    // After the session, which uses it.
    delete signaller;
    signaller = nullptr;
    delete lan_beacon;
    lan_beacon = nullptr;
    next_beacon = 0;
    rendezvous = Rendezvous::none;
    room_text.clear();
    match_started = false;
    publish();
}

namespace {
    // How often the room list is asked for again (the server allows one each 3 s, after a few).
    constexpr double list_seconds = 5.0;
}

bool conker::lockstep::browse(const netplay::Hello& me, std::string& error) {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr || signaller != nullptr) {
        error = "a match is being connected";
        return false;
    }
    if (server_option.empty()) {
        error = "no netplay server is set";
        return false;
    }
    if (!start_library(error)) {
        return false;
    }
    stop_browsing();
    stop_connection_test();
    // A new id each time, as for a match (the server keeps a released one from reuse for a while).
    if (!netplay::new_id(error)) {
        return false;
    }
    browser = new netplay::Signaller();
    if (!browser->connect(server_option, error)) {
        delete browser;
        browser = nullptr;
        return false;
    }
    browse_hello = me;
    next_list = 0;
    browse_since = now();
    browse_heard = false;
    return true;
}

void conker::lockstep::browse_again() {
    std::scoped_lock lock(session_mutex);
    next_list = 0;
}

conker::lockstep::Browse conker::lockstep::poll_browse() {
    using conker::netplay::Signaller;
    std::scoped_lock lock(session_mutex);
    Browse out;
    if (browser == nullptr) {
        out.state = Browse::State::failed;
        out.error = "not looking at the rooms";
        return out;
    }
    browser->poll();
    if (browser->state() == Signaller::State::failed) {
        out.state = Browse::State::failed;
        out.error = browser->error();
        return out;
    }
    if (browser->state() == Signaller::State::ready && !browser->has_public_rooms()) {
        out.state = Browse::State::failed;
        out.error = "the netplay server is older and has no public rooms (rooms by code still work)";
        return out;
    }
    if (browser->state() != Signaller::State::ready) {
        if (now() - browse_since > server_seconds) {
            out.state = Browse::State::failed;
            out.error = "the netplay server didn't answer";
        }
        return out;
    }
    browse_heard = browse_heard || browser->listing() == Signaller::Lookup::found;
    if (browser->listing() == Signaller::Lookup::waiting && now() - asked_at > server_seconds) {
        out.state = Browse::State::failed;
        out.error = "the netplay server stopped answering";
        return out;
    }
    if (browser->listing() != Signaller::Lookup::waiting && now() >= next_list) {
        browser->list_rooms(browse_hello);
        asked_at = now();
        next_list = asked_at + list_seconds;
    }
    // The last list, while the next is asked for.
    if (browse_heard) {
        out.state = Browse::State::listed;
        out.others = browser->others();
        out.older = browser->older();
        for (const netplay::signal::ListedRoom& room : browser->rooms()) {
            // Not a blocked player's (its host's id: none from an older server or host).
            if (room.host != 0 && std::find(blocked_players.begin(), blocked_players.end(), room.host) != blocked_players.end()) {
                continue;
            }
            out.rooms.push_back({ netplay::encode_room_code(room.room), netplay::room_name(room.room), room.players, room.joined,
                room.playing, room.direct, room.rollback, room.release, room.other_build, room.watching,
                // (A server older than signalling version 7 doesn't say: Watch is offered, and the host refuses if it must.)
                room.spectators || browser->version() < 7 });
        }
    }
    return out;
}

bool conker::lockstep::start_connection_test(std::string& error) {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr || signaller != nullptr) {
        error = "a match is being connected";
        return false;
    }
    if (!start_library(error)) {
        return false;
    }
    stop_browsing();
    stop_connection_test();
    if (!server_option.empty() && !netplay::new_id(error)) {
        return false;
    }
    connection_test = new netplay::ConnectionTest();
    connection_test->start(server_option, netplay::lan::default_port);
    return true;
}

bool conker::lockstep::poll_connection_test(netplay::ConnectionReport& report) {
    std::scoped_lock lock(session_mutex);
    if (connection_test == nullptr || !connection_test->poll()) {
        return false;
    }
    report = connection_test->report();
    return true;
}

void conker::lockstep::stop_connection_test() {
    std::scoped_lock lock(session_mutex);
    delete connection_test;
    connection_test = nullptr;
}

void conker::lockstep::stop_browsing() {
    std::scoped_lock lock(session_mutex);
    delete browser;
    browser = nullptr;
}

bool conker::lockstep::lan_browse(const netplay::Hello& me, std::string& error) {
    std::scoped_lock lock(session_mutex);
    stop_lan();
    lan_listener = new netplay::LanListener();
    lan_listener->listen_for(netplay::fit_of(me), netplay::this_platform());
    if (!lan_listener->open(netplay::lan::default_port, error)) {
        delete lan_listener;
        lan_listener = nullptr;
        return false;
    }
    lan_hello = me;
    return true;
}

std::vector<conker::lockstep::ListedRoom> conker::lockstep::poll_lan() {
    std::scoped_lock lock(session_mutex);
    std::vector<ListedRoom> out;
    if (lan_listener == nullptr) {
        return out;
    }
    lan_listener->poll(now());
    for (const netplay::LanMatch& match : lan_listener->matches()) {
        out.push_back({ match.code, netplay::room_name(match.beacon.key), match.beacon.players, match.beacon.joined, match.beacon.playing,
            true, match.beacon.rollback, {}, false, match.beacon.watching, match.beacon.spectators });
    }
    return out;
}

void conker::lockstep::stop_lan() {
    std::scoped_lock lock(session_mutex);
    delete lan_listener;
    lan_listener = nullptr;
}

namespace {
    // The roster's spectators, and its rules for them, into `room`.
    void add_spectators(conker::lockstep::WaitingRoom& room, const conker::netplay::Roster& roster) {
        room.watching = watching;
        room.takes_spectators = roster.spectators != 0;
        for (int n = 0; n < conker::netplay::max_spectators; n++) {
            const conker::netplay::Watcher& w = roster.watchers[n];
            if (w.player != 0) {
                room.spectators.push_back({ n, w.player, conker::netplay::player_name(w.player), w.platform, watching && w.player == player_option });
            }
        }
    }
}

conker::lockstep::WaitingRoom conker::lockstep::waiting_room() {
    std::scoped_lock lock(session_mutex);
    WaitingRoom room;
    if (session == nullptr || match_started || (!session->hosting() && !session->welcomed())) {
        return room;
    }
    const netplay::Roster roster = session->roster();
    room.in = true;
    room.hosting = session->hosting();
    room.players = roster.players;
    room.ready = ready_now;
    room.own_view = roster.own_view != 0;
    room.delay = roster.delay == netplay::auto_delay ? -1 : roster.delay;
    room.rollback = roster.rollback;
    for (int port = 0; port < netplay::max_players; port++) {
        const netplay::Seat& s = roster.seats[port];
        Seat& seat = room.seats[port];
        seat.player = s.player;
        if (s.player == 0) continue;
        seat.name = netplay::player_name(s.player);
        seat.ready = s.ready != 0;
        seat.platform = s.platform;
        seat.mine = (own_ports & (1u << port)) != 0;
        seat.round_trip_ms = seat.mine || s.round_trip_ms == 0xFFFF ? -1 : s.round_trip_ms;
        seat.movable = port > 0 && s.player != roster.seats[0].player;
    }
    add_spectators(room, roster);
    return room;
}

void conker::lockstep::set_ready(bool ready) {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr && !match_started) {
        ready_now = ready;
        session->set_ready(ready);
        if (!ready) start_open = false;
    }
}

void conker::lockstep::start_now() {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr && session->hosting() && !match_started) {
        start_open = true;
        ready_now = true;
        session->set_ready(true);
    }
}

bool conker::lockstep::kick(int port, uint64_t player) {
    std::scoped_lock lock(session_mutex);
    // Only the player the menu showed there (the place may have changed hands since).
    if (session == nullptr || ended || port < 0 || port >= netplay::max_players ||
        (player != 0 && session->roster().seats[port].player != player)) {
        return false;
    }
    return session->kick(port);
}

bool conker::lockstep::seat(int port, int spectator, uint64_t player) {
    std::scoped_lock lock(session_mutex);
    if (session == nullptr || !session->hosting() || ended || port < 1 || port >= match_players || spectator < 0 ||
        spectator >= netplay::max_spectators || player == 0 || session->roster().watchers[spectator].player != player) {
        return false;
    }
    // Before the start: at once (a joiner's machine there watching instead).
    if (!match_started) {
        if (const uint8_t machine = session->machine_of(port); machine != 0) {
            host_unseated(machine);
            if (session->machine_of(port) != 0) return false;
        }
        return seat_before_start(spectator, player, port);
    }
    // A joiner's machine there: it steps out first, then the spectator takes the place (a swap).
    const netplay::Roster roster = session->roster();
    if ((unseat_asked & (1u << port)) != 0 || swaps[port].spectator >= 0) {
        return false;
    }
    if (roster.seats[port].player != 0 && idle_from[port] == never) {
        const uint8_t machine = session->machine_of(port);
        if (!session->ask_unseat(port)) return false;
        unseat_asked |= machine;
        std::printf("[netplay] swapping %s for spectator %d\n", ports_text((uint8_t)(1u << port)).c_str(), spectator + 1);
        swaps[port] = { spectator, player, port };
        return true;
    }
    seat_moves.push_back({ spectator, player, port });
    return true;
}

bool conker::lockstep::make_spectator(int port, uint64_t player) {
    std::scoped_lock lock(session_mutex);
    const uint8_t machine = session != nullptr ? session->machine_of(port) : 0;
    if (session == nullptr || !session->hosting() || ended || machine == 0 || (unseat_asked & machine) != 0 ||
        session->roster().seats[port].player != player || player == 0) {
        return false;
    }
    for (int p = 0; p < netplay::max_players; p++) {
        if ((machine & (1u << p)) != 0 && swaps[p].spectator >= 0) return false;
    }
    // Before the start: at once.
    if (!match_started) {
        host_unseated(machine);
        return session->machine_of(port) == 0;
    }
    if (!session->ask_unseat(port)) {
        return false;
    }
    unseat_asked |= machine;
    return true;
}

void conker::lockstep::claim(int port) {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr && session->watching() && !ended && !rejoining && port >= 1 && port < match_players) {
        session->claim(port);
    }
}

void conker::lockstep::step_out() {
    std::scoped_lock lock(session_mutex);
    if (session == nullptr || session->hosting() || ended || rejoining || own_ports == 0) {
        return;
    }
    // Before the start: the host makes it a spectator at once (its roster says: moved).
    if (!match_started) {
        session->step_out();
        return;
    }
    step_out_wanted = true;
}

bool conker::lockstep::kick_spectator(int number, uint64_t player) {
    std::scoped_lock lock(session_mutex);
    return session != nullptr && session->hosting() && !ended && number >= 0 && number < netplay::max_spectators && player != 0 &&
        session->kick_spectator(number, player);
}

conker::lockstep::WaitingRoom conker::lockstep::match_room() {
    std::scoped_lock lock(session_mutex);
    WaitingRoom room;
    if (session == nullptr || !match_started || ended) {
        return room;
    }
    const netplay::Roster roster = session->roster();
    room.in = true;
    room.hosting = session->hosting();
    room.players = match_players;
    room.own_view = match_own_view;
    room.delay = delay;
    room.rollback = match_rollback;
    const std::array<int, netplay::max_players> trips = session->round_trips_by_port();
    for (int port = 0; port < netplay::max_players && port < match_players; port++) {
        const netplay::Seat& s = roster.seats[port];
        Seat& seat = room.seats[port];
        seat.player = s.player;
        seat.mine = (own_ports & (1u << port)) != 0;
        seat.left = gone[port] && (open_ports & (1u << port)) == 0;
        seat.open = (open_ports & (1u << port)) != 0;
        if (s.player == 0) continue;
        seat.name = netplay::player_name(s.player);
        seat.ready = true;
        seat.platform = s.platform;
        seat.round_trip_ms = seat.mine || seat.left ? -1 : trips[port];
        seat.movable = port > 0 && s.player != roster.seats[0].player;
    }
    add_spectators(room, roster);
    return room;
}

void conker::lockstep::leave_match() {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr && match_started && !ended) {
        end_match(Reason::quit, watching ? "you stopped watching" : "you left the match");
    }
}

void conker::lockstep::block(uint64_t player) {
    std::scoped_lock lock(session_mutex);
    if (player == 0 || player == player_option) {
        return;
    }
    if (std::find(blocked_players.begin(), blocked_players.end(), player) == blocked_players.end()) {
        blocked_players.push_back(player);
    }
    if (session == nullptr) {
        return;
    }
    session->set_blocked(blocked_players);
    session->set_release(netplay::release_name());
    // A host kicks them now, before the match or in it (a joiner leaves at its next poll, before the match:
    // poll_connect; in one, it plays on).
    if (session->hosting() && !ended) {
        const netplay::Roster roster = session->roster();
        for (int port = 0; port < netplay::max_players; port++) {
            if (roster.seats[port].player == player) {
                session->kick(port);
            }
        }
        for (int n = 0; n < netplay::max_spectators; n++) {
            if (roster.watchers[n].player == player) {
                session->kick_spectator(n, player);
            }
        }
    }
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

bool conker::lockstep::spectating() {
    return match_started && watching;
}

uint8_t conker::lockstep::watched_ports() {
    return ::watched;
}

uint8_t conker::lockstep::view_pairs() {
    return match_started ? match_pairs : 0;
}

namespace {
    // From the match's first read: watches for the program quitting (say_goodbye, at once).
    void watch_for_quit() {
        if (quit_watcher.joinable()) {
            return;
        }
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

    // Coming back, at read `read` of this machine's game as it starts (not the match's): the host's word and its game
    // taken as they come; nothing sent, and the game given idle pads. At its rejoin_boot_reads'th read the game waits
    // for the host's (then the harness loads it, take_state). False if the match is over.
    bool rejoining_read(uint64_t read, ultramodern::input::Pad* pads, conker::netplay::Motion* motions, std::unique_lock<std::recursive_mutex>& lock) {
        watch_for_quit();
        const bool wait = read >= rejoin_boot_reads;
        if (wait && !recovery.ready) {
            std::printf("[netplay] read %llu of this game: waiting for the host's\n", (unsigned long long)read);
        }
        const double from = now();
        do {
            for (const Event& event : session->poll()) {
                handle(event);
            }
            // (The host gone: no state is coming.)
            if (left && !ended) {
                end_match(Reason::quit, gone_silently ? "the host stopped answering (it may have quit)" : "the host ended the match");
            }
            if (ended) {
                return false;
            }
            if (now() - rejoining_since > wait_seconds + state_wait_seconds) {
                end_match(Reason::timeout, "the host's game didn't come");
                return false;
            }
            if (!wait || recovery.ready) {
                break;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            lock.lock();
            if (stopping) {
                return false;
            }
        } while (true);
        if (wait && now() - from > 0.001) {
            std::printf("[netplay] waited %.0f ms for the host's game\n", (now() - from) * 1000);
        }
        publish();
        for (int port = 0; port < match_players; port++) {
            pads[port] = { 0, 0, 0, pads[port].err_no };
            motions[port] = {};
            motions[port].controls = conker::motion::default_controls;
        }
        return true;
    }
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
    if (rejoining) {
        return rejoining_read(read, pads, motions, lock);
    }
    // Rollback: a read made again, after the harness went back to before it.
    const bool again = rollback_option > 0 && read < frontier;
    if (again) {
        if (read != last_made + 1) {
            rollbacks.count++;
            rollbacks.deepest = std::max(rollbacks.deepest, frontier - read);
        }
        rollbacks.reads_again++;
    }
    else {
        current_read = read;
        // A spectator: the host's delay changes that came before it reached the last, once it has.
        while (!later_delays.empty()) {
            const Delay& next = later_delays.front();
            bool reached = true;
            for (int port = 0; port < match_players; port++) {
                if ((next.ports & (1u << port)) != 0 && read < delays[port].from) reached = false;
            }
            if (!reached) break;
            change_delay(next.ports, next.delay, next.from_read);
            later_delays.erase(later_delays.begin());
        }
        delay = own_delay_at(read);
        note_read_made(read);
        // Stepping out (asked by the host, or this player's own choice): its inputs end with those sent; it watches.
        if (read == step_out_at) {
            step_out_wanted = true;
        }
        if (step_out_wanted && !session->hosting() && own_ports != 0 && recovery.read == never) {
            step_out_wanted = false;
            std::printf("[netplay] read %llu: stepping out of %s to watch, after read %zu\n", (unsigned long long)read,
                ports_text(own_ports).c_str(), received[std::countr_zero(own_ports)].size() - 1);
            session->step_out();
            for (int port = 0; port < conker::netplay::max_players; port++) {
                if ((own_ports & (1u << port)) != 0) checked[port] = std::max<uint64_t>(checked[port], received[port].size());
            }
            stepped_here = own_ports;
            own_ports = 0;
            watching = true;
        }
        session->set_horizon(horizon());
        if (read == claim_at && watching) {
            std::printf("[netplay] read %llu: claiming %s, as --netplay-claim-at asked\n", (unsigned long long)read,
                ports_text((uint8_t)(1u << claim_port)).c_str());
            session->claim(claim_port);
        }
        if (read == seat_at && session->hosting()) {
            std::printf("[netplay] read %llu: seating spectator %d in %s, as --netplay-seat-at asked\n", (unsigned long long)read,
                seat_spectator + 1, ports_text((uint8_t)(1u << seat_port)).c_str());
            conker::lockstep::seat(seat_port, seat_spectator, session->roster().watchers[seat_spectator].player);
        }
        if (!initial_sent) {
            watch_for_quit();
            if (watching) {
                tell("Watching: L and R switch between everyone and each player.");
            }
        }
        if (watching) {
            choose_view(pads[0].button);
        }
        // This machine's players' input for read + delay, and for any read before it not sent yet: at the first read,
        // the reads before it (nobody has played yet), and as the delay goes up, those it skips (the controllers'
        // pads now again: the movement goes with the first). As it goes down, the reads up to it are sent already.
        if (read + delay < UINT32_MAX - netplay::input_window) {
            for (int local = 0; local < std::popcount(own_ports); local++) {
                const int port = nth_own_port(local);
                const uint64_t up_to = read + (uint64_t)delay_at(port, read);
                for (uint64_t r = received[port].size(); r <= up_to; r++) {
                    send_own(r, port, source(r, initial_sent || r == up_to ? pads : nullptr, local));
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
            lag_between_spikes = lag_then;
            netplay::set_fake_network(fake);
            std::printf("[netplay] read %llu: the lag is %d ms now (--netplay-fake-network)\n", (unsigned long long)read, lag_then);
        }
        if (faking && spike_every > 0) {
            if (lag_between_spikes < 0) lag_between_spikes = fake.lag_ms;
            const int lag = read >= spike_every && read % spike_every < spike_reads ? spike_lag : lag_between_spikes;
            if (fake.lag_ms != lag) {
                fake.lag_ms = lag;
                netplay::set_fake_network(fake);
            }
        }
        if (const auto stall = stalls.find(read); stall != stalls.end()) {
            const long stall_ms = stall->second;
            std::printf("[netplay] read %llu: the game stops for %ld ms (--netplay-stall-at)\n", (unsigned long long)read, stall_ms);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
            lock.lock();
            if (stopping) {
                return false;
            }
        }
    }
    last_made = read;
    if (read == kick_at && session != nullptr && session->hosting()) {
        std::printf("[netplay] read %llu: kicking %s, as --netplay-kick-at asked\n", (unsigned long long)read, ports_text(1u << kick_port).c_str());
        session->kick(kick_port);
    }
    if (std::find(desync_at.begin(), desync_at.end(), read) != desync_at.end()) {
        rdram[recomp::save_states::game_memory_size / 2] ^= 0x5A;
        std::printf("[netplay] read %llu: game memory changed, as --netplay-desync-at asked\n", (unsigned long long)read);
    }
    // With rollback the game may not be right yet (a prediction): the hash waits until it's known to be (verify).
    if (read % hash_every == 0) {
        const OwnHash own{ memory.hash(), memory.keep(read) };
        // A read made again whose hash was sent was right then (the one gone back to can be): the same again,
        // and the others take one hash a read.
        if (rollback_option > 0) {
            if (own_hashes.empty() || read > own_hashes.rbegin()->first) unverified_hashes[(uint32_t)read] = own;
        }
        else {
            send_hash((uint32_t)read, own);
        }
    }
    if (again) {
        return give_inputs(read, pads, motions);
    }

    // With rollback, every input up to the window before this read (the others' predicted after it), but every
    // one in the boot's first reads, and near a replay's end (it ends with every read right).
    // (A spectator never predicts: nobody waits for it, so it may as well wait for every input.)
    const uint64_t needed = rollback_option == 0 || watching || read < rollback_from_read || read + rollback_option >= last_read
        ? read : read - rollback_option;
    // Desync recovery: a Resync again if the host's state couldn't be taken before its read (the game is past it).
    if (session->hosting() && recovery.read != never && read >= recovery.read && recovery.state.empty()) {
        std::printf("[netplay] read %llu: this game couldn't be taken before read %llu: again later\n", (unsigned long long)read,
            (unsigned long long)recovery.read);
        if (recovery.rejoin) {
            announce_back(read, recovery.ports, true, recovery.late);
        }
        else {
            announce_resync(read, recovery.ports, true);
        }
        if (ended) {
            return false;
        }
    }
    const double since = now();
    double last_note = since;
    uint8_t waiting_for = 0;
    uint8_t waited_on = 0;      // the ports this read waited for at some point, from when till when
    std::array<double, netplay::max_players> missing_at{};
    std::array<double, netplay::max_players> missing_last{};
    while (true) {
        // (Before waiting: the machine loading it waits for it before its inputs for this read can come.)
        send_resync_state(read);
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
        // (This machine let go of: it comes back instead.)
        if (rejoining) {
            return rejoining_read(read, pads, motions, lock);
        }
        const uint8_t missing_ports = missing(needed);
        if (missing_ports == 0) {
            break;
        }
        for (int port = 0; port < match_players; port++) {
            if ((missing_ports & (1u << port)) == 0) continue;
            missing_last[port] = now();
            if (missing_at[port] == 0) missing_at[port] = missing_last[port];
        }
        waited_on |= missing_ports;
        if (missing_ports != waiting_for) {
            waiting_for = missing_ports;
            publish(waiting_for, since);
        }
        for (int port = 0; port < match_players; port++) {
            if (received[port].size() <= needed && gone[port] && needed < idle_from[port]) {
                end_match(Reason::quit, gone_silently ? "a player stopped answering (they may have quit)" : "a player left");
                return false;
            }
        }
        const double t = now();
        if (session->hosting()) {
            let_go_of_stalled(read, missing_ports, t, missing_at);
        }
        // (While a state is on its way to a player, longer: they wait for it.)
        if (t - since > wait_seconds + (t - recovery.sent_at < state_wait_seconds ? state_wait_seconds : 0)) {
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

    // A stall goes on only while reads wait for that machine a while, one after another. (A read may wait long for one
    // machine while another's inputs came a moment late: that one isn't stalled.)
    for (int port = 0; port < netplay::max_players; port++) {
        if (missing_at[port] == 0 || missing_last[port] - missing_at[port] < stall_reset_seconds) stall_since[port] = 0;
    }

    // How far behind the others this peer is: how many reads ahead of it they are, by how far their inputs reach.
    int64_t behind = INT64_MIN;
    for (int port = 0; port < match_players; port++) {
        if ((own_ports & (1u << port)) == 0 && idle_from[port] == never && !only_idle_since_back(port)) {
            behind = std::max(behind, read_sending(port, (int64_t)received[port].size() - 1) - (int64_t)read);
        }
    }
    last_behind = behind;
    bool catch_up = false;
    if (watching) {
        catch_up = behind >= spectator_catch_up;
    }
    else if (rollback_catch_up(read)) {
        const int gap = widest_gap_to_me();
        wide_gap_for = gap >= std::max(2, rollback_option / 2 + delay) ? wide_gap_for + 1 : 0;
        catch_up = behind >= 1 && (gap >= std::max(2, rollback_option + delay - 2) ||
            (behind >= behind_to_catch_up && wide_gap_for >= catch_up_after_reads));
    }
    else {
        catch_up = behind >= behind_to_catch_up;
    }
    if (!catching_up && catch_up) {
        wide_gap_for = 0;
        std::printf("[netplay] read %llu: %lld reads behind the others: catching up\n", (unsigned long long)read, (long long)behind);
        set_catching_up(true);
        caught_up_from = read;
        catching_up_since = now();
        waits.catch_ups++;
    }
    else if (catching_up && behind <= (watching ? spectator_caught_up : 0)) {
        std::printf("[netplay] read %llu: caught up (%llu reads in %.0f ms)\n", (unsigned long long)read,
            (unsigned long long)(read - caught_up_from), (now() - catching_up_since) * 1000);
        set_catching_up(false);
        wide_gap_for = 0;
    }
    sync_time();

    const double done = now();
    const double waited = done - since;
    // Lockstep: a read that waited long for a port, at the game's own pace (the boot's first reads wait for everyone, as
    // the games load: those aren't counted, nor while catching up). With rollback a read waits only past the window.
    if (rollback_option == 0 && read >= rollback_from_read && !catching_up && waited * 1000 > long_wait_ms) {
        for (int port = 0; port < match_players; port++) {
            if ((waited_on & (1u << port)) != 0) waited_long_for_port[port]++;
        }
    }
    if (waits.reads > 0 && done - waits.last_read_at > late_seconds) {
        waits.late++;
        // (With rollback, not the boot's: everyone waits for every input there, as the games load.)
        if (waited > 0.001 && (rollback_option == 0 || read >= rollback_from_read)) {
            for (int port = 0; port < match_players; port++) {
                if ((waited_on & (1u << port)) != 0) waited_for_port[port]++;
            }
        }
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
    if (delay_option == auto_delay && session->hosting() && read % hash_every == 0) {
        adapt_delay(read);
    }
    if (session->hosting()) {
        // Desync recovery: the next Resync, once the last state is sent.
        if (recovery.read == never && recovery.wanted != 0) {
            const uint8_t ports = recovery.wanted;
            recovery.wanted = 0;
            announce_resync(read, ports);
            tell(players_text(ports) + (std::popcount(ports) == 1 ? "'s game" : "' games") +
                " went a different way: sending them the host's...");
        }
        // Coming back (or joining under way): the next Back, likewise; not in the boot's first reads (a player can join
        // an open port as the game starts), whose states can't be loaded once it's past them. Spectators coming in, or
        // whose game went another way, likewise (with no Back: nobody waits for them).
        else if (recovery.read == never && (rejoin_wanted != 0 ||
            ((spectators_late_wanted != 0 || spectators_resync_wanted != 0) && now() - spectator_state_at > spectator_state_seconds)) &&
            read >= rejoin_boot_reads && (session->arriving() == 0 || now() - rejoin_wanted_at > rejoin_gather_seconds)) {
            const uint8_t ports = rejoin_wanted;
            const uint8_t late = late_wanted;
            const uint8_t was_lost = (uint8_t)(lost_ports & ports);
            rejoin_wanted = 0;
            late_wanted = 0;
            announce_back(read, ports, false, late);
            if ((ports & late) != 0) tell(players_text(ports & late) + " joined: sending them the host's game...");
            if ((ports & ~late & ~was_lost) != 0) tell(players_text((uint8_t)(ports & ~late & ~was_lost)) + " came back: sending them the host's game...");
            if (was_lost != 0) tell(players_text(was_lost) + " is back: sending them the host's game...");
        }
        seat_next(read);
        // (As held ports become free too: the listing changes only when what it says does.)
        list_room();
        beacon_match();
    }
    else if (recovery.desync_read != never && read > recovery.desync_found_at + resync_wait_reads &&
        (!watching || now() - recovery.desync_found_time > spectator_resync_wait_seconds)) {
        end_match(Reason::desync, "the games went different ways (a desync), and the host didn't send its game");
        return false;
    }
    publish();

    if (rollback_option > 0) {
        find_mispredictions(read);
        for (int port = 0; port < match_players; port++) {
            if (!known(port, read)) {
                rollbacks.predicted++;
                break;
            }
        }
        given.resize(read + 1);
        frontier = read + 1;
    }
    // A joiner loading the host's game: after the read before it, till it's here (the harness then loads it, take_state).
    if (recovery.read == read + 1 && !recovery.ready && !session->hosting()) {
        const double from = now();
        std::printf("[netplay] read %llu: waiting for the host's game\n", (unsigned long long)read);
        while (recovery.read == read + 1 && !recovery.ready) {
            for (const Event& event : session->poll()) {
                handle(event);
            }
            compare_hashes();
            if (ended) {
                return false;
            }
            if (rejoining) {
                return rejoining_read(read, pads, motions, lock);
            }
            if (now() - from > state_wait_seconds) {
                end_match(Reason::timeout, "the host's game didn't come");
                return false;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            lock.lock();
            if (stopping) {
                return false;
            }
        }
        std::printf("[netplay] waited %.0f ms for the host's game\n", (now() - from) * 1000);
    }
    return give_inputs(read, pads, motions);
}

int conker::lockstep::rollback_window() {
    return ::rollback_option;
}

uint64_t conker::lockstep::take_rollback() {
    std::scoped_lock lock(session_mutex);
    const uint64_t to = rollback_to;
    rollback_to = never;
    // A state taken for a Resync after that read is of the game being replaced: it's taken again as it's played again.
    if (to != never && to < recovery.read) {
        recovery.state.clear();
    }
    return to;
}

uint64_t conker::lockstep::state_wanted() {
    std::scoped_lock lock(session_mutex);
    return session != nullptr && session->hosting() && !ended && recovery.state.empty() ? recovery.read : never;
}

void conker::lockstep::give_state(uint64_t read, const std::vector<uint8_t>& runtime, const std::vector<uint8_t>& host) {
    std::scoped_lock lock(session_mutex);
    if (read != recovery.read || !recovery.state.empty()) {
        return;
    }
    if (!pack_state(runtime, host, recovery.state, recovery.hash)) {
        std::printf("[netplay] this game before read %llu couldn't be packed\n", (unsigned long long)read);
    }
}

uint64_t conker::lockstep::take_state(uint64_t next_read, std::vector<uint8_t>& runtime, std::vector<uint8_t>& host) {
    std::scoped_lock lock(session_mutex);
    // (Coming back, not before the game has started: rejoining_read waits for it from there.)
    if (ended || !recovery.ready || (rejoining ? next_read < rejoin_boot_reads : recovery.read != next_read)) {
        return never;
    }
    runtime = std::move(recovery.runtime);
    host = std::move(recovery.host);
    recovery.runtime.clear();
    recovery.host.clear();
    recovery.ready = false;
    return recovery.read;
}

void conker::lockstep::state_loaded(uint64_t read) {
    std::scoped_lock lock(session_mutex);
    // The reads before it are the host's, all right: nothing to compare, check or go back to before it.
    std::erase_if(own_hashes, [read](const auto& h) { return h.first < read; });
    std::erase_if(unverified_hashes, [read](const auto& h) { return h.first < read; });
    std::erase_if(their_hashes, [read](const auto& h) { return h.first.second < read; });
    if (rollback_option > 0) {
        frontier = std::max(frontier, read);
        given.resize(std::max<size_t>(given.size(), read));
        verified = std::max(verified, read);
        for (uint64_t& c : checked) c = std::max(c, read);
        rollback_to = never;
        last_made = read - 1;
    }
    recovery.loaded_read = read;
    recovery.read = never;
    recovery.desync_read = never;
    if (rejoining) {
        // Back in the match: its reads from here are the match's.
        rejoining = false;
        lost_here = false;
        current_read = read;
        std::printf("[netplay] %s the match: the host's game was loaded before read %llu\n", watching ? "watching" : joining_late ? "in" : "back in",
            (unsigned long long)read);
        tell(watching ? "Watching the match." : joining_late ? "In the match." : "Back in the match.");
        return;
    }
    recovery.count++;
    std::printf("[netplay] the host's game was loaded before read %llu: the match goes on\n", (unsigned long long)read);
    tell("The host's game was loaded: the match goes on.");
}

void conker::lockstep::state_failed(const char* why) {
    std::scoped_lock lock(session_mutex);
    // This machine's game is wrong, the others' aren't: it leaves (its ports go idle), rather than ending it for all.
    std::printf("[netplay] the host's game couldn't be loaded: %s\n", why);
    end_match(Reason::quit, "the host's game couldn't be loaded here after a desync");
}

uint64_t conker::lockstep::verified_reads() {
    std::scoped_lock lock(session_mutex);
    return verified;
}

void conker::lockstep::set_last_read(uint64_t read) {
    last_read = read;
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

bool conker::lockstep::can_come_back() {
    std::scoped_lock lock(session_mutex);
    return session != nullptr && !session->hosting() && own_ports != 0 && ended && !host_closed &&
        end_reason != Reason::desync && end_reason != Reason::kicked;
}

std::string conker::lockstep::joined_code() {
    std::scoped_lock lock(session_mutex);
    return session != nullptr && !session->hosting() ? join_where : std::string();
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
