// A peer's side of signalling: one connection to the signalling server (signal_server.cpp), over
// GameNetworkingSockets like everything else, that registers this process's id (session.hpp: local_id())
// and passes the library's ICE signals between it and other peers by their ids (signal_packets.hpp).
// Sessions that connect through ICE (Session::host or join with a Signaller) use it to find a way to each
// other: directly through NAT, or through a TURN relay so neither sees the other's address.
//
// Through it, a host creates a room for its match and gets the room code to pass on (join_code.hpp), and a
// joiner looks the code up to learn the host's id and the match's key. The server may offer its TURN relay
// too, with credentials for this process: ice() makes the session's Ice from them.
//
// A host may list its room in public (publish), and anyone can ask for the listed rooms they could join
// (list_rooms): those of a game with the same build, ROM and settings (fit_of), on a platform that plays
// with this one.
//
// A host keeps it connected for as long as its match lasts, so that its room stays open and someone can
// still reach it then (a player rejoining, later).
//
// Single-threaded like the session, except the outbox: the library sends signals from its own thread, so
// they're only queued, and poll() sends them.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "session.hpp"
#include "signal_packets.hpp"

namespace conker::netplay {
    // Signals waiting to be sent to the server. The library's connections hold it too (session.cpp), as it
    // may still send a signal for a connection after the session and the signaller have gone: then it's
    // dropped with the outbox, once the last of them lets go.
    struct SignalOutbox {
        // Queues a signal for `peer` (any thread): at most max_queued, dropping the oldest (the library
        // resends what it needs).
        void push(uint64_t peer, std::span<const uint8_t> data);
        std::vector<signal::Signal> take();
    private:
        std::mutex mutex;
        std::vector<signal::Signal> queue;
    };

    // "host:port" (a name, IPv4 or [IPv6]) to an address the library takes ("1.2.3.4:5000" or "[::1]:5000"),
    // resolving a name; or empty, with `error`.
    std::string resolve_address(const std::string& host_port, std::string& error);

    // This build's platform, for the room list.
    constexpr signal::Platform this_platform() {
#if defined(_WIN32)
        return signal::Platform::windows;
#elif defined(__APPLE__)
        return signal::Platform::macos;
#else
        return signal::Platform::gnu_linux;
#endif
    }
    // What a listed room must match for a game whose Hello is `hello` (its build, ROM, settings and mods;
    // not its key nor players): one hash of them, never 0. game_of: of its build and ROM alone; rom_of: of
    // its ROM alone (signalling version 6: the rooms of other builds).
    uint64_t fit_of(const Hello& hello);
    uint64_t game_of(const Hello& hello);
    uint64_t rom_of(const Hello& hello);

    class Signaller {
    public:
        Signaller();
        ~Signaller();
        Signaller(const Signaller&) = delete;
        Signaller& operator=(const Signaller&) = delete;

        // Connects to the signalling server at `server` ("host:port") and registers this process's id
        // (the library must be started). A name is looked up on a thread of its own, so this doesn't wait
        // for it (poll goes on once it's found); false only for what isn't a server's name and port.
        bool connect(const std::string& server, std::string& error);

        // ready: the server has taken this process's id (Registered), so others can reach it.
        enum class State { idle, connecting, ready, failed };
        State state() const { return current_state; }
        // Why it failed (or the connection ended), for the player.
        const std::string& error() const { return failure; }

        // Runs the library's callbacks, sends what was queued and takes what arrived. Call it often.
        void poll();
        // Queues a signal for `peer` (any thread); sent at the next poll once registered.
        void send(uint64_t peer, std::span<const uint8_t> data) { out->push(peer, data); }
        // Where the library's connections queue their signals.
        std::shared_ptr<SignalOutbox> outbox() const { return out; }
        // The signals that have arrived since the last call, from their senders' ids.
        std::vector<signal::Signal> received();
        // The peers the server said aren't there, since the last call.
        std::vector<uint64_t> unreachable();

        // The TURN relay the server offered as this process registered, with its credentials (none: it
        // has none).
        const std::optional<signal::Turn>& turn() const { return offered_turn; }
        // The Ice for sessions through this server: its relay, and only that if `relay_only`; otherwise
        // directly too (with the relay's server for STUN, to learn this machine's public address).
        Ice ice(bool relay_only) const;

        // Once ready: creates a room for this process's match, whose key is `key` (one each); room() is
        // its number once the server has made it (0 until then; join_code.hpp: encode_room_code).
        void create_room(uint64_t key);
        uint64_t room() const { return created_room; }
        // Once ready: looks up `room`'s host. lookup() says how it went, and found() what it found.
        enum class Lookup { none, waiting, found, missing };
        void find_room(uint64_t room);
        Lookup lookup() const { return room_lookup; }
        const signal::RoomFound& found() const { return found_room; }

        // Once the room is made: lists it in public as `room` says (its number and host are filled in; its
        // release must be valid), for games like `hello`'s (fit_of), on `platform` (another than this one's:
        // a test); again whenever that changes (an unchanged one isn't sent).
        void publish(const Hello& hello, const signal::ListedRoom& room, signal::Platform platform = this_platform());
        // Once ready: asks for the listed rooms a game like `hello`'s on `platform` could join (the server
        // allows a few each minute). listing() says how it went (found: the rooms are in rooms(), those of
        // other builds last, and others() says how many more of the same game have other settings or mods;
        // older() how many are on older versions of the game, which can't say which).
        void list_rooms(const Hello& hello, signal::Platform platform = this_platform());
        Lookup listing() const { return room_listing; }
        const std::vector<signal::ListedRoom>& rooms() const { return listed.rooms; }
        int others() const { return listed.others; }
        int older() const { return listed.older; }

        // Speaks an older signalling version (signal::oldest_version on), as an older game does: a test of the
        // server's. Before connect.
        void speak(uint16_t version) { newest = speaks = version; }
        // A server older than this game refuses its version: then it connects again in the next older one,
        // down to signal::oldest_version, doing without what came later. Whether the server has public rooms
        // (publish and list_rooms; version 3), once ready.
        bool has_public_rooms() const { return speaks >= 3; }
        // Whether the server takes desync summaries (report_desync; version 4), once ready.
        bool has_desync_reports() const { return speaks >= 4; }
        // The signalling version it speaks with the server (once ready: the server's, if older).
        uint16_t version() const { return speaks; }
        // Once ready: tells the server this match desynced (it logs a summary for its operator; a few each
        // connection). False if it wasn't sent (the server is older, it isn't ready, or `report` isn't valid).
        bool report_desync(const signal::DesyncReport& report);

        // Closes the connection.
        void close();

    private:
        void on_status_changed(const void* info);
        void fail(const std::string& why);
        // Sent (false: not valid, or not ready).
        bool send_now(const signal::Packet& packet);
        // Opens the connection to `address` (resolved).
        void open(const std::string& address);
        friend void signaller_status_changed(void* info);

        uint32_t connection = 0;
        uint16_t newest = signal::version;   // the version it tries first
        uint16_t speaks = signal::version;   // the one it speaks now
        std::string address;            // the server's, resolved: to connect again in an older version
        bool speak_older = false;       // the server refused this version: again in the next older one
        // A name being looked up: its thread fills it in (and may outlive this, holding it).
        struct Resolving {
            std::mutex mutex;
            bool done = false;
            std::string address;
            std::string error;
        };
        std::shared_ptr<Resolving> lookup_in_progress;
        State current_state = State::idle;
        bool registering = false;   // connected, Register sent, Registered not here yet
        std::string failure;
        std::shared_ptr<SignalOutbox> out = std::make_shared<SignalOutbox>();
        std::vector<signal::Signal> incoming;
        std::vector<uint64_t> gone;
        std::optional<signal::Turn> offered_turn;
        bool creating_room = false;     // CreateRoom sent, RoomCreated not here yet
        uint64_t asked_room = 0;        // the room looked up
        uint64_t created_room = 0;
        Lookup room_lookup = Lookup::none;
        signal::RoomFound found_room;
        std::optional<signal::Publish> published;   // the last sent
        Lookup room_listing = Lookup::none;
        signal::Rooms listed;
    };
}
