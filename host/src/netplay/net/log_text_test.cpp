// Tests of without_addresses (session.cpp): every IP address in a line for the log is taken out, and the
// rest of the line is left alone. Exits 1 on a failure.

#include <cstdio>
#include <string>

#include "session.hpp"

using conker::netplay::without_addresses;

namespace {
    int failures = 0;

    void expect(const std::string& in, const std::string& want, int line) {
        const std::string got = without_addresses(in);
        if (got != want) {
            std::printf("log_text_test.cpp:%d: FAILED: \"%s\" gave \"%s\", not \"%s\"\n", line, in.c_str(), got.c_str(),
                want.c_str());
            failures++;
        }
    }
#define EXPECT(in, want) expect((in), (want), __LINE__)
}

int main() {
    // The library's descriptions of direct connections.
    EXPECT("#2914 UDP 203.0.113.7:27015", "#2914 UDP <address>");
    EXPECT("UDP peer@198.51.100.20:5000", "UDP peer@<address>");
    EXPECT("[#1 UDP [2001:db8::1]:27015] problem detected locally", "[#1 UDP <address>] problem detected locally");
    // Bare, with no port, IPv6 bare or mapped, and at the end of a sentence.
    EXPECT("from 10.0.0.2.", "from <address>.");
    EXPECT("to 2001:db8:0:0:0:0:0:7 and ::1", "to <address> and <address>");
    EXPECT("via ::ffff:192.0.2.1:9", "via <address>");
    EXPECT("two: 192.0.2.1:1, 192.0.2.2:2", "two: <address>, <address>");
    // The library's name for a peer with no other identity.
    EXPECT("Rejecting connection request from ip:203.0.113.7:27015 at 203.0.113.7:27016",
        "Rejecting connection request from ip:<address> at <address>");
    // Left alone: no address in them.
    EXPECT("P2P ICE #77 peer str:1234abcd", "P2P ICE #77 peer str:1234abcd");
    EXPECT("at 12:34:56, version 1.6.1", "at 12:34:56, version 1.6.1");
    EXPECT("CSteamNetworkConnectionBase::ConnectionState_ProblemDetectedLocally",
        "CSteamNetworkConnectionBase::ConnectionState_ProblemDetectedLocally");
    EXPECT("key deadbeefcafe, 3 ms", "key deadbeefcafe, 3 ms");
    EXPECT("", "");
    if (failures == 0) {
        std::printf("log_text_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
