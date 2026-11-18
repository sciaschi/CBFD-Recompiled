// The connection test: see connection_test.hpp.

#include "connection_test.hpp"
#include "lan.hpp"
#include "signalling.hpp"

#include <chrono>
#include <mutex>

namespace conker::netplay {
    namespace {
        // How long the server has to register this machine.
        constexpr double server_seconds = 10.0;

        double now() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }
    }

    // The relay's check, filled in by the worker.
    struct RelayResult {
        std::mutex mutex;
        bool done = false;
        stun::RelayCheck check;
    };

    ConnectionTest::ConnectionTest() = default;

    ConnectionTest::~ConnectionTest() {
        // Not waited for (up to 15 s): it ends by itself, and holds nothing of this but the shared result.
        if (worker.joinable()) {
            worker.detach();
        }
    }

    void ConnectionTest::start(const std::string& server, uint16_t port) {
        lan_port = port;
        started = now();
        if (server.empty()) {
            out.server_text = "No netplay server is set, so rooms and the relay weren't tested.";
            finish();
            return;
        }
        signaller = std::make_unique<Signaller>();
        std::string error;
        if (!signaller->connect(server, error)) {
            out.server = ConnectionReport::Verdict::bad;
            out.server_text = "The netplay server's address isn't right: " + error + ".";
            signaller.reset();
            finish();
        }
    }

    bool ConnectionTest::poll() {
        if (done) {
            return true;
        }
        if (signaller) {
            signaller->poll();
            if (signaller->state() == Signaller::State::failed ||
                (signaller->state() != Signaller::State::ready && now() - started > server_seconds)) {
                out.server = ConnectionReport::Verdict::bad;
                out.server_text = signaller->state() == Signaller::State::failed ?
                    "The netplay server couldn't be reached (" + signaller->error() + ")." : "The netplay server didn't answer.";
                signaller.reset();
                finish();
                return true;
            }
            if (signaller->state() != Signaller::State::ready) {
                return false;
            }
            out.server = ConnectionReport::Verdict::good;
            out.server_version = signaller->version();
            out.server_text = "Reached the netplay server (signalling version " + std::to_string(signaller->version()) + ")." +
                (signaller->version() < signal::version ? " It's older than this game: public rooms may be missing." : "");
            const std::optional<signal::Turn> turn = signaller->turn();
            signaller.reset();
            if (!turn) {
                out.relay_text = "The server offers no relay: rooms can only connect directly.";
                finish();
                return true;
            }
            relay = std::make_shared<RelayResult>();
            worker = std::thread([result = relay, servers = turn->servers, user = turn->user, password = turn->password] {
                stun::RelayCheck check = stun::check_relay(servers, user, password);
                std::lock_guard lock(result->mutex);
                result->check = std::move(check);
                result->done = true;
            });
            return false;
        }
        if (relay) {
            {
                std::lock_guard lock(relay->mutex);
                if (!relay->done) {
                    return false;
                }
            }
            worker.join();
            const stun::RelayCheck& check = relay->check;
            using Verdict = ConnectionReport::Verdict;
            if (!check.error.empty()) {
                out.relay = Verdict::bad;
                out.relay_text = "Couldn't test the relay: " + check.error + ".";
            }
            else if (!check.reached) {
                out.relay = Verdict::bad;
                out.relay_text = "The relay didn't answer: a firewall may be blocking UDP to it.";
            }
            else if (!check.allocated) {
                out.relay = Verdict::bad;
                out.relay_text = "The relay refused this machine (" + check.refused + ").";
            }
            else if (!check.relays) {
                out.relay = Verdict::bad;
                out.relay_text = "The relay answered, but nothing came back through it.";
            }
            else {
                out.relay = Verdict::good;
                out.relay_text = "The relay works.";
            }
            if (check.reached && !check.behind_nat) {
                out.direct = Verdict::good;
                out.direct_text = check.lets_in || check.mapping == stun::RelayCheck::Mapping::unknown ?
                    "This machine has a public address: direct connections will likely work." :
                    "This machine has a public address, but a firewall keeps out what it didn't ask for: direct connections may not work.";
            }
            else if (check.mapping == stun::RelayCheck::Mapping::same) {
                out.direct = Verdict::good;
                out.direct_text = "Your network's NAT keeps one port for every destination: direct connections will likely work.";
            }
            else if (check.mapping == stun::RelayCheck::Mapping::differs) {
                out.direct = Verdict::bad;
                out.direct_text = "Your network's NAT gives each destination a port of its own (a \"symmetric\" NAT): "
                    "direct connections are unlikely to work, so matches will go through the relay.";
            }
            else {
                out.direct_text = "Couldn't tell whether direct connections will work.";
            }
            relay.reset();
            finish();
            return true;
        }
        return done;
    }

    void ConnectionTest::finish() {
        using Verdict = ConnectionReport::Verdict;
        LanListener listener;
        std::string error;
        if (listener.open(lan_port, error)) {
            out.lan = Verdict::good;
            out.lan_text = "This machine can listen for matches on this network (UDP port " + std::to_string(lan_port) + ").";
        }
        else {
            out.lan = Verdict::bad;
            out.lan_text = "This machine can't listen for matches on this network: " + error + ".";
        }
        listener.close();
        if (out.server != Verdict::good) {
            out.summary = out.server == Verdict::unknown ? "Set a netplay server to test rooms." :
                "Rooms won't work until the netplay server can be reached; join codes and \"On this network\" still can.";
        }
        else if (out.relay == Verdict::good) {
            out.summary = out.direct == Verdict::good ? "Rooms will work, through the relay or directly." :
                "Rooms will work through the relay.";
        }
        else if (out.relay == Verdict::unknown) {
            out.summary = "Rooms will only work where a direct connection does (this server has no relay).";
        }
        else {
            out.summary = "Rooms through the relay won't work from here; ones that allow direct connections may.";
        }
        done = true;
    }
}
