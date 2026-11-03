// TURN credentials (turn_credentials.hpp), with SHA-1 (FIPS 180-4) and HMAC (RFC 2104) here, so the server
// needs no crypto library of its own.

#include "turn_credentials.hpp"

#include <array>
#include <cstdio>

namespace conker::netplay {
    namespace {
        uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }

        std::array<uint8_t, 20> sha1(std::string_view message) {
            uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
            std::string data(message);
            const uint64_t bits = (uint64_t)message.size() * 8;
            data += (char)0x80;
            while (data.size() % 64 != 56) data += (char)0;
            for (int shift = 56; shift >= 0; shift -= 8) data += (char)(bits >> shift);
            for (size_t block = 0; block < data.size(); block += 64) {
                uint32_t w[80];
                for (int i = 0; i < 16; i++) {
                    const auto* p = (const uint8_t*)data.data() + block + 4 * i;
                    w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
                }
                for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; i++) {
                    uint32_t f, k;
                    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                    const uint32_t t = rotl(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rotl(b, 30); b = a; a = t;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }
            std::array<uint8_t, 20> out;
            for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
            return out;
        }
    }

    std::string hmac_sha1(std::string_view key, std::string_view message) {
        std::string block(64, '\0');
        if (key.size() > 64) {
            const std::array<uint8_t, 20> hashed = sha1(key);
            std::copy(hashed.begin(), hashed.end(), block.begin());
        }
        else {
            std::copy(key.begin(), key.end(), block.begin());
        }
        std::string inner, outer;
        for (char c : block) {
            inner += (char)(c ^ 0x36);
            outer += (char)(c ^ 0x5C);
        }
        const std::array<uint8_t, 20> inner_hash = sha1(inner + std::string(message));
        const std::array<uint8_t, 20> outer_hash = sha1(outer + std::string(inner_hash.begin(), inner_hash.end()));
        return std::string(outer_hash.begin(), outer_hash.end());
    }

    std::string base64(std::string_view bytes) {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const size_t n = std::min<size_t>(3, bytes.size() - i);
            uint32_t v = 0;
            for (size_t j = 0; j < 3; j++) v = v << 8 | (j < n ? (uint8_t)bytes[i + j] : 0);
            for (size_t j = 0; j < 4; j++) out += j <= n ? alphabet[(v >> (18 - 6 * j)) & 63] : '=';
        }
        return out;
    }

    TurnCredentials turn_credentials(std::string_view secret, uint64_t id, uint64_t expires) {
        char user[48];
        std::snprintf(user, sizeof(user), "%llu:%016llx", (unsigned long long)expires, (unsigned long long)id);
        return { user, base64(hmac_sha1(secret, user)) };
    }
}
