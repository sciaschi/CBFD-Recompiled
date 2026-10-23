// Netplay's packets: everything peers send each other, and the only code that reads
// what arrives from the network. Each kind of packet has a fixed size and a version, and decode()
// checks every field: anything else is refused whole.
//
// Peers send inputs only: each its own ports' pads (one port per player on that machine) (the host passes them on to the others), the hashes
// of the game's memory for the desync check, and the handshake. Never files, mods or save states.
//
// The layout, little-endian: "CN", u16 version, u8 kind, then the kind's fields (see packets.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace conker::netplay {
    // Changes whenever any packet's layout or meaning does: peers with another one are refused.
    constexpr uint16_t protocol_version = 13;
    // The game's controller ports.
    constexpr int max_players = 4;
    // The most reads an Inputs packet carries: a peer catching up can send several at once. (Inputs are
    // sent reliably, so none is lost: lockstep can't go on without every one.)
    constexpr int input_window = 8;
    // The largest packet, to size buffers.
    constexpr size_t max_packet_size = 288;
    // The most reads a peer's own pads are sent ahead of the game (the input delay).
    constexpr int max_delay = 8;

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
    };
    constexpr uint8_t last_reason = (uint8_t)Reason::key;
    const char* describe(Reason reason);

    // Joiner to host, first: the match's key, then what it's running. The host checks the key first, and
    // refuses a wrong one before anything else; then it refuses any difference.
    struct Hello {
        uint64_t build = 0;     // hash of the game's build
        uint64_t rom = 0;       // XXH3-64 of the ROM
        uint64_t profile = 0;   // hash of the netplay settings profile
        uint64_t mods = 0;      // hash of the mods on and their settings, texture packs aside (0: none)
        uint64_t key = 0;       // the match's key, from the join code (the host's own: its random key)
        uint8_t players = 1;    // the players on the joiner's machine, each with a port of their own (1 to 3)
        bool operator==(const Hello&) const = default;
    };
    // Host to joiner: accepted, on these ports (bit n: port n; one for each of its players, never the
    // host's port 0), in a match of this many players.
    struct Welcome {
        uint8_t ports = 0;
        uint8_t players = 0;
        bool operator==(const Welcome&) const = default;
    };
    // Host to joiner: not accepted, then the connection is closed.
    struct Refuse {
        Reason reason = Reason::protocol;
        bool operator==(const Refuse&) const = default;
    };
    // One port's pads and movement for reads first_read to first_read + count - 1.
    struct Inputs {
        uint8_t port = 0;
        uint32_t first_read = 0;
        uint8_t count = 0;
        std::array<Pad, input_window> pads{};   // the first `count` are used, the rest are zero
        std::array<Motion, input_window> motions{};
        bool operator==(const Inputs&) const = default;
    };
    // The game's memory in regions of this size for the desync check: a Hash names the regions, so a desync
    // says where the games went different ways (8 MB in 128).
    constexpr uint32_t memory_region_size = 64 * 1024;
    constexpr int memory_regions = 128;
    // The XXH3-64 of the game's memory as the sender made read `read`, and each region's, cut to its low 16
    // bits (enough to tell which differ, once the whole hash says something does).
    struct Hash {
        uint32_t read = 0;
        uint64_t hash = 0;
        std::array<uint16_t, memory_regions> regions{};
        bool operator==(const Hash&) const = default;
    };
    // Host to everyone, once every port is taken: the match starts, with this many players, whether every
    // player sees only their own view of the split screen (the host's rule for the match: 1) or each chooses
    // (0), the input delay everyone uses (0 to max_delay), and the ports whose views are shown two to a window
    // (bit n: port n; the ports of each machine with two players), which the culls go by in "my view only".
    struct Start {
        uint8_t players = 0;
        uint8_t own_view = 0;
        uint8_t delay = 0;
        uint8_t pairs = 0;
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
    // others can tell the players they may have quit.
    struct Gone {
        uint8_t port = 0;
        uint32_t from_read = 0;
        uint8_t silent = 0;
        bool operator==(const Gone&) const = default;
    };

    // Host to the joiners: the input delay becomes `delay` (0 to max_delay) from read `from_read`, as the
    // connection changes. Every peer switches at that read, which is past any a joiner can be at as it
    // arrives; the reads a raised delay skips get each player's last pad again, and a lowered one's live
    // pads for the reads already sent aren't sent (lockstep.cpp).
    struct Delay {
        uint8_t delay = 0;
        uint32_t from_read = 0;
        bool operator==(const Delay&) const = default;
    };

    using Packet = std::variant<Hello, Welcome, Refuse, Inputs, Hash, Bye, Start, Gone, Delay>;

    // Writes `packet` into `out` and returns its size, or 0 if the packet isn't valid (the same checks
    // as decode's), so nothing invalid is ever sent.
    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of this protocol version.
    std::optional<Packet> decode(std::span<const uint8_t> data);
}
