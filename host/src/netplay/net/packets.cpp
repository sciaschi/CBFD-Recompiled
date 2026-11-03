// Netplay's packets (packets.hpp): encoding, and decoding with every field checked. This is the only
// code that parses what arrives from the network; fuzz_packets.cpp fuzzes it.
//
// Each packet: "CN", u16 protocol version, u8 kind, then (all little-endian):
//   1 Hello    u64 build, u64 rom, u64 profile, u64 mods, u64 key,                    55 bytes
//              u8 players (0 to 3), u64 player (not 0), u8 platform (1 to 3)
//   2 Welcome  u8 ports (bits: none at or past players, not port 0; none: a spectator), 12 bytes
//              u8 players (2 to 4), u8 rejoin (0 to 2), then as Start's (all 0 unless
//              rejoin): u8 own view (0 or 1), u8 pairs, u8 rollback (0 to 12),
//              u8 characters (0, 12 or 16)
//   3 Refuse   u8 reason, u8 size (0 to 40), the host's release (! to ~), zeros to 40  47 bytes
//              (the same in every version from 17 on: decode_refusal)
//   4 Inputs   u8 port (< 4), u32 first read, u8 count (1 to 8), 8 pads of 5 bytes  161 bytes
//              (u16 buttons, s8 stick x, s8 stick y, u8 err_no), then 8 motions of 13
//              (s16 mouse x, y, s16 gyro x, y, s8 wheel, u32 controls: only
//              valid_controls: a field of view code up to fov_codes, no bit past 16); the unused ones zero, and the last read not past
//              2^32 - 1; then 4 s8 gaps (time sync: any value), u16 controls (an early copy's: any)
//   5 Hash     u32 read, u64 hash, 128 u16 region hashes, 4 u8 spreads (any),         285 bytes
//              4 u8 reads waited (any), 4 u8 reads waited long (any)
//   6 Bye      u8 reason                                                               6 bytes
//   7 Start    u8 players (2 to 4), u8 own view (0 or 1), 4 u8 delays (0 to 8),      15 bytes
//              u8 pairs
//              (ports below players, an even number of them), u8 rollback (0 to 12),
//              u8 open (ports below players, not port 0, none of the pairs),
//              u8 characters (0, 12 or 16)
//   8 Gone     u8 port (1 to 3: never the host's 0), u32 from read,                    11 bytes
//              u8 silent (0 to 3: stopped answering, stepped out to watch, let go)
//   9 Delay    u8 ports (bits: some, below 4), u8 delay (0 to 8), u32 from read        11 bytes
//  10 Roster   4 seats of 12: u64 player, u8 platform, u8 ready, u16 round trip;    131 bytes
//              (a free seat, player 0, all zero; a taken one platform 1 to 3, ready
//              0 or 1; none taken at or past players), u8 players (2 to 4),
//              u8 own view (0 or 1), u8 delay (0 to 8, or 255: auto), u8 rollback (0 to 12),
//              u8 spectators (0 or 1), u8 characters (0, 12 or 16), then 8 watchers of 9: u64 player, u8 platform (none:
//              all zero; else platform 1 to 3)
//  11 Ready    u8 ready (0 or 1)                                                       6 bytes
//  12 Resync   u32 read, u8 ports (bits: not port 0; none: a spectator)               10 bytes
//  13 StatePart u32 read, u32 size (1 to max_state_size), u32 offset, u64 hash,   26 to 25 + max_state_part bytes
//              then 1 to max_state_part bytes, offset + their count not past size
//  14 Back     u8 ports (bits: some, not port 0), u32 read, u8 late (some of ports),  19 bytes
//              u64 player (0, or a spectator's: then one port, and late)
//  15 Rejoin   u32 read, u32 at (not below read), for each port u32 delay from,     126 bytes
//              u8 delay before, u8 delay after (both 0 to 8), then for each port u32 idle from
//              (2^32 - 1, or not below read), a pad (as Inputs'), a motion (as Inputs');
//              u8 open (idle ports, not port 0)
//  16 Claim    u8 port (1 to 3)                                                        6 bytes
//  17 Unseat   u8 ports (bits: some, not port 0)                                       6 bytes
// A packet of any other size, version or kind, or with any field out of range, is refused.

#include "packets.hpp"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace conker::netplay {
    namespace {
        enum Kind : uint8_t { hello = 1, welcome, refuse, inputs, hash, bye, start, gone, delay, roster, ready, resync, state_part, back, rejoin, claim, unseat };
        constexpr size_t header_size = 5;
        constexpr size_t pad_size = 5;
        constexpr size_t motion_size = 13;

        constexpr size_t size_of(uint8_t kind) {
            switch (kind) {
                case hello: return header_size + 50;
                case welcome: return header_size + 7;
                case refuse: return header_size + 2 + max_release_name;
                case inputs: return header_size + 6 + input_window * (pad_size + motion_size) + max_players + 2;
                case hash: return header_size + 12 + 2 * memory_regions + 3 * max_players;
                case bye: return header_size + 1;
                case start: return header_size + 6 + max_players;
                case gone: return header_size + 6;
                case delay: return header_size + 6;
                case roster: return header_size + 12 * max_players + 6 + 9 * max_spectators;
                case ready: return header_size + 1;
                case resync: return header_size + 5;
                case back: return header_size + 14;
                case claim: return header_size + 1;
                case unseat: return header_size + 1;
                case rejoin: return header_size + 9 + max_players * (6 + 4 + pad_size + motion_size);
            }
            return 0;
        }
        static_assert(size_of(inputs) <= max_packet_size && size_of(hash) <= max_packet_size && size_of(rejoin) <= max_packet_size &&
            size_of(roster) <= max_packet_size);
        // StatePart's fixed fields, before its bytes.
        constexpr size_t state_part_head = header_size + 20;
        static_assert(state_part_head + max_state_part == max_message_size);

        bool valid_reason(Reason reason) {
            return (uint8_t)reason >= 1 && (uint8_t)reason <= last_reason;
        }

        // Each kind's fields (plain overloads: MSVC misreads `p.count < 1 || p.count > n` in a generic
        // lambda as a template's arguments).
        bool valid_fields(const Hello& p) {
            return p.players < max_players && p.player != 0 && p.platform >= 1 && p.platform <= 3;
        }
        bool valid_fields(const Roster& p) {
            if (p.players < 2 || p.players > max_players || p.own_view > 1 || (p.delay > max_delay && p.delay != auto_delay) ||
                p.rollback > max_rollback || p.spectators > 1 || !valid_characters(p.characters)) {
                return false;
            }
            for (const Watcher& w : p.watchers) {
                if (w.player == 0 ? !(w == Watcher{}) : w.platform < 1 || w.platform > 3) {
                    return false;
                }
            }
            for (int port = 0; port < max_players; port++) {
                const Seat& s = p.seats[port];
                if (s.player == 0 ? !(s == Seat{}) : port >= p.players || s.platform < 1 || s.platform > 3 || s.ready > 1) {
                    return false;
                }
            }
            return true;
        }
        bool valid_fields(const Ready& p) {
            return p.ready <= 1;
        }
        // A match's pairs: ports below its players, an even number of them.
        bool valid_pairs(uint8_t pairs, uint8_t players) {
            return (pairs >> players) == 0 && std::popcount(pairs) % 2 == 0;
        }
        bool valid_fields(const Welcome& p) {
            return p.players >= 2 && p.players <= max_players && (p.ports & 1) == 0 &&
                (p.ports >> p.players) == 0 && p.rejoin <= 2 &&
                (p.rejoin == 0 ? p.own_view == 0 && p.pairs == 0 && p.rollback == 0 && p.characters == 0
                               : p.own_view <= 1 && valid_pairs(p.pairs, p.players) && p.rollback <= max_rollback &&
                                     valid_characters(p.characters));
        }
        bool valid_release(const std::string& release) {
            return release.size() <= max_release_name && printable_word(release);
        }
        bool valid_fields(const Refuse& p) {
            return valid_reason(p.reason) && valid_release(p.release);
        }
        // The first refusal-carrying version: Refuse's layout is kept from it on.
        constexpr uint16_t refuse_kept_since = 17;
        bool valid_fields(const Inputs& p) {
            if (p.port >= max_players || p.count < 1 || p.count > input_window ||
                p.first_read > UINT32_MAX - (uint32_t)(p.count - 1)) {
                return false;
            }
            for (int i = 0; i < input_window; i++) {
                if (i < p.count ? !valid(p.pads[i]) || !valid_controls(p.motions[i].controls)
                                : !(p.pads[i] == Pad{} && p.motions[i] == Motion{})) {
                    return false;
                }
            }
            return true;
        }
        bool valid_fields(const Hash&) {
            return true;
        }
        bool valid_fields(const Bye& p) {
            return valid_reason(p.reason);
        }
        bool valid_fields(const Gone& p) {
            return p.port >= 1 && p.port < max_players && p.silent <= 3;
        }
        bool valid_fields(const Delay& p) {
            return p.ports != 0 && (p.ports >> max_players) == 0 && p.delay <= max_delay;
        }
        bool valid_fields(const Start& p) {
            return p.players >= 2 && p.players <= max_players && p.own_view <= 1 &&
                std::all_of(p.delays.begin(), p.delays.end(), [](uint8_t d) { return d <= max_delay; }) &&
                valid_pairs(p.pairs, p.players) && p.rollback <= max_rollback && (p.open & 1) == 0 && (p.open >> p.players) == 0 &&
                (p.open & p.pairs) == 0 && valid_characters(p.characters);
        }

        bool valid_fields(const Resync& p) {
            return (p.ports & 1) == 0 && (p.ports >> max_players) == 0;
        }
        bool valid_fields(const StatePart& p) {
            return p.size >= 1 && p.size <= max_state_size && !p.bytes.empty() && p.bytes.size() <= max_state_part &&
                p.offset < p.size && p.bytes.size() <= p.size - p.offset;
        }

        bool valid_fields(const Back& p) {
            return p.ports != 0 && (p.ports & 1) == 0 && (p.ports >> max_players) == 0 && (p.late & ~p.ports) == 0 &&
                (p.player == 0 || (std::popcount(p.ports) == 1 && p.late == p.ports));
        }
        bool valid_fields(const Claim& p) {
            return p.port >= 1 && p.port < max_players;
        }
        bool valid_fields(const Unseat& p) {
            return p.ports != 0 && (p.ports & 1) == 0 && (p.ports >> max_players) == 0;
        }
        bool valid_fields(const Rejoin& p) {
            if (p.at < p.read || (p.open & 1) != 0 || (p.open >> max_players) != 0) {
                return false;
            }
            for (int port = 0; port < max_players; port++) {
                if (p.delay_before[port] > max_delay || p.delay_after[port] > max_delay ||
                    (p.idle_from[port] != no_read && p.idle_from[port] < p.read) || ((p.open & (1u << port)) != 0 && p.idle_from[port] == no_read) ||
                    !valid(p.pads[port]) ||
                    !valid_controls(p.motions[port].controls)) {
                    return false;
                }
            }
            return true;
        }

        bool valid_packet(const Packet& packet) {
            return std::visit([](const auto& p) { return valid_fields(p); }, packet);
        }

        struct Writer {
            uint8_t* at;
            void u8(uint8_t v) { *at++ = v; }
            void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
            void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
            void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
            void pad(const Pad& p) {
                u16(p.buttons);
                u8((uint8_t)p.stick_x);
                u8((uint8_t)p.stick_y);
                u8(p.err_no);
            }
            void motion(const Motion& m) {
                u16((uint16_t)m.mouse_x);
                u16((uint16_t)m.mouse_y);
                u16((uint16_t)m.gyro_x);
                u16((uint16_t)m.gyro_y);
                u8((uint8_t)m.wheel);
                u32(m.controls);
            }
        };

        // Reads a packet whose size has already been checked.
        struct Reader {
            const uint8_t* at;
            uint8_t u8() { return *at++; }
            uint16_t u16() { uint16_t v = u8(); return (uint16_t)(v | (u8() << 8)); }
            uint32_t u32() { uint32_t v = u16(); return v | ((uint32_t)u16() << 16); }
            uint64_t u64() { uint64_t v = u32(); return v | ((uint64_t)u32() << 32); }
            Pad pad() {
                Pad p;
                p.buttons = u16();
                p.stick_x = (int8_t)u8();
                p.stick_y = (int8_t)u8();
                p.err_no = u8();
                return p;
            }
            Motion motion() {
                Motion m;
                m.mouse_x = (int16_t)u16();
                m.mouse_y = (int16_t)u16();
                m.gyro_x = (int16_t)u16();
                m.gyro_y = (int16_t)u16();
                m.wheel = (int8_t)u8();
                m.controls = u32();
                return m;
            }
        };
    }

    bool valid(const Pad& pad) {
        return (pad.buttons & reserved_buttons) == 0 && (pad.err_no == 0 || pad.err_no == err_no_controller);
    }

    const char* describe(Reason reason) {
        switch (reason) {
            case Reason::quit: return "the player left";
            case Reason::version: return "a different version of netplay";
            case Reason::build: return "a different build of the game";
            case Reason::rom: return "a different ROM";
            case Reason::profile: return "different netplay settings";
            case Reason::full: return "the match is full";
            case Reason::started: return "the match has already started";
            case Reason::protocol: return "an unexpected or broken packet";
            case Reason::desync: return "the games went out of sync";
            case Reason::timeout: return "the connection timed out";
            case Reason::mods: return "mods on (netplay allows none but texture packs)";
            case Reason::key: return "not this match's join code";
            case Reason::kicked: return "the host removed this player";
            case Reason::watching: return "the host takes no spectators";
        }
        return "an unknown reason";
    }

    size_t encode(const Packet& packet, std::span<uint8_t> out) {
        if (!valid_packet(packet)) {
            return 0;
        }
        const StatePart* part = std::get_if<StatePart>(&packet);
        if (out.size() < (part != nullptr ? state_part_head + part->bytes.size() : size_of((uint8_t)(packet.index() + 1)))) {
            return 0;
        }
        // The kinds are numbered in Packet's order.
        const uint8_t kind = (uint8_t)(packet.index() + 1);
        Writer w{ out.data() };
        w.u8('C');
        w.u8('N');
        w.u16(protocol_version);
        w.u8(kind);
        std::visit([&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Hello>) {
                w.u64(p.build);
                w.u64(p.rom);
                w.u64(p.profile);
                w.u64(p.mods);
                w.u64(p.key);
                w.u8(p.players);
                w.u64(p.player);
                w.u8(p.platform);
            }
            else if constexpr (std::is_same_v<T, Roster>) {
                for (const Seat& s : p.seats) {
                    w.u64(s.player);
                    w.u8(s.platform);
                    w.u8(s.ready);
                    w.u16(s.round_trip_ms);
                }
                w.u8(p.players);
                w.u8(p.own_view);
                w.u8(p.delay);
                w.u8(p.rollback);
                w.u8(p.spectators);
                w.u8(p.characters);
                for (const Watcher& watcher : p.watchers) {
                    w.u64(watcher.player);
                    w.u8(watcher.platform);
                }
            }
            else if constexpr (std::is_same_v<T, Ready>) {
                w.u8(p.ready);
            }
            else if constexpr (std::is_same_v<T, Welcome>) {
                w.u8(p.ports);
                w.u8(p.players);
                w.u8(p.rejoin);
                w.u8(p.own_view);
                w.u8(p.pairs);
                w.u8(p.rollback);
                w.u8(p.characters);
            }
            else if constexpr (std::is_same_v<T, Refuse>) {
                w.u8((uint8_t)p.reason);
                w.u8((uint8_t)p.release.size());
                for (size_t i = 0; i < max_release_name; i++) {
                    w.u8(i < p.release.size() ? (uint8_t)p.release[i] : 0);
                }
            }
            else if constexpr (std::is_same_v<T, Bye>) {
                w.u8((uint8_t)p.reason);
            }
            else if constexpr (std::is_same_v<T, Inputs>) {
                w.u8(p.port);
                w.u32(p.first_read);
                w.u8(p.count);
                for (const Pad& pad : p.pads) {
                    w.pad(pad);
                }
                for (const Motion& m : p.motions) {
                    w.motion(m);
                }
                for (int8_t gap : p.gaps) {
                    w.u8((uint8_t)gap);
                }
                w.u16(p.controls);
            }
            else if constexpr (std::is_same_v<T, Hash>) {
                w.u32(p.read);
                w.u64(p.hash);
                for (uint16_t region : p.regions) {
                    w.u16(region);
                }
                for (uint8_t spread : p.spread) {
                    w.u8(spread);
                }
                for (uint8_t waited : p.waited) {
                    w.u8(waited);
                }
                for (uint8_t waited : p.waited_long) {
                    w.u8(waited);
                }
            }
            else if constexpr (std::is_same_v<T, Gone>) {
                w.u8(p.port);
                w.u32(p.from_read);
                w.u8(p.silent);
            }
            else if constexpr (std::is_same_v<T, Delay>) {
                w.u8(p.ports);
                w.u8(p.delay);
                w.u32(p.from_read);
            }
            else if constexpr (std::is_same_v<T, Start>) {
                w.u8(p.players);
                w.u8(p.own_view);
                for (uint8_t d : p.delays) {
                    w.u8(d);
                }
                w.u8(p.pairs);
                w.u8(p.rollback);
                w.u8(p.open);
                w.u8(p.characters);
            }
            else if constexpr (std::is_same_v<T, Resync>) {
                w.u32(p.read);
                w.u8(p.ports);
            }
            else if constexpr (std::is_same_v<T, Back>) {
                w.u8(p.ports);
                w.u32(p.read);
                w.u8(p.late);
                w.u64(p.player);
            }
            else if constexpr (std::is_same_v<T, Claim>) {
                w.u8(p.port);
            }
            else if constexpr (std::is_same_v<T, Unseat>) {
                w.u8(p.ports);
            }
            else if constexpr (std::is_same_v<T, Rejoin>) {
                w.u32(p.read);
                w.u32(p.at);
                for (int port = 0; port < max_players; port++) {
                    w.u32(p.delay_from[port]);
                    w.u8(p.delay_before[port]);
                    w.u8(p.delay_after[port]);
                }
                for (int port = 0; port < max_players; port++) {
                    w.u32(p.idle_from[port]);
                    w.pad(p.pads[port]);
                    w.motion(p.motions[port]);
                }
                w.u8(p.open);
            }
            else if constexpr (std::is_same_v<T, StatePart>) {
                w.u32(p.read);
                w.u32(p.size);
                w.u32(p.offset);
                w.u64(p.hash);
                std::copy(p.bytes.begin(), p.bytes.end(), w.at);
                w.at += p.bytes.size();
            }
        }, packet);
        return (size_t)(w.at - out.data());
    }

    std::optional<Packet> decode(std::span<const uint8_t> data) {
        if (data.size() < header_size || data[0] != 'C' || data[1] != 'N' ||
            (uint16_t)(data[2] | (data[3] << 8)) != protocol_version) {
            return std::nullopt;
        }
        const uint8_t kind = data[4];
        if (kind == state_part ? data.size() <= state_part_head || data.size() > max_message_size
                               : size_of(kind) == 0 || data.size() != size_of(kind)) {
            return std::nullopt;
        }
        Reader r{ data.data() + header_size };
        Packet packet;
        switch (kind) {
            case hello: {
                Hello p;
                p.build = r.u64();
                p.rom = r.u64();
                p.profile = r.u64();
                p.mods = r.u64();
                p.key = r.u64();
                p.players = r.u8();
                p.player = r.u64();
                p.platform = r.u8();
                packet = p;
                break;
            }
            case roster: {
                Roster p;
                for (Seat& s : p.seats) {
                    s.player = r.u64();
                    s.platform = r.u8();
                    s.ready = r.u8();
                    s.round_trip_ms = r.u16();
                }
                p.players = r.u8();
                p.own_view = r.u8();
                p.delay = r.u8();
                p.rollback = r.u8();
                p.spectators = r.u8();
                p.characters = r.u8();
                for (Watcher& w : p.watchers) {
                    w.player = r.u64();
                    w.platform = r.u8();
                }
                packet = p;
                break;
            }
            case ready:
                packet = Ready{ r.u8() };
                break;
            case welcome: {
                Welcome p;
                p.ports = r.u8();
                p.players = r.u8();
                p.rejoin = r.u8();
                p.own_view = r.u8();
                p.pairs = r.u8();
                p.rollback = r.u8();
                p.characters = r.u8();
                packet = p;
                break;
            }
            case refuse: {
                Refuse p{ (Reason)r.u8() };
                const uint8_t length = r.u8();
                if (length > max_release_name) {
                    return std::nullopt;
                }
                for (size_t i = 0; i < max_release_name; i++) {
                    const uint8_t c = r.u8();
                    if (i < length) {
                        p.release += (char)c;
                    }
                    else if (c != 0) {
                        return std::nullopt;
                    }
                }
                packet = std::move(p);
                break;
            }
            case inputs: {
                Inputs p;
                p.port = r.u8();
                p.first_read = r.u32();
                p.count = r.u8();
                for (Pad& pad : p.pads) {
                    pad = r.pad();
                }
                for (Motion& m : p.motions) {
                    m = r.motion();
                }
                for (int8_t& gap : p.gaps) {
                    gap = (int8_t)r.u8();
                }
                p.controls = r.u16();
                packet = p;
                break;
            }
            case hash: {
                Hash p;
                p.read = r.u32();
                p.hash = r.u64();
                for (uint16_t& region : p.regions) {
                    region = r.u16();
                }
                for (uint8_t& spread : p.spread) {
                    spread = r.u8();
                }
                for (uint8_t& waited : p.waited) {
                    waited = r.u8();
                }
                for (uint8_t& waited : p.waited_long) {
                    waited = r.u8();
                }
                packet = p;
                break;
            }
            case bye:
                packet = Bye{ (Reason)r.u8() };
                break;
            case start:
                {
                    Start p;
                    p.players = r.u8();
                    p.own_view = r.u8();
                    for (uint8_t& d : p.delays) {
                        d = r.u8();
                    }
                    p.pairs = r.u8();
                    p.rollback = r.u8();
                    p.open = r.u8();
                    p.characters = r.u8();
                    packet = p;
                }
                break;
            case gone: {
                Gone p;
                p.port = r.u8();
                p.from_read = r.u32();
                p.silent = r.u8();
                packet = p;
                break;
            }
            case delay: {
                Delay p;
                p.ports = r.u8();
                p.delay = r.u8();
                p.from_read = r.u32();
                packet = p;
                break;
            }
            case resync: {
                Resync p;
                p.read = r.u32();
                p.ports = r.u8();
                packet = p;
                break;
            }
            case back: {
                Back p;
                p.ports = r.u8();
                p.read = r.u32();
                p.late = r.u8();
                p.player = r.u64();
                packet = p;
                break;
            }
            case claim:
                packet = Claim{ r.u8() };
                break;
            case unseat:
                packet = Unseat{ r.u8() };
                break;
            case rejoin: {
                Rejoin p;
                p.read = r.u32();
                p.at = r.u32();
                for (int port = 0; port < max_players; port++) {
                    p.delay_from[port] = r.u32();
                    p.delay_before[port] = r.u8();
                    p.delay_after[port] = r.u8();
                }
                for (int port = 0; port < max_players; port++) {
                    p.idle_from[port] = r.u32();
                    p.pads[port] = r.pad();
                    p.motions[port] = r.motion();
                }
                p.open = r.u8();
                packet = p;
                break;
            }
            case state_part: {
                StatePart p;
                p.read = r.u32();
                p.size = r.u32();
                p.offset = r.u32();
                p.hash = r.u64();
                p.bytes.assign(r.at, data.data() + data.size());
                packet = std::move(p);
                break;
            }
        }
        if (!valid_packet(packet)) {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<Refuse> decode_refusal(std::span<const uint8_t> data) {
        if (data.size() != size_of(refuse) || data[0] != 'C' || data[1] != 'N' || data[4] != refuse ||
            (uint16_t)(data[2] | (data[3] << 8)) < refuse_kept_since) {
            return std::nullopt;
        }
        // As this version's, but for the version and a reason it may not know.
        std::array<uint8_t, size_of(refuse)> own;
        std::copy(data.begin(), data.end(), own.begin());
        own[2] = (uint8_t)protocol_version;
        own[3] = (uint8_t)(protocol_version >> 8);
        if (!valid_reason((Reason)own[header_size])) {
            own[header_size] = (uint8_t)Reason::version;
        }
        std::optional<Packet> packet = decode(std::span<const uint8_t>(own));
        if (!packet) {
            return std::nullopt;
        }
        return std::get<Refuse>(*packet);
    }
}
