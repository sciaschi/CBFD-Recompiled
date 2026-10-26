// The connection test's STUN and TURN client: see stun.hpp. Messages (RFC 5389): u16 type, u16 length (of
// the attributes), the magic cookie, a 12-byte transaction id, then attributes (u16 type, u16 length, the
// value padded to 4 bytes), all big-endian. A request is sent again every half second until its answer
// (the same transaction id) comes, for up to 3 s.

#include "stun.hpp"
#include "turn_credentials.hpp"

#include <chrono>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace conker::netplay::stun {
    namespace {
#if defined(_WIN32)
        using Socket = SOCKET;
        constexpr Socket no_socket = INVALID_SOCKET;
        bool start_sockets() {
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
        }
        void close_socket(Socket s) { closesocket(s); }
#else
        using Socket = int;
        constexpr Socket no_socket = -1;
        bool start_sockets() { return true; }
        void close_socket(Socket s) { ::close(s); }
#endif

        constexpr uint32_t magic = 0x2112A442;
        enum : uint16_t {
            binding = 0x0001, binding_ok = 0x0101,
            allocate = 0x0003, allocate_ok = 0x0103, allocate_error = 0x0113,
            refresh = 0x0004,
            create_permission = 0x0008, create_permission_ok = 0x0108,
            send_indication = 0x0016, data_indication = 0x0017,
        };
        enum : uint16_t {
            username_attribute = 0x0006, integrity_attribute = 0x0008, error_attribute = 0x0009, lifetime_attribute = 0x000D,
            peer_attribute = 0x0012, data_attribute = 0x0013, realm_attribute = 0x0014, nonce_attribute = 0x0015,
            relayed_attribute = 0x0016, transport_attribute = 0x0019, mapped_attribute = 0x0020,
        };
        using Id = std::array<uint8_t, 12>;

        struct Message {
            uint16_t type = 0;
            Id id{};
            std::vector<uint8_t> bytes;     // the whole message, as it's built

            Message(uint16_t message_type, std::mt19937_64& random) : type(message_type) {
                for (uint8_t& b : id) b = (uint8_t)random();
                u16(type);
                u16(0);
                u32(magic);
                bytes.insert(bytes.end(), id.begin(), id.end());
            }
            void u16(uint16_t v) { bytes.push_back((uint8_t)(v >> 8)); bytes.push_back((uint8_t)v); }
            void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
            void set_length(size_t length) { bytes[2] = (uint8_t)(length >> 8); bytes[3] = (uint8_t)length; }
            void attribute(uint16_t kind, const void* value, size_t size) {
                u16(kind);
                u16((uint16_t)size);
                bytes.insert(bytes.end(), (const uint8_t*)value, (const uint8_t*)value + size);
                while (bytes.size() % 4 != 0) bytes.push_back(0);
                set_length(bytes.size() - 20);
            }
            void text(uint16_t kind, const std::string& value) { attribute(kind, value.data(), value.size()); }
            void address(uint16_t kind, const Address& a) {
                const uint8_t value[8] = { 0, 1, (uint8_t)((a.port ^ (magic >> 16)) >> 8), (uint8_t)(a.port ^ (magic >> 16)),
                    (uint8_t)((a.ip ^ magic) >> 24), (uint8_t)((a.ip ^ magic) >> 16), (uint8_t)((a.ip ^ magic) >> 8), (uint8_t)(a.ip ^ magic) };
                attribute(kind, value, sizeof(value));
            }
            // MESSAGE-INTEGRITY, last: HMAC-SHA1 with `key` over the message so far, its length counting it.
            void sign(const std::array<uint8_t, 16>& key) {
                set_length(bytes.size() - 20 + 24);
                const std::string mac = hmac_sha1({ (const char*)key.data(), key.size() }, { (const char*)bytes.data(), bytes.size() });
                u16(integrity_attribute);
                u16(20);
                bytes.insert(bytes.end(), mac.begin(), mac.end());
            }
        };

        // A message that arrived, its attributes by kind (the first of each; values unpadded).
        struct Parsed {
            uint16_t type = 0;
            Id id{};
            std::vector<std::pair<uint16_t, std::vector<uint8_t>>> attributes;
            const std::vector<uint8_t>* find(uint16_t kind) const {
                for (const auto& [k, v] : attributes) if (k == kind) return &v;
                return nullptr;
            }
            std::optional<Address> address(uint16_t kind) const {
                const std::vector<uint8_t>* v = find(kind);
                if (v == nullptr || v->size() != 8 || (*v)[1] != 1) return std::nullopt;
                Address a;
                a.port = (uint16_t)((((*v)[2] << 8) | (*v)[3]) ^ (magic >> 16));
                a.ip = (((uint32_t)(*v)[4] << 24) | ((uint32_t)(*v)[5] << 16) | ((uint32_t)(*v)[6] << 8) | (*v)[7]) ^ magic;
                return a;
            }
            std::string text(uint16_t kind) const {
                const std::vector<uint8_t>* v = find(kind);
                return v == nullptr ? std::string() : std::string(v->begin(), v->end());
            }
        };

        std::optional<Parsed> parse(const uint8_t* data, size_t size) {
            if (size < 20 || (data[0] & 0xC0) != 0) return std::nullopt;
            const size_t length = (size_t)(data[2] << 8 | data[3]);
            const uint32_t cookie = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
            if (cookie != magic || length != size - 20 || length % 4 != 0) return std::nullopt;
            Parsed p;
            p.type = (uint16_t)(data[0] << 8 | data[1]);
            std::memcpy(p.id.data(), data + 8, 12);
            for (size_t at = 20; at < size;) {
                if (size - at < 4) return std::nullopt;
                const uint16_t kind = (uint16_t)(data[at] << 8 | data[at + 1]);
                const size_t value = (size_t)(data[at + 2] << 8 | data[at + 3]);
                if (size - at - 4 < value) return std::nullopt;
                p.attributes.push_back({ kind, std::vector<uint8_t>(data + at + 4, data + at + 4 + value) });
                at += 4 + (value + 3) / 4 * 4;
            }
            return p;
        }

        // ERROR-CODE's words: "401 Unauthorized".
        std::string error_of(const Parsed& p) {
            const std::vector<uint8_t>* v = p.find(error_attribute);
            if (v == nullptr || v->size() < 4) return "an error";
            const int code = ((*v)[2] & 7) * 100 + (*v)[3];
            std::string reason(v->begin() + 4, v->end());
            for (char& c : reason) if (c < ' ' || c > '~') c = '?';
            return std::to_string(code) + (reason.empty() ? "" : " " + reason);
        }
        int code_of(const Parsed& p) {
            const std::vector<uint8_t>* v = p.find(error_attribute);
            return v == nullptr || v->size() < 4 ? 0 : ((*v)[2] & 7) * 100 + (*v)[3];
        }

        sockaddr_in to_sockaddr(const Address& a) {
            sockaddr_in s{};
            s.sin_family = AF_INET;
            s.sin_port = htons(a.port);
            s.sin_addr.s_addr = htonl(a.ip);
            return s;
        }

        struct Client {
            Socket s = no_socket;
            Address relay;
            std::mt19937_64 random{ std::random_device{}() };
            // What arrived that wasn't a request's answer: Data indications' payloads with their peer, and
            // packets straight from elsewhere than the relay.
            std::vector<std::pair<Address, std::string>> data;
            std::vector<std::pair<Address, std::string>> straight;
            // When the whole check gives up: no request is sent past it (an unresponsive relay).
            std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)();

            ~Client() { if (s != no_socket) close_socket(s); }

            void send(const std::vector<uint8_t>& bytes, const Address& to) {
                const sockaddr_in a = to_sockaddr(to);
                sendto(s, (const char*)bytes.data(), (int)bytes.size(), 0, (const sockaddr*)&a, sizeof(a));
            }

            // Takes whatever arrives within `seconds`; an answer to `id` (if given) ends it early.
            std::optional<Parsed> receive(double seconds, const Id* id) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
                while (true) {
                    const double left = std::chrono::duration<double>(until - std::chrono::steady_clock::now()).count();
                    if (left <= 0) return std::nullopt;
                    fd_set read;
                    FD_ZERO(&read);
                    FD_SET(s, &read);
                    timeval wait;
                    wait.tv_sec = (decltype(wait.tv_sec))left;
                    wait.tv_usec = (decltype(wait.tv_usec))((left - (double)wait.tv_sec) * 1e6);
                    if (select((int)s + 1, &read, nullptr, nullptr, &wait) <= 0) continue;
                    uint8_t buffer[1500];
                    sockaddr_in from{};
                    socklen_t from_size = sizeof(from);
                    const int got = (int)recvfrom(s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &from_size);
                    if (got <= 0 || from.sin_family != AF_INET) continue;
                    const Address sender{ ntohl(from.sin_addr.s_addr), ntohs(from.sin_port) };
                    if (!(sender == relay)) {
                        straight.push_back({ sender, std::string((const char*)buffer, (size_t)got) });
                        continue;
                    }
                    const std::optional<Parsed> p = parse(buffer, (size_t)got);
                    if (!p) continue;
                    if (p->type == data_indication) {
                        const std::optional<Address> peer = p->address(peer_attribute);
                        const std::vector<uint8_t>* payload = p->find(data_attribute);
                        if (peer && payload != nullptr) data.push_back({ *peer, std::string(payload->begin(), payload->end()) });
                        continue;
                    }
                    if (id != nullptr && p->id == *id) return p;
                }
            }

            // Sends `m` every half second until its answer comes, for up to 3 s.
            std::optional<Parsed> transact(const Message& m) {
                for (int tries = 0; tries < 6 && std::chrono::steady_clock::now() < deadline; tries++) {
                    send(m.bytes, relay);
                    if (std::optional<Parsed> answer = receive(0.5, &m.id)) return answer;
                }
                return std::nullopt;
            }
        };

        // The first of "host:port,host:port" resolved to an IPv4 address.
        std::optional<Address> resolve(const std::string& servers, std::string& error) {
            const std::string first = servers.substr(0, servers.find(','));
            const size_t colon = first.rfind(':');
            if (colon == std::string::npos || first.find(':') != colon) {
                error = "the relay's address isn't an IPv4 host and port";
                return std::nullopt;
            }
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* found = nullptr;
            if (getaddrinfo(first.substr(0, colon).c_str(), first.substr(colon + 1).c_str(), &hints, &found) != 0 || found == nullptr) {
                error = "couldn't look up the relay's address";
                return std::nullopt;
            }
            const sockaddr_in* a = (const sockaddr_in*)found->ai_addr;
            const Address address{ ntohl(a->sin_addr.s_addr), ntohs(a->sin_port) };
            freeaddrinfo(found);
            return address;
        }

        // This machine's own address toward `to` (no packet is sent: a UDP socket's connect only picks it).
        uint32_t own_address_toward(const Address& to) {
            const Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == no_socket) return 0;
            const sockaddr_in a = to_sockaddr(to);
            sockaddr_in own{};
            socklen_t size = sizeof(own);
            uint32_t ip = 0;
            if (connect(s, (const sockaddr*)&a, sizeof(a)) == 0 && getsockname(s, (sockaddr*)&own, &size) == 0) {
                ip = ntohl(own.sin_addr.s_addr);
            }
            close_socket(s);
            return ip;
        }
    }

    std::array<uint8_t, 16> md5(std::string_view message) {
        static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const int r[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
        std::string data(message);
        const uint64_t bits = (uint64_t)message.size() * 8;
        data += (char)0x80;
        while (data.size() % 64 != 56) data += (char)0;
        for (int i = 0; i < 8; i++) data += (char)(bits >> (8 * i));
        for (size_t block = 0; block < data.size(); block += 64) {
            uint32_t w[16];
            for (int i = 0; i < 16; i++) {
                const auto* p = (const uint8_t*)data.data() + block + 4 * i;
                w[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            for (int i = 0; i < 64; i++) {
                uint32_t f;
                int g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                const uint32_t t = d;
                d = c;
                c = b;
                const uint32_t x = a + f + k[i] + w[g];
                b = b + (x << r[i] | x >> (32 - r[i]));
                a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        }
        std::array<uint8_t, 16> out;
        for (int i = 0; i < 16; i++) out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
        return out;
    }

    namespace {
        // One socket's allocation on the relay: its public address, its relayed one, and the credentials'
        // realm, nonce and key, for its later requests.
        struct Allocation {
            Client client;
            std::optional<Address> mapped;
            std::optional<Address> relayed;
            std::string refused;
            std::string user, realm, nonce;
            std::array<uint8_t, 16> key{};

            // A request signed with the credentials, `add` adding its own attributes first.
            template <typename Add>
            std::optional<Parsed> signed_request(uint16_t type, Add&& add) {
                Message m(type, client.random);
                add(m);
                m.text(username_attribute, user);
                m.text(realm_attribute, realm);
                m.text(nonce_attribute, nonce);
                m.sign(key);
                return client.transact(m);
            }
            bool permit(const Address& peer) {
                const std::optional<Parsed> answer = signed_request(create_permission, [&](Message& m) { m.address(peer_attribute, peer); });
                return answer && answer->type == create_permission_ok;
            }
            std::vector<uint8_t> indication(const Address& to, const std::string& payload) {
                Message m(send_indication, client.random);
                m.address(peer_attribute, to);
                m.attribute(data_attribute, payload.data(), payload.size());
                return m.bytes;
            }
            // Frees the allocation: sent once, its answer not waited for (it expires anyway).
            void release() {
                Message m(refresh, client.random);
                const uint8_t none[4] = {};
                m.attribute(lifetime_attribute, none, sizeof(none));
                m.text(username_attribute, user);
                m.text(realm_attribute, realm);
                m.text(nonce_attribute, nonce);
                m.sign(key);
                client.send(m.bytes, client.relay);
            }
        };

        // Opens its socket and asks for its public address (false: no answer; `error` if no socket), then
        // a relayed one: once without credentials, for the relay's realm and nonce, then with them.
        bool allocate_on(Allocation& a, const Address& relay, const std::string& user, const std::string& password,
            std::chrono::steady_clock::time_point deadline, std::string& error) {
            a.client.relay = relay;
            a.client.deadline = deadline;
            a.client.s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in any{};
            any.sin_family = AF_INET;
            if (a.client.s == no_socket || bind(a.client.s, (const sockaddr*)&any, sizeof(any)) != 0) {
                error = "couldn't open a UDP socket";
                return false;
            }
            const std::optional<Parsed> bound = a.client.transact(Message(binding, a.client.random));
            a.mapped = bound && bound->type == binding_ok ? bound->address(mapped_attribute) : std::nullopt;
            if (!a.mapped) {
                return false;
            }
            const uint8_t transport[4] = { 17, 0, 0, 0 };   // UDP
            Message ask(allocate, a.client.random);
            ask.attribute(transport_attribute, transport, sizeof(transport));
            std::optional<Parsed> answer = a.client.transact(ask);
            a.user = user;
            for (int tries = 0; tries < 2 && answer && answer->type == allocate_error && (code_of(*answer) == 401 || code_of(*answer) == 438); tries++) {
                a.realm = answer->text(realm_attribute);
                a.nonce = answer->text(nonce_attribute);
                a.key = md5(user + ":" + a.realm + ":" + password);
                answer = a.signed_request(allocate, [&](Message& m) { m.attribute(transport_attribute, transport, sizeof(transport)); });
            }
            if (answer && answer->type == allocate_ok) {
                a.relayed = answer->address(relayed_attribute);
            }
            if (!a.relayed) {
                a.refused = !answer ? "no answer" : answer->type == allocate_error ? error_of(*answer) : "an answer it didn't expect";
            }
            return true;
        }
    }

    RelayCheck check_relay(const std::string& servers, const std::string& user, const std::string& password) {
        RelayCheck out;
        if (!start_sockets()) {
            out.error = "couldn't start the networking";
            return out;
        }
        const std::optional<Address> relay = resolve(servers, out.error);
        if (!relay) {
            return out;
        }
        // Two sockets, each with its relayed address, as two players of a relay-only match have.
        Allocation a, b;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        if (!allocate_on(a, *relay, user, password, deadline, out.error)) {
            return out;
        }
        out.reached = true;
        out.behind_nat = own_address_toward(*relay) != a.mapped->ip;
        if (!a.relayed) {
            out.refused = a.refused;
            return out;
        }
        if (!allocate_on(b, *relay, user, password, deadline, out.error) || !b.relayed) {
            out.refused = b.refused.empty() ? "no second allocation" : b.refused;
            a.release();
            return out;
        }
        out.allocated = true;

        // Permissions: each for the other's relayed address (the relay's IP); b for a's public address too
        // (apart: the relay may deny it, a private one on a relay in the same network).
        const bool permitted = a.permit(*b.relayed) && b.permit(*a.relayed);
        b.permit(*a.mapped);
        if (permitted) {
            // From a through the relay to b's relayed address (a sends only to the relay's own port).
            const std::string relayed_payload = "conker-relayed", mapping_payload = "conker-mapping", filter_payload = "conker-filter";
            for (int i = 0; i < 4; i++) {
                a.client.send(a.indication(*b.relayed, relayed_payload), *relay);
                b.client.receive(0.3, nullptr);
            }
            // From b's relayed address to a's public one (a's filtering), before a has sent anything there: a
            // reply to nothing. After the relay's check, as a relay that can't send there (its own address,
            // behind a firewall of its own) may end b's allocation for it.
            for (int i = 0; i < 4; i++) {
                b.client.send(b.indication(*a.mapped, filter_payload), *relay);
                a.client.receive(0.3, nullptr);
            }
            // Then from a's socket straight to b's relayed address (b's relay sees a's mapping for that destination).
            for (int i = 0; i < 4; i++) {
                a.client.send(std::vector<uint8_t>(mapping_payload.begin(), mapping_payload.end()), *b.relayed);
                b.client.receive(0.3, nullptr);
            }
            for (const auto& [peer, payload] : b.client.data) {
                if (payload == relayed_payload && peer == *a.relayed) out.relays = true;
                if (payload == mapping_payload) {
                    out.mapping = peer == *a.mapped ? RelayCheck::Mapping::same : RelayCheck::Mapping::differs;
                }
            }
            for (const auto& [from, payload] : a.client.straight) {
                if (payload == filter_payload && from == *b.relayed) out.lets_in = true;
            }
        }
        a.release();
        b.release();
        return out;
    }
}
