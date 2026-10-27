// Join codes (join_code.hpp). The bytes, big-endian: the address (4 or 16 bytes), the port (2), the key
// (8), then a CRC-8 of those (polynomial 0x07, which catches any change within 8 bits in a row: so any one
// mistyped character, or two swapped neighbours). The 15 or 27 bytes are written 5 bits to a character,
// with the last character's spare bits zero: 24 or 44 characters.

#include "join_code.hpp"

#include <vector>

namespace conker::netplay {
    namespace {
        constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        constexpr size_t ipv4_bytes = 4 + 2 + 8 + 1;
        constexpr size_t ipv6_bytes = 16 + 2 + 8 + 1;
        constexpr size_t characters_for(size_t bytes) { return (bytes * 8 + 4) / 5; }

        // The value of character `c`, or -1.
        int value_of(char c) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c == 'O') c = '0';
            if (c == 'I' || c == 'L') c = '1';
            for (int i = 0; i < 32; i++) {
                if (alphabet[i] == c) return i;
            }
            return -1;
        }

        uint8_t crc8(const uint8_t* data, size_t size) {
            uint8_t crc = 0;
            for (size_t i = 0; i < size; i++) {
                crc ^= data[i];
                for (int bit = 0; bit < 8; bit++) {
                    crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
                }
            }
            return crc;
        }
    }

    std::string encode_join_code(const Endpoint& endpoint) {
        if (endpoint.port == 0) {
            return {};
        }
        const size_t ip_size = endpoint.ipv6 ? 16 : 4;
        for (size_t i = ip_size; i < endpoint.ip.size(); i++) {
            if (endpoint.ip[i] != 0) return {};
        }
        std::vector<uint8_t> bytes(endpoint.ip.begin(), endpoint.ip.begin() + ip_size);
        bytes.push_back((uint8_t)(endpoint.port >> 8));
        bytes.push_back((uint8_t)endpoint.port);
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back((uint8_t)(endpoint.key >> shift));
        }
        bytes.push_back(crc8(bytes.data(), bytes.size()));

        std::string code;
        const size_t characters = characters_for(bytes.size());
        for (size_t c = 0; c < characters; c++) {
            int value = 0;
            for (size_t bit = c * 5; bit < c * 5 + 5; bit++) {
                const bool set = bit < bytes.size() * 8 && (bytes[bit / 8] >> (7 - bit % 8)) & 1;
                value = value << 1 | (set ? 1 : 0);
            }
            if (c > 0 && c % 4 == 0) code += '-';
            code += alphabet[value];
        }
        return code;
    }

    std::optional<Endpoint> decode_join_code(std::string_view code) {
        std::vector<int> values;
        for (char c : code) {
            if (c == '-' || c == ' ') continue;
            const int value = value_of(c);
            if (value < 0 || values.size() == characters_for(ipv6_bytes)) return std::nullopt;
            values.push_back(value);
        }
        size_t size;
        if (values.size() == characters_for(ipv4_bytes)) size = ipv4_bytes;
        else if (values.size() == characters_for(ipv6_bytes)) size = ipv6_bytes;
        else return std::nullopt;

        std::vector<uint8_t> bytes(size, 0);
        for (size_t bit = 0; bit < values.size() * 5; bit++) {
            const bool set = (values[bit / 5] >> (4 - bit % 5)) & 1;
            if (bit >= size * 8) {
                if (set) return std::nullopt;   // the spare bits are zero
            }
            else if (set) {
                bytes[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
            }
        }
        if (crc8(bytes.data(), size - 1) != bytes[size - 1]) {
            return std::nullopt;
        }
        Endpoint endpoint;
        endpoint.ipv6 = size == ipv6_bytes;
        const size_t ip_size = endpoint.ipv6 ? 16 : 4;
        for (size_t i = 0; i < ip_size; i++) endpoint.ip[i] = bytes[i];
        endpoint.port = (uint16_t)(bytes[ip_size] << 8 | bytes[ip_size + 1]);
        for (size_t i = 0; i < 8; i++) {
            endpoint.key = endpoint.key << 8 | bytes[ip_size + 2 + i];
        }
        if (endpoint.port == 0) {
            return std::nullopt;
        }
        return endpoint;
    }

    std::string encode_room_code(uint64_t room) {
        if (room == 0 || room > max_room) {
            return {};
        }
        std::string code;
        for (int c = 0; c < 8; c++) {
            if (c == 4) code += '-';
            code += alphabet[(room >> (35 - 5 * c)) & 31];
        }
        return code;
    }

    std::optional<uint64_t> decode_room_code(std::string_view code) {
        uint64_t room = 0;
        int characters = 0;
        for (char c : code) {
            if (c == '-' || c == ' ') continue;
            const int value = value_of(c);
            if (value < 0 || ++characters > 8) return std::nullopt;
            room = room << 5 | (uint64_t)value;
        }
        if (characters != 8 || room == 0) {
            return std::nullopt;
        }
        return room;
    }

    std::string room_name(uint64_t room) {
        static constexpr const char* adjectives[32] = {
            "Grumpy", "Sleepy", "Brave", "Fuzzy", "Sneaky", "Jolly", "Rusty", "Lucky", "Muddy", "Shiny",
            "Bouncy", "Clumsy", "Dizzy", "Fancy", "Gentle", "Hasty", "Mighty", "Nimble", "Plucky", "Quiet",
            "Rowdy", "Silly", "Spooky", "Sturdy", "Tiny", "Wobbly", "Zesty", "Cheeky", "Crafty", "Dapper",
            "Frosty", "Golden",
        };
        static constexpr const char* nouns[32] = {
            "Squirrel", "Panther", "Weasel", "Hedgehog", "Bat", "Wasp", "Bee", "Teddy", "Haybale", "Acorn",
            "Barrel", "Cog", "Cheese", "Pumpkin", "Scarecrow", "Raptor", "Tank", "Castle", "Windmill", "Barn",
            "Beehive", "Sunflower", "Mushroom", "Frying Pan", "Toilet", "Scythe", "Anchor", "Lantern", "Kettle",
            "Cannon", "Bucket", "Trophy",
        };
        // Mixed first, so near numbers don't share a word.
        uint64_t x = room * 0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        return std::string(adjectives[x & 31]) + " " + nouns[(x >> 5) & 31];
    }

    std::string player_name(uint64_t player) {
        // Another mix than rooms', so a player and their room don't share a name.
        uint64_t x = (player ^ 0xC2B2AE3D27D4EB4Full) * 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31;
        return room_name(x == 0 ? 1 : x);
    }
}
