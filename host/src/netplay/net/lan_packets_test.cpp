// Tests of LAN discovery's beacon (lan_packets.cpp): it round-trips, its layout is as documented, and every
// out-of-range field, size and version is refused. Exits 1 on a failure.

#include <cstdio>
#include <vector>

#include "lan_packets.hpp"
#include "packets.hpp"

using namespace conker::netplay::lan;
using conker::netplay::signal::Platform;
using conker::netplay::max_players;
using conker::netplay::max_rollback;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("lan_packets_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::vector<uint8_t> bytes_of(const Beacon& beacon) {
        uint8_t buffer[beacon_size];
        return encode(beacon, buffer) ? std::vector<uint8_t>(buffer, buffer + beacon_size) : std::vector<uint8_t>();
    }

    bool decodes(const std::vector<uint8_t>& data) {
        return decode(data).has_value();
    }
}

int main() {
    const Beacon beacon{ 0x0102030405060708ull, Platform::windows, 27015, 0x1112131415161718ull, 4, 2, false, 8, 5, true };
    const std::vector<uint8_t> data = bytes_of(beacon);
    CHECK(data.size() == beacon_size && decode(data) && *decode(data) == beacon);
    CHECK(data[0] == 'C' && data[1] == 'L' && data[2] == version && data[3] == 0 && data[4] == 8 && data[12] == 2 &&
        data[13] == (27015 & 255) && data[14] == (27015 >> 8) && data[15] == 0x18 && data[23] == 4 && data[24] == 2 && data[25] == 2 &&
        data[26] == 8 && data[27] == 5);
    const Beacon playing{ 1, Platform::last, 1, 1, (uint8_t)max_players, (uint8_t)max_players, true, max_rollback };
    CHECK(decode(bytes_of(playing)) == playing);

    // Not encoded: any field out of range.
    Beacon bad = beacon; bad.fit = 0; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.platform = (Platform)0; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.platform = (Platform)((int)Platform::last + 1); CHECK(bytes_of(bad).empty());
    bad = beacon; bad.port = 0; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.key = 0; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.players = 1; bad.joined = 1; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.players = max_players + 1; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.joined = 0; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.joined = 5; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.rollback = max_rollback + 1; CHECK(bytes_of(bad).empty());
    bad = beacon; bad.watching = conker::netplay::max_spectators + 1; CHECK(bytes_of(bad).empty());

    // Refused: another magic, version or size; every field out of range.
    std::vector<uint8_t> b = data; b[1] = 'S'; CHECK(!decodes(b));
    b = data; b[2]++; CHECK(!decodes(b));
    b = data; b.push_back(0); CHECK(!decodes(b));
    b = data; b.pop_back(); CHECK(!decodes(b));
    CHECK(!decodes({}));
    b = data; for (int i = 4; i < 12; i++) b[i] = 0; CHECK(!decodes(b));     // fit 0
    b = data; b[12] = 0; CHECK(!decodes(b));
    b = data; b[12] = 4; CHECK(!decodes(b));
    b = data; b[13] = b[14] = 0; CHECK(!decodes(b));                          // port 0
    b = data; for (int i = 15; i < 23; i++) b[i] = 0; CHECK(!decodes(b));    // key 0
    b = data; b[23] = 1; b[24] = 1; CHECK(!decodes(b));
    b = data; b[23] = max_players + 1; CHECK(!decodes(b));
    b = data; b[24] = 0; CHECK(!decodes(b));
    b = data; b[24] = 5; CHECK(!decodes(b));
    b = data; b[25] = 4; CHECK(!decodes(b));                                  // an unknown flag
    b = data; b[27] = conker::netplay::max_spectators + 1; CHECK(!decodes(b));
    b = data; b[26] = max_rollback + 1; CHECK(!decodes(b));

    if (failures == 0) {
        std::printf("lan_packets_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
