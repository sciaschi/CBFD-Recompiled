// The Netplay menu's Test connection: what will work from this machine, said plainly. It reaches the netplay
// server and registers (which signalling version it speaks, whether it offers a relay), checks the relay
// (stun.hpp: through it, and what the network's NAT does), and opens the port LAN discovery listens on.
// It shows no addresses.
//
// It uses a Signaller of its own, and only one may exist at once: nothing else may use the server while it
// runs. The library must be started. Single-threaded but for the relay's check, on a thread of its own.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "stun.hpp"

namespace conker::netplay {
    class Signaller;

    struct ConnectionReport {
        enum class Verdict { unknown, good, bad };
        // The server: reached and registered, in which signalling version.
        Verdict server = Verdict::unknown;
        uint16_t server_version = 0;
        // The relay: none offered (unknown), works (good) or not.
        Verdict relay = Verdict::unknown;
        // Direct connections: likely (good), unlikely (bad), or can't tell (no relay to check with).
        Verdict direct = Verdict::unknown;
        // The LAN discovery port opened.
        Verdict lan = Verdict::unknown;
        // A line each, for the player; and `summary`, what will work.
        std::string server_text, relay_text, direct_text, lan_text, summary;
    };

    class ConnectionTest {
    public:
        ConnectionTest();
        ~ConnectionTest();
        ConnectionTest(const ConnectionTest&) = delete;
        ConnectionTest& operator=(const ConnectionTest&) = delete;
        // Starts on `server` ("host:port"; empty: none set, only the LAN port) and the LAN port `lan_port`.
        void start(const std::string& server, uint16_t lan_port);
        // Goes on; true once done (report() is complete). Call it often.
        bool poll();
        const ConnectionReport& report() const { return out; }

    private:
        void finish();
        std::unique_ptr<Signaller> signaller;
        std::thread worker;
        std::shared_ptr<struct RelayResult> relay;
        uint16_t lan_port = 0;
        double started = 0;
        bool done = false;
        ConnectionReport out;
    };
}
