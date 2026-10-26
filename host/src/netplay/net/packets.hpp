// Netplay's packets: everything peers send each other, and the only code that reads
// what arrives from the network. Each kind of packet has a fixed size and a version, and decode()
// checks every field: anything else is refused whole.
//
// Peers send inputs: each its own ports' pads (one port per player on that machine) (the host passes them on to the others), the hashes
// of the game's memory for the desync check, and the handshake. Never files or mods. The one exception: after a
// desync, the host sends its game's state to the joiners whose game went another way (Resync, StatePart), which
// they check (lockstep.cpp) and load in place of theirs; and to a player who left and comes back (Rejoin).
//
// The layout, little-endian: "CN", u16 version, u8 kind, then the kind's fields (see packets.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace conker::netplay {
    // Changes whenever any packet's layout or meaning does: peers with another one are refused.
    constexpr uint16_t protocol_version = 24;
    // The longest release name a Refuse carries (hello.hpp's release_name; signal_packets.hpp's max_release).
    constexpr size_t max_release_name = 40;
    // Every character of `text` printable, with no spaces (release names, a server's texts): the length is each one's own.
    inline bool printable_word(std::string_view text) {
        for (const char c : text) {
            if (c <= ' ' || c > '~') return false;
        }
        return true;
    }
    // The game's controller ports.
    constexpr int max_players = 4;
    // The most spectators a match takes: machines with no players, watching (they get every input, send none).
    constexpr int max_spectators = 8;
    // The most reads an Inputs packet carries: a peer catching up can send several at once. (Inputs are
    // sent reliably, so none is lost: lockstep can't go on without every one.)
    constexpr int input_window = 8;
    // The largest packet, to size buffers, but StatePart's (max_message_size).
    constexpr size_t max_packet_size = 288;
    // The most bytes of a state one StatePart carries, and the largest message of all (a StatePart's).
    constexpr size_t max_state_part = 64 * 1024;
    constexpr size_t max_message_size = 25 + max_state_part;
    // The largest state sent (compressed): a game's 8 MB and the rest, should it not compress.
    constexpr uint32_t max_state_size = 12 * 1024 * 1024;
    // The most reads a peer's own pads are sent ahead of the game (the input delay).
    constexpr int max_delay = 8;
    // The most reads a peer predicts ahead with rollback (Start).
    constexpr int max_rollback = 12;

    // One controller read of one pad, as the N64 gives it to the game (the harness's 5 bytes).
    struct Pad {
        uint16_t buttons = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t err_no = 0;
        bool operator==(const Pad&) const = default;
    };
    // A player's settings that change what their mouse, gyro, stick and camera do in the game (motion.cpp,
    // field_of_view.cpp, look_aim.cpp): each machine applies every player's own. The bits: 0 mouse Direct, 1 gyro
    // Direct, 2 stick Direct, 3 the mouse doesn't turn the camera, 4-5 mouse invert, 6-7 gyro invert, 8-9 stick
    // invert (0 None, 1 X, 2 Y, 3 both), 10-15 their camera's field of view: 0 the game's own, 1 to fov_codes for
    // 35 to 80 degrees (34 + the code), 16 Aiming: Swap Sticks. A field of view code past fov_codes, or any bit
    // past 16, and the packet is refused.
    using Controls = uint32_t;
    constexpr Controls controls_mask = 0x03FF;  // the bits below the field of view
    constexpr int fov_shift = 10;
    constexpr int fov_codes = 46;
    constexpr Controls swap_sticks_bit = 0x10000;
    constexpr bool valid_controls(Controls c) { return (c >> 17) == 0 && ((c >> fov_shift) & 0x3F) <= fov_codes; }
    // One controller read's mouse and gyro movement for one player, beside their pad: what moved since their
    // previous read, already scaled by their sensitivity (motion.cpp), in fixed point so every machine
    // gives the game exactly the same. The mouse in 1/8 pixel, gyro in halves of recompinput's unit, the
    // scroll wheel in notches; and the player's Controls as they are at that read, so a change of setting
    // takes effect at the same read on every machine.
    struct Motion {
        int16_t mouse_x = 0;
        int16_t mouse_y = 0;
        int16_t gyro_x = 0;
        int16_t gyro_y = 0;
        int8_t wheel = 0;
        Controls controls = 0;
        bool operator==(const Motion&) const = default;
    };

    // The N64's two unused button bits; a pad with them set is refused.
    constexpr uint16_t reserved_buttons = 0x00C0;
    // A pad's err_no: 0, or the N64's "no controller" (CONT_NO_RESPONSE_ERROR).
    constexpr uint8_t err_no_controller = 0x08;
    bool valid(const Pad& pad);

    // Why a peer is refused or a match ends.
    enum class Reason : uint8_t {
        quit = 1,       // the player left
        version,        // another protocol version
        build,          // another build of the game
        rom,            // another ROM
        profile,        // another netplay settings profile
        full,           // every port is taken
        started,        // the match has already started
        protocol,       // a packet that wasn't expected or didn't decode
        desync,         // the game's memory differs between peers
        timeout,        // a peer stopped answering
        mods,           // mods on (texture packs aside): netplay allows none
        key,            // not this match's key (the join code's): nothing else is checked
        kicked,         // the host removed the player (or keeps them out: its block list)
        watching,       // a spectator, and the host takes none
    };
    constexpr uint8_t last_reason = (uint8_t)Reason::watching;
    const char* describe(Reason reason);

    // Joiner to host, first: the match's key, then what it's running. The host checks the key first, and
    // refuses a wrong one before anything else; then it refuses any difference.
    struct Hello {
        uint64_t build = 0;     // hash of the game's build
        uint64_t rom = 0;       // XXH3-64 of the ROM
        uint64_t profile = 0;   // hash of the netplay settings profile
        uint64_t mods = 0;      // hash of the mods on and their settings, texture packs aside (0: none)
        uint64_t key = 0;       // the match's key, from the join code (the host's own: its random key)
        uint8_t players = 1;    // the players on the joiner's machine, each with a port of their own (1 to 3; 0: a spectator)
        // Who's playing, for the waiting room and block lists: the machine's player id (random, kept in its
        // settings; never 0: the menu shows a name made from it, join_code.hpp's player_name), and its
        // platform (signal::Platform's numbers: 1 Linux, 2 Windows, 3 macOS).
        uint64_t player = 0;
        uint8_t platform = 0;
        bool operator==(const Hello&) const = default;
    };
    // The same game (build, ROM, settings profile, mods): what a match needs to be the same everywhere.
    inline bool same_game(const Hello& a, const Hello& b) {
        return a.build == b.build && a.rom == b.rom && a.profile == b.profile && a.mods == b.mods;
    }
    // Host to joiner: accepted, on these ports (bit n: port n; one for each of its players, never the
    // host's port 0; none for a spectator), in a match of this many players. `rejoin` (1): the match is under way, and this is a
    // player who left it coming back to their ports; (2) the match is under way, and this is a new player
    // taking free ones (an open seat, or one whose player left and didn't come back in time): either way their
    // game starts at once, and loads the host's (Rejoin); with the match's rules from Start (0 and none for a new
    // joiner before the start, who gets Start).
    struct Welcome {
        uint8_t ports = 0;
        uint8_t players = 0;
        uint8_t rejoin = 0;
        uint8_t own_view = 0;
        uint8_t pairs = 0;
        uint8_t rollback = 0;
        bool operator==(const Welcome&) const = default;
    };
    // Host to joiner: not accepted, then the connection is closed; and the host's release (empty, or 1 to
    // max_release_name printable characters, no spaces), so a joiner of another one is told which to get.
    // Its layout (and Reason's numbers) stays the same in every protocol version from 17 on, so a joiner of
    // another version can read it too (decode_refusal).
    struct Refuse {
        Reason reason = Reason::protocol;
        std::string release;
        bool operator==(const Refuse&) const = default;
    };
    // One port's pads and movement for reads first_read to first_read + count - 1, and for time sync (rollback),
    // how many reads the sender's game was past each port's latest input it had as it sent them (`gaps`: its own
    // ports 0, -128 to 127).
    struct Inputs {
        uint8_t port = 0;
        uint32_t first_read = 0;
        uint8_t count = 0;
        std::array<Pad, input_window> pads{};   // the first `count` are used, the rest are zero
        std::array<Motion, input_window> motions{};
        std::array<int8_t, max_players> gaps{};
        // An early copy (sent unreliably: session.cpp): how many reliable messages but Inputs and Hashes the sender had
        // sent on this connection before it. It's taken only once those have all arrived, so it never overtakes one
        // (a Delay, a Gone, a Back...) that the inputs after it go by. (Wraps.)
        uint16_t controls = 0;
        bool operator==(const Inputs&) const = default;
    };
    // The game's memory in regions of this size for the desync check: a Hash names the regions, so a desync
    // says where the games went different ways (8 MB in 128).
    constexpr uint32_t memory_region_size = 64 * 1024;
    constexpr int memory_regions = 128;
    // The XXH3-64 of the game's memory as the sender made read `read`, and each region's, cut to its low 16
    // bits (enough to tell which differ, once the whole hash says something does). With it, for the host's
    // input delays (lockstep.cpp): how each port's inputs reach the sender, by port. `spread`: how unevenly they
    // arrive, in 2 ms (how much later than their usual time the latest tenth of them came; no_spread: not known,
    // as the sender's own ports'); `waited`: the reads the sender's game has waited for that port's inputs in all
    // (it wraps); `waited_long`: those of them that waited over lockstep.cpp's long_wait_ms (it wraps too).
    constexpr uint8_t no_spread = 255;
    struct Hash {
        uint32_t read = 0;
        uint64_t hash = 0;
        std::array<uint16_t, memory_regions> regions{};
        std::array<uint8_t, max_players> spread{ no_spread, no_spread, no_spread, no_spread };
        std::array<uint8_t, max_players> waited{};
        std::array<uint8_t, max_players> waited_long{};
        bool operator==(const Hash&) const = default;
    };
    // Host to everyone, once every port is taken: the match starts, with this many players, whether every
    // player sees only their own view of the split screen (the host's rule for the match: 1) or each chooses
    // (0), each port's input delay (0 to max_delay: how many reads ahead its pads are sent), and the ports whose views are shown two to a window
    // (bit n: port n; the ports of each machine with two players), which the culls go by in "my view only", and
    // how many reads every peer predicts the others' inputs ahead (rollback: 0, off, to max_rollback). The host
    // may start before every port is taken: `open` (bit n: port n; never the host's port 0, nor a pair's) are
    // free, idle from read 0 (as for a player who left at once), for players joining later (Welcome's 2).
    struct Start {
        uint8_t players = 0;
        uint8_t own_view = 0;
        std::array<uint8_t, max_players> delays{};
        uint8_t pairs = 0;
        uint8_t rollback = 0;
        uint8_t open = 0;
        bool operator==(const Start&) const = default;
    };
    // Leaving, and why.
    struct Bye {
        Reason reason = Reason::quit;
        bool operator==(const Bye&) const = default;
    };
    // Host to the joiners still in: a player has left (their machine said goodbye or stopped answering), and
    // from read `from_read` their port gets no input (an idle pad: nothing pressed, nothing moved). Sent once
    // every input of theirs the host had is passed on, so it's the read after the last of them for everyone.
    // `silent`: they stopped answering (1: a crash, a lost connection) rather than saying goodbye (0), so the
    // others can tell the players they may have quit; or (2) they stepped out of their place to watch (Unseat), which
    // is open from then on; or (3) their inputs stopped coming for a while, so the others play on without them: their
    // machine (which gets this too) is still connected, and comes back by itself once it can (its Claim of its port),
    // with the host's game as for a player coming back (Rejoin). Should it leave instead, a Gone (0 or 1) of that same
    // read says so (its port idle already).
    struct Gone {
        uint8_t port = 0;
        uint32_t from_read = 0;
        uint8_t silent = 0;
        bool operator==(const Gone&) const = default;
    };

    // Host to the joiners: the input delay of `ports` (bit n: port n; some) becomes `delay` (0 to max_delay) from
    // read `from_read`, as their connection changes. Every peer switches at that read, which is past any a joiner can
    // be at as it arrives; the reads a raised delay skips get each player's last pad again, and a lowered one's live
    // pads for the reads already sent aren't sent (lockstep.cpp).
    struct Delay {
        uint8_t ports = 0;
        uint8_t delay = 0;
        uint32_t from_read = 0;
        bool operator==(const Delay&) const = default;
    };

    // The waiting room, before the match starts. Host to everyone, whenever it changes: each port's player
    // (`player` 0: free; a machine with several players is on several ports), whether they're ready, their
    // platform and the round trip to the host (ms, the host's own 0; 0xFFFF not known yet), and the match's
    // settings: its players, whether everyone sees only their own view, its input delay (0 to max_delay, or
    // auto_delay: chosen as it starts) and its rollback; whether it takes spectators, and theirs (`player` 0: none
    // there; the host's spectator numbers, which stay theirs while they watch). Sent in the match too.
    constexpr uint8_t auto_delay = 0xFF;
    struct Seat {
        uint64_t player = 0;
        uint8_t platform = 0;
        uint8_t ready = 0;
        uint16_t round_trip_ms = 0;
        bool operator==(const Seat&) const = default;
    };
    struct Watcher {
        uint64_t player = 0;
        uint8_t platform = 0;
        bool operator==(const Watcher&) const = default;
    };
    struct Roster {
        std::array<Seat, max_players> seats{};
        uint8_t players = 2;
        uint8_t own_view = 0;
        uint8_t delay = auto_delay;
        uint8_t rollback = 0;
        uint8_t spectators = 1;
        std::array<Watcher, max_spectators> watchers{};
        bool operator==(const Roster&) const = default;
    };
    // Joiner to host, before the start: its players are ready (1) or not (0). The match starts once it's full
    // and everyone's ready (the host's Ready is its Start).
    struct Ready {
        uint8_t ready = 0;
        bool operator==(const Ready&) const = default;
    };

    // Desync recovery. Host to the joiners: the machines on `ports` (bit n: port n; never the host's port 0)
    // load the host's game as it is before read `read` in place of theirs (their game went another way), sent to
    // them in StateParts. Each stops after the read before it until the state is there, which none has made yet as
    // it hears (the host chooses it as it does a Delay's). The others play on. A later Resync replaces it (the
    // host couldn't take its state there). `ports` 0: to a spectator alone, whose game went another way (only it
    // gets that one).
    struct Resync {
        uint32_t read = 0;
        uint8_t ports = 0;
        bool operator==(const Resync&) const = default;
    };
    // Host to a joiner: part of the state a Resync is for, in order: bytes `offset` on of the state (`size` bytes in
    // all, 1 to max_state_size, compressed), whose XXH3-64 once decompressed is `hash`; 1 to max_state_part bytes,
    // none past the end. The only packet of no fixed size: a joiner takes messages up to max_message_size from the
    // host, the host up to max_packet_size from anyone.
    struct StatePart {
        uint32_t read = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
        uint64_t hash = 0;
        std::vector<uint8_t> bytes;
        bool operator==(const StatePart&) const = default;
    };

    // Rejoining: a player who left comes back to their ports. Host to the joiners (not the one coming back):
    // `ports` play again from read `read` (their idle pads until then), which none has made yet as it hears (chosen
    // past the furthest any input can reach, so none of theirs has come). Every peer then waits for their inputs from
    // that read, which come once their machine has loaded the host's game as it is before it. `late`: those of them
    // that are new players (joining the match under way: Welcome's 2), not coming back. `player` (not 0): a spectator
    // with that player id takes the port (one, `late`), its game already the match's: its inputs come from that read.
    struct Back {
        uint8_t ports = 0;
        uint32_t read = 0;
        uint8_t late = 0;
        uint64_t player = 0;
        bool operator==(const Back&) const = default;
    };
    // Seat changes, before the match or in it. Spectator to host: it would take `port` (free: open, or its player's
    // hold over). A player's machine whose ports the host let go of as their inputs stopped coming (Gone's 3), to the
    // host: it's here again, for the host's game, `port` its first.
    struct Claim {
        uint8_t port = 0;
        bool operator==(const Claim&) const = default;
    };
    // Host to a joiner's machine: step out of its `ports` (bit n: port n; all it has) to watch. The machine to the host:
    // it has (asked or not): its inputs end with those sent before this, and it watches from then on (the host tells the
    // others, Gone's 2). Before the start, nothing's been sent: the host just makes it a spectator.
    struct Unseat {
        uint8_t ports = 0;
        bool operator==(const Unseat&) const = default;
    };
    // Host to the player coming back, before the state (StateParts) it loads: the match as it is at read `read`
    // (theirs from it on), the host being at read `at` (the inputs since `read` it has follow, then every one as it
    // comes). Each port's input delay's last change (from read `delay_from`, `delay_before` until it, `delay_after`
    // from it), and each port's input at the read before `read` (what's predicted from, and an idle port's last
    // settings), and the read it plays no more from (no_read: it plays on; else `read` or later); and of the idle
    // ports, those nobody has (`open`: bit n port n, never the host's 0), rather than a player who left.
    constexpr uint32_t no_read = UINT32_MAX;
    struct Rejoin {
        uint32_t read = 0;
        uint32_t at = 0;
        std::array<uint32_t, max_players> delay_from{};
        std::array<uint8_t, max_players> delay_before{};
        std::array<uint8_t, max_players> delay_after{};
        std::array<uint32_t, max_players> idle_from{ no_read, no_read, no_read, no_read };
        std::array<Pad, max_players> pads{};
        std::array<Motion, max_players> motions{};
        uint8_t open = 0;
        bool operator==(const Rejoin&) const = default;
    };

    using Packet = std::variant<Hello, Welcome, Refuse, Inputs, Hash, Bye, Start, Gone, Delay, Roster, Ready, Resync, StatePart,
        Back, Rejoin, Claim, Unseat>;

    // Writes `packet` into `out` and returns its size, or 0 if the packet isn't valid (the same checks
    // as decode's) or doesn't fit, so nothing invalid is ever sent.
    size_t encode(const Packet& packet, std::span<uint8_t> out);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of this protocol version.
    std::optional<Packet> decode(std::span<const uint8_t> data);
    // A Refuse of any protocol version from 17 on (its layout is kept), or nothing. A reason this version
    // doesn't know (a newer one's) is Reason::version.
    std::optional<Refuse> decode_refusal(std::span<const uint8_t> data);
}
