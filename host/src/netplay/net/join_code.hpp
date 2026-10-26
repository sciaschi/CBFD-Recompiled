// Join codes: what a player needs to join a match, as a code to pass on. There is no server: the code
// is the host's address and port, and the match's key (64 random bits the host makes for each match),
// with a check against typos. Without the key a joiner is refused, so only those given the code can take
// a place in the match; but the code is the host's address too, so it's for people the host trusts.
//
// An IPv4 code has 24 characters, an IPv6 one 44. They're Crockford's base32 (0-9 and the letters but
// I, L, O and U), read without regard to case, dashes or spaces, with O as 0 and I and L as 1.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace conker::netplay {
    struct Endpoint {
        bool ipv6 = false;
        std::array<uint8_t, 16> ip{};   // an IPv4 address in the first 4 bytes, the rest zero
        uint16_t port = 0;
        uint64_t key = 0;               // the match's key
        bool operator==(const Endpoint&) const = default;
    };

    // The code for `endpoint`, in groups of 4 with dashes; empty if its port is 0 (or an IPv4
    // endpoint's last 12 bytes aren't zero).
    std::string encode_join_code(const Endpoint& endpoint);
    // The endpoint in `code`, or nothing if it isn't one (wrong length or characters, or a typo the
    // check caught).
    std::optional<Endpoint> decode_join_code(std::string_view code);

    // Room codes: a room on a netplay server (signal_packets.hpp), which the server gives the host and a
    // joiner looks up there. No address in it: a random number from 1 to max_room, as 8 characters of the
    // same alphabet, in two groups of 4 ("7KQ2-M9XD"), read the same way. No check: a typo is a room that
    // isn't there (a lookup the server counts), or, at worst, one of the few thousand there are among a
    // trillion.
    constexpr uint64_t max_room = (1ull << 40) - 1;
    // The code for `room`, or empty if it's 0 or past max_room.
    std::string encode_room_code(uint64_t room);
    // The room in `code`, or nothing if it isn't one.
    std::optional<uint64_t> decode_room_code(std::string_view code);
    // A listed room's name, made from its number (two words, "Grumpy Squirrel"): the same in every game,
    // with nothing a player wrote. Names repeat (a thousand of them); the code doesn't.
    std::string room_name(uint64_t room);
    // A player's name, made from their player id (packets.hpp's Hello) the same way: the same on every
    // machine, with nothing anyone wrote.
    std::string player_name(uint64_t player);
}
