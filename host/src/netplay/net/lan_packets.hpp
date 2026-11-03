// LAN discovery's packet: the beacon a host that's visible on its network broadcasts (lan.hpp), about once
// a second, so that players on the same network find its match without a server or a code. It's the join
// code's contents but the address (the beacon's sender is the host), with what the list shows. This is the
// only code that reads what arrives on the LAN port: every field is checked, and anything else is refused
// whole.
//
// The layout, little-endian: "CL", u16 version, then u64 fit (not 0: signalling.hpp's fit_of), u8 platform
// (1 to signal::Platform::last), u16 port (not 0: the host's game port), u64 key (not 0: the match's),
// u8 players (2 to max_players), u8 joined (1 to players), u8 flags (bit 0 playing, bit 1 takes spectators; the
// others 0), u8 rollback (0 to max_rollback), u8 watching (0 to max_spectators): 28 bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "signal_packets.hpp"

namespace conker::netplay::lan {
    // Changes whenever the beacon's layout or meaning does: another version's beacons are ignored.
    constexpr uint16_t version = 2;
    // The UDP port beacons are broadcast to.
    constexpr uint16_t default_port = 27016;
    constexpr size_t beacon_size = 28;

    struct Beacon {
        uint64_t fit = 0;
        signal::Platform platform = signal::Platform::gnu_linux;
        uint16_t port = 0;
        uint64_t key = 0;
        uint8_t players = 2;
        uint8_t joined = 1;
        bool playing = false;
        uint8_t rollback = 0;
        uint8_t watching = 0;
        bool spectators = false;
        bool operator==(const Beacon&) const = default;
    };

    // The beacon's bytes into `out`: false (nothing written) if it isn't valid.
    bool encode(const Beacon& beacon, std::span<uint8_t, beacon_size> out);
    // The beacon in `data`, or nothing if it isn't exactly one valid beacon of this version.
    std::optional<Beacon> decode(std::span<const uint8_t> data);
}
