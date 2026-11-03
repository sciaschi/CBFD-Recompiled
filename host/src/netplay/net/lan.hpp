// LAN discovery: a host that's visible on its network broadcasts a beacon (lan_packets.hpp) on a UDP port
// of its own (lan::default_port) about once a second, and sends it to this machine too (a firewall may keep
// the network's broadcasts out unless that port is open, but not what comes from the machine itself, which
// reaches one of the games listening on it there); a
// player looking for matches listens there and lists the ones that fit its game (the same fit, platforms that play together). Joining one is joining by
// its join code, made from the beacon's sender, port and key: no server, no code to pass on. Anyone on the
// network can join such a match (the key is in the beacon), so a host only broadcasts if it chose to.
//
// It's for a network whose machines are trusted, like a home or a LAN party: anyone on it can copy a
// beacon. The listener keeps only its own game's matches, at most 2 from each address, and a match keeps
// the address it was first heard from (a copy sent from elsewhere later doesn't move it); but a machine
// that copies a beacon before the real one is heard, or floods made-up ones from many addresses, can still
// mislead it.
//
// Plain UDP sockets, apart from GameNetworkingSockets (the library has no broadcasts), non-blocking.
// Single-threaded: poll from one thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lan_packets.hpp"

namespace conker::netplay {
    // A host's broadcasts.
    class LanBeacon {
    public:
        LanBeacon() = default;
        ~LanBeacon();
        LanBeacon(const LanBeacon&) = delete;
        LanBeacon& operator=(const LanBeacon&) = delete;
        // Opens its socket, to broadcast to `port` (false: `error` says why).
        bool open(uint16_t port, std::string& error);
        // Broadcasts `beacon` (the caller's pace: about once a second).
        void send(const lan::Beacon& beacon);
        // Whether it also sends to this machine (a test turns that off, to try the broadcast alone).
        void send_to_this_machine(bool on) { to_this_machine = on; }
        void close();
    private:
        intptr_t socket_handle = -1;
        uint16_t to_port = 0;
        bool to_this_machine = true;
    };

    // A match a beacon told of: its host's address and port, as a join code takes them ("192.168.1.20:27015"),
    // its join code, and the beacon.
    struct LanMatch {
        std::string address;
        std::string code;
        lan::Beacon beacon;
    };

    // Listening for beacons.
    class LanListener {
    public:
        LanListener() = default;
        ~LanListener();
        LanListener(const LanListener&) = delete;
        LanListener& operator=(const LanListener&) = delete;
        // The game whose matches it keeps: `fit` (signalling.hpp's fit_of) on `platform`. Before open.
        void listen_for(uint64_t fit, signal::Platform platform) { want_fit = fit; want_platform = platform; }
        // Opens its socket on `port`, shared with any other game on this machine listening there (false:
        // `error` says why).
        bool open(uint16_t port, std::string& error);
        // Takes the beacons that have arrived. `now` is in seconds, on any steady clock.
        void poll(double now);
        // The matches heard from in the last few seconds: open ones first, then those being played.
        std::vector<LanMatch> matches() const;
        void close();
    private:
        struct Heard {
            LanMatch match;
            double at = 0;
        };
        intptr_t socket_handle = -1;
        std::vector<Heard> heard;
        uint64_t want_fit = 0;
        signal::Platform want_platform = signal::Platform::gnu_linux;
    };
}
