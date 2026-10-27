// netplay_probe: tries the netplay transport (session.hpp) without the game. Peers connect as a match
// would and each sends made-up pads for its own ports, 30 reads a second as the game reads them; each
// checks that every other port's pads arrive, and are the ones sent.
//
//   netplay_probe host <port> [--players 2] [options]     (prints its join code, for 127.0.0.1)
//   netplay_probe join <join code> [options]
//   netplay_probe join <address:port> --silent | --oversize | --future
//   netplay_probe code <address:port> <key, hex>
//   netplay_probe host room --signal <server:port> [--direct 1] [options]   (prints its room code)
//   netplay_probe join <room code> --signal <server:port> [--direct 1] [options]
//   netplay_probe list --signal <server:port> [--build/--rom/--profile/--mods <hex>] [--platform <n>]
//   netplay_probe lan [--lan-port 27016] [--build/--rom/--profile/--mods <hex>] [--platform <n>]
//   netplay_probe desync --signal <server:port> [--count 1]
//   netplay_probe test [--signal <server:port>] [--lan-port 27016]
//     --reads <n>        how many reads each peer sends (300: 10 s)
//     --local <n>        players on this peer, each with a port (1 by default)
//     --loss <percent>   once its match starts, the library drops this share of the packets this one sends
//                        (they're resent); not while connecting, where the library's 10 s to connect would
//                        now and then run out
//     --build/--rom/--profile/--mods <hex>   the handshake's hashes (all 1 by default), to test refusals
//     --silent           connect but never say Hello: the host must drop it after its handshake time
//     --oversize         connect and send a message bigger than any packet: the library must close it
//     --future           connect and say Hello as a newer protocol version would: the host must refuse it, with
//                        its release, in a Refuse this one can read (decode_refusal)
//     --own-view 1       (hosting) the match's rule that every player sees only their own view
//     --hostile early    (joining) send Inputs before the match starts: the host must drop it
//     --hostile far      (joining) send Inputs far beyond the reads it could have got to: dropped too
//     --hostile port     (joining) send Inputs for the host's port 0 once the match starts: dropped too
//     --hostile hash     (joining) send the same Hash twice once the match starts: dropped too
//     --signal           connect through ICE, by a room on the server netplay_server at this address (which
//                        offers its TURN relay, if it has one)
//     --direct 1         allow direct connections too, not only through the TURN relay
//     --turn-password <p>  (a test) use the server's relay with this password instead of the one it gave
//     --again 1          (a test, hosting a room) make a room and leave first, then host with a new id, as
//                        the game does when a player tries again
//     --public 1         (hosting a room) list it in public, as the players join and the match starts
//     --platform <n>     (hosting a public room, or listing) say this platform (1 Linux, 2 Windows, 3 macOS)
//                        instead of this one's
//     --speak <version>  (a test, with --signal) speak this older signalling version, as an older game does
//     --lan 1            (hosting on a port) broadcast a beacon on this network once a second (lan.hpp)
//     --lan-port <port>  the port beacons go to (lan::default_port)
//     --lan-loopback 0   (a test, with --lan 1) broadcast only: not also to this machine
//     --count <n>        (desync) how many summaries to send
//     --player <hex>     this machine's player id (local_id() by default)
//     --block <hex>      (hosting) keep this player id out (refused: kicked)
//     --release <name>   this game's release name ("probe" by default): a public room's, and what a host tells
//                        a joiner it refuses
//     --kick 1           (hosting) kick each joiner as it joins
//     --ready-after <s>  (joining) say Ready that many seconds after being welcomed (at once by default):
//                        the host starts once every port is taken and everyone's ready
//
// It exits 0 once it has every other port's pads for every read and they're all right, 1 otherwise;
// --silent, once the host has closed the connection for a timeout; --oversize, once it's closed before
// that; --future, once it's refused for another version ("refused: <reason>, host's release <name>"). `code` prints the join code for an address and key. `list` prints the public rooms a game with
// those hashes could join ("listed <code> <joined>/<players>", then "playing" and "direct" if so,
// "rollback <n>", "host <id>" if the server says, "release <name>" if it does, and "other-build" for a room of
// another build), then "rooms <count>, others <count>, older <count>" (those of the same build and ROM with other
// settings or mods; those on older signalling versions), and exits 0 (1 if it couldn't get them). `lan` listens for beacons for 3 s and prints
// the matches on this network a game with those hashes could join ("lan <address:port> <join code>
// <joined>/<players>", then "playing" if so, and "rollback <n>"), then "matches <count>". `desync` sends the
// server that many made-up desync summaries (as a player whose match desynced would) and prints "sent <n>".
// `test` runs the menu's connection test (connection_test.hpp) and prints its lines, then "verdict server <v>
// relay <v> direct <v> lan <v>" (each good, bad or unknown), and exits 0.

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "join_code.hpp"
#include "connection_test.hpp"
#include "lan.hpp"
#include "session.hpp"
#include "signalling.hpp"

using namespace conker::netplay;

namespace {
    // The pad `port` sends for `read`: anything valid, and known to every peer.
    Pad made_up_pad(int port, uint32_t read) {
        const uint32_t x = (read + 1) * 2654435761u ^ (uint32_t)(port + 1) * 0x9E3779B9u;
        return Pad{ (uint16_t)(x & ~reserved_buttons), (int8_t)(x >> 16), (int8_t)(x >> 24), 0 };
    }

    // The pad of `port` for `read`, to send.
    Inputs inputs_for(int port, uint32_t read) {
        Inputs inputs;
        inputs.port = (uint8_t)port;
        inputs.first_read = read;
        inputs.count = 1;
        inputs.pads[0] = made_up_pad(port, read);
        return inputs;
    }

    struct Received {
        uint32_t next = 0;      // the first read not had yet (they arrive in order)
        uint32_t wrong = 0;     // pads that weren't the ones sent
        uint32_t packets = 0;
    };

    int usage() {
        std::fprintf(stderr, "usage: netplay_probe host <port> [--players 2] | join <code> | join <address:port> --silent|--oversize|--future\n"
                             "       netplay_probe host room | join <room code>, with --signal <server:port> [--direct 1]\n"
                             "       [--turn-password <p>] (a test: not the server's)\n"
                             "       [--reads 300] [--loss <percent>] [--build <hex>] [--rom <hex>] [--profile <hex>] [--mods <hex>]\n"
                             "       [--local 0: joining, a spectator] [--spectators 0: hosting, none taken]\n"
                             "       netplay_probe code <address:port> <key, hex>\n"
                             "       netplay_probe list --signal <server:port> [--build/--rom/--profile/--mods <hex>] [--platform <n>]\n"
                             "       netplay_probe lan [--lan-port 27016] [--build/--rom/--profile/--mods <hex>] [--platform <n>]\n"
                             "       netplay_probe desync --signal <server:port> [--count 1]\n");
        return 2;
    }

    // --silent: connects to `where` without a Session, so no Hello is sent, and waits for the host to
    // close the connection. Passes if it does so for a timeout, after 2 to 10 seconds. --oversize: sends a
    // message bigger than any packet once connected, and passes if the connection is closed for it (not
    // for a timeout) within 2 seconds. --future: says Hello as protocol_version + 1, and passes if a Refuse
    // for another version comes back.
    int join_silently(const std::string& where, bool oversize, bool future) {
        SteamNetworkingIPAddr address;
        if (!address.ParseString(where.c_str())) {
            std::fprintf(stderr, "--silent and --oversize take an address and port\n");
            return 2;
        }
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        const HSteamNetConnection connection = sockets->ConnectByIPAddress(address, 0, nullptr);
        const auto start = std::chrono::steady_clock::now();
        bool connected = false;
        double connected_at = 0;
        while (true) {
            sockets->RunCallbacks();
            SteamNetConnectionInfo_t info;
            if (!sockets->GetConnectionInfo(connection, &info)) {
                std::printf("FAILED: the connection is gone\n");
                return 1;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (info.m_eState == k_ESteamNetworkingConnectionState_Connected && !connected) {
                connected = true;
                connected_at = seconds;
                if (future) {
                    uint8_t buffer[max_packet_size];
                    const size_t size = encode(Hello{ 1, 1, 1, 0, 1, 1, 1, 1 }, buffer);
                    buffer[2] = (uint8_t)(protocol_version + 1);
                    buffer[3] = (uint8_t)((protocol_version + 1) >> 8);
                    sockets->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_Reliable, nullptr);
                    std::printf("connected, said Hello as version %d\n", protocol_version + 1);
                }
                else if (oversize) {
                    const std::vector<uint8_t> big(1024, 0x43);
                    sockets->SendMessageToConnection(connection, big.data(), (uint32)big.size(), k_nSteamNetworkingSend_Reliable, nullptr);
                    std::printf("connected, sent %zu bytes\n", big.size());
                }
                else {
                    std::printf("connected, saying nothing\n");
                }
            }
            SteamNetworkingMessage_t* message = nullptr;
            if (future && sockets->ReceiveMessagesOnConnection(connection, &message, 1) == 1) {
                const std::optional<Refuse> refusal = decode_refusal({ (const uint8_t*)message->m_pData, (size_t)message->m_cbSize });
                message->Release();
                sockets->CloseConnection(connection, 0, nullptr, false);
                if (refusal && refusal->reason == Reason::version) {
                    std::printf("refused: %s, host's release %s\n", describe(refusal->reason), refusal->release.c_str());
                    return 0;
                }
                std::printf("FAILED: not a refusal for another version\n");
                return 1;
            }
            if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                const bool timeout = info.m_eEndReason == k_ESteamNetConnectionEnd_App_Min + (int)Reason::timeout;
                std::printf("closed after %.1f s: %s (%d)\n", seconds, info.m_szEndDebug, info.m_eEndReason);
                sockets->CloseConnection(connection, 0, nullptr, false);
                const bool right = connected && (oversize ? !timeout && seconds - connected_at < 2 : timeout && seconds > 2 && seconds < 10);
                std::printf("%s\n", !right ? "FAILED" : oversize ? "closed for the oversized message" : "dropped for saying nothing");
                return right ? 0 : 1;
            }
            if (seconds > 30) {
                std::printf("FAILED: still connected after 30 s\n");
                sockets->CloseConnection(connection, 0, nullptr, false);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "code") == 0) {
        const std::string code = join_code_for(argv[2], std::strtoull(argv[3], nullptr, 16));
        if (code.empty()) {
            return usage();
        }
        std::printf("%s\n", code.c_str());
        return 0;
    }
    const bool lan_listing = argc >= 2 && std::strcmp(argv[1], "lan") == 0;
    const bool reporting = argc >= 2 && std::strcmp(argv[1], "desync") == 0;
    const bool testing = argc >= 2 && std::strcmp(argv[1], "test") == 0;
    const bool listing = lan_listing || reporting || testing || (argc >= 2 && std::strcmp(argv[1], "list") == 0);
    if (!listing && (argc < 3 || (std::strcmp(argv[1], "host") != 0 && std::strcmp(argv[1], "join") != 0))) {
        return usage();
    }
    const bool hosting = std::strcmp(argv[1], "host") == 0;
    const std::string where = listing ? std::string() : argv[2];
    int players = 2;
    uint32_t reads = 300;
    float loss = 0;
    Hello me{ 1, 1, 1, 1 };
    std::string hostile;
    bool own_view = false;
    int local = 1;
    std::string signal_server;
    bool relay_only = true;
    std::string turn_password;
    bool again = false;
    bool listed = false;
    std::string release = "probe";
    bool lan_visible = false;
    bool lan_loopback = true;
    int count = 1;
    std::vector<uint64_t> blocked;
    bool kick = false;
    bool spectators = true;
    double ready_after = 0;
    uint16_t lan_port = lan::default_port;
    signal::Platform platform = this_platform();
    uint16_t speaks = signal::version;
    for (int i = listing ? 2 : 3; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const char* value = argv[i + 1];
        if (option == "--players") players = std::atoi(value);
        else if (option == "--local") local = me.players = (uint8_t)std::atoi(value);
        else if (option == "--reads") reads = (uint32_t)std::strtoul(value, nullptr, 10);
        else if (option == "--loss") loss = std::strtof(value, nullptr);
        else if (option == "--build") me.build = std::strtoull(value, nullptr, 16);
        else if (option == "--rom") me.rom = std::strtoull(value, nullptr, 16);
        else if (option == "--profile") me.profile = std::strtoull(value, nullptr, 16);
        else if (option == "--mods") me.mods = std::strtoull(value, nullptr, 16);
        else if (option == "--hostile") hostile = value;
        else if (option == "--own-view") own_view = std::atoi(value) != 0;
        else if (option == "--signal") signal_server = value;
        else if (option == "--direct") relay_only = std::atoi(value) == 0;
        else if (option == "--turn-password") turn_password = value;
        else if (option == "--again") again = std::atoi(value) != 0;
        else if (option == "--public") listed = std::atoi(value) != 0;
        else if (option == "--platform") platform = (signal::Platform)std::atoi(value);
        else if (option == "--speak") speaks = (uint16_t)std::atoi(value);
        else if (option == "--lan") lan_visible = std::atoi(value) != 0;
        else if (option == "--lan-port") lan_port = (uint16_t)std::atoi(value);
        else if (option == "--lan-loopback") lan_loopback = std::atoi(value) != 0;
        else if (option == "--count") count = std::atoi(value);
        else if (option == "--player") me.player = std::strtoull(value, nullptr, 16);
        else if (option == "--block") blocked.push_back(std::strtoull(value, nullptr, 16));
        else if (option == "--release") release = value;
        else if (option == "--kick") kick = std::atoi(value) != 0;
        else if (option == "--spectators") spectators = std::atoi(value) != 0;
        else if (option == "--ready-after") ready_after = std::strtod(value, nullptr);
        else return usage();
    }
    if ((uint8_t)platform < 1 || platform > signal::Platform::last || (listing && !lan_listing && !testing && signal_server.empty()) || lan_port == 0 ||
        (lan_listing && !signal_server.empty()) || (lan_visible && !signal_server.empty()) || count < 1 ||
        speaks < signal::oldest_version || speaks > signal::version) {
        return usage();
    }
    const bool oversize = !hosting && argc == 4 && std::strcmp(argv[3], "--oversize") == 0;
    const bool future = !hosting && argc == 4 && std::strcmp(argv[3], "--future") == 0;
    const bool silent = !hosting && argc == 4 && (std::strcmp(argv[3], "--silent") == 0 || oversize || future);
    if (reads == 0) {
        return usage();
    }

    std::string error;
    if (!startup(error)) {
        std::fprintf(stderr, "couldn't start the networking: %s\n", error.c_str());
        return 1;
    }
    if (me.player == 0) {
        me.player = local_id();
    }
    me.platform = (uint8_t)platform;
    if (silent) {
        const int silent_result = join_silently(where, oversize, future);
        shutdown();
        return silent_result;
    }
    if (testing) {
        ConnectionTest test;
        test.start(signal_server, lan_port);
        while (!test.poll()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const ConnectionReport& report = test.report();
        static const char* const verdicts[] = { "unknown", "good", "bad" };
        for (const std::string* line : { &report.server_text, &report.relay_text, &report.direct_text, &report.lan_text, &report.summary }) {
            if (!line->empty()) std::printf("%s\n", line->c_str());
        }
        std::printf("verdict server %s relay %s direct %s lan %s\n", verdicts[(int)report.server], verdicts[(int)report.relay],
            verdicts[(int)report.direct], verdicts[(int)report.lan]);
        shutdown();
        return 0;
    }
    if (lan_listing) {
        LanListener listener;
        listener.listen_for(fit_of(me), platform);
        if (!listener.open(lan_port, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            shutdown();
            return 1;
        }
        const auto start = std::chrono::steady_clock::now();
        double seconds = 0;
        while (seconds < 3) {
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            listener.poll(seconds);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const std::vector<LanMatch> matches = listener.matches();
        for (const LanMatch& match : matches) {
            std::printf("lan %s %s %d/%d%s rollback %d%s", match.address.c_str(), match.code.c_str(), match.beacon.joined,
                match.beacon.players, match.beacon.playing ? " playing" : "", match.beacon.rollback, match.beacon.spectators ? " spectators" : "");
            if (match.beacon.watching != 0) std::printf(" watching %d", match.beacon.watching);
            std::printf("\n");
        }
        std::printf("matches %zu\n", matches.size());
        shutdown();
        return 0;
    }
    if (reporting) {
        int report_result = 1;
        {
            Signaller signaller;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (signaller.state() != Signaller::State::ready && signaller.state() != Signaller::State::failed &&
                std::chrono::steady_clock::now() < until) {
                if (signaller.state() == Signaller::State::idle && !signaller.connect(signal_server, error)) break;
                signaller.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int sent = 0;
            signal::DesyncReport report{ 0x5EED1234u, 0x0123456789ABCDEFull, platform, 2, 1, 0, 300, 0, 2, {} };
            report.regions[8] = 1;   // region 64
            for (int i = 0; i < count && signaller.state() == Signaller::State::ready; i++) {
                sent += signaller.report_desync(report) ? 1 : 0;
                report.read += 30;
            }
            // A moment for them to go out, before goodbye.
            for (int i = 0; i < 100; i++) {
                signaller.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::printf("sent %d\n", sent);
            report_result = sent == count ? 0 : 1;
        }
        shutdown();
        return report_result;
    }
    if (listing) {
        int list_result = 1;
        {
            Signaller signaller;
            signaller.speak(speaks);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            const auto wait = [&](auto done) {
                while (!done() && signaller.state() != Signaller::State::failed && std::chrono::steady_clock::now() < until) {
                    signaller.poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return done();
            };
            if (signaller.connect(signal_server, error) && wait([&] { return signaller.state() == Signaller::State::ready; })) {
                if (!signaller.has_public_rooms()) {
                    error = "the server has no public rooms (an older one)";
                }
                signaller.list_rooms(me, platform);
                if (signaller.has_public_rooms() && wait([&] { return signaller.listing() == Signaller::Lookup::found; })) {
                    for (const signal::ListedRoom& room : signaller.rooms()) {
                        std::printf("listed %s %d/%d%s%s rollback %d", encode_room_code(room.room).c_str(), room.joined, room.players,
                            room.playing ? " playing" : "", room.direct ? " direct" : "", room.rollback);
                        if (room.host != 0) std::printf(" host %016llx", (unsigned long long)room.host);
                        if (!room.release.empty()) std::printf(" release %s", room.release.c_str());
                        if (room.other_build) std::printf(" other-build");
                        if (room.spectators) std::printf(" spectators");
                        if (room.watching != 0) std::printf(" watching %d", room.watching);
                        std::printf("\n");
                    }
                    std::printf("rooms %zu, others %d, older %d\n", signaller.rooms().size(), signaller.others(), signaller.older());
                    list_result = 0;
                }
            }
            if (list_result != 0) {
                std::fprintf(stderr, "couldn't list the rooms: %s\n", signaller.state() == Signaller::State::failed ? signaller.error().c_str() :
                    !error.empty() ? error.c_str() : "no answer");
            }
        }
        shutdown();
        return list_result;
    }
    int result = 1;
    {
        // Through a room on the server: registered there first, then the host makes its room and a joiner
        // looks it up.
        Signaller signaller;
        signaller.speak(speaks);
        Session session;
        bool started = false;
        uint64_t room = 0;
        const auto wait = [&signaller](auto done) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!done() && signaller.state() != Signaller::State::failed && std::chrono::steady_clock::now() < until) {
                signaller.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return done();
        };
        if (again && hosting && !signal_server.empty()) {
            if (!signaller.connect(signal_server, error) || !wait([&] { return signaller.state() == Signaller::State::ready; })) {
                std::fprintf(stderr, "the first time: %s\n", signaller.error().c_str());
            }
            else {
                signaller.create_room(1);
                wait([&] { return signaller.room() != 0; });
                std::printf("left room %s\n", encode_room_code(signaller.room()).c_str());
            }
            signaller.close();
            if (!new_id(error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
            }
        }
        if (signal_server.empty()) {
            started = hosting ? session.host((uint16_t)std::atoi(where.c_str()), players, me, error, local) : session.join(where, me, error);
        }
        else if (!hosting && !decode_room_code(where)) {
            error = "\"" + where + "\" isn't a room code";
        }
        else if (!signaller.connect(signal_server, error)) {
            error = "signalling: " + error;
        }
        else if (!wait([&] { return signaller.state() == Signaller::State::ready; })) {
            error = "signalling: " + (signaller.state() == Signaller::State::failed ? signaller.error() : std::string("no answer"));
        }
        else if (relay_only && !signaller.turn()) {
            error = "the server offers no TURN relay: only --direct 1 can connect through it";
        }
        else {
            std::printf("registered with %s as %016llx%s\n", signal_server.c_str(), (unsigned long long)local_id(),
                signaller.turn() ? (", relay " + signaller.turn()->servers).c_str() : "");
            Ice ice = signaller.ice(relay_only);
            if (!turn_password.empty()) ice.turn_password = turn_password;
            if (hosting) {
                started = session.host(signaller, ice, players, me, error, local);
                if (started) {
                    signaller.create_room(session.key());
                    started = wait([&] { return signaller.room() != 0; });
                    room = signaller.room();
                    if (!started) error = "no room: " + signaller.error();
                }
            }
            else {
                signaller.find_room(*decode_room_code(where));
                wait([&] { return signaller.lookup() == Signaller::Lookup::found || signaller.lookup() == Signaller::Lookup::missing; });
                if (signaller.lookup() == Signaller::Lookup::found) {
                    started = session.join(signaller, ice, signaller.found().host, signaller.found().key, me, error);
                }
                else {
                    error = signaller.lookup() == Signaller::Lookup::missing ? "no room " + where + " on the server" : "no answer: " + signaller.error();
                }
            }
        }
        if (!started) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        else {
            std::printf("%s %s%s as player %016llx\n", hosting ? "hosting" : "joining", signal_server.empty() && hosting ? "on port " : "", where.c_str(),
                (unsigned long long)me.player);
            if (hosting) {
                // The host is ready from the start: the match starts once every port is taken and every
                // joiner is ready too.
                session.set_ready(true);
                session.set_blocked(blocked);
                session.set_release(release);
                session.set_room_settings(own_view, -1, 0, spectators);
            }
            if (hosting && signal_server.empty()) {
                std::printf("join code %s\n", session.join_code("127.0.0.1:" + where).c_str());
            }
            else if (hosting) {
                std::printf("room %s\n", encode_room_code(room).c_str());
            }
            std::fflush(stdout);
            std::map<int, Received> received;   // by port
            int joined = local;                 // the host counts its own
            bool playing = false;               // started: all the ports are taken
            bool lossy = false;                 // --loss on (once playing)
            // A public room: listed as it is now (again as that changes; unchanged, nothing is sent).
            const auto list_room = [&] {
                if (listed && hosting && room != 0) {
                    signal::ListedRoom listing{ 0, (uint8_t)players, (uint8_t)joined, playing, !relay_only, 0 };
                    listing.release = release;
                    signaller.publish(me, listing, platform);
                }
            };
            list_room();
            // Visible on this network: a beacon each second.
            LanBeacon beacon;
            beacon.send_to_this_machine(lan_loopback);
            if (lan_visible && hosting && signal_server.empty() && !beacon.open(lan_port, error)) {
                std::fprintf(stderr, "LAN: %s\n", error.c_str());
            }
            auto next_beacon = std::chrono::steady_clock::now();
            bool ended = false;
            uint32_t sent = 0;                  // reads sent so far
            double done_at = 0;
            const auto start = std::chrono::steady_clock::now();
            auto next_tick = start;
            bool welcomed = false;
            bool ready_sent = false;
            auto welcomed_at = start;
            while (!ended) {
                list_room();
                if (lan_visible && hosting && signal_server.empty() && std::chrono::steady_clock::now() >= next_beacon) {
                    beacon.send({ fit_of(me), platform, (uint16_t)std::atoi(where.c_str()), session.key(), (uint8_t)players,
                        (uint8_t)joined, playing, 0 });
                    next_beacon += std::chrono::seconds(1);
                }
                for (Event& event : session.poll()) {
                    switch (event.type) {
                        case Event::Type::joined:
                            if (event.spectator >= 0) {
                                std::printf("spectator %d joined\n", event.spectator + 1);
                                break;
                            }
                            joined += std::popcount(event.ports);
                            std::printf("ports %x joined (%d of %d) by %s\n", event.ports, joined, players, session.route(event.port).c_str());
                            if (kick) {
                                session.kick(event.port);
                                joined -= std::popcount(event.ports);
                                std::printf("kicked ports %x\n", event.ports);
                            }
                            break;
                        case Event::Type::welcomed:
                            players = event.players;
                            welcomed_at = std::chrono::steady_clock::now();
                            welcomed = true;
                            std::printf("welcomed: ports %x of %d, the host by %s\n", event.ports, event.players, session.route(0).c_str());
                            if (hostile == "early") {
                                session.send(inputs_for(event.port, 0));
                                std::printf("sent Inputs before the start\n");
                            }
                            break;
                        case Event::Type::started:
                            playing = true;
                            std::printf("started%s\n", event.own_view ? " (own view)" : "");
                            if (hostile == "far") {
                                session.send(inputs_for(std::countr_zero(session.ports()), 100000));
                                std::printf("sent Inputs far ahead\n");
                            }
                            if (hostile == "port") {
                                session.send(inputs_for(0, 0));
                                std::printf("sent Inputs for another's port\n");
                            }
                            if (hostile == "hash") {
                                session.send(Hash{ 30, 1 });
                                session.send(Hash{ 30, 1 });
                                std::printf("sent the same Hash twice\n");
                            }
                            break;
                        case Event::Type::left:
                            if (event.spectator >= 0) {
                                std::printf("spectator %d left: %s (%s)\n", event.spectator + 1, describe(event.reason), event.detail.c_str());
                                break;
                            }
                            std::printf("port %d left: %s (%s)\n", event.port, describe(event.reason), event.detail.c_str());
                            if (!playing && event.reason != Reason::kicked) {
                                joined -= std::popcount(event.ports);
                            }
                            break;
                        case Event::Type::moved:
                            std::printf("moved: ports %x\n", event.ports);
                            break;
                        case Event::Type::ended:
                            std::printf("ended: %s (%s)%s%s\n", describe(event.reason), event.detail.c_str(),
                                event.release.empty() ? "" : ", host's release ", event.release.c_str());
                            ended = true;
                            break;
                        case Event::Type::packet:
                            if (const Inputs* inputs = std::get_if<Inputs>(&event.packet)) {
                                Received& r = received[inputs->port];
                                // (An early copy past the reads that came is for the reliable Inputs to fill in, as in the
                                // game; and the reads any Inputs repeats are taken once.)
                                if (event.copy && inputs->first_read > r.next) break;
                                r.packets++;
                                for (int i = 0; i < inputs->count; i++) {
                                    const uint32_t read = inputs->first_read + i;
                                    if (read < r.next) continue;
                                    // Out of order, missing or not the pad sent: all wrong.
                                    if (read != r.next || !(inputs->pads[i] == made_up_pad(inputs->port, read))) {
                                        r.wrong++;
                                    }
                                    r.next = read + 1;
                                }
                            }
                            break;
                    }
                }

                // The waiting room: a joiner says Ready (after --ready-after); the host starts once every port is
                // taken and everyone's ready.
                if (!hosting && welcomed && !ready_sent &&
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - welcomed_at).count() >= ready_after) {
                    session.set_ready(true);
                    ready_sent = true;
                    std::printf("ready\n");
                }
                if (hosting && !playing && joined == players && session.everyone_ready() && session.start(own_view)) {
                    playing = true;
                    std::printf("started after %.1f s%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                        own_view ? " (own view)" : "");
                }
                // The library's made-up loss is for every connection of this process (it can't be had for one), and
                // this process is one peer: so it starts with the match, all connected.
                if (playing && loss > 0 && !lossy) {
                    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, loss);
                    lossy = true;
                }
                // Every 1/30 s once the match has started: the next read's pad.
                const auto now = std::chrono::steady_clock::now();
                if (!playing) {
                    next_tick = now;
                }
                else if (now >= next_tick && sent < reads) {
                    // The peers here aren't in lockstep, only sending 30 reads a second each: they may run
                    // a few seconds apart (the joiners start as Start reaches them), so their Inputs may be
                    // that far ahead of this one's.
                    session.set_horizon(sent + 150);
                    next_tick += std::chrono::microseconds(33333);
                    for (int port = 0; port < max_players; port++) {
                        if ((session.ports() & (1u << port)) != 0) {
                            session.send(inputs_for(port, sent));
                        }
                    }
                    sent++;
                }

                bool complete = playing && sent == reads && (int)received.size() == players - std::popcount(session.ports());
                for (const auto& [port, r] : received) {
                    complete = complete && r.next >= reads;
                }
                const double seconds = std::chrono::duration<double>(now - start).count();
                if (complete && done_at == 0) {
                    done_at = seconds;
                }
                // A while longer for the others to finish, then goodbye.
                if (done_at != 0 && seconds - done_at > 2.0) {
                    session.leave(Reason::quit);
                    ended = true;
                }
                if (seconds > reads / 30.0 + 60) {
                    std::printf("gave up waiting\n");
                    session.leave(Reason::timeout);
                    ended = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            bool right = done_at != 0;
            for (const auto& [port, r] : received) {
                std::printf("port %d: %u of %u reads, %u wrong, in %u packets\n", port, r.next, reads, r.wrong, r.packets);
                right = right && r.next >= reads && r.wrong == 0;
            }
            std::printf("%s\n", right ? "all pads arrived and were right" : "FAILED");
            result = right ? 0 : 1;
        }
    }
    shutdown();
    return result;
}
