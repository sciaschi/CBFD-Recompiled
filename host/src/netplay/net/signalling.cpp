// A peer's side of signalling: see signalling.hpp.

#include "signalling.hpp"

#include <initializer_list>
#include <cstdio>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace conker::netplay {
    namespace {
        // Signals waiting to be sent, at most: the library sends a few a second per connection, so more
        // means the server is gone or slow, and the oldest are dropped (the library resends what it needs).
        constexpr size_t max_queued = 64;
        // The most packets one poll takes from the server.
        constexpr int max_messages_per_poll = 64;

        Signaller* current = nullptr;
    }

    void signaller_status_changed(void* info) {
        if (current != nullptr) {
            current->on_status_changed(info);
        }
    }

    namespace {
        void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
            signaller_status_changed(info);
        }
    }

    std::string resolve_address(const std::string& host_port, std::string& error) {
        // A literal address as it is.
        SteamNetworkingIPAddr parsed;
        if (parsed.ParseString(host_port.c_str()) && parsed.m_port != 0) {
            char out[SteamNetworkingIPAddr::k_cchMaxString];
            parsed.ToString(out, sizeof(out), true);
            return out;
        }
        const size_t colon = host_port.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == host_port.size()) {
            error = "\"" + host_port + "\" isn't a server's name and port";
            return {};
        }
        const std::string host = host_port.substr(0, colon);
        const std::string port = host_port.substr(colon + 1);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
            error = "couldn't find the server \"" + host + "\"";
            return {};
        }
        // The first IPv4 address, else the first IPv6 one.
        SteamNetworkingIPAddr address;
        address.Clear();
        for (int want_v4 = 1; want_v4 >= 0 && address.IsIPv6AllZeros(); want_v4--) {
            for (addrinfo* r = results; r != nullptr; r = r->ai_next) {
                if (want_v4 && r->ai_family == AF_INET) {
                    const auto* in = (const sockaddr_in*)r->ai_addr;
                    address.SetIPv4(ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
                    break;
                }
                if (!want_v4 && r->ai_family == AF_INET6) {
                    const auto* in6 = (const sockaddr_in6*)r->ai_addr;
                    address.SetIPv6(in6->sin6_addr.s6_addr, ntohs(in6->sin6_port));
                    break;
                }
            }
        }
        freeaddrinfo(results);
        if (address.IsIPv6AllZeros()) {
            error = "couldn't find the server \"" + host + "\"";
            return {};
        }
        char out[SteamNetworkingIPAddr::k_cchMaxString];
        address.ToString(out, sizeof(out), true);
        return out;
    }

    Signaller::Signaller() {
        current = this;
    }

    Signaller::~Signaller() {
        close();
        current = nullptr;
    }

    bool Signaller::connect(const std::string& server, std::string& error) {
        close();
        failure.clear();
        speaks = newest;
        offered_turn.reset();
        creating_room = false;
        asked_room = created_room = 0;
        room_lookup = Lookup::none;
        published.reset();
        room_listing = Lookup::none;
        listed = {};
        const size_t colon = server.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == server.size()) {
            error = "\"" + server + "\" isn't a server's name and port";
            return false;
        }
        current_state = State::connecting;
        // A literal address at once; a name on a thread of its own (a slow or missing DNS server would
        // hold the caller for many seconds).
        SteamNetworkingIPAddr literal;
        if (literal.ParseString(server.c_str()) && literal.m_port != 0) {
            open(server);
            return true;
        }
        auto lookup = std::make_shared<Resolving>();
        lookup_in_progress = lookup;
        std::thread([lookup, server] {
            std::string error;
            const std::string address = resolve_address(server, error);
            std::lock_guard lock(lookup->mutex);
            lookup->address = address;
            lookup->error = error;
            lookup->done = true;
        }).detach();
        return true;
    }

    void Signaller::open(const std::string& address_text) {
        address = address_text;
        SteamNetworkingIPAddr ip;
        ip.ParseString(address_text.c_str());
        SteamNetworkingConfigValue_t options[4];
        options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
        options[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)signal::max_packet_size);
        options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 256);
        options[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, 256 * (int32_t)signal::max_packet_size);
        connection = SteamNetworkingSockets()->ConnectByIPAddress(ip, 4, options);
        if (connection == k_HSteamNetConnection_Invalid) {
            fail("couldn't connect to the server");
        }
    }

    void Signaller::fail(const std::string& why) {
        if (connection != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, false);
            connection = k_HSteamNetConnection_Invalid;
        }
        current_state = State::failed;
        registering = false;
        failure = why;
    }

    void Signaller::on_status_changed(const void* data) {
        const auto* info = (const SteamNetConnectionStatusChangedCallback_t*)data;
        if (info->m_hConn != connection || connection == k_HSteamNetConnection_Invalid) {
            return;
        }
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connected: {
                uint8_t buffer[signal::max_packet_size];
                const size_t size = signal::encode(signal::Register{ local_id() }, buffer, speaks);
                SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
                registering = true;
                break;
            }
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // An older server, which doesn't speak this version: it closes the connection for that (its
                // Refused, in its own version, may not even be read first).
                if (registering && info->m_info.m_eEndReason == k_ESteamNetConnectionEnd_App_Min + (int)signal::Refusal::version &&
                    speaks > signal::oldest_version) {
                    SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, false);
                    connection = k_HSteamNetConnection_Invalid;
                    registering = false;
                    speak_older = true;
                    break;
                }
                std::string why = info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
                    ? "couldn't reach the server" : "the server closed the connection";
                std::string detail;
                for (const char* c = info->m_info.m_szEndDebug; *c != '\0'; c++) {
                    detail += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
                }
                fail(detail.empty() ? why : why + " (" + detail + ")");
                break;
            }
            default:
                break;
        }
    }

    void Signaller::poll() {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        if (lookup_in_progress) {
            std::string address, error;
            {
                std::lock_guard lock(lookup_in_progress->mutex);
                if (lookup_in_progress->done) {
                    address = lookup_in_progress->address;
                    error = lookup_in_progress->error;
                }
            }
            if (!address.empty() || !error.empty()) {
                lookup_in_progress.reset();
                if (address.empty()) fail(error);
                else open(address);
            }
        }
        sockets->RunCallbacks();
        if (speak_older) {
            speak_older = false;
            speaks--;
            std::printf("[netplay] the netplay server is older: speaking its signalling version %u\n", (unsigned)speaks);
            open(address);
            return;
        }
        if (current_state != State::ready && !registering) {
            return;
        }

        SteamNetworkingMessage_t* messages[16];
        int count;
        for (int taken = 0; (current_state == State::ready || registering) && taken < max_messages_per_poll &&
            (count = sockets->ReceiveMessagesOnConnection(connection, messages, 16)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                const std::span<const uint8_t> data{ (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize };
                const std::optional<signal::Packet> packet = signal::decode(data, speaks);
                // Read before the message is released, with its bytes.
                const bool other_version = !packet && signal::other_version(data, speaks);
                messages[i]->Release();
                if (current_state != State::ready && !registering) {
                    continue;
                }
                if (other_version && registering) {
                    // An older server's Refused, before it closes the connection (on_status_changed).
                    continue;
                }
                if (!packet) {
                    fail(other_version ? "the server runs another version" : "the server sent a broken packet");
                }
                else if (const signal::Registered* r = std::get_if<signal::Registered>(&*packet); r != nullptr && registering && r->id == local_id()) {
                    registering = false;
                    current_state = State::ready;
                }
                else if (const signal::Turn* t = std::get_if<signal::Turn>(&*packet); t != nullptr && registering && !offered_turn) {
                    offered_turn = *t;
                }
                else if (registering) {
                    fail("the server sent a packet before taking this one's id");
                }
                else if (const signal::Signal* s = std::get_if<signal::Signal>(&*packet)) {
                    incoming.push_back(*s);
                }
                else if (const signal::Unreachable* u = std::get_if<signal::Unreachable>(&*packet)) {
                    gone.push_back(u->peer);
                }
                else if (const signal::RoomCreated* c = std::get_if<signal::RoomCreated>(&*packet); c != nullptr && creating_room) {
                    creating_room = false;
                    created_room = c->room;
                }
                else if (const signal::RoomFound* f = std::get_if<signal::RoomFound>(&*packet);
                    f != nullptr && room_lookup == Lookup::waiting && f->room == asked_room) {
                    found_room = *f;
                    room_lookup = Lookup::found;
                }
                else if (const signal::NoRoom* n = std::get_if<signal::NoRoom>(&*packet);
                    n != nullptr && room_lookup == Lookup::waiting && n->room == asked_room) {
                    room_lookup = Lookup::missing;
                }
                else if (const signal::Rooms* l = std::get_if<signal::Rooms>(&*packet); l != nullptr && room_listing == Lookup::waiting) {
                    listed = *l;
                    room_listing = Lookup::found;
                }
                else if (const signal::Refused* r = std::get_if<signal::Refused>(&*packet)) {
                    switch (r->reason) {
                        case signal::Refusal::version: fail("the server runs another version"); break;
                        case signal::Refusal::taken: fail("the server refused this id (try again)"); break;
                        case signal::Refusal::busy: fail("the server is busy (or too many room codes were wrong: wait a minute)"); break;
                        default: fail("the server refused a packet"); break;
                    }
                }
                else {
                    fail("the server sent a packet a server doesn't");
                }
            }
        }

        // Once registered: what was queued.
        if (current_state == State::ready) {
            uint8_t buffer[signal::max_packet_size];
            for (const signal::Signal& s : out->take()) {
                const size_t size = signal::encode(s, buffer, speaks);
                if (size != 0) {
                    sockets->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
                }
            }
        }
    }

    bool Signaller::send_now(const signal::Packet& packet) {
        uint8_t buffer[signal::max_packet_size];
        const size_t size = signal::encode(packet, buffer, speaks);
        return size != 0 && current_state == State::ready &&
            SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK;
    }

    void Signaller::create_room(uint64_t key) {
        if (current_state != State::ready || created_room != 0 || creating_room) {
            return;
        }
        creating_room = true;
        send_now(signal::CreateRoom{ key });
    }

    void Signaller::find_room(uint64_t room) {
        if (current_state != State::ready || room_lookup == Lookup::waiting) {
            return;
        }
        asked_room = room;
        room_lookup = Lookup::waiting;
        send_now(signal::FindRoom{ room });
    }

    void Signaller::publish(const Hello& hello, const signal::ListedRoom& room, signal::Platform platform) {
        if (current_state != State::ready || created_room == 0 || !has_public_rooms()) {
            return;
        }
        signal::Publish packet{ fit_of(hello), game_of(hello), rom_of(hello), platform, room };
        packet.room.room = created_room;
        packet.room.host = hello.player;
        if (published != packet) {
            published = packet;
            send_now(packet);
        }
    }

    void Signaller::list_rooms(const Hello& hello, signal::Platform platform) {
        if (current_state != State::ready || room_listing == Lookup::waiting || !has_public_rooms()) {
            return;
        }
        room_listing = Lookup::waiting;
        send_now(signal::ListRooms{ fit_of(hello), game_of(hello), rom_of(hello), platform });
    }

    namespace {
        // splitmix64 over the hashes, in order; never 0.
        uint64_t mix(std::initializer_list<uint64_t> parts) {
            uint64_t hash = 0;
            for (const uint64_t part : parts) {
                uint64_t x = hash ^ part;
                x += 0x9E3779B97F4A7C15ull;
                x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
                x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
                hash = x ^ (x >> 31);
            }
            return hash != 0 ? hash : 1;
        }
    }

    bool Signaller::report_desync(const signal::DesyncReport& report) {
        return has_desync_reports() && send_now(report);
    }

    uint64_t fit_of(const Hello& hello) {
        return mix({ hello.build, hello.rom, hello.profile, hello.mods });
    }

    uint64_t game_of(const Hello& hello) {
        return mix({ hello.build, hello.rom });
    }

    uint64_t rom_of(const Hello& hello) {
        return mix({ hello.rom });
    }

    Ice Signaller::ice(bool relay_only) const {
        Ice ice;
        ice.relay_only = relay_only;
        if (offered_turn) {
            // The library wants a user and password for each server, or takes none.
            ice.turn = ice.stun = offered_turn->servers;
            ice.turn_user = offered_turn->user;
            ice.turn_password = offered_turn->password;
            for (char c : offered_turn->servers) {
                if (c == ',') {
                    ice.turn_user += "," + offered_turn->user;
                    ice.turn_password += "," + offered_turn->password;
                }
            }
        }
        return ice;
    }

    void SignalOutbox::push(uint64_t peer, std::span<const uint8_t> data) {
        if (peer == 0 || data.empty() || data.size() > signal::max_signal) {
            return;
        }
        std::lock_guard lock(mutex);
        if (queue.size() >= max_queued) {
            queue.erase(queue.begin());
        }
        queue.push_back({ peer, { data.begin(), data.end() } });
    }

    std::vector<signal::Signal> SignalOutbox::take() {
        std::lock_guard lock(mutex);
        return std::exchange(queue, {});
    }

    std::vector<signal::Signal> Signaller::received() {
        return std::exchange(incoming, {});
    }

    std::vector<uint64_t> Signaller::unreachable() {
        return std::exchange(gone, {});
    }

    void Signaller::close() {
        lookup_in_progress.reset();
        if (connection != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, true);
            connection = k_HSteamNetConnection_Invalid;
        }
        if (current_state != State::failed) {
            current_state = State::idle;
        }
        registering = false;
    }
}
