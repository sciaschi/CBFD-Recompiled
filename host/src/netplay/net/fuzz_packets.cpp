// libFuzzer target for the netplay packets (packets.cpp), signalling's (signal_packets.cpp, each version the
// server speaks) and LAN beacons (lan_packets.cpp), the only code that reads what arrives from the network: any bytes either fail to decode, or decode to a packet that encodes back to exactly the
// same bytes (so nothing is read that isn't checked). Built with CONKER_NETPLAY_FUZZ (clang):
//
//   cmake -S host/src/netplay/net -B build-fuzz -DCONKER_NETPLAY_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_packets && build-fuzz/fuzz_packets -max_total_time=60

#include <cstdlib>
#include <cstring>

#include "packets.hpp"
#include "lan_packets.hpp"
#include "signal_packets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::optional<conker::netplay::Packet> packet = conker::netplay::decode({ data, size });
    if (packet) {
        static uint8_t buffer[conker::netplay::max_message_size];
        const size_t encoded = conker::netplay::encode(*packet, buffer);
        if (encoded != size || std::memcmp(buffer, data, size) != 0) {
            std::abort();
        }
    }
    namespace signal = conker::netplay::signal;
    for (uint16_t as = signal::oldest_version; as <= signal::version; as++) {
        const std::optional<signal::Packet> signal_packet = signal::decode({ data, size }, as);
        if (signal_packet) {
            static uint8_t buffer[signal::max_packet_size];
            const size_t encoded = signal::encode(*signal_packet, buffer, as);
            if (encoded != size || std::memcmp(buffer, data, size) != 0) {
                std::abort();
            }
        }
    }
    namespace lan = conker::netplay::lan;
    if (const std::optional<lan::Beacon> beacon = lan::decode({ data, size })) {
        uint8_t buffer[lan::beacon_size];
        if (!lan::encode(*beacon, buffer) || size != lan::beacon_size || std::memcmp(buffer, data, size) != 0) {
            std::abort();
        }
    }
    return 0;
}
