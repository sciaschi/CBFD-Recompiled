// The signalling server's status: see server_status.hpp.

#include "server_status.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace conker::netplay::status {
    namespace {
        const char* platform_name(int platform) {
            static const char* const names[] = { "?", "Linux", "Windows", "macOS" };
            return platform >= 0 && platform < 4 ? names[platform] : "?";
        }

        std::string number(uint64_t n) { return std::to_string(n); }

        std::string hex16(uint64_t n) {
            char out[17];
            std::snprintf(out, sizeof(out), "%016llx", (unsigned long long)n);
            return out;
        }

        std::string time_of(int64_t unix_time) {
            const std::time_t t = (std::time_t)unix_time;
            std::tm parts{};
#if defined(_WIN32)
            gmtime_s(&parts, &t);
#else
            gmtime_r(&t, &parts);
#endif
            char out[32];
            std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &parts);
            return out;
        }

        // 1d 2h, 3h 4m, 5m 6s.
        std::string duration(double seconds) {
            const uint64_t s = seconds > 0 ? (uint64_t)seconds : 0;
            if (s >= 86400) return number(s / 86400) + "d " + number(s % 86400 / 3600) + "h";
            if (s >= 3600) return number(s / 3600) + "h " + number(s % 3600 / 60) + "m";
            return number(s / 60) + "m " + number(s % 60) + "s";
        }

        std::string bytes(uint64_t n) {
            char out[32];
            if (n >= 1000ull * 1000 * 1000) std::snprintf(out, sizeof(out), "%.1f GB", n / 1e9);
            else if (n >= 1000ull * 1000) std::snprintf(out, sizeof(out), "%.1f MB", n / 1e6);
            else if (n >= 1000) std::snprintf(out, sizeof(out), "%.1f KB", n / 1e3);
            else std::snprintf(out, sizeof(out), "%llu B", (unsigned long long)n);
            return out;
        }

        // A release as players' games say it (text from outside): printable ASCII only, the rest as '?'.
        std::string plain(std::string_view words) {
            std::string out;
            for (char c : words) out += c >= 0x20 && c < 0x7f ? c : '?';
            return out;
        }

        std::string json_string(std::string_view words) {
            std::string out = "\"";
            for (char c : plain(words)) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            return out + "\"";
        }

        std::string html_text(std::string_view words) {
            std::string out;
            for (char c : plain(words)) {
                switch (c) {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    case '"': out += "&quot;"; break;
                    case '\'': out += "&#39;"; break;
                    default: out += c;
                }
            }
            return out;
        }

        std::string release_name(const std::string& release) {
            return release.empty() ? "(older games)" : plain(release);
        }

        std::string relay_text(const Status& s) {
            if (!s.relay) return "none offered";
            if (!s.relay_known) return "offered (its metrics weren't read: open relays unknown)";
            return number((uint64_t)s.relays) + " open; " + bytes(s.relayed_bytes) + " relayed in sessions that ended";
        }

        std::string desync_text(const Desync& d) {
            return time_of(d.at) + "  build " + hex16(d.build) + ", " + platform_name(d.platform) + ", " + number(d.players) +
                " players, port " + number(d.port + 1) + " against port " + number(d.other + 1) + ", read " + number(d.read) +
                ", " + (d.regions == 0 ? "no region told apart" : number(d.regions) + (d.regions == 1 ? " region" : " regions")) +
                ", rollback " + number(d.rollback) + ", delay " + number(d.delay);
        }

        std::string reply(int code, const char* reason, const char* type, const std::string& body, bool with_body) {
            std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
            out += std::string("Content-Type: ") + type + "\r\n";
            out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
            out += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n";
            out += "Connection: close\r\n\r\n";
            return with_body ? out + body : out;
        }
    }

    std::string date_of(int64_t unix_time) {
        return time_of(unix_time).substr(0, 10);
    }

    std::string json(const Status& s) {
        std::string out = "{";
        out += "\"time\":" + std::to_string(s.now) + ",\"uptime_seconds\":" + number((uint64_t)s.uptime_seconds);
        out += ",\"versions\":[" + std::to_string(s.oldest_version) + "," + std::to_string(s.newest_version) + "]";
        out += ",\"players\":{\"connected\":" + number(s.connected) + ",\"registered\":" + number(s.players) + "}";
        out += ",\"rooms\":{\"open\":" + number(s.rooms) + ",\"private\":" + number(s.rooms - s.public_rooms) + ",\"public\":" +
            number(s.public_rooms) + ",\"public_playing\":" + number(s.public_playing) + ",\"public_waiting\":" +
            number(s.public_rooms - s.public_playing) + ",\"watching\":" + number(s.watching) + ",\"public_by_release\":{";
        bool first = true;
        for (const auto& [release, count] : s.releases) {
            out += (first ? "" : ",") + json_string(release) + ":" + number(count);
            first = false;
        }
        out += "}}";
        out += ",\"relay\":{\"offered\":" + std::string(s.relay ? "true" : "false") + ",\"known\":" + (s.relay_known ? "true" : "false");
        if (s.relay_known) {
            out += ",\"open\":" + std::to_string(s.relays) + ",\"relayed_bytes\":" + number(s.relayed_bytes);
        }
        out += "}";
        out += ",\"today\":{\"date\":" + json_string(s.today.date) + ",\"players\":" + number(s.today.players) + ",\"peak_players\":" +
            number(s.today.peak_players) + ",\"rooms\":" + number(s.today.rooms) + ",\"matches\":" + number(s.today.matches) +
            ",\"desyncs\":" + number(s.today.desyncs) + "}";
        out += ",\"since_start\":{\"time\":" + std::to_string(s.counting_since) + ",\"signals\":" + number(s.signals) +
            ",\"rooms_created\":" + number(s.rooms_created) + ",\"rooms_found\":" + number(s.rooms_found) + ",\"rooms_missed\":" + number(s.rooms_missed) + ",\"lists\":" + number(s.lists) +
            ",\"relay_credentials\":" + number(s.credentials) + ",\"desyncs\":" + number(s.desyncs) + ",\"desyncs_not_logged\":" +
            number(s.desyncs_unlogged) + "}";
        out += ",\"recent_desyncs\":[";
        for (size_t i = 0; i < s.recent.size(); i++) {
            const Desync& d = s.recent[i];
            out += std::string(i == 0 ? "" : ",") + "{\"time\":" + std::to_string(d.at) + ",\"build\":\"" + hex16(d.build) +
                "\",\"platform\":\"" + platform_name(d.platform) + "\",\"players\":" + std::to_string(d.players) + ",\"port\":" +
                std::to_string(d.port + 1) + ",\"against\":" + std::to_string(d.other + 1) + ",\"read\":" + number(d.read) +
                ",\"regions\":" + std::to_string(d.regions) + ",\"rollback\":" + std::to_string(d.rollback) + ",\"delay\":" +
                std::to_string(d.delay) + "}";
        }
        return out + "]}\n";
    }

    std::string text(const Status& s) {
        std::string out = "Netplay server, " + time_of(s.now) + " UTC, up " + duration(s.uptime_seconds) + " (signalling versions " +
            std::to_string(s.oldest_version) + " to " + std::to_string(s.newest_version) + ")\n\n";
        out += "Now\n";
        out += "  players: " + number(s.players) + " online (" + number(s.connected) + " connections)\n";
        out += "  rooms: " + number(s.rooms) + " (" + number(s.rooms - s.public_rooms) + " private, " + number(s.public_rooms) +
            " public: " + number(s.public_playing) + " playing, " + number(s.public_rooms - s.public_playing) + " waiting; " +
            number(s.watching) + " watching)\n";
        if (!s.releases.empty()) {
            out += "  public rooms by release:\n";
        }
        for (const auto& [release, count] : s.releases) {
            out += "    " + release_name(release) + ": " + number(count) + "\n";
        }
        out += "  relay: " + relay_text(s) + "\n\n";
        out += "Today (" + s.today.date + ", UTC)\n";
        out += "  players: " + number(s.today.players) + " (at most " + number(s.today.peak_players) + " at once)\n";
        out += "  rooms: " + number(s.today.rooms) + " made, " + number(s.today.matches) + " joined\n";
        out += "  desyncs: " + number(s.today.desyncs) + "\n\n";
        out += "Since " + time_of(s.counting_since) + " UTC\n";
        out += "  rooms: " + number(s.rooms_created) + " made, " + number(s.rooms_found) + " found, " + number(s.rooms_missed) +
            " codes not found\n";
        out += "  " + number(s.signals) + " signals passed, " + number(s.lists) + " room lists, " + number(s.credentials) +
            " relay credentials\n";
        out += "  desyncs: " + number(s.desyncs) + " (" + number(s.desyncs_unlogged) + " not logged)\n\n";
        out += "Latest desyncs" + std::string(s.recent.empty() ? ": none\n" : "\n");
        for (const Desync& d : s.recent) {
            out += "  " + desync_text(d) + "\n";
        }
        return out;
    }

    std::string html(const Status& s) {
        const auto row = [](const std::string& name, const std::string& value) {
            return "<tr><th>" + name + "</th><td>" + value + "</td></tr>\n";
        };
        std::string out =
            "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<meta http-equiv=\"refresh\" content=\"30\">\n<title>Netplay server</title>\n<style>\n"
            ":root{color-scheme:light dark;--bg:#fff;--fg:#1a1a1a;--dim:#666;--line:#ddd}\n"
            "@media (prefers-color-scheme:dark){:root{--bg:#111418;--fg:#e8e8e8;--dim:#9aa;--line:#2a3038}}\n"
            "body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;margin:0 auto;max-width:56rem;padding:16px}\n"
            "h1{font-size:1.4rem;margin:0}h2{font-size:1.05rem;margin:1.6rem 0 .4rem}p{color:var(--dim);margin:.2rem 0}\n"
            "table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:.3rem .6rem .3rem 0;border-bottom:1px solid var(--line);"
            "vertical-align:top}\nth{font-weight:600;width:12rem}td{font-variant-numeric:tabular-nums}\n"
            "ol{padding-left:1.2rem;font-variant-numeric:tabular-nums}a{color:inherit}\n</style></head><body>\n";
        out += "<h1>Netplay server</h1>\n<p>" + time_of(s.now) + " UTC &middot; up " + duration(s.uptime_seconds) +
            " &middot; signalling versions " + std::to_string(s.oldest_version) + " to " + std::to_string(s.newest_version) +
            " &middot; refreshes every 30 s &middot; <a href=\"status.json\">JSON</a> &middot; <a href=\"status.txt\">text</a></p>\n";
        out += "<h2>Now</h2>\n<table>\n";
        out += row("Players", number(s.players) + " online (" + number(s.connected) + " connections)");
        out += row("Rooms", number(s.rooms) + ": " + number(s.rooms - s.public_rooms) + " private, " + number(s.public_rooms) + " public");
        out += row("Public rooms", number(s.public_playing) + " playing, " + number(s.public_rooms - s.public_playing) + " waiting, " +
            number(s.watching) + " watching");
        for (const auto& [release, count] : s.releases) {
            out += row("Release " + html_text(release_name(release)), number(count) + (count == 1 ? " public room" : " public rooms"));
        }
        out += row("Relay", html_text(relay_text(s)));
        out += "</table>\n<h2>Today (" + html_text(s.today.date) + ", UTC)</h2>\n<table>\n";
        out += row("Players", number(s.today.players) + " (at most " + number(s.today.peak_players) + " at once)");
        out += row("Rooms", number(s.today.rooms) + " made, " + number(s.today.matches) + " joined");
        out += row("Desyncs", number(s.today.desyncs));
        out += "</table>\n<h2>Since " + time_of(s.counting_since) + " UTC</h2>\n<table>\n";
        out += row("Rooms", number(s.rooms_created) + " made, " + number(s.rooms_found) + " found, " + number(s.rooms_missed) +
            " codes not found");
        out += row("Signalling", number(s.signals) + " signals passed, " + number(s.lists) + " room lists, " + number(s.credentials) +
            " relay credentials");
        out += row("Desyncs", number(s.desyncs) + " (" + number(s.desyncs_unlogged) + " not logged)");
        out += "</table>\n<h2>Latest desyncs</h2>\n";
        if (s.recent.empty()) {
            out += "<p>None.</p>\n";
        }
        else {
            out += "<ol>\n";
            for (const Desync& d : s.recent) {
                out += "<li>" + html_text(desync_text(d)) + "</li>\n";
            }
            out += "</ol>\n";
        }
        return out + "</body></html>\n";
    }

    namespace {
        constexpr const char* saved_header = "conker-netplay-server-state 1";

        bool is_date(const std::string& date) {
            if (date.size() != 10) return false;
            for (size_t i = 0; i < date.size(); i++) {
                if (i == 4 || i == 7 ? date[i] != '-' : date[i] < '0' || date[i] > '9') return false;
            }
            return true;
        }
    }

    std::string saved(const Status& s) {
        std::string out = std::string(saved_header) + "\n";
        out += "since " + std::to_string(s.counting_since) + "\n";
        out += "today " + (is_date(s.today.date) ? s.today.date : date_of(s.now)) + " " + number(s.today.players) + " " +
            number(s.today.peak_players) + " " + number(s.today.rooms) + " " + number(s.today.matches) + " " + number(s.today.desyncs) + "\n";
        out += "counts " + number(s.signals) + " " + number(s.rooms_created) + " " + number(s.rooms_found) + " " + number(s.rooms_missed) +
            " " + number(s.lists) + " " + number(s.credentials) + " " + number(s.desyncs) + " " + number(s.desyncs_unlogged) + "\n";
        for (const Desync& d : s.recent) {
            out += "desync " + std::to_string(d.at) + " " + hex16(d.build) + " " + std::to_string(d.platform) + " " +
                std::to_string(d.players) + " " + std::to_string(d.port) + " " + std::to_string(d.other) + " " + number(d.read) + " " +
                std::to_string(d.regions) + " " + std::to_string(d.rollback) + " " + std::to_string(d.delay) + "\n";
        }
        return out;
    }

    std::optional<Status> load(std::string_view text) {
        // A line's words; numbers read whole (no sign on a count, nothing after one).
        const auto words_of = [](std::string_view line) {
            std::vector<std::string_view> words;
            for (size_t at = 0; at < line.size();) {
                const size_t end = std::min(line.find(' ', at), line.size());
                if (end > at) words.push_back(line.substr(at, end - at));
                at = end + 1;
            }
            return words;
        };
        const auto read = [](std::string_view word, auto& value, int base = 10) {
            const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value, base);
            return error == std::errc() && end == word.data() + word.size();
        };
        Status s;
        bool header = false, since = false, today = false, counts = false;
        size_t at = 0;
        while (at < text.size()) {
            const size_t end = std::min(text.find('\n', at), text.size());
            std::string_view line = text.substr(at, end - at);
            at = end + 1;
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (!header) {
                if (line != saved_header) return std::nullopt;
                header = true;
                continue;
            }
            const std::vector<std::string_view> w = words_of(line);
            if (w.empty()) continue;
            if (w[0] == "since" && w.size() == 2 && !since) {
                since = read(w[1], s.counting_since);
            }
            else if (w[0] == "today" && w.size() == 7 && !today) {
                Day& d = s.today;
                d.date = std::string(w[1]);
                today = is_date(d.date) && read(w[2], d.players) && read(w[3], d.peak_players) && read(w[4], d.rooms) &&
                    read(w[5], d.matches) && read(w[6], d.desyncs);
            }
            else if (w[0] == "counts" && w.size() == 9 && !counts) {
                counts = read(w[1], s.signals) && read(w[2], s.rooms_created) && read(w[3], s.rooms_found) && read(w[4], s.rooms_missed) &&
                    read(w[5], s.lists) && read(w[6], s.credentials) && read(w[7], s.desyncs) && read(w[8], s.desyncs_unlogged);
            }
            else if (w[0] == "desync" && w.size() == 11) {
                Desync d;
                if (!(read(w[1], d.at) && read(w[2], d.build, 16) && read(w[3], d.platform) && read(w[4], d.players) && read(w[5], d.port) &&
                      read(w[6], d.other) && read(w[7], d.read) && read(w[8], d.regions) && read(w[9], d.rollback) && read(w[10], d.delay))) {
                    return std::nullopt;
                }
                if (s.recent.size() < max_recent) s.recent.push_back(d);
            }
            else {
                return std::nullopt;
            }
        }
        if (!since || !today || !counts) return std::nullopt;
        return s;
    }

    std::optional<TurnMetrics> parse_turn_metrics(std::string_view metrics) {
        TurnMetrics out;
        bool seen = false;
        size_t at = 0;
        while (at < metrics.size()) {
            size_t end = metrics.find('\n', at);
            if (end == std::string_view::npos) end = metrics.size();
            std::string_view line = metrics.substr(at, end - at);
            at = end + 1;
            if (line.empty() || line[0] == '#') {
                // The HELP lines say which metrics it has, even before there's a value for them.
                seen = seen || line.starts_with("# HELP turn_total_allocations ");
                continue;
            }
            // name{labels} value
            const size_t name_end = line.find_first_of("{ ");
            if (name_end == std::string_view::npos) continue;
            const std::string_view name = line.substr(0, name_end);
            // The value is the first word after the labels (a timestamp may follow it).
            size_t value_at = name_end;
            if (line[name_end] == '{') {
                value_at = line.find('}', name_end);
                if (value_at == std::string_view::npos) continue;
                value_at++;
            }
            value_at = line.find_first_not_of(' ', value_at);
            if (value_at == std::string_view::npos) continue;
            const std::string value(line.substr(value_at, line.find(' ', value_at) - value_at));
            char* parsed_end = nullptr;
            const double v = std::strtod(value.c_str(), &parsed_end);
            if (parsed_end == value.c_str() || !(v >= 0) || v > 1e18) continue;
            if (name == "turn_total_allocations") {
                out.allocations += (int64_t)v;
                seen = true;
            }
            else if (name == "turn_total_traffic_sentb" || name == "turn_total_traffic_peer_sentb") {
                out.bytes += (uint64_t)v;
            }
        }
        if (!seen) return std::nullopt;
        return out;
    }

    std::string respond(std::string_view head, const Status& status) {
        const size_t line_end = head.find_first_of("\r\n");
        const std::string_view line = head.substr(0, line_end);
        const size_t first = line.find(' ');
        const size_t second = first == std::string_view::npos ? std::string_view::npos : line.find(' ', first + 1);
        if (second == std::string_view::npos || !line.substr(second + 1).starts_with("HTTP/1.")) {
            return reply(400, "Bad Request", "text/plain; charset=utf-8", "Not a request.\n", true);
        }
        const std::string_view method = line.substr(0, first);
        std::string_view path = line.substr(first + 1, second - first - 1);
        path = path.substr(0, path.find('?'));
        if (method != "GET" && method != "HEAD") {
            return reply(405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET.\n", true);
        }
        const bool body = method == "GET";
        if (path == "/" || path == "/index.html") {
            return reply(200, "OK", "text/html; charset=utf-8", html(status), body);
        }
        if (path == "/status.json") {
            return reply(200, "OK", "application/json", json(status), body);
        }
        if (path == "/status.txt") {
            return reply(200, "OK", "text/plain; charset=utf-8", text(status), body);
        }
        return reply(404, "Not Found", "text/plain; charset=utf-8", "Not here: try /, /status.json or /status.txt.\n", body);
    }
}
