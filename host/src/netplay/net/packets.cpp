// Netplay's packets (packets.hpp): encoding, and decoding with every field checked. This is the only
// code that parses what arrives from the network; fuzz_packets.cpp fuzzes it.
//
// Each packet: "CN", u16 protocol version, u8 kind, then (all little-endian):
//   1 Hello    u64 build, u64 rom, u64 profile, u64 mods, u64 key,                    46 bytes
//              u8 players (1 to 3)
//   2 Welcome  u8 ports (bits: some, none at or past players, not port 0),             7 bytes
//              u8 players (2 to 4)
//   3 Refuse   u8 reason                                                               6 bytes
//   4 Inputs   u8 port (< 4), u32 first read, u8 count (1 to 8), 8 pads of 5 bytes  155 bytes
//              (u16 buttons, s8 stick x, s8 stick y, u8 err_no), then 8 motions of 13
//              (s16 mouse x, y, s16 gyro x, y, s8 wheel, u32 controls: only
//              valid_controls: a field of view code up to fov_codes, no bit past 16); the unused ones zero, and the last read not past
//              2^32 - 1
//   5 Hash     u32 read, u64 hash, 128 u16 region hashes                             273 bytes
//   6 Bye      u8 reason                                                               6 bytes
//   7 Start    u8 players (2 to 4), u8 own view (0 or 1), u8 delay (0 to 8), u8 pairs  9 bytes
//              (ports below players, an even number of them)
//   8 Gone     u8 port (1 to 3: never the host's 0), u32 from read,                    11 bytes
//              u8 silent (0 or 1: stopped answering)
//   9 Delay    u8 delay (0 to 8), u32 from read                                        10 bytes
// A packet of any other size, version or kind, or with any field out of range, is refused.

#include "packets.hpp"

#include <bit>
#include <type_traits>

namespace conker::netplay {
    namespace {
        enum Kind : uint8_t { hello = 1, welcome, refuse, inputs, hash, bye, start, gone, delay };
        constexpr size_t header_size = 5;
        constexpr size_t pad_size = 5;
        constexpr size_t motion_size = 13;

        constexpr size_t size_of(uint8_t kind) {
            switch (kind) {
                case hello: return header_size + 41;
                case welcome: return header_size + 2;
                case refuse: return header_size + 1;
                case inputs: return header_size + 6 + input_window * (pad_size + motion_size);
                case hash: return header_size + 12 + 2 * memory_regions;
                case bye: return header_size + 1;
                case start: return header_size + 4;
                case gone: return header_size + 6;
                case delay: return header_size + 5;
            }
            return 0;
        }
        static_assert(size_of(inputs) <= max_packet_size && size_of(hash) <= max_packet_size);

        bool valid_reason(Reason reason) {
            return (uint8_t)reason >= 1 && (uint8_t)reason <= last_reason;
        }

        // Each kind's fields (plain overloads: MSVC misreads `p.count < 1 || p.count > n` in a generic
        // lambda as a template's arguments).
        bool valid_fields(const Hello& p) {
            return p.players >= 1 && p.players < max_players;
        }
        bool valid_fields(const Welcome& p) {
            return p.players >= 2 && p.players <= max_players && p.ports != 0 && (p.ports & 1) == 0 &&
                (p.ports >> p.players) == 0;
        }
        bool valid_fields(const Refuse& p) {
            return valid_reason(p.reason);
        }
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
            return p.port >= 1 && p.port < max_players && p.silent <= 1;
        }
        bool valid_fields(const Delay& p) {
            return p.delay <= max_delay;
        }
        bool valid_fields(const Start& p) {
            return p.players >= 2 && p.players <= max_players && p.own_view <= 1 && p.delay <= max_delay &&
                (p.pairs >> p.players) == 0 && std::popcount(p.pairs) % 2 == 0;
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
        };

        // Reads a packet whose size has already been checked.
        struct Reader {
            const uint8_t* at;
            uint8_t u8() { return *at++; }
            uint16_t u16() { uint16_t v = u8(); return (uint16_t)(v | (u8() << 8)); }
            uint32_t u32() { uint32_t v = u16(); return v | ((uint32_t)u16() << 16); }
            uint64_t u64() { uint64_t v = u32(); return v | ((uint64_t)u32() << 32); }
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
        }
        return "an unknown reason";
    }

    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out) {
        if (!valid_packet(packet)) {
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
            }
            else if constexpr (std::is_same_v<T, Welcome>) {
                w.u8(p.ports);
                w.u8(p.players);
            }
            else if constexpr (std::is_same_v<T, Refuse> || std::is_same_v<T, Bye>) {
                w.u8((uint8_t)p.reason);
            }
            else if constexpr (std::is_same_v<T, Inputs>) {
                w.u8(p.port);
                w.u32(p.first_read);
                w.u8(p.count);
                for (const Pad& pad : p.pads) {
                    w.u16(pad.buttons);
                    w.u8((uint8_t)pad.stick_x);
                    w.u8((uint8_t)pad.stick_y);
                    w.u8(pad.err_no);
                }
                for (const Motion& m : p.motions) {
                    w.u16((uint16_t)m.mouse_x);
                    w.u16((uint16_t)m.mouse_y);
                    w.u16((uint16_t)m.gyro_x);
                    w.u16((uint16_t)m.gyro_y);
                    w.u8((uint8_t)m.wheel);
                    w.u32(m.controls);
                }
            }
            else if constexpr (std::is_same_v<T, Hash>) {
                w.u32(p.read);
                w.u64(p.hash);
                for (uint16_t region : p.regions) {
                    w.u16(region);
                }
            }
            else if constexpr (std::is_same_v<T, Gone>) {
                w.u8(p.port);
                w.u32(p.from_read);
                w.u8(p.silent);
            }
            else if constexpr (std::is_same_v<T, Delay>) {
                w.u8(p.delay);
                w.u32(p.from_read);
            }
            else if constexpr (std::is_same_v<T, Start>) {
                w.u8(p.players);
                w.u8(p.own_view);
                w.u8(p.delay);
                w.u8(p.pairs);
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
        if (size_of(kind) == 0 || data.size() != size_of(kind)) {
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
                packet = p;
                break;
            }
            case welcome: {
                Welcome p;
                p.ports = r.u8();
                p.players = r.u8();
                packet = p;
                break;
            }
            case refuse:
                packet = Refuse{ (Reason)r.u8() };
                break;
            case inputs: {
                Inputs p;
                p.port = r.u8();
                p.first_read = r.u32();
                p.count = r.u8();
                for (Pad& pad : p.pads) {
                    pad.buttons = r.u16();
                    pad.stick_x = (int8_t)r.u8();
                    pad.stick_y = (int8_t)r.u8();
                    pad.err_no = r.u8();
                }
                for (Motion& m : p.motions) {
                    m.mouse_x = (int16_t)r.u16();
                    m.mouse_y = (int16_t)r.u16();
                    m.gyro_x = (int16_t)r.u16();
                    m.gyro_y = (int16_t)r.u16();
                    m.wheel = (int8_t)r.u8();
                    m.controls = r.u32();
                }
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
                    p.delay = r.u8();
                    p.pairs = r.u8();
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
                p.delay = r.u8();
                p.from_read = r.u32();
                packet = p;
                break;
            }
        }
        if (!valid_packet(packet)) {
            return std::nullopt;
        }
        return packet;
    }
}
