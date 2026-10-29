// A netplay session over GameNetworkingSockets: the host listens on a UDP port and the
// others connect to it directly, by its join code (its address, port and the match's key). Connections
// are encrypted by the library (they aren't authenticated: there is no certificate authority, so someone
// in the path could still pose as the host). All that's sent are the packets in packets.hpp, reliably
// (resent until they arrive, in order); the library closes a connection that sends a bigger message, and
// buffers little from each peer.
//
// The handshake: a joiner sends Hello (the match's key, then build, ROM, settings profile and mods
// hashes) within 3 s. The host closes a wrong key at once, telling it nothing
// else; then it refuses any other difference, or a full or started match, and otherwise gives it the
// lowest free ports, one for each player on its machine (the Hello says how many; the host's players have
// the first ports, from 0), with Welcome. Once every port is taken, the host starts the match
// (Start: with the input delay). Then each peer sends its own ports' pads and
// movement (Inputs) and memory hashes (Hash) to the host, which passes Inputs on to the others: the host is
// the hub, so joiners only ever connect to it. A peer that sends anything else (another peer's port's
// inputs, a second Hello) is dropped.
//
// Single-threaded: call poll() often (each controller read, or in a loop) from one thread; it runs the
// library's callbacks and returns what happened.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "packets.hpp"

namespace conker::netplay {
    struct Event {
        enum class Type {
            joined,     // host: a joiner passed the handshake, on `ports`
            welcomed,   // joiner: accepted, on `ports`, in a match of `players`
            started,    // joiner: every port is taken and the match starts
            packet,     // `packet` arrived (Inputs, Hash, or for a joiner the host's Gone or Delay), from `port`'s peer
                        // (via the host for a joiner; Gone: the port gone)
            left,       // host: the peer on `ports` left or was dropped, for `reason`
            ended,      // joiner: refused, or the connection to the host ended, for `reason`
        };
        Type type;
        int port = -1;          // the peer's first port (packet: the Inputs' port, for a joiner)
        uint8_t ports = 0;      // joined, welcomed, left: the peer's ports (bit n: port n)
        int players = 0;
        bool own_view = false;  // started: the host's rule that every player sees only their own view
        int delay = 0;          // started: the match's input delay
        uint8_t pairs = 0;      // started: the ports shown two to a window (each machine with two players')
        Reason reason = Reason::quit;
        std::string detail;     // the library's account of a closed connection, for the log
        Packet packet;
    };

    // The join code (join_code.hpp) for `address` ("1.2.3.4:5000" or "[::1]:5000") and a match's `key`, or
    // empty if it isn't an address and port.
    std::string join_code_for(const std::string& address, uint64_t key);

    // Starts and stops the library, once per process, around any sessions.
    bool startup(std::string& error);
    void shutdown();

    // A bad network, faked by the library for tests (lockstep's --netplay-fake-network): what this process
    // sends is held back `lag_ms`, plus jitter (an average of `jitter_ms`, at most 4 times that), `loss`% of
    // its packets dropped, `reorder`% held back 20 ms more (so they arrive out of order) and `dup`% sent twice.
    // `outage`: everything this process sends and receives is dropped, until it's set false.
    struct FakeNetwork {
        int lag_ms = 0;
        float jitter_ms = 0;
        float loss = 0;
        float reorder = 0;
        float dup = 0;
        bool outage = false;
        bool operator==(const FakeNetwork&) const = default;
    };
    // Sets it for every connection, once the library is started.
    void set_fake_network(const FakeNetwork& fake);

    class Session {
    public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Hosts a match of `players` (2 to 4) on UDP `port`, on every local address, with a new random key;
        // `local` of them (fewer than `players`) play on this machine, on the first ports.
        bool host(uint16_t port, int players, const Hello& me, std::string& error, int local = 1);
        // Host: the join code for this match at `address` (the host's, as the others reach it), or empty
        // if it isn't an address and port.
        std::string join_code(const std::string& address) const;
        // Joins the host by its join code (`me`'s key is the code's; its players, this machine's).
        bool join(const std::string& code, const Hello& me, std::string& error);
        // Host: once every port is taken (false until then), starts the match: tells the joiners, and
        // refuses any more. `own_view`: every player sees only their own view (the match's rule); `delay`:
        // the input delay everyone uses.
        bool start(bool own_view = false, int delay = 2);
        // Host: the round trip to each joiner, in milliseconds, as the library measures it (none it doesn't
        // know yet), for choosing the input delay.
        std::vector<int> round_trips_ms() const;
        // The round trip to each port's machine, in milliseconds, for the player to see (-1: not known, or not
        // another machine's port): the host's to every joiner's ports; a joiner's to the host, on port 0 (it
        // can't tell which other ports are the host's, and the other joiners' pads come through the host).
        std::array<int, max_players> round_trips_by_port() const;
        // Host: the joiners in (machines, not players: one may have several ports).
        int joiners() const;
        // Host: the ports whose views are shown two to a window (each machine with two players', the host's too),
        // sent with Start.
        uint8_t pairs() const;

        // What has happened since the last poll.
        std::vector<Event> poll();

        // One of this peer's own ports' pads (a joiner's go to the host, the host's to everyone).
        void send(const Inputs& inputs);
        // This peer's memory hash (a joiner's go to the host, the host's to everyone).
        void send(const Hash& hash);
        // Host: a player has left, to the joiners still in (after their last inputs, passed on as they came).
        void send(const Gone& gone);
        // Host: the input delay changes (Delay), to the joiners still in.
        void send(const Delay& delay);
        // Says goodbye to every peer and closes the connections, once what was sent has arrived (up to 2 s,
        // for a lossy network). Poll a moment before deleting the session or stopping the library.
        void leave(Reason reason);
        // The furthest read a peer's Inputs may reach (their first read plus their count); beyond it they're
        // refused as broken or hostile, before the host passes them on. Lockstep sets it as the game goes;
        // until then, the first reads only.
        void set_horizon(uint64_t reads) { horizon = reads; }

        bool hosting() const { return is_host; }
        uint8_t ports() const { return own_ports; }
        int players() const { return match_players; }

    private:
        struct Peer {
            uint32_t connection = 0;
            int port = -1;          // its first port; -1 until the handshake is done
            uint8_t ports = 0;      // all of them (bit n: port n)
            double connected_at = 0;
            int64_t last_hash = -1; // the read of the last Hash from it (one per read, in order)
        };

        void on_status_changed(const void* info);
        void handle(Peer& peer, const Packet& packet, std::vector<Event>& events);
        // A message that arrived on `connection`: decoded and handled, or its peer dropped.
        void take(uint32_t connection, std::span<const uint8_t> data, std::vector<Event>& events);
        // Takes every message still unread from `connection` (a peer that closed it).
        void drain(uint32_t connection, std::vector<Event>& events);
        void drop(Peer& peer, Reason reason, std::vector<Event>* events);
        // Waits until every peer has acknowledged what was sent to it (up to 2 s).
        void flush();
        void send_to(uint32_t connection, const Packet& packet);
        Peer* find(uint32_t connection);
        friend void status_changed_callback(void* info);

        bool is_host = false;
        bool entry_open = true;
        bool started = false;       // joiner: Start has arrived
        uint8_t own_ports = 0;      // joiner: 0 until welcomed
        int match_players = 0;
        uint64_t horizon = 64;
        std::array<uint64_t, max_players> next_inputs{};   // host: the read each port's next Inputs start at
        Hello hello;
        uint32_t listen_socket = 0;
        uint32_t poll_group = 0;
        std::vector<Peer> peers;            // host: everyone connected; joiner: the host
        std::vector<Event> pending_events;  // from the status callback, until the next poll
    };
}
