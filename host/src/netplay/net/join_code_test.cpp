// Tests of the join codes (join_code.cpp): IPv4 and IPv6 endpoints round-trip at their documented
// lengths, the code is read loosely (case, dashes, spaces, O/I/L), and every one-character typo and
// every swap of two neighbouring characters is refused. Exits 1 on a failure.

#include <cctype>
#include <cstdio>
#include <random>
#include <string>

#include "join_code.hpp"

using namespace conker::netplay;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("join_code_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    Endpoint ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port, uint64_t key = 0x0123456789ABCDEFull) {
        Endpoint endpoint;
        endpoint.ip = { a, b, c, d };
        endpoint.port = port;
        endpoint.key = key;
        return endpoint;
    }

    bool round_trips(const Endpoint& endpoint, size_t characters) {
        const std::string code = encode_join_code(endpoint);
        std::string bare;
        for (char c : code) {
            if (c != '-') bare += c;
        }
        const std::optional<Endpoint> back = decode_join_code(code);
        return bare.size() == characters && back && *back == endpoint;
    }

    // Every code that differs from `code` in one character, or by two neighbours swapped, is refused.
    bool typos_refused(const std::string& code) {
        static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        for (size_t i = 0; i < code.size(); i++) {
            if (code[i] == '-') continue;
            for (char c : std::string(alphabet)) {
                if (c == code[i]) continue;
                std::string typo = code;
                typo[i] = c;
                if (decode_join_code(typo)) return false;
            }
            size_t next = i + 1;
            if (next < code.size() && code[next] == '-') next++;
            if (next < code.size() && code[next] != code[i]) {
                std::string swapped = code;
                std::swap(swapped[i], swapped[next]);
                if (decode_join_code(swapped)) return false;
            }
        }
        return true;
    }
}

int main() {
    CHECK(encode_join_code(ipv4(203, 0, 113, 7, 0)).empty());
    CHECK(round_trips(ipv4(203, 0, 113, 7, 27015), 24));
    CHECK(round_trips(ipv4(127, 0, 0, 1, 1, 0), 24));
    CHECK(round_trips(ipv4(255, 255, 255, 255, 65535, ~0ull), 24));
    Endpoint v6;
    v6.ipv6 = true;
    v6.ip = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    v6.port = 5000;
    v6.key = 0xFEDCBA9876543210ull;
    CHECK(round_trips(v6, 44));
    // The key is in the code: another key, another code.
    CHECK(encode_join_code(ipv4(203, 0, 113, 7, 27015, 1)) != encode_join_code(ipv4(203, 0, 113, 7, 27015, 2)));
    Endpoint bad = ipv4(1, 2, 3, 4, 5);
    bad.ip[4] = 1;                                  // an IPv4 address with more than 4 bytes
    CHECK(encode_join_code(bad).empty());

    // Read loosely.
    const std::string code = encode_join_code(ipv4(203, 0, 113, 7, 27015));
    std::string loose;
    for (char c : code) {
        if (c == '-') loose += "  ";
        else if (c == '0') loose += 'o';
        else if (c == '1') loose += 'l';
        else loose += (char)std::tolower(c);
    }
    CHECK(decode_join_code(loose) == ipv4(203, 0, 113, 7, 27015));

    // Not codes.
    CHECK(!decode_join_code(""));
    CHECK(!decode_join_code("----"));
    CHECK(!decode_join_code(code + "0"));           // too long
    CHECK(!decode_join_code(code.substr(1)));       // too short
    CHECK(!decode_join_code("U" + code.substr(1))); // not in the alphabet
    CHECK(!decode_join_code(code + std::string(19, '0')));
    CHECK(!decode_join_code(std::string(24, '0'))); // port 0, with its check right
    std::string spare = encode_join_code(v6);       // the last character's spare bit set (IPv6; IPv4 has none)
    spare.back() = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[std::string("0123456789ABCDEFGHJKMNPQRSTVWXYZ").find(spare.back()) ^ 1];
    CHECK(!decode_join_code(spare));

    // Typos, over many endpoints.
    std::mt19937 random(1);
    for (int i = 0; i < 300; i++) {
        Endpoint endpoint;
        endpoint.ipv6 = i % 3 == 0;
        for (size_t b = 0; b < (endpoint.ipv6 ? 16u : 4u); b++) endpoint.ip[b] = (uint8_t)random();
        endpoint.port = (uint16_t)(random() % 65535 + 1);
        endpoint.key = (uint64_t)random() << 32 | random();
        const std::string c = encode_join_code(endpoint);
        CHECK(decode_join_code(c) == endpoint);
        CHECK(typos_refused(c));
    }

    // Room codes.
    CHECK(encode_room_code(1) == "0000-0001");
    CHECK(encode_room_code(max_room) == "ZZZZ-ZZZZ");
    CHECK(encode_room_code(0).empty());
    CHECK(encode_room_code(max_room + 1).empty());
    CHECK(decode_room_code("7kq2 m9xd") == decode_room_code("7KQ2-M9XD"));
    CHECK(decode_room_code("OOOO-OOOI") == 1u);
    CHECK(!decode_room_code("0000-0000"));          // room 0
    CHECK(!decode_room_code("7KQ2-M9X"));           // too short
    CHECK(!decode_room_code("7KQ2-M9XDA"));         // too long
    CHECK(!decode_room_code("7KQ2-M9XU"));          // not in the alphabet
    CHECK(!decode_room_code(code));                 // a join code isn't a room code
    CHECK(!decode_join_code("7KQ2-M9XD"));          // nor the other way round
    for (int i = 0; i < 300; i++) {
        const uint64_t room = ((uint64_t)random() << 32 | random()) % max_room + 1;
        CHECK(decode_room_code(encode_room_code(room)) == room);
    }

    if (failures == 0) {
        std::printf("join_code_test: all passed (e.g. 203.0.113.7:27015 is %s)\n", code.c_str());
    }
    return failures == 0 ? 0 : 1;
}
