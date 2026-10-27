// The connection test's look at the TURN relay (connection_test.hpp): a small STUN and TURN client (RFC 5389,
// RFC 5766) of its own, on plain UDP sockets, apart from GameNetworkingSockets (whose ICE client says
// nothing of why a connection didn't work). With the credentials the netplay server gave this peer
// (turn_credentials.hpp), it asks the relay for this machine's public address, allocates relayed ones, and
// sends packets between two sockets of its own, each with a relayed address (as two players of a relay-only
// match have), to learn what will work:
//   - through the relay: a packet sent from one, through the relay, to the other's relayed address arrives
//     (relay-only rooms, and the way every room falls back to);
//   - the NAT's mapping: a packet sent from the first socket straight to the other's relayed address
//     (another port than the relay's own) is seen there as coming from the same public address and port as
//     the first socket's: then the NAT keeps one mapping for every destination, so direct connections will
//     likely work (ICE's server-reflexive candidates do); another port means a "symmetric" NAT, and the relay;
//   - its filtering: a packet the relay sends from the other's relayed address (the relay's IP, but a port
//     the first socket hadn't sent to first) reaches it or not.
// IPv4 only, like the rest. Only the relay's own address is ever sent anything.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace conker::netplay::stun {
    // MD5 (RFC 1321): TURN's long-term key is MD5("user:realm:password"). For the tests too.
    std::array<uint8_t, 16> md5(std::string_view message);

    struct Address {
        uint32_t ip = 0;        // host order
        uint16_t port = 0;
        bool operator==(const Address&) const = default;
    };

    struct RelayCheck {
        std::string error;          // couldn't check at all (the relay's name, a socket): why
        bool reached = false;       // the relay answered (a STUN Binding)
        bool behind_nat = false;    // the public address isn't this machine's own
        bool allocated = false;     // it gave this machine a relayed address
        std::string refused;        // not allocated: the relay's error ("401 Unauthorized"), or "no answer"
        bool relays = false;        // a packet sent through it to itself came back
        enum class Mapping { unknown, same, differs };
        Mapping mapping = Mapping::unknown;     // the NAT's port for another destination (the relayed address)
        bool lets_in = false;       // a packet from the relayed address (not sent to before) arrived
    };

    // Checks the TURN relay at the first of `servers` ("host:port", separated by commas, as signal::Turn
    // has them) with `user` and `password`. Blocks, for up to about 20 s: run it on a thread of its own.
    RelayCheck check_relay(const std::string& servers, const std::string& user, const std::string& password);
}
