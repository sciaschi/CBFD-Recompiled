// Tests of the server's status (server_status.cpp): its JSON, text and page say the numbers, the releases
// players' games sent are escaped, coturn's metrics are read, and requests get the right answers. Exits 1 on a
// failure.

#include <cstdio>
#include <string>

#include "server_status.hpp"

using namespace conker::netplay::status;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("server_status_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    bool has(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    Status example() {
        Status s;
        s.now = 1792500000;     // 2026-10-20 (UTC)
        s.uptime_seconds = 90061;
        s.oldest_version = 2;
        s.newest_version = 7;
        s.connected = 9;
        s.players = 8;
        s.rooms = 3;
        s.public_rooms = 2;
        s.public_playing = 1;
        s.watching = 2;
        s.releases["v0.1.6-netplay.23"] = 1;
        s.releases["<script>\"x\"</script>"] = 1;
        s.relay = true;
        s.relay_known = true;
        s.relays = 6;
        s.relayed_bytes = 2500000;
        s.today = { "2026-10-20", 14, 8, 5, 4, 1 };
        s.signals = 120;
        s.rooms_created = 11;
        s.rooms_found = 9;
        s.rooms_missed = 2;
        s.lists = 40;
        s.credentials = 30;
        s.desyncs = 3;
        s.desyncs_unlogged = 1;
        s.recent.push_back({ 1792499000, 0x5e6f000011112222ull, 2, 2, 1, 0, 4512, 2, 8, 2 });
        return s;
    }
}

int main() {
    const Status s = example();

    CHECK(date_of(0) == "1970-01-01");
    CHECK(date_of(1792500000) == "2026-10-20");

    // JSON: the numbers, under their names.
    const std::string j = json(s);
    CHECK(has(j, "\"players\":{\"connected\":9,\"registered\":8}"));
    CHECK(has(j, "\"rooms\":{\"open\":3,\"private\":1,\"public\":2,\"public_playing\":1,\"public_waiting\":1,\"watching\":2"));
    CHECK(has(j, "\"v0.1.6-netplay.23\":1"));
    CHECK(has(j, "\"<script>\\\"x\\\"</script>\":1"));   // quotes escaped (and served as JSON, not a page)
    CHECK(has(j, "\"relay\":{\"offered\":true,\"known\":true,\"open\":6,\"relayed_bytes\":2500000}"));
    CHECK(has(j, "\"today\":{\"date\":\"2026-10-20\",\"players\":14,\"peak_players\":8,\"rooms\":5,\"matches\":4,\"desyncs\":1}"));
    CHECK(has(j, "\"relay_credentials\":30"));
    CHECK(has(j, "{\"time\":1792499000,\"build\":\"5e6f000011112222\",\"platform\":\"Windows\",\"players\":2,\"port\":2,\"against\":1,"
                 "\"read\":4512,\"regions\":2,\"rollback\":8,\"delay\":2}"));
    CHECK(j.front() == '{' && j.substr(j.size() - 2) == "}\n");
    Status unknown = s;
    unknown.relay_known = false;
    CHECK(has(json(unknown), "\"relay\":{\"offered\":true,\"known\":false}"));

    // Text.
    const std::string t = text(s);
    CHECK(has(t, "up 1d 1h"));
    CHECK(has(t, "players: 8 online (9 connections)"));
    CHECK(has(t, "rooms: 3 (1 private, 2 public: 1 playing, 1 waiting; 2 watching)"));
    CHECK(has(t, "relay: 6 open; 2.5 MB relayed"));
    CHECK(has(t, "Windows, 2 players, port 2 against port 1, read 4512, 2 regions, rollback 8, delay 2"));
    CHECK(has(text(unknown), "open relays unknown"));

    // The page: a release from a player's game is only text.
    const std::string h = html(s);
    CHECK(has(h, "<meta http-equiv=\"refresh\" content=\"30\">"));
    CHECK(!has(h, "<script>"));
    CHECK(has(h, "&lt;script&gt;&quot;x&quot;&lt;/script&gt;"));
    CHECK(has(h, "8 online (9 connections)"));
    Status odd = s;
    odd.releases.clear();
    odd.releases[std::string("a\x01\xff" "b")] = 1;
    CHECK(has(html(odd), "a??b"));
    CHECK(has(json(odd), "\"a??b\":1"));

    // What --state keeps: read back as it was, and nothing from anything else.
    Status counted = s;
    counted.counting_since = 1792400000;
    counted.recent.push_back({ 1792498000, 0xffffffffffffffffull, 1, 4, 3, 2, 99, 0, 0, 1 });
    const std::string kept = saved(counted);
    CHECK(!has(kept, "netplay.23") && !has(kept, "connected"));     // not what's now: those start again
    const auto back = load(kept);
    CHECK(back && back->counting_since == 1792400000 && back->today.date == "2026-10-20" && back->today.players == 14 &&
        back->today.peak_players == 8 && back->today.rooms == 5 && back->today.matches == 4 && back->today.desyncs == 1);
    CHECK(back && back->signals == 120 && back->rooms_created == 11 && back->rooms_found == 9 && back->rooms_missed == 2 &&
        back->lists == 40 && back->credentials == 30 && back->desyncs == 3 && back->desyncs_unlogged == 1);
    CHECK(back && back->recent.size() == 2 && back->recent[0].build == 0x5e6f000011112222ull && back->recent[0].read == 4512 &&
        back->recent[0].delay == 2 && back->recent[1].build == 0xffffffffffffffffull && back->recent[1].port == 3);
    CHECK(back && saved(*back) == kept);
    CHECK(has(text(counted), "Since 2026-10-19 "));
    CHECK(has(json(counted), "\"since_start\":{\"time\":1792400000,"));
    CHECK(!load(""));
    CHECK(!load("since 1\ntoday 2026-10-20 1 1 1 1 1\ncounts 1 1 1 1 1 1 1 1\n"));    // no header
    CHECK(!load(kept.substr(0, kept.find("counts"))));                                  // cut short
    CHECK(!load(kept + "desync 1 zz\n"));
    CHECK(!load(kept + "what 1\n"));
    CHECK(!load(kept + "since 5\n"));                                                   // twice
    std::string negative = kept, junk = kept, crlf;
    negative.replace(negative.find("counts 120"), 10, "counts -120");
    CHECK(!load(negative));
    junk.replace(junk.find("counts 120"), 10, "counts 120x");
    CHECK(!load(junk));
    for (char c : kept) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    CHECK(load(crlf) && saved(*load(crlf)) == kept);
    std::string bad_date = kept;
    bad_date.replace(bad_date.find("2026-10-20"), 10, "2026/10/20");
    CHECK(!load(bad_date));

    // coturn's metrics.
    const char* metrics =
        "# HELP turn_total_allocations Represents current allocations number\n"
        "# TYPE turn_total_allocations gauge\n"
        "turn_total_allocations{type=\"UDP\"} 4\n"
        "turn_total_allocations{type=\"TCP\"} 1\n"
        "# HELP turn_total_traffic_sentb Represents total finished sessions sent bytes\n"
        "turn_total_traffic_sentb 1000\n"
        "turn_total_traffic_peer_sentb 234\n"
        "turn_total_traffic_rcvb 99999\n";
    const auto read = parse_turn_metrics(metrics);
    CHECK(read && read->allocations == 5 && read->bytes == 1234);
    const auto none_yet = parse_turn_metrics("# HELP turn_total_allocations Represents current allocations number\n"
                                             "# TYPE turn_total_allocations gauge\n");
    CHECK(none_yet && none_yet->allocations == 0);
    CHECK(!parse_turn_metrics("<html>not metrics</html>"));
    CHECK(!parse_turn_metrics(""));
    CHECK(!parse_turn_metrics("turn_total_allocations{type=\"UDP\"} -3\n"));
    // A timestamp after the value (the format allows one), and labels with spaces in them.
    const auto stamped = parse_turn_metrics("turn_total_allocations{type=\"UDP\"} 3 1729440000000\n"
                                            "turn_total_allocations{type=\"a b\"} 2\nturn_total_traffic_sentb 7 1729440000000\n");
    CHECK(stamped && stamped->allocations == 5 && stamped->bytes == 7);

    // Requests.
    const std::string page = respond("GET / HTTP/1.1\r\nHost: nas:27180\r\n", s);
    CHECK(page.starts_with("HTTP/1.1 200 OK\r\n") && has(page, "Content-Type: text/html") && has(page, "Connection: close") &&
        has(page, "<h1>Netplay server</h1>"));
    CHECK(has(respond("GET /status.json?x=1 HTTP/1.1\r\n", s), "Content-Type: application/json"));
    CHECK(has(respond("GET /status.txt HTTP/1.0\r\n", s), "players: 8 online"));
    const std::string head = respond("HEAD /status.json HTTP/1.1\r\n", s);
    CHECK(head.starts_with("HTTP/1.1 200 OK") && head.substr(head.size() - 4) == "\r\n\r\n" && !has(head, "\"players\""));
    CHECK(respond("GET /nope HTTP/1.1\r\n", s).starts_with("HTTP/1.1 404 "));
    CHECK(respond("POST / HTTP/1.1\r\n", s).starts_with("HTTP/1.1 405 "));
    CHECK(respond("hello", s).starts_with("HTTP/1.1 400 "));
    CHECK(respond("GET /\r\n", s).starts_with("HTTP/1.1 400 "));
    CHECK(respond("GET /status.txt HTTP/1.1\nHost: x\n", s).starts_with("HTTP/1.1 200 "));   // lines ending in a bare LF

    if (failures == 0) {
        std::printf("server_status_test: passed\n");
    }
    return failures == 0 ? 0 : 1;
}
