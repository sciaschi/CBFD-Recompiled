// netplay_server: the signalling server. Peers connect over GameNetworkingSockets, each registers an id
// (Register, signal_packets.hpp; answered with Registered) and sends the library's ICE signals for another
// id (Signal); the server passes them on, from the sender's id, or says the other isn't there
// (Unreachable). It keeps rooms too: a host creates one for its match (CreateRoom, with the match's key)
// and gets a room code to pass on; a joiner looks it up (FindRoom) and gets the host's id and the key. A
// room lasts as long as its host's connection. A host may list its room in public (Publish), and anyone may
// ask for the listed rooms they could join (ListRooms): those whose game fits theirs (the same hash of
// build, ROM and settings) on a platform that plays with theirs (Linux and Windows together; macOS apart
// until it's tested with them). It never sees the game's packets, nor the TURN relay's
// traffic (coturn, beside it: tools/netplay/server/).
//
//   netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096] [--connection-burst 20]
//                  [--max-listed-per-address 2] [--desync-burst 4] [--newest-version <n>]   (a test: speak no newer one)
//                  [--turn <host:port> [--turn-hours 12] [--turn-metrics <address:port>]]   (with the secret in TURN_SECRET)
//                  [--status <address:port>] [--state <file>]
//
// With --turn, each peer is offered that TURN relay as it registers (Turn), with credentials of its own
// made from the secret it shares with coturn (turn_credentials.hpp), which expire after --turn-hours.
//
// Limits: a peer must register within 5 s, once, with an id nobody else has (nor had in the last hour, so
// a peer that leaves can't be posed as straight away); at most --max-per-address connections at once from
// one IP address (an IPv6 one's /64), --connection-burst new ones (then one each 2 s), and --max-peers in
// all; at most 20 packets a second from each (bursts of 60); one room each; from each address, at most 10
// lookups of rooms that aren't there (then one each 6 s), so codes can't be guessed; 6 room lists (then one
// each 3 s), each of at most signal::max_listed rooms; --max-listed-per-address public rooms from each
// address at once (more stay private). A room's code isn't given out again for an hour after it closes. A
// peer that neither hosts a room nor found one that's still open, and sends nothing for 10 minutes, is
// closed, and any after 12 hours. Anything broken or past a limit closes the connection, after Refused. It
// logs counts, never addresses, codes or signals.
//
// A peer whose match desynced may send a summary of it (DesyncReport: the build, its platform, the read,
// the regions of memory that differed; no address, memory or inputs), which it logs for its operator to see
// desyncs happen: up to 4 from each connection, 4 from each address (then one each 5 minutes) and 30 in
// all (then one each 30 s); the rest are only counted, so the log can't be filled.
//
// It speaks every signalling version from signal::oldest_version on, each peer in the one its first packet
// has (a version without public rooms just can't use them), and keeps the versions apart: a peer only
// reaches the peers of its own, and only finds their rooms. Except that from version 6 the room list shows
// the rooms of the same ROM on other builds of version 6 on too, with their releases (they can't be joined
// from there: the player sees which release to get), and counts those of older versions.
//
// With --status, it serves its operator a status on that TCP address (IPv4) and port: a page at /, JSON at
// /status.json and text at /status.txt (server_status.hpp): players, rooms, the relay's open relays, today's
// numbers and the latest desync summaries, never an address or a room code. Off by default; on its own port, for
// the operator's network only (nothing to log in with). With --turn-metrics, coturn's metrics (its --prometheus,
// on this machine) are read every 15 s for how many relays it has open.
//
// With --state, the status's numbers (today's, the counts and the latest desync summaries; never an address or
// a room code) are kept in that file, read as it starts and written (a new file, then renamed over it) each
// minute they changed and as it stops, so they count on across restarts. A file that can't be read is kept
// beside it as <file>.broken, and counting starts again.
//
// It runs until SIGINT or SIGTERM (docker stop).

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
// (windows.h, which winsock2.h brings, would otherwise define min and max as macros.)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include "join_code.hpp"
#include "packets.hpp"
#include "server_status.hpp"
#include "signal_packets.hpp"
#include "turn_credentials.hpp"

using namespace conker::netplay;

namespace {
    constexpr double register_seconds = 5.0;
    constexpr double id_reuse_seconds = 3600.0;
    constexpr double signals_per_second = 20.0;
    constexpr double signal_burst = 60.0;
    constexpr int max_messages_per_loop = 1024;
    constexpr double misses_per_second = 1.0 / 6.0;
    constexpr double miss_burst = 10.0;
    // New connections from one address: each leaves an id (and maybe a room code) not reused for an hour,
    // so they're limited, to bound what that keeps.
    constexpr double connections_per_second = 0.5;
    // A registered peer without a room that sends nothing for this long is closed (it's done: a joiner
    // only signals while connecting), and any connection after max_seconds (longer than any match).
    constexpr double idle_seconds = 600.0;
    constexpr double max_seconds = 12 * 3600.0;
    // Room lists: each looks at every listed room.
    constexpr int max_desync_reports = 4;
    constexpr double reports_per_second = 1.0 / 300.0;     // from each address, after a burst (--desync-burst)
    constexpr double all_reports_per_second = 1.0 / 30.0;  // from everyone, after a burst of 30
    constexpr double all_report_burst = 30.0;
    constexpr double lists_per_second = 1.0 / 3.0;
    constexpr double list_burst = 6.0;

    volatile std::sig_atomic_t stopping = 0;
    void on_signal(int) { stopping = 1; }

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // What the per-address limits count by: an IPv4 address, or an IPv6 one's /64 (one network: a machine
    // can have any number of addresses in it).
    std::string address_key(const SteamNetworkingIPAddr& address) {
        if (address.IsIPv4()) {
            const uint32_t ip = address.GetIPv4();
            char text[16];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
            return text;
        }
        char text[32];
        const uint8_t* b = address.m_ipv6;
        std::snprintf(text, sizeof(text), "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        return text;
    }

#if defined(_WIN32)
    using Socket = SOCKET;
    constexpr Socket no_socket = INVALID_SOCKET;
    bool start_sockets() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    void close_socket(Socket s) { closesocket(s); }
    bool non_blocking(Socket s) {
        u_long on = 1;
        return ioctlsocket(s, FIONBIO, &on) == 0;
    }
    bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS; }
    constexpr int send_flags = 0;
    constexpr int shut_write = SD_SEND;
#else
    using Socket = int;
    constexpr Socket no_socket = -1;
    bool start_sockets() { return true; }
    void close_socket(Socket s) { ::close(s); }
    bool non_blocking(Socket s) {
        const int flags = fcntl(s, F_GETFL, 0);
        return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
    constexpr int shut_write = SHUT_WR;
    // (A page closed before it's all sent mustn't stop the server: no SIGPIPE, here or, on macOS, from main.)
#if defined(MSG_NOSIGNAL)
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
#endif

    // "address:port" (IPv4) as a socket address, or nothing.
    std::optional<sockaddr_in> ipv4_endpoint(const std::string& text) {
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos) return std::nullopt;
        const int port = std::atoi(text.c_str() + colon + 1);
        sockaddr_in out{};
        out.sin_family = AF_INET;
        if (port <= 0 || port > 65535 || inet_pton(AF_INET, text.substr(0, colon).c_str(), &out.sin_addr) != 1) return std::nullopt;
        out.sin_port = htons((uint16_t)port);
        return out;
    }

    // A rate: `tokens` that come back at `per_second`, up to `burst`.
    struct Bucket {
        double tokens = 0;
        double at = 0;
        // Takes one, if there is one.
        bool take(double t, double per_second, double burst) {
            tokens = std::min(burst, tokens + (t - at) * per_second);
            at = t;
            if (tokens < 1) return false;
            tokens -= 1;
            return true;
        }
    };

    struct Peer {
        uint16_t version = 0;      // the signalling version it speaks, from its first packet
        uint64_t id = 0;           // 0 until it registers
        std::string address;       // its IP, for the per-address limits only
        double connected_at = 0;
        double last_packet = 0;
        Bucket packets;
        uint64_t room = 0;         // the room it hosts, if any
        uint64_t found_room = 0;   // the room it last found (it joined that match, most likely)
        uint64_t room_key = 0;
        int desync_reports = 0;    // DesyncReports taken from it
        bool listed = false;       // its room is in the public list (Publish), as `listing` says
        signal::Publish listing;
        double listed_at = 0;
        Bucket lists;
    };

    struct Server {
        HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
        std::unordered_map<HSteamNetConnection, Peer> peers;
        std::unordered_map<uint64_t, HSteamNetConnection> by_id;
        // Ids whose peers left, and rooms closed, within the hour: not given out again yet. Oldest first.
        std::unordered_set<uint64_t> released;
        std::deque<std::pair<double, uint64_t>> released_order;
        std::unordered_map<uint64_t, HSteamNetConnection> rooms;
        std::unordered_set<uint64_t> closed_rooms;
        std::deque<std::pair<double, uint64_t>> closed_order;
        std::map<std::string, int> per_address;
        // Each IP address's lookups of rooms that weren't there, kept after its peers leave (until it's
        // full again), so reconnecting doesn't refill it.
        std::unordered_map<std::string, Bucket> misses;
        std::unordered_map<std::string, Bucket> arrivals;   // new connections, kept the same way
        int max_per_address = 8;
        // Public rooms from one address at once (--max-listed-per-address): more aren't listed (they stay
        // private), so one network can't fill the list with rooms nobody answers in.
        int max_listed_per_address = 2;
        double report_burst = 4;    // desync summaries logged from an address at once (--desync-burst)
        // The newest signalling version spoken (--newest-version: a test, posing as an older server).
        uint16_t newest_version = signal::version;
        size_t max_peers = 4096;
        std::string turn;           // the TURN relay offered, if any
        std::string turn_secret;
        double turn_hours = 12;
        double connection_burst = 20;   // --connection-burst
        std::random_device random;
        uint64_t signals_passed = 0;
        uint64_t rooms_created = 0;
        uint64_t rooms_found = 0;
        uint64_t rooms_missed = 0;
        uint64_t lists_sent = 0;
        uint64_t desyncs_reported = 0;
        uint64_t desyncs_unlogged = 0;     // reported past the limits: counted, not logged
        std::unordered_map<std::string, Bucket> reports;   // each address's, kept like `misses`
        Bucket all_reports{ all_report_burst, 0 };
        // For the status (--status): when it started, relay credentials given, today's numbers (and the rooms
        // joined today, to count each once), the latest desync summaries logged, and coturn's last metrics.
        double started_at = now();
        int64_t counting_since = (int64_t)std::time(nullptr);
        uint64_t credentials_given = 0;
        status::Day today;
        std::unordered_set<uint64_t> joined_today;
        std::deque<status::Desync> recent_desyncs;
        std::optional<status::TurnMetrics> turn_metrics;
        double turn_metrics_at = -1e9;

        // Today's numbers start again at midnight (UTC).
        void roll_day() {
            const std::string date = status::date_of((int64_t)std::time(nullptr));
            if (date != today.date) {
                today = status::Day{ date };
                joined_today.clear();
            }
            today.peak_players = std::max(today.peak_players, by_id.size());
        }

        // In the peer's version (the newest, before it's said).
        void send(HSteamNetConnection connection, const signal::Packet& packet) {
            const auto peer = peers.find(connection);
            const uint16_t as = peer != peers.end() && peer->second.version != 0 ? peer->second.version : newest_version;
            uint8_t buffer[signal::max_packet_size];
            const size_t size = signal::encode(packet, buffer, as);
            if (size != 0) {
                SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
            }
        }

        void forget(HSteamNetConnection connection) {
            auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            if (it->second.id != 0) {
                by_id.erase(it->second.id);
                released.insert(it->second.id);
                released_order.push_back({ now(), it->second.id });
            }
            if (it->second.room != 0) {
                rooms.erase(it->second.room);
                closed_rooms.insert(it->second.room);
                closed_order.push_back({ now(), it->second.room });
            }
            if (--per_address[it->second.address] <= 0) {
                per_address.erase(it->second.address);
            }
            peers.erase(it);
        }

        void refuse(HSteamNetConnection connection, signal::Refusal reason) {
            send(connection, signal::Refused{ reason });
            SteamNetworkingSockets()->CloseConnection(connection, k_ESteamNetConnectionEnd_App_Min + (int)reason, "refused", true);
            forget(connection);
        }

        void on_status(SteamNetConnectionStatusChangedCallback_t* info) {
            ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
            switch (info->m_info.m_eState) {
                case k_ESteamNetworkingConnectionState_Connecting: {
                    const std::string address = address_key(info->m_info.m_addrRemote);
                    const auto known = per_address.find(address);
                    const double t = now();
                    if (peers.size() >= max_peers || (known != per_address.end() && known->second >= max_per_address) ||
                        !arrivals.try_emplace(address, Bucket{ connection_burst, t }).first->second.take(t, connections_per_second, connection_burst)) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)signal::Refusal::busy, "busy", false);
                        return;
                    }
                    if (sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, group);
                    per_address[address]++;
                    Peer peer;
                    peer.address = address;
                    peer.connected_at = peer.last_packet = now();
                    peer.packets = { signal_burst, peer.connected_at };
                    peer.lists = { list_burst, peer.connected_at };
                    peers[info->m_hConn] = peer;
                    break;
                }
                case k_ESteamNetworkingConnectionState_ClosedByPeer:
                case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                    sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                    forget(info->m_hConn);
                    break;
                default:
                    break;
            }
        }

        void take(HSteamNetConnection connection, std::span<const uint8_t> data) {
            auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            Peer& peer = it->second;
            peer.last_packet = now();
            // The version is the first packet's, for good.
            uint16_t as = signal::spoken_version(data);
            if (as > newest_version) {
                as = 0;
            }
            if (peer.version == 0 && as != 0) {
                peer.version = as;
            }
            const uint16_t expected = peer.version != 0 ? peer.version : newest_version;
            const std::optional<signal::Packet> packet = signal::decode(data, expected);
            if (!packet) {
                refuse(connection, as == 0 && signal::other_version(data, expected) ? signal::Refusal::version : signal::Refusal::protocol);
                return;
            }
            if (const signal::Register* r = std::get_if<signal::Register>(&*packet)) {
                if (peer.id != 0) {
                    refuse(connection, signal::Refusal::protocol);
                }
                else if (by_id.contains(r->id) || released.contains(r->id)) {
                    refuse(connection, signal::Refusal::taken);
                }
                else {
                    peer.id = r->id;
                    by_id[r->id] = connection;
                    roll_day();
                    today.players++;
                    // The relay first: once registered, a peer may connect at once.
                    if (!turn.empty()) {
                        const uint64_t expires = (uint64_t)std::time(nullptr) + (uint64_t)(turn_hours * 3600);
                        const TurnCredentials credentials = turn_credentials(turn_secret, r->id, expires);
                        send(connection, signal::Turn{ turn, credentials.user, credentials.password });
                        credentials_given++;
                    }
                    send(connection, signal::Registered{ r->id });
                }
                return;
            }
            if (peer.id == 0) {
                refuse(connection, signal::Refusal::protocol);
                return;
            }
            const double t = now();
            if (!peer.packets.take(t, signals_per_second, signal_burst)) {
                refuse(connection, signal::Refusal::busy);
                return;
            }
            if (const signal::Signal* s = std::get_if<signal::Signal>(&*packet)) {
                const auto to = by_id.find(s->peer);
                const auto to_peer = to == by_id.end() ? peers.end() : peers.find(to->second);
                if (to_peer == peers.end() || to_peer->second.version != peer.version) {
                    send(connection, signal::Unreachable{ s->peer });
                    return;
                }
                send(to->second, signal::Signal{ peer.id, s->data });
                signals_passed++;
            }
            else if (const signal::CreateRoom* c = std::get_if<signal::CreateRoom>(&*packet)) {
                if (peer.room != 0) {
                    refuse(connection, signal::Refusal::protocol);
                    return;
                }
                // A random code, not one open now or lately.
                uint64_t room = 0;
                while (room == 0 || rooms.contains(room) || closed_rooms.contains(room)) {
                    room = ((uint64_t)random() << 32 | random()) & max_room;
                }
                peer.room = room;
                peer.room_key = c->key;
                rooms[room] = connection;
                rooms_created++;
                roll_day();
                today.rooms++;
                send(connection, signal::RoomCreated{ room });
            }
            else if (const signal::FindRoom* f = std::get_if<signal::FindRoom>(&*packet)) {
                const auto found = rooms.find(f->room);
                const auto host_it = found == rooms.end() ? peers.end() : peers.find(found->second);
                if (host_it != peers.end() && host_it->second.version != peer.version) {
                    // Another version's room: there for this peer's game, but not to be reached by it (and
                    // not a guess).
                    send(connection, signal::NoRoom{ f->room });
                    return;
                }
                if (host_it != peers.end()) {
                    const Peer& host = host_it->second;
                    rooms_found++;
                    peer.found_room = f->room;
                    roll_day();
                    if (joined_today.insert(f->room).second) {
                        today.matches++;
                    }
                    send(connection, signal::RoomFound{ f->room, host.id, host.room_key });
                    return;
                }
                rooms_missed++;
                auto bucket = misses.try_emplace(peer.address, Bucket{ miss_burst, t }).first;
                if (!bucket->second.take(t, misses_per_second, miss_burst)) {
                    refuse(connection, signal::Refusal::busy);
                    return;
                }
                send(connection, signal::NoRoom{ f->room });
            }
            else if (const signal::Publish* p = std::get_if<signal::Publish>(&*packet)) {
                if (peer.room == 0) {
                    refuse(connection, signal::Refusal::protocol);
                    return;
                }
                if (!peer.listed) {
                    const auto from_here = std::count_if(peers.begin(), peers.end(), [&peer](const auto& p) {
                        return p.second.listed && p.second.address == peer.address;
                    });
                    if (from_here >= max_listed_per_address) {
                        return;
                    }
                    peer.listed_at = t;
                }
                peer.listed = true;
                peer.listing = *p;
                peer.listing.room.room = peer.room;
            }
            else if (const signal::DesyncReport* d = std::get_if<signal::DesyncReport>(&*packet)) {
                // A few each, from each address, and in all: more are only counted (the log isn't to be filled).
                desyncs_reported++;
                roll_day();
                today.desyncs++;
                if (++peer.desync_reports > max_desync_reports ||
                    !reports.try_emplace(peer.address, Bucket{ report_burst, t }).first->second.take(t, reports_per_second, report_burst) ||
                    !all_reports.take(t, all_reports_per_second, all_report_burst)) {
                    desyncs_unlogged++;
                    return;
                }
                std::string regions;
                int count = 0;
                for (int region = 0; region < memory_regions; region++) {
                    if ((d->regions[region / 8] & (1u << (region % 8))) != 0 && count++ < 16) {
                        regions += (regions.empty() ? "" : ",") + std::to_string(region);
                    }
                }
                if (count > 16) regions += " and " + std::to_string(count - 16) + " more";
                recent_desyncs.push_front({ (int64_t)std::time(nullptr), d->build, (int)d->platform, d->players, d->port, d->other, d->read,
                    count, d->rollback, d->delay });
                if (recent_desyncs.size() > status::max_recent) {
                    recent_desyncs.pop_back();
                }
                static const char* const platforms[] = { "?", "Linux", "Windows", "macOS" };
                std::printf("netplay_server: desync: match %08x, build %016llx, %s, %d players, port %d against port %d, read %u, "
                            "regions %s, rollback %d, delay %d\n", d->match, (unsigned long long)d->build, platforms[(int)d->platform],
                    d->players, d->port + 1, d->other + 1, d->read, regions.empty() ? "none told apart" : regions.c_str(), d->rollback, d->delay);
                std::fflush(stdout);
            }
            else if (const signal::ListRooms* l = std::get_if<signal::ListRooms>(&*packet)) {
                if (!peer.lists.take(t, lists_per_second, list_burst)) {
                    refuse(connection, signal::Refusal::busy);
                    return;
                }
                send(connection, listed_for(*l, peer.version));
                lists_sent++;
            }
            else {
                // A server's packet.
                refuse(connection, signal::Refusal::protocol);
            }
        }

        // The listed rooms for `ask` (from a peer of `version`), on its platform group: of that version, its
        // fit, open ones first (the fullest, then the oldest: to fill matches), then those being played; at
        // most signal::max_listed. And how many of its game have another fit. From version 6, the rooms of
        // its ROM on other builds (of version 6 on: they can say their release) come after those, and how many
        // rooms of older versions there are.
        signal::Rooms listed_for(const signal::ListRooms& ask, uint16_t version) const {
            std::vector<const Peer*> fitting;
            std::vector<const Peer*> other_builds;
            size_t others = 0;
            size_t older = 0;
            for (const auto& [room, connection] : rooms) {
                const auto host = peers.find(connection);
                if (host == peers.end() || !host->second.listed ||
                    !signal::platforms_mix(host->second.listing.platform, ask.platform)) {
                    continue;
                }
                const signal::Publish& listing = host->second.listing;
                if (host->second.version != version) {
                    if (version >= 6 && host->second.version >= 6 && listing.rom == ask.rom && listing.game != ask.game) {
                        other_builds.push_back(&host->second);
                    }
                    else if (version >= 6 && host->second.version < 6) {
                        older++;
                    }
                }
                else if (listing.fit == ask.fit) {
                    fitting.push_back(&host->second);
                }
                else if (listing.game == ask.game) {
                    others++;
                }
                else if (version >= 6 && listing.rom == ask.rom) {
                    other_builds.push_back(&host->second);
                }
            }
            const auto open = [](const Peer* p) { return !p->listing.room.playing && p->listing.room.joined < p->listing.room.players; };
            const auto order = [&open](const Peer* a, const Peer* b) {
                if (open(a) != open(b)) return open(a);
                if (a->listing.room.joined != b->listing.room.joined) return a->listing.room.joined > b->listing.room.joined;
                return a->listed_at < b->listed_at;
            };
            std::sort(fitting.begin(), fitting.end(), order);
            std::sort(other_builds.begin(), other_builds.end(), order);
            signal::Rooms out;
            out.others = (uint16_t)std::min<size_t>(others, UINT16_MAX);
            out.older = (uint16_t)std::min<size_t>(older, UINT16_MAX);
            for (size_t i = 0; i < fitting.size() && out.rooms.size() < signal::max_listed; i++) {
                out.rooms.push_back(fitting[i]->listing.room);
            }
            for (size_t i = 0; i < other_builds.size() && out.rooms.size() < signal::max_listed; i++) {
                out.rooms.push_back(other_builds[i]->listing.room);
                out.rooms.back().other_build = true;
            }
            return out;
        }

        // Peers that never registered, idle or too old; ids and rooms released long enough ago to be given
        // out again; addresses whose limits are full again. Once a second.
        void expire() {
            const double t = now();
            roll_day();
            std::vector<HSteamNetConnection> late;
            for (const auto& [connection, peer] : peers) {
                // A joiner sends nothing during its match, but stays while the room it found is open (it
                // may still report a desync, or need the host again).
                const bool in_match = peer.room != 0 || (peer.found_room != 0 && rooms.contains(peer.found_room));
                if ((peer.id == 0 && t - peer.connected_at > register_seconds) ||
                    (!in_match && t - peer.last_packet > idle_seconds) || t - peer.connected_at > max_seconds) {
                    late.push_back(connection);
                }
            }
            for (HSteamNetConnection connection : late) {
                refuse(connection, signal::Refusal::protocol);
            }
            for (auto [order, set] : { std::pair{ &released_order, &released }, std::pair{ &closed_order, &closed_rooms } }) {
                while (!order->empty() && t - order->front().first >= id_reuse_seconds) {
                    set->erase(order->front().second);
                    order->pop_front();
                }
            }
            std::erase_if(misses, [t](const auto& entry) { return entry.second.tokens + (t - entry.second.at) * misses_per_second >= miss_burst; });
            std::erase_if(reports, [this, t](const auto& entry) { return entry.second.tokens + (t - entry.second.at) * reports_per_second >= report_burst; });
            std::erase_if(arrivals, [this, t](const auto& entry) {
                return entry.second.tokens + (t - entry.second.at) * connections_per_second >= connection_burst;
            });
        }

        // What the status shows, now.
        status::Status snapshot() const {
            status::Status s;
            s.now = (int64_t)std::time(nullptr);
            s.uptime_seconds = now() - started_at;
            s.oldest_version = signal::oldest_version;
            s.newest_version = newest_version;
            s.connected = peers.size();
            s.players = by_id.size();
            s.rooms = rooms.size();
            for (const auto& [room, connection] : rooms) {
                const auto host = peers.find(connection);
                if (host == peers.end() || !host->second.listed) {
                    continue;
                }
                const signal::ListedRoom& listed = host->second.listing.room;
                s.public_rooms++;
                s.public_playing += listed.playing ? 1 : 0;
                s.watching += listed.watching;
                s.releases[listed.release]++;
            }
            s.relay = !turn.empty();
            s.relay_known = turn_metrics.has_value() && now() - turn_metrics_at < 60;
            if (s.relay_known) {
                s.relays = turn_metrics->allocations;
                s.relayed_bytes = turn_metrics->bytes;
            }
            s.today = today;
            s.counting_since = counting_since;
            s.signals = signals_passed;
            s.rooms_created = rooms_created;
            s.rooms_found = rooms_found;
            s.rooms_missed = rooms_missed;
            s.lists = lists_sent;
            s.credentials = credentials_given;
            s.desyncs = desyncs_reported;
            s.desyncs_unlogged = desyncs_unlogged;
            s.recent.assign(recent_desyncs.begin(), recent_desyncs.end());
            return s;
        }

        // The numbers --state kept (status::load's), counted on from.
        void restore(const status::Status& s) {
            counting_since = s.counting_since;
            today = s.today;
            signals_passed = s.signals;
            rooms_created = s.rooms_created;
            rooms_found = s.rooms_found;
            rooms_missed = s.rooms_missed;
            lists_sent = s.lists;
            credentials_given = s.credentials;
            desyncs_reported = s.desyncs;
            desyncs_unlogged = s.desyncs_unlogged;
            recent_desyncs.assign(s.recent.begin(), s.recent.end());
            roll_day();
        }
    };

    Server* server = nullptr;
    void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
        if (server != nullptr) server->on_status(info);
    }

    // The status's HTTP listener (--status): each request read, answered (server_status.hpp's respond) and the
    // connection closed. A few at once, each with a few seconds to say what it wants: it's for one operator. It
    // never keeps the signalling from starting: a port it can't have yet (taken, or an address that isn't up yet,
    // a VPN's at boot) is tried again every 30 s.
    struct StatusPage {
        static constexpr size_t max_clients = 16;
        static constexpr size_t max_request = 8192;
        static constexpr double client_seconds = 5.0;
        static constexpr double closing_seconds = 1.0;
        struct Client {
            Socket socket = no_socket;
            std::string in;
            std::string out;
            size_t sent = 0;
            double since = 0;
            double closing_at = 0;  // all sent: what's left of the request is read and dropped for a moment, so
                                    // closing doesn't reset the connection before the answer is read
        };
        static constexpr double retry_seconds = 30.0;
        Socket listener = no_socket;
        std::vector<Client> clients;
        std::optional<sockaddr_in> wanted;
        double retry_at = 0;
        bool warned = false;

        bool open(const sockaddr_in& at) {
            listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == no_socket) return false;
            // A port another program has is an error (Windows' SO_REUSEADDR would share it instead).
            int on = 1;
#if defined(_WIN32)
            setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
#else
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#endif
            if (!non_blocking(listener) || bind(listener, (const sockaddr*)&at, sizeof(at)) != 0 || listen(listener, 16) != 0) {
                close_socket(listener);
                listener = no_socket;
                return false;
            }
            return true;
        }

        void poll(const Server& state) {
            const double t = now();
            if (listener == no_socket) {
                if (!wanted || t < retry_at) return;
                retry_at = t + retry_seconds;
                const int port = (int)ntohs(wanted->sin_port);
                if (!open(*wanted)) {
                    if (!warned) {
                        std::printf("netplay_server: couldn't serve the status on TCP port %d (taken, or its address isn't up yet): "
                                    "trying again every %d s\n", port, (int)retry_seconds);
                        std::fflush(stdout);
                        warned = true;
                    }
                    return;
                }
                std::printf("netplay_server: status on TCP port %d (/, /status.json, /status.txt)\n", port);
                std::fflush(stdout);
            }
            for (int i = 0; i < 16; i++) {
                const Socket s = accept(listener, nullptr, nullptr);
                if (s == no_socket) break;
                if (clients.size() >= max_clients || !non_blocking(s)) {
                    close_socket(s);
                    continue;
                }
                clients.push_back({ s, {}, {}, 0, t });
            }
            for (Client& c : clients) {
                if (c.out.empty()) {
                    char buffer[2048];
                    const int got = (int)recv(c.socket, buffer, sizeof(buffer), 0);
                    if (got > 0) {
                        c.in.append(buffer, (size_t)got);
                    }
                    else if (got == 0 || !would_block()) {
                        c.since = -1e9;     // gone, or broken: closed below
                        continue;
                    }
                    size_t head_end = c.in.find("\r\n\r\n");
                    if (head_end == std::string::npos) head_end = c.in.find("\n\n");     // (a request typed by hand)
                    if (head_end != std::string::npos || c.in.size() >= max_request) {
                        c.out = status::respond(std::string_view(c.in).substr(0, head_end), state.snapshot());
                    }
                }
                if (!c.out.empty() && c.sent < c.out.size()) {
                    const int put = (int)::send(c.socket, c.out.data() + c.sent, (int)(c.out.size() - c.sent), send_flags);
                    if (put > 0) c.sent += (size_t)put;
                    else if (!would_block()) c.since = -1e9;
                    if (c.sent >= c.out.size()) {
                        shutdown(c.socket, shut_write);
                        c.closing_at = t;
                    }
                }
                else if (c.closing_at != 0) {
                    char drop[2048];
                    const int got = (int)recv(c.socket, drop, sizeof(drop), 0);
                    if (got == 0 || (got < 0 && !would_block())) c.since = -1e9;    // the client closed: done
                }
            }
            std::erase_if(clients, [t](Client& c) {
                const bool done = (c.closing_at != 0 && t - c.closing_at > closing_seconds) || t - c.since > client_seconds;
                if (done) close_socket(c.socket);
                return done;
            });
        }
    };

    // coturn's metrics (--turn-metrics: its --prometheus, on this machine), asked for every 15 s, a few seconds
    // allowed for each answer: how many relays it has open.
    struct TurnMetricsReader {
        static constexpr double every_seconds = 15.0;
        static constexpr double answer_seconds = 3.0;
        static constexpr size_t max_answer = 1 << 20;
        sockaddr_in at{};
        Socket socket = no_socket;
        bool asked = false;
        std::string in;
        double since = 0;
        double next = 0;

        void finish(Server& state, bool read) {
            close_socket(socket);
            socket = no_socket;
            if (read) {
                const size_t body = in.find("\r\n\r\n");
                const auto metrics = status::parse_turn_metrics(body == std::string::npos ? std::string_view() : std::string_view(in).substr(body + 4));
                if (metrics) {
                    state.turn_metrics = metrics;
                    state.turn_metrics_at = now();
                }
            }
            in.clear();
        }

        void poll(Server& state) {
            const double t = now();
            if (socket == no_socket) {
                if (t < next) return;
                next = t + every_seconds;
                socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (socket == no_socket) return;
                if (!non_blocking(socket) || (connect(socket, (const sockaddr*)&at, sizeof(at)) != 0 && !would_block())) {
                    finish(state, false);
                    return;
                }
                asked = false;
                since = t;
                return;
            }
            if (t - since > answer_seconds) {
                finish(state, false);
                return;
            }
            if (!asked) {
                // Connected once it can be written to.
#if !defined(_WIN32)
                if (socket >= FD_SETSIZE) {
                    finish(state, false);
                    return;
                }
#endif
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(socket, &writable);
                FD_SET(socket, &failed);
                timeval none{ 0, 0 };
                if (select((int)socket + 1, nullptr, &writable, &failed, &none) <= 0) return;
                // (Windows says a connection that failed in `failed`.)
                if (FD_ISSET(socket, &failed)) {
                    finish(state, false);
                    return;
                }
                static const char request[] = "GET /metrics HTTP/1.0\r\nHost: localhost\r\n\r\n";
                if (::send(socket, request, (int)sizeof(request) - 1, send_flags) != (int)sizeof(request) - 1) {
                    finish(state, false);
                    return;
                }
                asked = true;
            }
            char buffer[4096];
            for (int i = 0; i < 64; i++) {
                const int got = (int)recv(socket, buffer, sizeof(buffer), 0);
                if (got > 0 && in.size() < max_answer) {
                    in.append(buffer, (size_t)got);
                    continue;
                }
                if (got == 0) finish(state, true);          // the whole answer (HTTP/1.0: it closes after)
                else if (got > 0 || !would_block()) finish(state, false);
                return;
            }
        }
    };

    // --state's file: read as the server starts, written each minute the numbers changed and as it stops.
    struct StateFile {
        std::string path;
        std::string written;        // what's in it
        bool failed = false;        // the last write failed (said once, until one works)

        void read(Server& state) {
            std::error_code error;
            const auto type = std::filesystem::status(path, error).type();
            if (type == std::filesystem::file_type::not_found) {
                std::printf("netplay_server: no %s yet: counting from now\n", path.c_str());
                return;
            }
            std::ifstream in(path, std::ios::binary);
            if (type != std::filesystem::file_type::regular || !in) {
                // (Never written over: it may have the counts in it.)
                std::printf("netplay_server: couldn't read %s: counting from now, and not keeping the counts\n", path.c_str());
                path.clear();
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            const std::optional<status::Status> kept = status::load(text);
            if (!kept) {
                std::error_code error;
                std::filesystem::rename(path, path + ".broken", error);
                std::printf("netplay_server: %s isn't a state this server wrote: kept as %s.broken, counting from now\n", path.c_str(), path.c_str());
                return;
            }
            state.restore(*kept);
            written = text;
            std::printf("netplay_server: counts read from %s (since %lld)\n", path.c_str(), (long long)kept->counting_since);
        }

        void write(const Server& state) {
            const std::string text = status::saved(state.snapshot());
            if (text == written) {
                return;
            }
            const std::string temporary = path + ".new";
            bool ok = false;
            int failure = 0;
            if (std::FILE* out = std::fopen(temporary.c_str(), "wb")) {
                ok = std::fwrite(text.data(), 1, text.size(), out) == text.size() && std::fflush(out) == 0;
#if !defined(_WIN32)
                ok = ok && fsync(fileno(out)) == 0;
#endif
                if (!ok) failure = errno;
                ok = std::fclose(out) == 0 && ok;
            }
            if (!ok && failure == 0) failure = errno;
            std::error_code error;
            if (ok) {
                std::filesystem::rename(temporary, path, error);
                ok = !error;
            }
            if (!ok) {
                if (!failed) {
                    std::printf("netplay_server: couldn't write %s (%s): the counts start again when it restarts\n", path.c_str(),
                        error ? error.message().c_str() : std::strerror(failure));
                    std::fflush(stdout);
                }
                failed = true;
                std::filesystem::remove(temporary, error);
                return;
            }
            if (failed) {
                std::printf("netplay_server: %s written again\n", path.c_str());
                std::fflush(stdout);
            }
            failed = false;
            written = text;
        }
    };

    int usage() {
        std::fprintf(stderr, "usage: netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096] [--connection-burst 20]\n"
                             "                      [--max-listed-per-address 2] [--desync-burst 4] [--newest-version <n>]\n"
                             "                      [--turn <host:port> [--turn-hours 12] [--turn-metrics <address:port>]]   (the secret in TURN_SECRET)\n"
                             "                      [--status <address:port>] [--state <file>]\n");
        return 2;
    }
}

int main(int argc, char** argv) {
    int port = 27100;
    Server state;
    StateFile state_file;
    std::optional<sockaddr_in> status_at;
    std::optional<sockaddr_in> metrics_at;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage();
        if (arg == "--turn") {
            state.turn = argv[++i];
            continue;
        }
        if (arg == "--state") {
            state_file.path = argv[++i];
            continue;
        }
        if (arg == "--status" || arg == "--turn-metrics") {
            // (Both optional: a mistake in one is said, and the signalling runs without it.)
            const std::optional<sockaddr_in> at = ipv4_endpoint(argv[++i]);
            if (!at) {
                std::fprintf(stderr, "netplay_server: %s takes an IPv4 address and a port (0.0.0.0:27180, 127.0.0.1:9641), not \"%s\": "
                                     "going without it\n", arg.c_str(), argv[i]);
                continue;
            }
            (arg == "--status" ? status_at : metrics_at) = at;
            continue;
        }
        const int value = std::atoi(argv[++i]);
        if (arg == "--port" && value > 0 && value < 65536) port = value;
        else if (arg == "--turn-hours" && value > 0 && value <= 48) state.turn_hours = value;
        else if (arg == "--connection-burst" && value > 0) state.connection_burst = value;
        else if (arg == "--max-per-address" && value > 0) state.max_per_address = value;
        else if (arg == "--max-listed-per-address" && value > 0) state.max_listed_per_address = value;
        else if (arg == "--desync-burst" && value > 0) state.report_burst = value;
        else if (arg == "--newest-version" && value >= signal::oldest_version && value <= signal::version) state.newest_version = (uint16_t)value;
        else if (arg == "--max-peers" && value > 0) state.max_peers = (size_t)value;
        else return usage();
    }

    if (!state.turn.empty()) {
        const char* secret = std::getenv("TURN_SECRET");
        state.turn_secret = secret != nullptr ? secret : "";
        if (state.turn_secret.size() < 16) {
            std::fprintf(stderr, "netplay_server: --turn needs the secret it shares with the TURN server in TURN_SECRET (16 characters or more)\n");
            return 2;
        }
        static uint8_t buffer[signal::max_packet_size];
        if (signal::encode(signal::Turn{ state.turn, "u", "p" }, buffer) == 0) {
            std::fprintf(stderr, "netplay_server: --turn takes host:port, separated by commas (no spaces, %zu characters at most)\n", signal::max_text);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if ((status_at || metrics_at) && !start_sockets()) {
        std::fprintf(stderr, "netplay_server: no sockets for the status and coturn's metrics: going without them\n");
        status_at.reset();
        metrics_at.reset();
    }
    StatusPage page;
    page.wanted = status_at;
    TurnMetricsReader metrics;
    if (metrics_at) {
        metrics.at = *metrics_at;
    }

    SteamNetworkingErrMsg message;
    if (!GameNetworkingSockets_Init(nullptr, message)) {
        std::fprintf(stderr, "netplay_server: %s\n", message);
        return 1;
    }
    server = &state;

    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = (uint16)port;
    SteamNetworkingConfigValue_t options[4];
    options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
    options[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)signal::max_packet_size);
    options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 128);
    options[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, 16 * (int32_t)signal::max_packet_size);
    state.listen = SteamNetworkingSockets()->CreateListenSocketIP(address, 4, options);
    if (state.listen == k_HSteamListenSocket_Invalid) {
        std::fprintf(stderr, "netplay_server: couldn't listen on UDP port %d\n", port);
        GameNetworkingSockets_Kill();
        return 1;
    }
    state.group = SteamNetworkingSockets()->CreatePollGroup();
    std::printf("netplay_server: listening on UDP port %d (signalling versions %u to %u)%s\n", port, (unsigned)signal::oldest_version,
        (unsigned)state.newest_version,
        state.turn.empty() ? ", no TURN relay" : ", offering a TURN relay");
    std::fflush(stdout);
    if (!state_file.path.empty()) {
        state_file.read(state);
        if (!state_file.path.empty()) {
            state_file.write(state);    // (whether it can be, said now rather than in a minute)
        }
    }
    std::fflush(stdout);
    page.poll(state);   // the status's port, at once (said if it's served, or not yet)

    double next_report = now() + 600;
    double next_save = now() + 60;
    double next_expiry = now();
    while (!stopping) {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();
        SteamNetworkingMessage_t* messages[64];
        int count;
        for (int taken = 0; taken < max_messages_per_loop && (count = sockets->ReceiveMessagesOnPollGroup(state.group, messages, 64)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                state.take(messages[i]->m_conn, { (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize });
                messages[i]->Release();
            }
        }
        if (now() >= next_expiry) {
            state.expire();
            next_expiry = now() + 1;
        }
        page.poll(state);
        if (metrics_at) {
            metrics.poll(state);
        }
        if (now() >= next_report) {
            const status::Status s = state.snapshot();
            const std::string relays = !s.relay ? "" : s.relay_known ? ", " + std::to_string(s.relays) + " relays open" : ", relays unknown";
            std::printf("netplay_server: %zu players online, %zu rooms (%zu public, %zu playing, %zu watching)%s; today %llu players (%zu at once), "
                        "%llu rooms, %llu joined, %llu desyncs; since the start %llu signals passed, rooms %llu created, %llu found, %llu missed; "
                        "%llu lists; %llu desyncs reported (%llu not logged)\n",
                s.players, s.rooms, s.public_rooms, s.public_playing, s.watching, relays.c_str(), (unsigned long long)s.today.players,
                s.today.peak_players, (unsigned long long)s.today.rooms, (unsigned long long)s.today.matches, (unsigned long long)s.today.desyncs,
                (unsigned long long)s.signals, (unsigned long long)s.rooms_created, (unsigned long long)s.rooms_found,
                (unsigned long long)s.rooms_missed, (unsigned long long)s.lists, (unsigned long long)s.desyncs, (unsigned long long)s.desyncs_unlogged);
            std::fflush(stdout);
            next_report = now() + 600;
        }
        if (!state_file.path.empty() && now() >= next_save) {
            state.roll_day();
            state_file.write(state);
            next_save = now() + 60;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!state_file.path.empty()) {
        state_file.write(state);
    }
    std::printf("netplay_server: stopping\n");
    SteamNetworkingSockets()->CloseListenSocket(state.listen);
    server = nullptr;
    GameNetworkingSockets_Kill();
    return 0;
}
