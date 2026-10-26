// Tests of the TURN credentials (turn_credentials.cpp): HMAC-SHA1 against RFC 2202's vectors, base64
// against RFC 4648's, and a credential against one worked out with OpenSSL. Exits 1 on a failure.

#include <cstdio>
#include <string>

#include "turn_credentials.hpp"

using namespace conker::netplay;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("turn_credentials_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::string hex(const std::string& bytes) {
        std::string out;
        char digits[3];
        for (char c : bytes) {
            std::snprintf(digits, sizeof(digits), "%02x", (unsigned)(uint8_t)c);
            out += digits;
        }
        return out;
    }
}

int main() {
    // RFC 2202, test cases 1, 2, 6 (a key longer than a block) and 7.
    CHECK(hex(hmac_sha1(std::string(20, '\x0b'), "Hi There")) == "b617318655057264e28bc0b6fb378c8ef146be00");
    CHECK(hex(hmac_sha1("Jefe", "what do ya want for nothing?")) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    CHECK(hex(hmac_sha1(std::string(80, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
        "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    CHECK(hex(hmac_sha1(std::string(80, '\xaa'), "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data")) ==
        "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");

    // RFC 4648.
    CHECK(base64("") == "");
    CHECK(base64("f") == "Zg==");
    CHECK(base64("fo") == "Zm8=");
    CHECK(base64("foo") == "Zm9v");
    CHECK(base64("foobar") == "Zm9vYmFy");

    // printf '%s' 1700000000:0123456789abcdef | openssl dgst -sha1 -hmac test-secret -binary | base64
    const TurnCredentials credentials = turn_credentials("test-secret", 0x0123456789abcdefull, 1700000000);
    CHECK(credentials.user == "1700000000:0123456789abcdef");
    CHECK(credentials.password == "T5hCnpq4Jfgdb2LPBVKRi/QWYTs=");

    if (failures == 0) {
        std::printf("turn_credentials_test: all passed (%s %s)\n", credentials.user.c_str(), credentials.password.c_str());
    }
    return failures == 0 ? 0 : 1;
}
