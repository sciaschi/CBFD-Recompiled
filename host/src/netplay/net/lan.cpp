// LAN discovery: see lan.hpp.

#include "lan.hpp"

#include <algorithm>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "join_code.hpp"

namespace conker::netplay {
    namespace {
        // A match not heard from for this long has gone (its host broadcasts about once a second).
        constexpr double heard_seconds = 3.5;
        // The most matches kept (a network can't fill memory with made-up ones), and from one address (one
        // machine can't push the others' off the list).
        constexpr size_t max_heard = 64;
        constexpr int max_per_sender = 2;
        // The most beacons one poll takes.
        constexpr int max_per_poll = 256;

#if defined(_WIN32)
        using Socket = SOCKET;
        constexpr Socket no_socket = INVALID_SOCKET;
        // Winsock, once (it counts its users; never undone: the process ends with it).
        bool start_sockets() {
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
        }
        void close_socket(Socket s) { closesocket(s); }
        bool non_blocking(Socket s) {
            u_long on = 1;
            return ioctlsocket(s, FIONBIO, &on) == 0;
        }
#else
        using Socket = int;
        constexpr Socket no_socket = -1;
        bool start_sockets() { return true; }
        void close_socket(Socket s) { ::close(s); }
        bool non_blocking(Socket s) {
            const int flags = fcntl(s, F_GETFL, 0);
            return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
        }
#endif

        // A UDP socket, non-blocking, or no_socket (with `error`).
        Socket open_socket(std::string& error) {
            if (!start_sockets()) {
                error = "couldn't start the networking";
                return no_socket;
            }
            const Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == no_socket) {
                error = "couldn't open a UDP socket";
                return no_socket;
            }
            if (!non_blocking(s)) {
                close_socket(s);
                error = "couldn't make a UDP socket non-blocking";
                return no_socket;
            }
            return s;
        }

        bool set_flag(Socket s, int level, int name) {
            const int on = 1;
            return setsockopt(s, level, name, (const char*)&on, sizeof(on)) == 0;
        }
    }

    LanBeacon::~LanBeacon() {
        close();
    }

    bool LanBeacon::open(uint16_t port, std::string& error) {
        close();
        const Socket s = open_socket(error);
        if (s == no_socket) {
            return false;
        }
        if (!set_flag(s, SOL_SOCKET, SO_BROADCAST)) {
            close_socket(s);
            error = "couldn't broadcast on this network";
            return false;
        }
        socket_handle = (intptr_t)s;
        to_port = port;
        return true;
    }

    void LanBeacon::send(const lan::Beacon& beacon) {
        uint8_t data[lan::beacon_size];
        if (socket_handle == -1 || !lan::encode(beacon, data)) {
            return;
        }
        // To the network (the default one), and to this machine: a firewall may keep broadcasts out, but not
        // what comes from the machine itself, so two games on it always find each other.
        for (const uint32_t address : { (uint32_t)INADDR_BROADCAST, (uint32_t)INADDR_LOOPBACK }) {
            if (address == INADDR_LOOPBACK && !to_this_machine) {
                continue;
            }
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(to_port);
            to.sin_addr.s_addr = htonl(address);
            // A lost one is no matter: the next comes in a second.
            sendto((Socket)socket_handle, (const char*)data, (int)sizeof(data), 0, (const sockaddr*)&to, sizeof(to));
        }
    }

    void LanBeacon::close() {
        if (socket_handle != -1) {
            close_socket((Socket)socket_handle);
            socket_handle = -1;
        }
    }

    LanListener::~LanListener() {
        close();
    }

    bool LanListener::open(uint16_t port, std::string& error) {
        close();
        const Socket s = open_socket(error);
        if (s == no_socket) {
            return false;
        }
        // Shared: two games on one machine can both listen (a broadcast reaches every socket bound to it).
        set_flag(s, SOL_SOCKET, SO_REUSEADDR);
#if defined(SO_REUSEPORT)
        set_flag(s, SOL_SOCKET, SO_REUSEPORT);
#endif
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(port);
        at.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (const sockaddr*)&at, sizeof(at)) != 0) {
            close_socket(s);
            error = "couldn't listen on UDP port " + std::to_string(port);
            return false;
        }
        socket_handle = (intptr_t)s;
        heard.clear();
        return true;
    }

    void LanListener::poll(double now) {
        if (socket_handle == -1) {
            return;
        }
        for (int taken = 0; taken < max_per_poll; taken++) {
            uint8_t data[lan::beacon_size + 1];   // one more: a bigger datagram isn't a beacon
            sockaddr_in from{};
            socklen_t from_size = sizeof(from);
            const auto size = recvfrom((Socket)socket_handle, (char*)data, (int)sizeof(data), 0, (sockaddr*)&from, &from_size);
            if (size < 0) {
#if defined(_WIN32)
                // Windows says so for a datagram bigger than the buffer (cut short elsewhere), and for an ICMP
                // error: neither is the end of what's there.
                const int error = WSAGetLastError();
                if (error == WSAEMSGSIZE || error == WSAECONNRESET) {
                    continue;
                }
#endif
                break;
            }
            const std::optional<lan::Beacon> beacon = lan::decode({ data, (size_t)size });
            // Only this game's.
            if (!beacon || from.sin_family != AF_INET || beacon->fit != want_fit || !signal::platforms_mix(beacon->platform, want_platform)) {
                continue;
            }
            Endpoint endpoint;
            const uint32_t ip = ntohl(from.sin_addr.s_addr);
            for (int i = 0; i < 4; i++) {
                endpoint.ip[i] = (uint8_t)(ip >> (24 - 8 * i));
            }
            endpoint.port = beacon->port;
            endpoint.key = beacon->key;
            LanMatch match;
            match.address = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
                std::to_string(ip & 255) + ":" + std::to_string(beacon->port);
            match.code = encode_join_code(endpoint);
            match.beacon = *beacon;
            // The same match again (its key: one heard both from the network and from this machine is one): as it
            // is now, from the address it was first heard from (a beacon copied from elsewhere doesn't move it).
            const auto same = std::find_if(heard.begin(), heard.end(), [&match](const Heard& h) {
                return h.match.beacon.key == match.beacon.key;
            });
            if (same != heard.end()) {
                if (same->match.address == match.address) {
                    *same = { match, now };
                }
            }
            else {
                const std::string sender = match.address.substr(0, match.address.rfind(':'));
                const auto from_sender = std::count_if(heard.begin(), heard.end(), [&sender](const Heard& h) {
                    return h.match.address.compare(0, sender.size() + 1, sender + ":") == 0;
                });
                if (from_sender >= max_per_sender) {
                    continue;
                }
                // Full: the one heard from longest ago makes way.
                if (heard.size() >= max_heard) {
                    heard.erase(std::min_element(heard.begin(), heard.end(), [](const Heard& a, const Heard& b) { return a.at < b.at; }));
                }
                heard.push_back({ match, now });
            }
        }
        std::erase_if(heard, [now](const Heard& h) { return now - h.at > heard_seconds; });
    }

    std::vector<LanMatch> LanListener::matches() const {
        std::vector<LanMatch> out;
        for (const Heard& h : heard) {
            out.push_back(h.match);
        }
        std::stable_sort(out.begin(), out.end(), [](const LanMatch& a, const LanMatch& b) {
            const bool a_open = !a.beacon.playing && a.beacon.joined < a.beacon.players;
            const bool b_open = !b.beacon.playing && b.beacon.joined < b.beacon.players;
            return a_open && !b_open;
        });
        return out;
    }

    void LanListener::close() {
        if (socket_handle != -1) {
            close_socket((Socket)socket_handle);
            socket_handle = -1;
        }
        heard.clear();
    }
}
