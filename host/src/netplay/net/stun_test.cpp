// Tests of the connection test's MD5 (stun.cpp), with RFC 1321's own examples. Exits 1 on a failure. (The rest is tested against a relay: tools/netplay/server/test.sh.)

#include <cstdio>
#include <string>

#include "stun.hpp"

namespace {
    int failures = 0;

    std::string hex(const std::array<uint8_t, 16>& bytes) {
        std::string out;
        char digits[3];
        for (uint8_t b : bytes) {
            std::snprintf(digits, sizeof(digits), "%02x", b);
            out += digits;
        }
        return out;
    }

    void check(const std::string& message, const char* expected) {
        const std::string got = hex(conker::netplay::stun::md5(message));
        if (got != expected) {
            std::printf("stun_test: FAILED: md5(\"%s\") is %s, not %s\n", message.c_str(), got.c_str(), expected);
            failures++;
        }
    }
}

int main() {
    check("", "d41d8cd98f00b204e9800998ecf8427e");
    check("a", "0cc175b9c0f1b6a831c399e269772661");
    check("abc", "900150983cd24fb0d6963f7d28e17f72");
    check("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    check("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    check("12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a");
    if (failures == 0) {
        std::printf("stun_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
