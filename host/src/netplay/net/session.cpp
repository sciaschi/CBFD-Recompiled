// A netplay session over GameNetworkingSockets: see session.hpp.

#include "session.hpp"
#include "join_code.hpp"
#include "signalling.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/types.h>
#include <sys/random.h>   // getentropy (Linux, macOS)
#endif

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>

namespace conker::netplay {
    namespace {
        // `inputs` from read `from` on (within them).
        Inputs inputs_from_read(const Inputs& inputs, uint64_t from) {
            const int skip = (int)(from - inputs.first_read);
            Inputs rest = inputs;
            rest.first_read = (uint32_t)from;
            rest.count = (uint8_t)(inputs.count - skip);
            rest.pads = {};
            rest.motions = {};
            for (int i = 0; i < rest.count; i++) {
                rest.pads[i] = inputs.pads[skip + i];
                rest.motions[i] = inputs.motions[skip + i];
            }
            return rest;
        }

        // A joiner that hasn't finished the handshake in this long is dropped (an honest Hello takes a
        // round trip).
        constexpr double handshake_seconds = 3.0;
        // Connections the host takes at once, joined or not: more are turned away unread (the players and
        // spectators, and a few on their way in).
        constexpr size_t max_connections = max_players - 1 + max_spectators + 4;
        // What the library buffers from one peer, unread: a message bigger than the largest packet closes
        // the connection before it's buffered, and past these limits more are dropped (and resent, as
        // everything is sent reliably). An honest peer is never more than a few dozen messages ahead.
        constexpr int recv_buffer_messages = 512;
        // Inputs' early copies go only while less than this waits to be sent on the connection.
        constexpr int copy_most_pending = 8 * (int)max_packet_size;
        constexpr int recv_buffer_bytes = recv_buffer_messages * (int)max_packet_size;
        // A joiner's from the host, which may send it a state after a desync (StatePart, up to max_message_size):
        // a few parts' worth, and the rest is resent as it's read. Its only peer is the host, whose state it loads
        // anyway (lockstep checks it).
        constexpr int joiner_recv_buffer_bytes = 4 * 1024 * 1024;
        // The host sending a state: at this rate (the library's own is 256 KB/s, which would take seconds over a
        // state of 1 or 2 MB), with room to queue it whole, then back to the library's own once it's all gone.
        constexpr int state_send_rate = 1024 * 1024;
        constexpr int default_send_rate = 256 * 1024;
        constexpr int state_send_buffer = (int)max_state_size + 1024 * 1024;
        constexpr int default_send_buffer = 512 * 1024;
        // The most messages one poll() takes: a flood can't hold the game's thread there.
        constexpr int max_messages_per_poll = 256;
        // The most times a port can be come back to (or joined, under way) in a match: each holds everyone up while
        // the state goes.
        constexpr int max_rejoins = 10;

        // The options for every connection, as the host's listen socket or a joiner's connection, and how an
        // ICE one finds its way (the strings stay `ice`'s, which must outlive the call they're given to).
        struct ConnectionOptions {
            SteamNetworkingConfigValue_t values[10];
            int count = 4;
            // `joining`: a joiner's connection to the host, which takes the host's states.
            ConnectionOptions(void (*status_changed)(SteamNetConnectionStatusChangedCallback_t*), const Ice* ice = nullptr, bool joining = false) {
                values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
                values[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)(joining ? max_message_size : max_packet_size));
                values[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, joining ? joiner_recv_buffer_bytes : recv_buffer_bytes);
                values[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, recv_buffer_messages);
                if (ice != nullptr) {
                    values[count++].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, ice->relay_only
                        ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, ice->relay_only ? "" : ice->stun.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, ice->turn.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, ice->turn_user.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, ice->turn_password.c_str());
                }
            }
        };


        // Whether two keys are the same, taking the same time whatever they are (no early exit for a
        // stranger to time).
        bool same_key(uint64_t a, uint64_t b) {
            volatile uint64_t difference = 0;
            for (int i = 0; i < 64; i += 8) {
                difference = difference | (((a >> i) ^ (b >> i)) & 0xFF);
            }
            return difference == 0;
        }

        Session* current = nullptr;
        uint64_t process_id = 0;

        // The library's identity for a process's id, and back (0: not one of ours).
        SteamNetworkingIdentity identity_of(uint64_t id) {
            char text[32];
            std::snprintf(text, sizeof(text), "conker-%016llx", (unsigned long long)id);
            SteamNetworkingIdentity identity;
            identity.SetGenericString(text);
            return identity;
        }
        uint64_t id_of(const SteamNetworkingIdentity& identity) {
            const char* text = identity.GetGenericString();
            unsigned long long id = 0;
            char end = 0;
            if (text == nullptr || std::strlen(text) != 23 || std::sscanf(text, "conker-%16llx%c", &id, &end) != 1) {
                return 0;
            }
            return id;
        }

        // A connection's signals, to its peer's id through the signaller's outbox (which it keeps, as the
        // library may send one after the session has gone).
        class ConnectionSignals : public ISteamNetworkingConnectionSignaling {
        public:
            ConnectionSignals(std::shared_ptr<SignalOutbox> outbox, uint64_t peer) : outbox(std::move(outbox)), peer(peer) {}
            bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override {
                outbox->push(peer, { (const uint8_t*)data, (size_t)size });
                return true;
            }
            void Release() override { delete this; }
        private:
            std::shared_ptr<SignalOutbox> outbox;
            uint64_t peer;
        };

        double now() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        void debug_output(ESteamNetworkingSocketsDebugOutputType, const char* message) {
            std::fprintf(stderr, "[netplay] %s\n", message);
        }

        // The reason a connection was closed for: ours if the peer gave one, otherwise the library's.
        Reason reason_of(const SteamNetConnectionInfo_t& info) {
            const int code = info.m_eEndReason - k_ESteamNetConnectionEnd_App_Min;
            if (code >= 1 && code <= last_reason) {
                return (Reason)code;
            }
            return info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ? Reason::timeout : Reason::quit;
        }
    }

    // Signals that arrive for this process: someone connecting to the host (each gets its own
    // ConnectionSignals), or a refusal to send back. A joiner takes no new connections.
    struct Session::IceContext : ISteamNetworkingSignalingRecvContext {
        std::shared_ptr<SignalOutbox> outbox;
        bool accepting = false;
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection, const SteamNetworkingIdentity& peer, int) override {
            const uint64_t id = id_of(peer);
            return accepting && id != 0 ? new ConnectionSignals(outbox, id) : nullptr;
        }
        void SendRejectionSignal(const SteamNetworkingIdentity& peer, const void* data, int size) override {
            if (const uint64_t id = id_of(peer); id != 0) {
                outbox->push(id, { (const uint8_t*)data, (size_t)size });
            }
        }
    };

    void status_changed_callback(void* info) {
        if (current != nullptr) {
            current->on_status_changed(info);
        }
    }

    namespace {
        void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
            status_changed_callback(info);
        }
    }

    std::string join_code_for(const std::string& address, uint64_t key) {
        SteamNetworkingIPAddr parsed;
        if (!parsed.ParseString(address.c_str())) {
            return {};
        }
        Endpoint endpoint;
        endpoint.port = parsed.m_port;
        endpoint.key = key;
        if (parsed.IsIPv4()) {
            const uint32_t ip = parsed.GetIPv4();
            endpoint.ip = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        }
        else {
            endpoint.ipv6 = true;
            std::copy(std::begin(parsed.m_ipv6), std::end(parsed.m_ipv6), endpoint.ip.begin());
        }
        return encode_join_code(endpoint);
    }

    // 64 bits from the system's random source (never 0, which is "none"): a match's key, an id.
    bool random_key(uint64_t& key) {
        do {
#if defined(_WIN32)
            if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&key, sizeof(key), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                return false;
            }
#else
            if (getentropy(&key, sizeof(key)) != 0) {
                return false;
            }
#endif
        } while (key == 0);
        return true;
    }

    bool startup(std::string& error) {
        if (!random_key(process_id)) {
            error = "couldn't make this process's id (no random source)";
            return false;
        }
        const SteamNetworkingIdentity identity = identity_of(process_id);
        SteamNetworkingErrMsg message;
        if (!GameNetworkingSockets_Init(&identity, message)) {
            error = message;
            return false;
        }
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, debug_output);
        return true;
    }

    uint64_t local_id() {
        return process_id;
    }

    bool new_id(std::string& error) {
        uint64_t id = 0;
        if (!random_key(id)) {
            error = "couldn't make a new id (no random source)";
            return false;
        }
        process_id = id;
        const SteamNetworkingIdentity identity = identity_of(process_id);
        SteamNetworkingSockets()->ResetIdentity(&identity);
        return true;
    }

    void shutdown() {
        GameNetworkingSockets_Kill();
    }

    void set_fake_network(const FakeNetwork& fake) {
        ISteamNetworkingUtils* utils = SteamNetworkingUtils();
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, fake.lag_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, fake.jitter_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, 4 * fake.jitter_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, fake.jitter_ms > 0 ? 100.0f : 0.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, fake.outage ? 100.0f : fake.loss);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, fake.outage ? 100.0f : 0.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, fake.reorder);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 20);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, fake.dup);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, 20);
    }

    Session::Session() {
        current = this;
    }

    Session::~Session() {
        if (!peers.empty()) {
            leave(Reason::quit);
        }
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            SteamNetworkingSockets()->CloseListenSocket(listen_socket);
        }
        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingSockets()->DestroyPollGroup(poll_group);
        }
        current = nullptr;
    }

    bool Session::begin_hosting(int players, const Hello& me, std::string& error, int local) {
        if (players < 2 || players > max_players) {
            error = "a match is 2 to 4 players";
            return false;
        }
        if (local < 1 || local >= players) {
            error = "a match needs a player on another machine";
            return false;
        }
        hello = me;
        if (!random_key(hello.key)) {
            error = "couldn't make the match's key (no random source)";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        is_host = true;
        own_ports = (uint8_t)((1u << local) - 1);
        match_players = players;
        return true;
    }

    bool Session::host(uint16_t port, int players, const Hello& me, std::string& error, int local) {
        if (!begin_hosting(players, me, error, local)) {
            return false;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        address.m_port = port;
        ConnectionOptions options(status_changed);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketIP(address, options.count, options.values);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen on UDP port " + std::to_string(port);
            return false;
        }
        return true;
    }

    bool Session::host(Signaller& through, const Ice& ice, int players, const Hello& me, std::string& error, int local) {
        if (!begin_hosting(players, me, error, local)) {
            return false;
        }
        ConnectionOptions options(status_changed, &ice);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketP2P(0, options.count, options.values);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen for peer-to-peer connections";
            return false;
        }
        signaller = &through;
        ice_context = std::make_unique<IceContext>();
        ice_context->outbox = through.outbox();
        ice_context->accepting = true;
        return true;
    }

    bool Session::join(const std::string& code, const Hello& me, std::string& error) {
        const std::optional<Endpoint> endpoint = decode_join_code(code);
        if (!endpoint) {
            error = "\"" + code + "\" isn't a join code (or has a typo)";
            return false;
        }
        SteamNetworkingIPAddr host_address;
        if (endpoint->ipv6) {
            host_address.SetIPv6(endpoint->ip.data(), endpoint->port);
        }
        else {
            const auto& ip = endpoint->ip;
            host_address.SetIPv4((uint32_t)ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3], endpoint->port);
        }
        ConnectionOptions options(status_changed, nullptr, true);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectByIPAddress(host_address, options.count, options.values);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the host";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        hello = me;
        hello.key = endpoint->key;
        peers.push_back({ connection, 0, 0, now() });  // the host, from port 0
        return true;
    }

    bool Session::join(Signaller& through, const Ice& ice, uint64_t host_id, uint64_t key, const Hello& me, std::string& error) {
        if (host_id == 0 || host_id == process_id) {
            error = "that isn't another host's id";
            return false;
        }
        signaller = &through;
        ice_context = std::make_unique<IceContext>();
        ice_context->outbox = through.outbox();
        const SteamNetworkingIdentity host_identity = identity_of(host_id);
        ConnectionOptions options(status_changed, &ice, true);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectP2PCustomSignaling(
            new ConnectionSignals(through.outbox(), host_id), &host_identity, 0, options.count, options.values);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the host";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        ice_host = host_id;
        hello = me;
        hello.key = key;
        peers.push_back({ connection, 0, 0, now() });  // the host, from port 0
        return true;
    }

    void Session::take_signals(std::vector<Event>& events) {
        if (signaller == nullptr) {
            return;
        }
        signaller->poll();
        for (const signal::Signal& s : signaller->received()) {
            SteamNetworkingSockets()->ReceivedP2PCustomSignal(s.data.data(), (int)s.data.size(), ice_context.get());
        }
        // A joiner still finding its way to the host: ended if the host isn't on the server, or the server
        // can't be reached. (Once connected, the signaller doesn't matter to it.)
        const std::vector<uint64_t> gone = signaller->unreachable();
        if (!is_host && !peers.empty() && !is_welcomed) {
            std::string why;
            if (std::find(gone.begin(), gone.end(), ice_host) != gone.end()) {
                why = "the host isn't on the signalling server";
            }
            else if (signaller->state() == Signaller::State::failed) {
                why = "signalling: " + signaller->error();
            }
            if (!why.empty()) {
                SteamNetworkingSockets()->CloseConnection(peers[0].connection, 0, nullptr, false);
                peers.clear();
                Event event{ Event::Type::ended };
                event.reason = Reason::timeout;
                event.detail = why;
                event.by_signalling = true;
                events.push_back(std::move(event));
            }
        }
    }

    bool Session::start(bool own_view, const std::array<uint8_t, max_players>& delays, int rollback, bool open_seats) {
        uint8_t taken = own_ports;
        for (const Peer& peer : peers) {
            taken |= peer.ports;
        }
        const uint8_t open = (uint8_t)(((1u << match_players) - 1) & ~taken);
        if (!is_host || !entry_open || (open != 0 && !open_seats) ||
            std::any_of(delays.begin(), delays.end(), [](uint8_t d) { return d > max_delay; }) || rollback < 0 || rollback > max_rollback) {
            return false;
        }
        // (The roster as it is now first: a joiner moved this very poll learns where it is before the Start.)
        if (const Roster now_roster = roster(); !(now_roster == sent_roster)) {
            for (const Peer& peer : peers) {
                if (in(peer)) send_to(peer.connection, now_roster);
            }
            sent_roster = now_roster;
        }
        entry_open = false;
        match_started = true;
        next_inputs.fill(0);
        const Start start{ (uint8_t)match_players, (uint8_t)(own_view ? 1 : 0), delays, pairs(), (uint8_t)rollback, open };
        match_own_view = start.own_view;
        match_pairs = start.pairs;
        match_rollback = start.rollback;
        match_open = start.open;
        for (const Peer& peer : peers) {
            if (in(peer)) {
                send_to(peer.connection, start);
            }
        }
        return true;
    }

    uint8_t Session::spectators() const {
        uint8_t out = 0;
        for (const Peer& peer : peers) {
            if (peer.spectator >= 0) out |= (uint8_t)(1u << peer.spectator);
        }
        return out;
    }

    int Session::free_spectator() const {
        const uint8_t taken = spectators();
        for (int n = 0; n < max_spectators; n++) {
            if ((taken & (1u << n)) == 0) return n;
        }
        return -1;
    }

    uint8_t Session::free_ports() const {
        uint8_t taken = own_ports;
        for (const Peer& peer : peers) {
            taken |= peer.ports;
        }
        const double t = now();
        uint8_t free = 0;
        for (int p = 0; p < match_players; p++) {
            const bool held = rejoins[p] >= max_rejoins || (left_players[p] != 0 && t - left_at[p] < seat_hold);
            if ((taken & (1u << p)) == 0 && !held) free |= (uint8_t)(1u << p);
        }
        return free;
    }

    uint8_t Session::pairs() const {
        uint8_t out = std::popcount(own_ports) == 2 ? own_ports : 0;
        for (const Peer& peer : peers) {
            if (std::popcount(peer.ports) == 2) {
                out |= peer.ports;
            }
        }
        return out;
    }

    std::vector<int> Session::round_trips_ms() const {
        std::vector<int> out;
        for (const Peer& peer : peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (peer.port >= 0 &&
                SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK &&
                status.m_nPing >= 0) {
                out.push_back(status.m_nPing);
            }
        }
        return out;
    }

    std::array<int, max_players> Session::round_trips_by_port() const {
        std::array<int, max_players> out;
        out.fill(-1);
        for (const Peer& peer : peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) != k_EResultOK ||
                status.m_nPing < 0) {
                continue;
            }
            const uint8_t ports = is_host ? peer.ports : 0x01;
            for (int port = 0; port < max_players; port++) {
                if ((ports & (1u << port)) != 0) out[port] = status.m_nPing;
            }
        }
        return out;
    }

    std::string Session::route(int port) const {
        for (const Peer& peer : peers) {
            if (is_host ? (peer.ports & (1u << port)) == 0 : port != 0) {
                continue;
            }
            SteamNetConnectionInfo_t info;
            if (!SteamNetworkingSockets()->GetConnectionInfo(peer.connection, &info)) {
                return "?";
            }
            // Through ICE, not the address (the library gives the peer's own on a direct route: not for logs).
            if (signaller != nullptr || info.m_addrRemote.IsIPv6AllZeros()) {
                return info.m_szConnectionDescription;
            }
            char address[SteamNetworkingIPAddr::k_cchMaxString];
            info.m_addrRemote.ToString(address, sizeof(address), true);
            return std::string(address) + " (" + info.m_szConnectionDescription + ")";
        }
        return "?";
    }

    int Session::joiners() const {
        return (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return p.port >= 0; });
    }

    int Session::arriving() const {
        return (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return !in(p); });
    }

    void Session::set_room_settings(bool own_view, int delay, int rollback, bool spectators) {
        settings.own_view = own_view ? 1 : 0;
        settings.spectators = spectators ? 1 : 0;
        settings.delay = delay < 0 ? auto_delay : (uint8_t)(std::min)(delay, max_delay);
        settings.rollback = (uint8_t)std::clamp(rollback, 0, max_rollback);
    }

    void Session::set_ready(bool ready) {
        if (is_host) {
            own_ready = ready;
        }
        else if (is_welcomed && own_ports != 0 && !started && !peers.empty()) {
            send_to(peers[0].connection, Ready{ (uint8_t)(ready ? 1 : 0) });
        }
    }

    bool Session::kick(int port, bool keep_out) {
        if (!is_host || port < 0 || port >= max_players) {
            return false;
        }
        for (Peer& peer : peers) {
            if (peer.port >= 0 && (peer.ports & (1u << port)) != 0) {
                // (Not back in this match, as a new player either.) Not kept out: it leaves as one quitting.
                if (match_started && keep_out) kicked.push_back(peer.player);
                drop(peer, keep_out ? Reason::kicked : Reason::quit, &pending_events);
                return true;
            }
        }
        return false;
    }

    bool Session::kick_spectator(int spectator, uint64_t player, bool keep_out) {
        for (Peer& peer : peers) {
            if (is_host && spectator >= 0 && peer.spectator == spectator && (player == 0 || peer.player == player)) {
                if (match_started && keep_out) kicked.push_back(peer.player);
                drop(peer, Reason::kicked, &pending_events);
                return true;
            }
        }
        return false;
    }

    Roster Session::roster() const {
        if (!is_host) {
            return host_roster;
        }
        Roster r = settings;
        r.players = (uint8_t)match_players;
        const auto seat = [&r](uint8_t ports, const Seat& s) {
            for (int port = 0; port < max_players; port++) {
                if ((ports & (1u << port)) != 0) r.seats[port] = s;
            }
        };
        seat(own_ports, { hello.player, hello.platform, (uint8_t)(own_ready ? 1 : 0), 0 });
        for (const Peer& peer : peers) {
            if (peer.spectator >= 0) {
                r.watchers[peer.spectator] = { peer.player, peer.platform };
            }
            if (peer.port < 0) continue;
            SteamNetConnectionRealTimeStatus_t status;
            uint16_t trip = 0xFFFF;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK && status.m_nPing >= 0) {
                trip = (uint16_t)(std::min)(status.m_nPing, 0xFFFE);
            }
            seat(peer.ports, { peer.player, peer.platform, (uint8_t)(peer.ready ? 1 : 0), trip });
        }
        return r;
    }

    bool Session::everyone_ready(bool full) const {
        if (!is_host || !own_ready) {
            return false;
        }
        int taken = std::popcount(own_ports);
        for (const Peer& peer : peers) {
            if (peer.port < 0) continue;
            if (!peer.ready) return false;
            taken += std::popcount(peer.ports);
        }
        return taken == match_players || !full;
    }

    std::string Session::join_code(const std::string& address) const {
        return is_host ? join_code_for(address, hello.key) : std::string();
    }

    Session::Peer* Session::find(uint32_t connection) {
        auto it = std::find_if(peers.begin(), peers.end(), [connection](const Peer& p) { return p.connection == connection; });
        return it == peers.end() ? nullptr : &*it;
    }

    void Session::on_status_changed(const void* data) {
        const auto* info = (const SteamNetConnectionStatusChangedCallback_t*)data;
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                // Someone connecting to the host: taken in, to say Hello (or be refused) within the time.
                if (is_host && info->m_info.m_hListenSocket == listen_socket) {
                    // Once the match has started, only a player who left it coming back, a new one with a port free, or a
                    // spectator with room for one (its Hello says which): else turned away at once, not held until their Hello.
                    if (!entry_open && (closed || (free_ports() == 0 && (!takes_spectators() || free_spectator() < 0) &&
                        std::all_of(left_players.begin(), left_players.end(), [](uint64_t p) { return p == 0; })))) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::started, "started", false);
                        return;
                    }
                    if (peers.size() >= max_connections || sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::full, "full", false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, poll_group);
                    peers.push_back({ info->m_hConn, -1, 0, now() });
                }
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                if (!is_host && find(info->m_hConn) != nullptr) {
                    send_to(info->m_hConn, hello);
                }
                // The handshake time runs from here: through ICE, finding the way can take longer than
                // the time itself (the library's own connect timeout covers a way never found).
                else if (Peer* peer = find(info->m_hConn); is_host && peer != nullptr) {
                    peer->connected_at = now();
                    peer->up = true;
                }
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // What the peer sent before closing is read first, in order: closing the connection would
                // throw it away, and a peer's last words matter (after a desync, its Hash for that read, then
                // its Bye). A Bye among them ends the peer itself.
                drain(info->m_hConn, pending_events);
                Peer* peer = find(info->m_hConn);
                // With why, should the other end still hear it (a connection that timed out here only).
                sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)reason_of(info->m_info), nullptr, false);
                if (peer == nullptr) {
                    break;
                }
                Event event{ is_host ? Event::Type::left : Event::Type::ended };
                event.port = peer->port;
                event.ports = peer->ports;
                event.reason = reason_of(info->m_info);
                // The peer's own words, for the log: printable characters only.
                for (const char* c = info->m_info.m_szEndDebug; *c != '\0'; c++) {
                    event.detail += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
                }
                event.spectator = peer->spectator;
                if (!is_host || in(*peer)) {
                    pending_events.push_back(std::move(event));
                }
                if (is_host) {
                    remember(*peer, reason_of(info->m_info));
                }
                peers.erase(peers.begin() + (peer - peers.data()));
                break;
            }
            default:
                break;
        }
    }

    std::vector<Event> Session::poll() {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        std::vector<Event> signal_events;
        take_signals(signal_events);
        sockets->RunCallbacks();
        std::vector<Event> events = std::move(pending_events);
        pending_events.clear();
        events.insert(events.end(), signal_events.begin(), signal_events.end());

        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingMessage_t* messages[32];
            int count;
            int taken = 0;
            // (The library's clock, to tell when each message came on the steady one.)
            const SteamNetworkingMicroseconds library_now = SteamNetworkingUtils()->GetLocalTimestamp();
            const double steady_now = now();
            while (taken < max_messages_per_poll && (count = sockets->ReceiveMessagesOnPollGroup(poll_group, messages, 32)) > 0) {
                taken += count;
                for (int i = 0; i < count; i++) {
                    SteamNetworkingMessage_t* message = messages[i];
                    taking_unreliable = (message->m_nFlags & k_nSteamNetworkingSend_Reliable) == 0;
                    taking_arrived = steady_now - (double)(library_now - message->m_usecTimeReceived) / 1e6;
                    take(message->m_conn, { (const uint8_t*)message->m_pData, (size_t)message->m_cbSize }, events);
                    taking_unreliable = false;
                    message->Release();
                }
            }
        }

        // The waiting room's roster, to everyone in: at once when someone comes, goes or gets ready, and every 2 s
        // for the round trips.
        const double t = now();
        // (And in the match, for the players' names as they leave, come back or join under way.)
        if (is_host && !closed) {
            Roster now_roster = roster();
            Roster without_trips = now_roster, sent_without = sent_roster;
            for (int port = 0; port < max_players; port++) {
                without_trips.seats[port].round_trip_ms = sent_without.seats[port].round_trip_ms = 0;
            }
            if (!(without_trips == sent_without) || t >= next_roster) {
                for (const Peer& peer : peers) {
                    if (in(peer)) send_to(peer.connection, now_roster);
                }
                sent_roster = now_roster;
                next_roster = t + 2.0;
            }
        }
        // A state sent: the library's own rate again once it has all gone out (inputs keep some unacknowledged).
        for (Peer& peer : peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (peer.sending_state && SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK &&
                status.m_cbPendingReliable < (int)max_packet_size * 8) {
                set_send_rate(peer.connection, false);
                peer.sending_state = false;
            }
        }
        // Joiners that never said Hello.
        for (size_t i = 0; i < peers.size();) {
            if (is_host && !in(peers[i]) && peers[i].up && t - peers[i].connected_at > handshake_seconds) {
                drop(peers[i], Reason::timeout, &events);
            }
            else {
                i++;
            }
        }
        return events;
    }

    void Session::take(uint32_t connection, std::span<const uint8_t> data, std::vector<Event>& events) {
        Peer* peer = find(connection);
        if (peer == nullptr) {
            return;
        }
        std::optional<Packet> packet = decode(data);
        if (packet) {
            // (Only Inputs' early copies come unreliably; one is taken once every message it says went before it is in.)
            if (taking_unreliable && (!std::holds_alternative<Inputs>(*packet) ||
                (int16_t)(uint16_t)(peer->controls_taken - std::get<Inputs>(*packet).controls) < 0)) {
                return;
            }
            if (!taking_unreliable && !std::holds_alternative<Inputs>(*packet) && !std::holds_alternative<Hash>(*packet)) {
                peer->controls_taken++;
            }
            handle(*peer, *packet, events);
            return;
        }
        // Another protocol version is told so; anything else is a broken packet.
        const bool other_version = data.size() >= 4 && data[0] == 'C' && data[1] == 'N' &&
            (uint16_t)(data[2] | (data[3] << 8)) != protocol_version;
        if (other_version && !is_host && !is_welcomed) {
            // A host of another version refusing this joiner (its Refuse is kept the same): why, and its release.
            if (std::optional<Refuse> refusal = decode_refusal(data)) {
                refusal->reason = Reason::version;
                handle(*peer, *refusal, events);
                return;
            }
        }
        if (other_version && is_host && !in(*peer)) {
            // A joiner of another version: refused as any other, so it reads which release this is.
            refuse(*peer, Reason::version);
            return;
        }
        drop(*peer, other_version ? Reason::version : Reason::protocol, &events);
    }

    void Session::drain(uint32_t connection, std::vector<Event>& events) {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        SteamNetworkingMessage_t* messages[32];
        int count;
        // At most what the library buffers from a peer (recv_buffer_messages), and only while it's a peer.
        for (int taken = 0; taken < recv_buffer_messages && find(connection) != nullptr &&
            (count = sockets->ReceiveMessagesOnConnection(connection, messages, 32)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                taking_unreliable = (messages[i]->m_nFlags & k_nSteamNetworkingSend_Reliable) == 0;
                taking_arrived = now() - (double)(SteamNetworkingUtils()->GetLocalTimestamp() - messages[i]->m_usecTimeReceived) / 1e6;
                take(connection, { (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize }, events);
                taking_unreliable = false;
                messages[i]->Release();
            }
        }
    }

    void Session::handle(Peer& peer, const Packet& packet, std::vector<Event>& events) {
        if (is_host) {
            if (!in(peer)) {
                const Hello* theirs = std::get_if<Hello>(&packet);
                if (theirs == nullptr) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                // The key first, and alone: a wrong one is closed at once, told nothing else (whether its
                // build or ROM would have done).
                if (!same_key(theirs->key, hello.key)) {
                    SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)Reason::key,
                        describe(Reason::key), false);
                    peers.erase(peers.begin() + (&peer - peers.data()));
                    return;
                }
                std::optional<Reason> refusal;
                if (theirs->build != hello.build) refusal = Reason::build;
                else if (theirs->rom != hello.rom) refusal = Reason::rom;
                else if (theirs->profile != hello.profile) refusal = Reason::profile;
                else if (theirs->mods != hello.mods) refusal = Reason::mods;
                // Kept out (the host's block list, or kicked from this match), or posing as the host itself: told only that
                // the host removed it.
                else if (std::find(blocked.begin(), blocked.end(), theirs->player) != blocked.end() ||
                    std::find(kicked.begin(), kicked.end(), theirs->player) != kicked.end() || theirs->player == hello.player) {
                    refusal = Reason::kicked;
                }
                // Not as someone still in (a player or spectator: their id is theirs). Seated twice, the Roster would give the
                // one who's there the other's ports too. (One whose old connection the host hasn't let go of yet can try
                // again once it has: told, as for a match it can't get in, that it's under way.)
                if (!refusal && std::any_of(peers.begin(), peers.end(), [&](const Peer& p) { return in(p) && p.player == theirs->player; })) {
                    refusal = Reason::started;
                }
                // A spectator (nobody playing): if the host takes them and has room, whenever the match is (not after it).
                // (A player who left may watch, their place held for them all the same.)
                if (!refusal && theirs->players == 0) {
                    const int number = free_spectator();
                    if (!takes_spectators() || closed || number < 0) {
                        refuse(peer, !takes_spectators() ? Reason::watching : closed ? Reason::started : Reason::full);
                        return;
                    }
                    peer.spectator = number;
                    peer.player = theirs->player;
                    peer.platform = theirs->platform;
                    peer.in_match = entry_open;
                    peer.late = !entry_open;
                    send_to(peer.connection, entry_open ? Welcome{ 0, (uint8_t)match_players }
                        : Welcome{ 0, (uint8_t)match_players, 2, match_own_view, match_pairs, match_rollback });
                    Event event{ Event::Type::joined };
                    event.spectator = number;
                    event.players = match_players;
                    event.rejoin = event.late = !entry_open;
                    events.push_back(std::move(event));
                    return;
                }
                // Under way: a player who left it, back to their ports (as many players), not too often; or a new one
                // (late), on free ports (below).
                uint8_t ports = 0;
                const bool under_way = !refusal && !entry_open;
                bool rejoin = false;
                if (under_way) {
                    for (int p = 0; p < max_players; p++) {
                        if (left_players[p] == theirs->player && rejoins[p] < max_rejoins) ports |= (uint8_t)(1u << p);
                    }
                    rejoin = ports != 0 && std::popcount(ports) == theirs->players;
                    // (A player who left with another number of players, or too often: not let in on other ports while
                    // theirs are held for them.)
                    const bool claims = std::any_of(left_players.begin(), left_players.end(),
                        [&](uint64_t p) { return p == theirs->player; });
                    if (closed || (claims && !rejoin)) refusal = Reason::started;
                    if (!rejoin) ports = 0;
                }
                // The lowest free ports, one for each of its players: under way, the open ones and those whose player
                // left over seat_hold_seconds ago without coming back (not too often either).
                const bool late = under_way && !refusal && !rejoin;
                if (!refusal && !rejoin) {
                    uint8_t taken = own_ports;
                    for (const Peer& other : peers) {
                        taken |= other.ports;
                    }
                    const uint8_t free = late ? free_ports() : (uint8_t)~taken;
                    for (int p = 0, wanted = theirs->players; p < match_players && wanted > 0; p++) {
                        if ((free & (1u << p)) != 0) {
                            ports |= (uint8_t)(1u << p);
                            wanted--;
                        }
                    }
                    if (std::popcount(ports) != theirs->players) refusal = late && ports == 0 ? Reason::started : Reason::full;
                }
                if (refusal) {
                    refuse(peer, *refusal);
                    return;
                }
                peer.port = std::countr_zero(ports);
                peer.ports = ports;
                peer.player = theirs->player;
                peer.platform = theirs->platform;
                Welcome welcome{ ports, (uint8_t)match_players };
                if (under_way) {
                    // Nothing of the match till lockstep brings it in (back, welcome_back). (A new player on a port
                    // someone left: they can't come back to it now.)
                    peer.in_match = false;
                    peer.late = late;
                    for (int p = 0; p < max_players; p++) {
                        if ((ports & (1u << p)) != 0) {
                            peer.replaced_players[p] = left_players[p];
                            peer.replaced_at[p] = left_at[p];
                            left_players[p] = 0;
                            rejoins[p]++;
                        }
                    }
                    welcome = { ports, (uint8_t)match_players, (uint8_t)(rejoin ? 1 : 2), match_own_view, match_pairs, match_rollback };
                }
                send_to(peer.connection, welcome);
                Event event{ Event::Type::joined };
                event.port = peer.port;
                event.ports = ports;
                event.players = match_players;
                event.rejoin = under_way;
                event.late = late;
                events.push_back(std::move(event));
                return;
            }
            // Ready, only before the start.
            if (const Ready* ready = std::get_if<Ready>(&packet)) {
                // One the joiner sent as the match started (Start on its way to it) is too late, not wrong.
                if (entry_open) {
                    peer.ready = ready->ready != 0;
                }
                return;
            }
            // Inputs and hashes only once the match has started (before, a player can still leave and
            // another take the port), and Inputs no further ahead than the game can have got (set_horizon):
            // anything else is a broken or hostile peer, refused before it's passed on.
            const Inputs* inputs = std::get_if<Inputs>(&packet);
            const Hash* hash = std::get_if<Hash>(&packet);
            // (A machine let go of: what it sent before it heard is late, not wrong; nor what it sends till it's back.)
            if (peer.let_go && (inputs != nullptr || hash != nullptr || std::holds_alternative<Unseat>(packet))) {
                return;
            }
            // An Inputs' reads that came already (in its early copy, or the copy after it) are cut off, and an early copy
            // that starts past the next read waits for the reliable Inputs to fill in (it isn't taken).
            // (A copy that isn't for now, as one sent before a seat change arriving after it, is late, not wrong.)
            if (taking_unreliable && (inputs == nullptr || entry_open || !peer.in_match || inputs->port >= max_players ||
                (peer.ports & (1u << inputs->port)) == 0)) {
                return;
            }
            if (inputs != nullptr && !entry_open && peer.in_match && inputs->port < max_players &&
                (peer.ports & (1u << inputs->port)) != 0) {
                const uint64_t next = next_inputs[inputs->port];
                if (taking_unreliable && inputs->first_read > next) {
                    return;
                }
                if (inputs->first_read < next) {
                    if ((uint64_t)inputs->first_read + inputs->count > next) {
                        const bool unreliable = taking_unreliable;
                        taking_unreliable = false;
                        handle(peer, inputs_from_read(*inputs, next), events);
                        taking_unreliable = unreliable;
                    }
                    return;
                }
            }
            // Each port's Inputs go on from the last read of its earlier ones, and a Hash's read is within the
            // horizon too: else the others would end the match on it (lockstep checks the same), where dropping
            // the peer lets them play on without it.
            if ((inputs != nullptr || hash != nullptr) &&
                (entry_open || !peer.in_match || (inputs != nullptr && ((uint64_t)inputs->first_read + inputs->count > horizon ||
                    inputs->first_read != next_inputs[inputs->port])) || (hash != nullptr && hash->read > horizon))) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            // One hash per read, in order: a repeat is a broken or hostile peer.
            if (hash != nullptr) {
                if ((int64_t)hash->read <= peer.last_hash) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                peer.last_hash = hash->read;
            }
            if (inputs != nullptr && (peer.ports & (1u << inputs->port)) != 0) {
                peer.played = true;
                next_inputs[inputs->port] = inputs->first_read + inputs->count;
                remember_sent(*inputs);
                // Passed on to the others, as they only connect to the host.
                for (const Peer& other : peers) {
                    if (other.connection != peer.connection) {
                        send_inputs(other, *inputs);
                    }
                }
            }
            else if (const Bye* bye = std::get_if<Bye>(&packet)) {
                Event event{ Event::Type::left };
                event.port = peer.port;
                event.ports = peer.ports;
                event.spectator = peer.spectator;
                event.reason = bye->reason;
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                remember(peer, bye->reason);
                peers.erase(peers.begin() + (&peer - peers.data()));
                events.push_back(std::move(event));
                return;
            }
            // Seat changes, in the match: a spectator would take a port; a player of one port (not coming in) has stepped out.
            // (Before the start too: the host makes the change at once.)
            // (One from a machine the host has moved already is late, not wrong: ignored.)
            else if (const Claim* claim = std::get_if<Claim>(&packet)) {
                if (claim->port >= match_players) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                // A machine let go of, here again: it comes back for the host's game (once).
                if (peer.let_go) {
                    if (claim->port != peer.port || peer.returning || closed) {
                        return;
                    }
                    peer.returning = true;
                    Event event{ Event::Type::joined };
                    event.port = peer.port;
                    event.ports = peer.ports;
                    event.players = match_players;
                    event.rejoin = true;
                    event.returned = true;
                    events.push_back(std::move(event));
                    return;
                }
                if (peer.spectator < 0 || !peer.in_match || closed) {
                    return;
                }
            }
            else if (const Unseat* unseat = std::get_if<Unseat>(&packet)) {
                // (Before the start, answered at once, whatever the host makes of it: the roster that follows says where
                // the machine is, and it knows to go by it, its step-out not crossing the Start unknown.)
                if (entry_open && !closed) {
                    send_to(peer.connection, *unseat);
                }
                if (peer.port < 0 || peer.ports != unseat->ports || !peer.in_match || closed) {
                    return;
                }
            }
            else if (!std::holds_alternative<Hash>(packet)) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = peer.port;
            event.spectator = peer.spectator;
            event.player = peer.player;
            event.packet = packet;
            event.arrived = taking_arrived;
            events.push_back(std::move(event));
            return;
        }

        // A joiner, from the host.
        if (!is_welcomed) {
            // (An early copy can overtake a Welcome lost and resent: it's for after it.)
            if (taking_unreliable) {
                return;
            }
            const Welcome* welcome = std::get_if<Welcome>(&packet);
            if (welcome != nullptr && std::popcount(welcome->ports) == hello.players) {
                own_ports = welcome->ports;
                is_welcomed = true;
                match_players = welcome->players;
                rejoining = welcome->rejoin != 0;
                Event event{ Event::Type::welcomed };
                event.port = own_ports == 0 ? -1 : std::countr_zero(own_ports);
                event.ports = own_ports;
                event.players = match_players;
                event.rejoin = rejoining;
                event.late = welcome->rejoin == 2;
                event.own_view = welcome->own_view != 0;
                event.pairs = welcome->pairs;
                event.rollback = welcome->rollback;
                events.push_back(std::move(event));
            }
            else if (std::holds_alternative<Refuse>(packet) || std::holds_alternative<Bye>(packet)) {
                // Refused, or dropped before being welcomed (the host's handshake time ran out): with why.
                Event event{ Event::Type::ended };
                event.reason = std::holds_alternative<Refuse>(packet) ? std::get<Refuse>(packet).reason : std::get<Bye>(packet).reason;
                if (const Refuse* refusal = std::get_if<Refuse>(&packet)) {
                    event.release = refusal->release;
                }
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                peers.clear();
                events.push_back(std::move(event));
            }
            else {
                drop(peer, Reason::protocol, &events);
            }
            return;
        }
        if (const Roster* roster = std::get_if<Roster>(&packet)) {
            // The waiting room's, with this machine's ports in it. Before the start the host may move it (to a place, or
            // out of its places to watch): the roster says where it is.
            uint8_t seats = 0;
            for (int port = 0; port < max_players; port++) {
                if (roster->seats[port].player == hello.player) seats |= (uint8_t)(1u << port);
            }
            const bool watcher = std::any_of(roster->watchers.begin(), roster->watchers.end(),
                [&](const Watcher& w) { return w.player == hello.player; });
            const bool moved = !started && !rejoining && seats != own_ports && (seats != 0) != watcher && (seats & 1) == 0;
            if (roster->players != match_players || ((seats & own_ports) != own_ports && !moved)) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            if (moved) {
                own_ports = seats;
                stepping_out = false;
                Event event{ Event::Type::moved };
                event.ports = own_ports;
                events.push_back(std::move(event));
            }
            host_roster = *roster;
            return;
        }
        // Coming back to a match under way: the match as it is at the read this machine plays from (lockstep checks it).
        if (std::holds_alternative<Rejoin>(packet)) {
            if (!rejoining) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            rejoining = false;
            started = true;
            // The inputs it has since follow at once (before lockstep sets the horizon from its read): as far as
            // any can reach from there.
            horizon = (std::max)(horizon, (uint64_t)std::get<Rejoin>(packet).at + 2 * max_delay + input_window + 16);
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
            return;
        }
        if (const Start* start = std::get_if<Start>(&packet)) {
            // (Its step-out crossed the Start: it watches, its places open or the host's to fill, and sends nothing.)
            if (stepping_out && !started) {
                stepping_out = false;
                Event event{ Event::Type::moved };
                event.late = true;      // (its places go idle in the match: Gone)
                event.port = own_ports;
                own_ports = 0;
                events.push_back(std::move(event));
            }
            if (started || rejoining || start->players != match_players || (start->open & own_ports) != 0) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            started = true;
            Event event{ Event::Type::started };
            event.own_view = start->own_view != 0;
            event.delays = start->delays;
            event.pairs = start->pairs;
            event.rollback = start->rollback;
            event.open = start->open;
            events.push_back(std::move(event));
        }
        else if (const Inputs* inputs = std::get_if<Inputs>(&packet); inputs != nullptr) {
            if (!started || (own_ports & (1u << inputs->port)) != 0 || inputs->port >= match_players ||
                (uint64_t)inputs->first_read + inputs->count > horizon) {
                // (An early copy sent before a change it arrives after, as a seat change, is late, not wrong.)
                if (!taking_unreliable) drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = inputs->port;
            event.packet = packet;
            event.copy = taking_unreliable;
            event.arrived = taking_arrived;
            events.push_back(std::move(event));
        }
        else if (const Hash* hash = std::get_if<Hash>(&packet); hash != nullptr) {
            // Once started, one per read, in order.
            if (!started || (int64_t)hash->read <= peer.last_hash) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            peer.last_hash = hash->read;
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Gone* gone = std::get_if<Gone>(&packet); gone != nullptr) {
            // Once started, about another machine's player; or this machine let go of, its inputs having stopped coming
            // for a while (one for each of its ports: it comes back with the host's game, Rejoin).
            const bool mine = (own_ports & (1u << gone->port)) != 0;
            if (!started || gone->port >= match_players || (mine && gone->silent != 3)) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            if (mine) rejoining = true;
            Event event{ Event::Type::packet };
            event.port = gone->port;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Unseat* unseat = std::get_if<Unseat>(&packet)) {
            // Before the start: the host's answer to its step-out (its roster says where it is). In the match: asked to
            // step out. (One for ports it no longer has is late, not wrong: it has stepped out already, or been seated
            // again since.)
            if (!started) {
                stepping_out = false;
                return;
            }
            if (rejoining) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            if (own_ports != unseat->ports) {
                return;
            }
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (std::holds_alternative<Delay>(packet) || std::holds_alternative<Resync>(packet) ||
            std::holds_alternative<StatePart>(packet) || std::holds_alternative<Back>(packet)) {
            // Once started (lockstep checks the reads).
            if (!started) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Bye* bye = std::get_if<Bye>(&packet)) {
            Event event{ Event::Type::ended };
            event.reason = bye->reason;
            SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
            peers.clear();
            events.push_back(std::move(event));
        }
        else {
            drop(peer, Reason::protocol, &events);
        }
    }

    void Session::flush() {
        // Long enough for a few resends at a bad network's round trip; a peer that's gone isn't waited for.
        constexpr double flush_seconds = 2.0;
        const double until = now() + flush_seconds;
        while (now() < until) {
            bool any = false;
            for (const Peer& peer : peers) {
                any = any || pending(peer.connection);
            }
            if (!any) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void Session::set_release(const std::string& name) {
        const bool valid = name.size() <= max_release_name && printable_word(name);
        release = valid ? name : std::string();
    }

    void Session::refuse(Peer& peer, Reason reason) {
        send_to(peer.connection, Refuse{ reason, release });
        SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
            describe(reason), true);
        peers.erase(peers.begin() + (&peer - peers.data()));
    }

    void Session::remember(const Peer& peer, Reason reason) {
        if (!match_started || closed || peer.port < 0 || reason == Reason::kicked) {
            return;
        }
        // A new player gone before it played (its first input: it may have had the match and the state, but never
        // loaded it): its ports as they were (open, or held for who left them), and the try not counted against them.
        if (peer.late && !peer.played) {
            for (int p = 0; p < max_players; p++) {
                if ((peer.ports & (1u << p)) != 0) {
                    left_players[p] = peer.replaced_players[p];
                    left_at[p] = peer.replaced_at[p];
                    rejoins[p]--;
                }
            }
            return;
        }
        for (int p = 0; p < max_players; p++) {
            if ((peer.ports & (1u << p)) != 0) {
                left_players[p] = peer.player;
                left_at[p] = now();
            }
        }
    }

    void Session::drop(Peer& peer, Reason reason, std::vector<Event>* events) {
        if (is_host) {
            remember(peer, reason);
        }
        send_to(peer.connection, Bye{ reason });
        SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
            describe(reason), true);
        if (events != nullptr && (!is_host || in(peer))) {
            Event event{ is_host ? Event::Type::left : Event::Type::ended };
            event.port = peer.port;
            event.ports = peer.ports;
            event.spectator = peer.spectator;
            event.reason = reason;
            events->push_back(std::move(event));
        }
        peers.erase(peers.begin() + (&peer - peers.data()));
    }

    bool Session::pending(uint32_t connection) const {
        SteamNetConnectionRealTimeStatus_t status;
        return SteamNetworkingSockets()->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) == k_EResultOK &&
            status.m_eState == k_ESteamNetworkingConnectionState_Connected &&
            status.m_cbPendingReliable + status.m_cbSentUnackedReliable > 0;
    }

    void Session::set_send_rate(uint32_t connection, bool state) {
        ISteamNetworkingUtils* utils = SteamNetworkingUtils();
        const int rate = state ? state_send_rate : default_send_rate;
        utils->SetConnectionConfigValueInt32(connection, k_ESteamNetworkingConfig_SendRateMin, rate);
        utils->SetConnectionConfigValueInt32(connection, k_ESteamNetworkingConfig_SendRateMax, rate);
        utils->SetConnectionConfigValueInt32(connection, k_ESteamNetworkingConfig_SendBufferSize,
            state ? state_send_buffer : default_send_buffer);
    }

    // Everything is sent reliably (resent until it arrives, in order), and at once: lockstep needs every
    // input, and waits for it anyway. But Inputs' early copies (`reliable` false): those lost aren't resent.
    void Session::send_to(uint32_t connection, const Packet& packet, bool reliable) {
        uint8_t buffer[max_packet_size];
        std::vector<uint8_t> large;
        std::span<uint8_t> out = buffer;
        if (std::holds_alternative<StatePart>(packet)) {
            large.resize(max_message_size);
            out = large;
        }
        const size_t size = encode(packet, out);
        if (size == 0) {
            std::fprintf(stderr, "[netplay] not sending an invalid packet\n");
            return;
        }
        const EResult result = SteamNetworkingSockets()->SendMessageToConnection(connection, out.data(), (uint32)size,
            reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
        if (result != k_EResultOK && result != k_EResultNoConnection && result != k_EResultInvalidParam) {
            std::fprintf(stderr, "[netplay] a message couldn't be sent (%d)\n", (int)result);
        }
        if (result == k_EResultOK && reliable && !std::holds_alternative<Inputs>(packet) && !std::holds_alternative<Hash>(packet)) {
            if (Peer* peer = find(connection)) peer->controls_sent++;
        }
    }

    void Session::send_inputs(const Peer& peer, const Inputs& inputs) {
        if (!in(peer) || !peer.in_match) {
            return;
        }
        const uint32_t from = peer.inputs_from[inputs.port];
        const uint64_t end = (uint64_t)inputs.first_read + inputs.count;
        // Only the reads from `from` on, if any.
        if (end <= from) {
            return;
        }
        send_to(peer.connection, inputs.first_read >= from ? inputs : inputs_from_read(inputs, from));
        // The early copy: the port's recent reads up to these (if they're the latest remembered).
        const std::deque<std::pair<Pad, Motion>>& r = recent[inputs.port];
        const uint64_t first = std::max<uint64_t>(recent_first[inputs.port], from);
        if (r.empty() || recent_first[inputs.port] + r.size() != end || first >= end) {
            return;
        }
        // Only on a link that keeps up: a copy is for a packet lost, not for one queued (as a peer far behind, or a state
        // going out), and must never take the room of the reliable messages in the send buffer.
        SteamNetConnectionRealTimeStatus_t status;
        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) != k_EResultOK ||
            status.m_cbPendingReliable + status.m_cbPendingUnreliable > copy_most_pending) {
            return;
        }
        Inputs copy;
        copy.port = inputs.port;
        copy.first_read = (uint32_t)first;
        copy.count = (uint8_t)(end - first);
        for (int i = 0; i < copy.count; i++) {
            const auto& [pad, motion] = r[(size_t)(first - recent_first[inputs.port]) + i];
            copy.pads[i] = pad;
            copy.motions[i] = motion;
        }
        copy.gaps = inputs.gaps;
        copy.controls = peer.controls_sent;
        send_to(peer.connection, copy, false);
    }

    void Session::remember_sent(const Inputs& inputs) {
        if (inputs.port >= max_players) return;
        std::deque<std::pair<Pad, Motion>>& r = recent[inputs.port];
        if (r.empty() || recent_first[inputs.port] + r.size() != inputs.first_read) {
            r.clear();
            recent_first[inputs.port] = inputs.first_read;
        }
        for (int i = 0; i < inputs.count; i++) {
            r.emplace_back(inputs.pads[i], inputs.motions[i]);
        }
        while (r.size() > (size_t)input_window) {
            r.pop_front();
            recent_first[inputs.port]++;
        }
    }

    void Session::send(const Inputs& inputs) {
        remember_sent(inputs);
        for (const Peer& peer : peers) {
            send_inputs(peer, inputs);
        }
    }

    void Session::send(const Hash& hash) {
        for (const Peer& peer : peers) {
            if (in(peer) && peer.in_match) {
                send_to(peer.connection, hash);
            }
        }
    }

    void Session::send(const Gone& gone) {
        for (const Peer& peer : peers) {
            if (in(peer) && peer.in_match) {
                send_to(peer.connection, gone);
            }
        }
    }

    void Session::send(const Resync& resync) {
        for (const Peer& peer : peers) {
            if (in(peer) && peer.in_match) {
                send_to(peer.connection, resync);
            }
        }
    }

    void Session::back(uint8_t ports, uint32_t read, uint8_t late, uint64_t player) {
        for (int p = 0; p < max_players; p++) {
            if ((ports & (1u << p)) != 0) next_inputs[p] = read;
        }
        for (const Peer& peer : peers) {
            if (in(peer) && peer.in_match) {
                send_to(peer.connection, Back{ ports, read, late, player });
            }
        }
    }

    bool Session::seat_spectator(int spectator, int port) {
        if (!is_host || closed || port < 1 || port >= match_players || ((free_ports() & (1u << port)) == 0 && left_players[port] == 0)) {
            return false;
        }
        for (const Peer& other : peers) {
            if ((other.ports & (1u << port)) != 0) return false;
        }
        for (Peer& peer : peers) {
            if (peer.spectator == spectator && peer.in_match) {
                peer.spectator = -1;
                peer.port = port;
                peer.ports = (uint8_t)(1u << port);
                peer.ready = false;
                // (Its place is its own now: whoever left it can't come back to it; nor can it to any it left.)
                left_players[port] = 0;
                for (uint64_t& held : left_players) {
                    if (held == peer.player) held = 0;
                }
                peer.late = false;
                peer.played = true;
                return true;
            }
        }
        return false;
    }

    int Session::unseat(uint8_t ports) {
        const int number = takes_spectators() ? free_spectator() : -1;
        for (Peer& peer : peers) {
            if (number >= 0 && peer.port >= 0 && peer.ports == ports && peer.in_match) {
                peer.ports = 0;
                peer.port = -1;
                peer.spectator = number;
                peer.ready = false;
                return number;
            }
        }
        return -1;
    }

    uint8_t Session::machine_of(int port) const {
        for (const Peer& peer : peers) {
            if (port >= 1 && peer.port >= 0 && (peer.ports & (1u << port)) != 0) return peer.ports;
        }
        return 0;
    }

    bool Session::ask_unseat(int port) {
        for (const Peer& peer : peers) {
            if (is_host && port >= 1 && peer.port >= 0 && (peer.ports & (1u << port)) != 0 && peer.in_match && takes_spectators() &&
                free_spectator() >= 0) {
                send_to(peer.connection, Unseat{ peer.ports });
                return true;
            }
        }
        return false;
    }

    void Session::claim(int port) {
        if (!is_host && watching() && !rejoining && !peers.empty()) {
            send_to(peers[0].connection, Claim{ (uint8_t)port });
        }
    }

    void Session::step_out() {
        if (!is_host && own_ports != 0 && is_welcomed && !rejoining && !peers.empty()) {
            send_to(peers[0].connection, Unseat{ own_ports });
            // (Before the start the host moves it, and its roster says so.)
            if (started) own_ports = 0;
            else stepping_out = true;
        }
    }

    void Session::let_go_of(uint8_t ports) {
        for (Peer& peer : peers) {
            if (is_host && peer.port >= 0 && peer.ports == ports && peer.in_match) {
                peer.in_match = false;
                peer.let_go = true;
                peer.returning = false;
            }
        }
    }

    void Session::come_back() {
        if (!is_host && rejoining && own_ports != 0 && !peers.empty()) {
            send_to(peers[0].connection, Claim{ (uint8_t)std::countr_zero(own_ports) });
        }
    }

    void Session::take_seat(uint8_t ports) {
        if (!is_host && watching()) {
            own_ports = ports;
        }
    }

    void Session::welcome_back(uint8_t ports, uint8_t spectators, const Rejoin& rejoin, const std::vector<Inputs>& since,
        const std::array<uint32_t, max_players>& inputs_from) {
        for (Peer& peer : peers) {
            // (Each machine of those coming back, and each spectator coming in.)
            const bool watching = peer.spectator >= 0 && (spectators & (1u << peer.spectator)) != 0;
            if (!in(peer) || peer.in_match || ((peer.ports & ports) == 0 && !watching)) {
                continue;
            }
            send_to(peer.connection, rejoin);
            for (const Inputs& inputs : since) {
                send_to(peer.connection, inputs);
            }
            peer.in_match = true;
            peer.inputs_from = inputs_from;
            peer.let_go = false;
            peer.returning = false;
        }
    }

    void Session::send_state(uint8_t ports, uint8_t spectators, uint32_t read, const std::vector<uint8_t>& state, uint64_t hash) {
        for (Peer& peer : peers) {
            const bool watching = peer.spectator >= 0 && (spectators & (1u << peer.spectator)) != 0;
            if (!in(peer) || !peer.in_match || ((peer.ports & ports) == 0 && !watching)) {
                continue;
            }
            set_send_rate(peer.connection, true);
            peer.sending_state = true;
            for (size_t offset = 0; offset < state.size(); offset += max_state_part) {
                const size_t count = (std::min)(max_state_part, state.size() - offset);
                send_to(peer.connection, StatePart{ read, (uint32_t)state.size(), (uint32_t)offset, hash,
                    std::vector<uint8_t>(state.begin() + offset, state.begin() + offset + count) });
            }
        }
    }

    void Session::resync_spectators(uint8_t spectators, uint32_t read) {
        for (const Peer& peer : peers) {
            if (peer.spectator >= 0 && peer.in_match && (spectators & (1u << peer.spectator)) != 0) {
                send_to(peer.connection, Resync{ read, 0 });
            }
        }
    }

    void Session::send(const Delay& delay) {
        for (const Peer& peer : peers) {
            if (in(peer) && peer.in_match) {
                send_to(peer.connection, delay);
            }
        }
    }

    void Session::leave(Reason reason) {
        // The goodbyes first, then a moment for everything sent to arrive, as on a lossy network some of it is
        // still being resent: the program quitting would drop what isn't acknowledged yet (a quitting player's
        // last inputs and goodbye among it), and the others would wait for the connection to time out.
        for (const Peer& peer : peers) {
            send_to(peer.connection, Bye{ reason });
        }
        flush();
        while (!peers.empty()) {
            SteamNetworkingSockets()->CloseConnection(peers.back().connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
                describe(reason), true);
            peers.pop_back();
        }
        // The listen socket stays open (nobody more is let in, nor back) until the session goes: closing it closes
        // every connection accepted on it at once, goodbyes not yet sent and all.
        entry_open = false;
        closed = true;
    }
}
