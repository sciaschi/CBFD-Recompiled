// Short-lived credentials for the TURN relay (coturn's "TURN REST API", its use-auth-secret): the signalling
// server and coturn share a secret; the server gives each peer a user "<expiry>:<id>" (the Unix time it
// expires at, in seconds) and its password, the base64 of HMAC-SHA1(secret, user). coturn checks them
// itself, so it isn't an open relay, and a peer's credentials stop working once they expire.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace conker::netplay {
    struct TurnCredentials {
        std::string user;
        std::string password;
    };
    // The credentials for `id`, until Unix time `expires`.
    TurnCredentials turn_credentials(std::string_view secret, uint64_t id, uint64_t expires);
    // For the tests: HMAC-SHA1's 20 bytes, and base64 (padded).
    std::string hmac_sha1(std::string_view key, std::string_view message);
    std::string base64(std::string_view bytes);
}
