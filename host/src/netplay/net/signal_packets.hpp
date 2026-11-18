// Signalling's packets: what a peer and the signalling server (signalling.hpp) send each other, so that
// GameNetworkingSockets' ICE can find a way between two peers (through NAT, or a TURN relay). The server
// only passes signals on, between the ids its peers registered; it never sees the game's packets, which
// go over the connection ICE makes. This is the only code that reads what arrives from the server or a
// peer of it: every field is checked, and anything else is refused whole.
//
// The server also keeps rooms: a host creates one (with its match's key) and is given a room code
// (join_code.hpp) to pass on; a joiner looks the code up and is given the host's id and the key, to connect
// to it through ICE. A room lasts as long as its host stays connected to the server (the whole match, so
// that someone could rejoin it later). And the server may offer each peer its TURN relay, with credentials
// of its own that expire (Turn).
//
// A room's host may list it in public too (Publish), and anyone can ask for the listed rooms they could
// join (ListRooms): those of a game with the same build, ROM, settings and mods (one hash of them, the
// fit: the server only compares it) on a platform that plays with theirs (the server decides which mix),
// and how many more there are of the same game (build and ROM: another hash) with other settings or mods. A listed room
// has no name on the wire: the game makes one from its number (join_code.hpp: room_name). From version 6
// a room carries its release's name too (hello.hpp: release_name), and the list has the rooms of the same
// ROM on other builds besides (marked: they can't be joined, but the player sees which release to get),
// and how many rooms there are of older signalling versions (which can't say their ROM or release). From version 7
// a room says how many are watching it and whether it takes spectators.
//
// And a player whose game went another way than another's (a desync) may tell the server, which logs it
// for its operator (DesyncReport): a summary, with no address, memory or inputs in it.
//
// The layout, little-endian: "CS", u16 version, u8 kind, then the kind's fields (see signal_packets.cpp).
// New kinds come with a new version, which leaves the older kinds as they were: so the server still speaks
// the older versions it can (oldest_version on), each to the peers that speak it, and keeps them apart (a
// peer only reaches, and finds the rooms of, peers of its own version).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace conker::netplay::signal {
    // Changes whenever any packet's layout or meaning does: a peer with another one is refused.
    constexpr uint16_t version = 7;
    // The oldest version the server still speaks: 2 (no public rooms: Publish, ListRooms and Rooms came with 3;
    // DesyncReport with 4; a listed room's host player id with 5; its release, the ROM's hash alone and the
    // rooms of other builds with 6; a room's spectators with 7).
    constexpr uint16_t oldest_version = 2;
    // The largest signal GameNetworkingSockets sends (4200 bytes of message data at most, plus its
    // own), with room to spare.
    constexpr size_t max_signal = 8192;
    // The most rooms one Rooms lists.
    constexpr size_t max_listed = 32;
    // The longest release name (hello.hpp's release_name).
    constexpr size_t max_release = 40;
    // The largest packet, to size buffers and the library's message limit.
    constexpr size_t max_packet_size = 5 + 10 + max_signal;
    static_assert(5 + 5 + max_listed * (22 + max_release) <= max_packet_size, "a full Rooms fits");

    // Peer to server, first and once: the id it's reached by (never 0). The library's identity for this
    // process is made from it (session.hpp: local_id()).
    struct Register {
        uint64_t id = 0;
        bool operator==(const Register&) const = default;
    };
    // Peer to server: to pass on to `peer`; server to peer: from `peer`. `data` is the library's own
    // (1 to max_signal bytes), handed to it as it came.
    struct Signal {
        uint64_t peer = 0;
        std::vector<uint8_t> data;
        bool operator==(const Signal&) const = default;
    };
    // Server to peer: nobody has registered `peer` (gone, or never there).
    struct Unreachable {
        uint64_t peer = 0;
        bool operator==(const Unreachable&) const = default;
    };
    // Server to peer: its Register is taken, under `id`; only then can others reach it.
    struct Registered {
        uint64_t id = 0;
        bool operator==(const Registered&) const = default;
    };
    // Server to peer, before closing its connection: why (the id is taken, a broken packet, too many; or
    // too many rooms looked up that weren't there).
    enum class Refusal : uint8_t { version = 1, taken, protocol, busy, last = busy };
    struct Refused {
        Refusal reason = Refusal::protocol;
        bool operator==(const Refused&) const = default;
    };

    // Peer to server, once registered: makes a room for this peer's match, with its key (not 0), until it
    // leaves. One each.
    struct CreateRoom {
        uint64_t key = 0;
        bool operator==(const CreateRoom&) const = default;
    };
    // Server to peer: its room, 1 to max_room (join_code.hpp: the room code).
    struct RoomCreated {
        uint64_t room = 0;
        bool operator==(const RoomCreated&) const = default;
    };
    // Peer to server, once registered: who hosts `room` (1 to max_room)?
    struct FindRoom {
        uint64_t room = 0;
        bool operator==(const FindRoom&) const = default;
    };
    // Server to peer: `room`'s host's id, and the key its match was created with (neither 0).
    struct RoomFound {
        uint64_t room = 0;
        uint64_t host = 0;
        uint64_t key = 0;
        bool operator==(const RoomFound&) const = default;
    };
    // Server to peer: there's no `room` (gone, or never there).
    struct NoRoom {
        uint64_t room = 0;
        bool operator==(const NoRoom&) const = default;
    };
    // Server to peer, before Registered, if it has a TURN relay: where (as Ice::turn: "host:port" separated by
    // commas), and the user and password this peer may use it with, for a while. Each is 1 to max_text
    // printable characters (no spaces), as the library takes them.
    constexpr size_t max_text = 128;
    struct Turn {
        std::string servers;
        std::string user;
        std::string password;
        bool operator==(const Turn&) const = default;
    };

    // The game's platform, as a listed room's host and a peer asking for the list report it.
    enum class Platform : uint8_t { gnu_linux = 1, windows, macos, last = macos };   // not "linux": GCC defines it
    // Whether games on these platforms play together (the server's room list and LAN discovery both go by it): Linux
    // and Windows have, live; macOS hasn't been tried with them.
    constexpr bool platforms_mix(Platform a, Platform b) {
        return (a == Platform::macos) == (b == Platform::macos);
    }
    // A listed room as everyone sees it: its number (the room code's), its match's players, how many are
    // in (the host's too, 1 to players), whether the match is being played (joining it isn't possible
    // then), whether its host allows direct connections, and its rollback (packets.hpp: 0 to max_rollback);
    // and from version 5, its host's player id (packets.hpp's Hello: for block lists; 0, not known: a host
    // of an older version), which versions before it don't carry. From version 6, its release's name (1 to
    // max_release printable characters, no spaces), and in a list, whether it's of another build than the
    // asker's (`other_build`: the server says; never in Publish). From version 7, how many are watching (0 to
    // max_spectators) and whether it takes spectators.
    struct ListedRoom {
        uint64_t room = 0;
        uint8_t players = 2;
        uint8_t joined = 1;
        bool playing = false;
        bool direct = false;
        uint8_t rollback = 0;
        uint64_t host = 0;
        std::string release;
        bool other_build = false;
        uint8_t watching = 0;
        bool spectators = false;
        bool operator==(const ListedRoom&) const = default;
    };
    // Peer to server, once its room is made: lists it in public, as `room` says (its number is ignored:
    // the peer's room), for games whose `fit` (not 0) and platform go with its own; again whenever that
    // changes. It stays listed until its host leaves. `game` (not 0) is its build and ROM's hash alone; from
    // version 6, `rom` (not 0) the ROM's alone.
    struct Publish {
        uint64_t fit = 0;
        uint64_t game = 0;
        uint64_t rom = 0;
        Platform platform = Platform::gnu_linux;
        ListedRoom room;
        bool operator==(const Publish&) const = default;
    };
    // Peer to server, once registered: the listed rooms for a game whose `fit` and `game` (neither 0) and
    // platform these are (and from version 6, `rom`, not 0)? A few each minute.
    struct ListRooms {
        uint64_t fit = 0;
        uint64_t game = 0;
        uint64_t rom = 0;
        Platform platform = Platform::gnu_linux;
        bool operator==(const ListRooms&) const = default;
    };
    // Server to peer: the answer, up to max_listed of them (open ones first), and how many listed rooms of
    // the same game and platforms there are besides, whose fit differs (other settings or mods). From version
    // 6 the rooms of the same ROM on other builds come after those that fit (other_build), and `older` is
    // how many listed rooms there are of older signalling versions on platforms that play with the asker's.
    struct Rooms {
        uint16_t others = 0;
        uint16_t older = 0;
        std::vector<ListedRoom> rooms;
        bool operator==(const Rooms&) const = default;
    };

    // Peer to server, once registered: its game's memory at `read` wasn't `other`'s (port), in the regions set
    // in `regions` (bit n of byte n / 8: region n of packets.hpp's memory_regions). `match` (not 0) tells one
    // match's reports from another's (made from its key: it can't be turned back into it); `build` (not 0)
    // is the game's build hash; `port` is the reporting machine's first; `players` 2 to max_players, ports
    // below it and apart; `rollback` 0 to max_rollback, `delay` 0 to max_delay. A few each connection.
    struct DesyncReport {
        uint32_t match = 0;
        uint64_t build = 0;
        Platform platform = Platform::gnu_linux;
        uint8_t players = 2;
        uint8_t port = 0;
        uint8_t other = 1;
        uint32_t read = 0;
        uint8_t rollback = 0;
        uint8_t delay = 0;
        std::array<uint8_t, 16> regions{};
        bool operator==(const DesyncReport&) const = default;
    };

    // In the kinds' order (a kind's number is its index here, plus 1).
    using Packet = std::variant<Register, Signal, Unreachable, Refused, Registered, CreateRoom, RoomCreated, FindRoom,
        RoomFound, NoRoom, Turn, Publish, ListRooms, Rooms, DesyncReport>;

    // The packet's bytes into `out` (max_packet_size), as version `as` (oldest_version to version) has it:
    // their size, or 0 for a packet that isn't valid (or a kind `as` hasn't).
    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out, uint16_t as = version);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of version `as`.
    std::optional<Packet> decode(std::span<const uint8_t> data, uint16_t as = version);
    // Whether `data` starts like a signalling packet of another version than `as` (to say so, not "broken").
    bool other_version(std::span<const uint8_t> data, uint16_t as = version);
    // The version `data` starts like, if it's one this code speaks (oldest_version to version); 0 if not.
    uint16_t spoken_version(std::span<const uint8_t> data);
}
