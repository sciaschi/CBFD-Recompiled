// The signalling server's status, for its operator (netplay_server's --status, signal_server.cpp): what it
// counts, made into a page, JSON or plain text, and the HTTP around them; and coturn's metrics, read for how
// many relays it has open. Never an address or a room code: counts, the releases public rooms say, and desync
// summaries as the log has them. No dependencies: server_status_test.cpp tests it alone.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace conker::netplay::status {
    // A desync summary (signal::DesyncReport), as the log line has it.
    struct Desync {
        int64_t at = 0;             // when it came in (Unix time)
        uint64_t build = 0;
        int platform = 0;           // signal::Platform: 1 Linux, 2 Windows, 3 macOS
        int players = 2;
        int port = 0;               // from 0
        int other = 1;
        uint32_t read = 0;
        int regions = 0;            // how many 64 KB regions differed
        int rollback = 0;
        int delay = 0;
    };

    // One day's numbers (UTC).
    struct Day {
        std::string date;           // 2026-10-20
        uint64_t players = 0;       // registrations: a player each time they go online for a match or a list
        size_t peak_players = 0;    // the most online (registered) at once
        uint64_t rooms = 0;         // rooms made
        uint64_t matches = 0;       // rooms someone found by their code (a player or a spectator joining)
        uint64_t desyncs = 0;       // summaries reported
    };

    struct Status {
        int64_t now = 0;            // Unix time
        double uptime_seconds = 0;
        int oldest_version = 0;     // the signalling versions spoken
        int newest_version = 0;
        size_t connected = 0;       // connections, registered or not
        size_t players = 0;         // registered
        size_t rooms = 0;
        size_t public_rooms = 0;
        size_t public_playing = 0;  // public rooms whose match has started (private rooms don't say)
        size_t watching = 0;        // spectators in public rooms
        std::map<std::string, size_t> releases;   // public rooms by the release they say ("" for older games)
        bool relay = false;         // a TURN relay is offered
        bool relay_known = false;   // and its metrics were read lately
        int64_t relays = 0;         // relays (allocations) coturn has open
        uint64_t relayed_bytes = 0; // through the relay, in sessions that have ended, since coturn started
        Day today;
        // Since counting_since (Unix time): the server's start, or with --state its first start with that file.
        int64_t counting_since = 0;
        uint64_t signals = 0;
        uint64_t rooms_created = 0;
        uint64_t rooms_found = 0;
        uint64_t rooms_missed = 0;
        uint64_t lists = 0;
        uint64_t credentials = 0;   // relay credentials given out
        uint64_t desyncs = 0;
        uint64_t desyncs_unlogged = 0;
        std::vector<Desync> recent; // the latest logged desync summaries, newest first
    };

    // The most desync summaries `recent` keeps.
    constexpr size_t max_recent = 20;

    std::string json(const Status& status);
    std::string text(const Status& status);
    // A page that shows it, refreshing itself every 30 s.
    std::string html(const Status& status);

    // What netplay_server --state keeps across restarts: today's numbers, the counts since counting_since and the
    // latest desync summaries (no address nor room code, as everywhere here), as text, a line each.
    std::string saved(const Status& status);
    // Those fields read back (the rest left as they are); nothing if it isn't such a text.
    std::optional<Status> load(std::string_view saved);

    // The date (UTC) of a Unix time: 2026-10-20.
    std::string date_of(int64_t unix_time);

    // coturn's metrics (its --prometheus, as text): how many relays it has open (turn_total_allocations, of
    // every type), and the bytes relayed in sessions that have ended (turn_total_traffic_sentb and
    // turn_total_traffic_peer_sentb); nothing if it's not that.
    struct TurnMetrics {
        int64_t allocations = 0;
        uint64_t bytes = 0;
    };
    std::optional<TurnMetrics> parse_turn_metrics(std::string_view metrics);

    // The answer to an HTTP request whose head (up to its blank line) is `head`: the page at "/", the JSON at
    // "/status.json", the text at "/status.txt" (each to GET, and HEAD without the body); 404 for another path,
    // 405 for another method, 400 if it isn't a request. Always closes the connection.
    std::string respond(std::string_view head, const Status& status);
}
