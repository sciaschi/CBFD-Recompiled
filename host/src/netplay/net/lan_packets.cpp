// LAN discovery's beacon (lan_packets.hpp): encoding, and decoding with every field checked. fuzz_packets.cpp
// fuzzes it.

#include "lan_packets.hpp"
#include "packets.hpp"

namespace conker::netplay::lan {
    namespace {
        bool valid(const Beacon& b) {
            return b.fit != 0 && (uint8_t)b.platform >= 1 && b.platform <= signal::Platform::last && b.port != 0 && b.key != 0 &&
                b.players >= 2 && b.players <= max_players && b.joined >= 1 && b.joined <= b.players && b.rollback <= max_rollback &&
                b.watching <= max_spectators;
        }
    }

    bool encode(const Beacon& beacon, std::span<uint8_t, beacon_size> out) {
        if (!valid(beacon)) {
            return false;
        }
        uint8_t* p = out.data();
        const auto u8 = [&p](uint8_t v) { *p++ = v; };
        const auto u16 = [&u8](uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); };
        const auto u64 = [&u8](uint64_t v) { for (int i = 0; i < 8; i++) u8((uint8_t)(v >> (8 * i))); };
        u8('C');
        u8('L');
        u16(version);
        u64(beacon.fit);
        u8((uint8_t)beacon.platform);
        u16(beacon.port);
        u64(beacon.key);
        u8(beacon.players);
        u8(beacon.joined);
        u8((uint8_t)((beacon.playing ? 1 : 0) | (beacon.spectators ? 2 : 0)));
        u8(beacon.rollback);
        u8(beacon.watching);
        return true;
    }

    std::optional<Beacon> decode(std::span<const uint8_t> data) {
        if (data.size() != beacon_size || data[0] != 'C' || data[1] != 'L') {
            return std::nullopt;
        }
        const uint8_t* p = data.data() + 2;
        const auto u8 = [&p]() { return *p++; };
        const auto u16 = [&p]() { const uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; };
        const auto u64 = [&p]() { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); p += 8; return v; };
        if (u16() != version) {
            return std::nullopt;
        }
        Beacon b;
        b.fit = u64();
        b.platform = (signal::Platform)u8();
        b.port = u16();
        b.key = u64();
        b.players = u8();
        b.joined = u8();
        const uint8_t flags = u8();
        b.rollback = u8();
        b.watching = u8();
        b.playing = (flags & 1) != 0;
        b.spectators = (flags & 2) != 0;
        if ((flags & ~3) != 0 || !valid(b)) {
            return std::nullopt;
        }
        return b;
    }
}
