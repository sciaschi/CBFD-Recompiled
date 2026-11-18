// Signalling's packets (signal_packets.hpp): encoding, and decoding with every field checked. This is the
// only code that parses what arrives from the signalling server or its peers; fuzz_packets.cpp fuzzes it.
//
// Each packet: "CS", u16 version, u8 kind, then (all little-endian):
//   1 Register     u64 id (not 0)                                                  13 bytes
//   2 Signal       u64 peer (not 0), u16 size (1 to max_signal), size bytes     15 + size bytes
//   3 Unreachable  u64 peer (not 0)                                                13 bytes
//   4 Refused      u8 reason (1 to Refusal::last)                                   6 bytes
//   5 Registered   u64 id (not 0)                                                  13 bytes
//   6 CreateRoom   u64 key (not 0)                                                 13 bytes
//   7 RoomCreated  u64 room (1 to max_room)                                        13 bytes
//   8 FindRoom     u64 room (1 to max_room)                                        13 bytes
//   9 RoomFound    u64 room (1 to max_room), u64 host, u64 key (not 0)             29 bytes
//  10 NoRoom       u64 room (1 to max_room)                                        13 bytes
//  11 Turn         3 times: u8 size (1 to max_text), size characters (! to ~)  8 + sizes bytes
//  12 Publish      u64 fit, u64 game (neither 0), [6: u64 rom (not 0)], u8 platform (1 to Platform::last), a room
//  13 ListRooms    u64 fit, u64 game (neither 0), [6: u64 rom (not 0)], u8 platform (1 to Platform::last)
//                  22 bytes (30 from version 6)
//  14 Rooms        u16 others, [6: u16 older], u8 count (0 to max_listed), count rooms
//  15 DesyncReport u32 match, u64 build (neither 0), u8 platform, u8 players (2 to max_players), u8 port,
//                  u8 other (each below players, not the same), u32 read, u8 rollback (0 to max_rollback),
//                  u8 delay (0 to max_delay), 16 bytes of regions                                    43 bytes
// A room (ListedRoom): u64 room (1 to max_room; Publish's 0 or any), u8 players (2 to max_players),
// u8 joined (1 to players), u8 flags (bit 0 playing, bit 1 direct; the others 0), u8 rollback (0 to
// max_rollback); from version 5, then u64 host (any: 0, not known); from version 6, then u8 size (1 to
// max_release) and the release's name (! to ~), and flags' bit 2 is other_build (Publish's 0); from version 7,
// then u8 watching (0 to max_spectators), and flags' bit 3 is spectators (taken). So 12 bytes, 20 from version 5,
// 21 + the name's from version 6, 22 + the name's from 7.
// Kinds 12 to 14 came with version 3, and 15 with 4 (6 changed 12 to 14 as above); version 2 has the others, as they are here. A packet of any other
// size, version or kind (or a kind its version hasn't), or with any field out of range, is refused.

#include "signal_packets.hpp"
#include "join_code.hpp"
#include "packets.hpp"

#include <algorithm>

namespace conker::netplay::signal {
    namespace {
        enum Kind : uint8_t { register_ = 1, signal, unreachable, refused, registered, create_room, room_created, find_room,
            room_found, no_room, turn, publish, list_rooms, rooms, desync_report };
        constexpr size_t header_size = 5;
        // The version a kind came with.
        uint16_t since(uint8_t kind) { return kind >= desync_report ? 4 : kind >= publish ? 3 : 2; }
        bool spoken(uint16_t as) { return as >= oldest_version && as <= version; }

        struct Writer {
            uint8_t* p;
            void u8(uint8_t v) { *p++ = v; }
            void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
            void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
            void u64(uint64_t v) { for (int i = 0; i < 8; i++) u8((uint8_t)(v >> (8 * i))); }
        };
        struct Reader {
            const uint8_t* p;
            uint8_t u8() { return *p++; }
            uint16_t u16() { const uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
            uint32_t u32() { const uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
            uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); p += 8; return v; }
        };

        bool valid_id(uint64_t id) { return id != 0; }
        bool valid_room(uint64_t room) { return room != 0 && room <= max_room; }
        bool valid_platform(Platform platform) { return (uint8_t)platform >= 1 && platform <= Platform::last; }
        // A desync report's fields.
        bool valid_report(const DesyncReport& d) {
            return d.match != 0 && d.build != 0 && valid_platform(d.platform) && d.players >= 2 && d.players <= max_players &&
                d.port < d.players && d.other < d.players && d.port != d.other && d.rollback <= max_rollback && d.delay <= max_delay;
        }
        bool valid_release(const std::string& text) {
            return !text.empty() && text.size() <= max_release && printable_word(text);
        }
        // A listed room's fields but its number (Publish's is the server's to fill in), as version `as` has
        // them (a release from 6 on; versions before it leave it out, and other_build).
        bool valid_listing(const ListedRoom& room, uint16_t as) {
            return room.players >= 2 && room.players <= max_players && room.joined >= 1 && room.joined <= room.players &&
                room.rollback <= max_rollback && (as < 6 || valid_release(room.release)) && room.watching <= max_spectators;
        }
        void write_room(Writer& w, const ListedRoom& room, uint16_t as) {
            w.u64(room.room);
            w.u8(room.players);
            w.u8(room.joined);
            w.u8((uint8_t)((room.playing ? 1 : 0) | (room.direct ? 2 : 0) | (as >= 6 && room.other_build ? 4 : 0) |
                (as >= 7 && room.spectators ? 8 : 0)));
            w.u8(room.rollback);
            if (as >= 5) w.u64(room.host);
            if (as >= 6) {
                w.u8((uint8_t)room.release.size());
                w.p = std::copy(room.release.begin(), room.release.end(), w.p);
            }
            if (as >= 7) w.u8(room.watching);
        }
        // A room ending by `end`, if its fields but the number are valid (the caller checks that).
        std::optional<ListedRoom> read_room(Reader& r, const uint8_t* end, uint16_t as) {
            if (end - r.p < (as >= 7 ? 22 : as >= 6 ? 21 : as >= 5 ? 20 : 12)) return std::nullopt;
            ListedRoom room;
            room.room = r.u64();
            room.players = r.u8();
            room.joined = r.u8();
            const uint8_t flags = r.u8();
            room.rollback = r.u8();
            room.playing = (flags & 1) != 0;
            room.direct = (flags & 2) != 0;
            room.other_build = (flags & 4) != 0;
            if (as >= 5) room.host = r.u64();
            if (as >= 6) {
                const uint8_t length = r.u8();
                if (length > end - r.p) return std::nullopt;
                room.release.assign((const char*)r.p, length);
                r.p += length;
            }
            if (as >= 7) {
                if (end - r.p < 1) return std::nullopt;
                room.watching = r.u8();
                room.spectators = (flags & 8) != 0;
            }
            if ((flags & ~(as >= 7 ? 15 : as >= 6 ? 7 : 3)) != 0 || !valid_listing(room, as)) return std::nullopt;
            return room;
        }
        bool valid_text(const std::string& text) {
            return !text.empty() && text.size() <= max_text && printable_word(text);
        }
    }

    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out, uint16_t as) {
        // A kind's number is its index in Packet, plus 1: kinds newer than `as` aren't in it.
        if (!spoken(as) || as < since((uint8_t)(packet.index() + 1))) {
            return 0;
        }
        Writer w{ out.data() };
        w.u8('C');
        w.u8('S');
        w.u16(as);
        // A kind of one u64, checked.
        const auto one = [&w](Kind kind, uint64_t value, bool valid) {
            if (!valid) return false;
            w.u8(kind);
            w.u64(value);
            return true;
        };
        bool ok = true;
        if (const Register* p = std::get_if<Register>(&packet)) ok = one(register_, p->id, valid_id(p->id));
        else if (const Unreachable* p = std::get_if<Unreachable>(&packet)) ok = one(unreachable, p->peer, valid_id(p->peer));
        else if (const Registered* p = std::get_if<Registered>(&packet)) ok = one(registered, p->id, valid_id(p->id));
        else if (const CreateRoom* p = std::get_if<CreateRoom>(&packet)) ok = one(create_room, p->key, valid_id(p->key));
        else if (const RoomCreated* p = std::get_if<RoomCreated>(&packet)) ok = one(room_created, p->room, valid_room(p->room));
        else if (const FindRoom* p = std::get_if<FindRoom>(&packet)) ok = one(find_room, p->room, valid_room(p->room));
        else if (const NoRoom* p = std::get_if<NoRoom>(&packet)) ok = one(no_room, p->room, valid_room(p->room));
        else if (const Signal* p = std::get_if<Signal>(&packet)) {
            if (p->peer == 0 || p->data.empty() || p->data.size() > max_signal) return 0;
            w.u8(signal);
            w.u64(p->peer);
            w.u16((uint16_t)p->data.size());
            w.p = std::copy(p->data.begin(), p->data.end(), w.p);
        }
        else if (const RoomFound* p = std::get_if<RoomFound>(&packet)) {
            if (!valid_room(p->room) || p->host == 0 || p->key == 0) return 0;
            w.u8(room_found);
            w.u64(p->room);
            w.u64(p->host);
            w.u64(p->key);
        }
        else if (const Turn* p = std::get_if<Turn>(&packet)) {
            if (!valid_text(p->servers) || !valid_text(p->user) || !valid_text(p->password)) return 0;
            w.u8(turn);
            for (const std::string* text : { &p->servers, &p->user, &p->password }) {
                w.u8((uint8_t)text->size());
                w.p = std::copy(text->begin(), text->end(), w.p);
            }
        }
        else if (const Publish* p = std::get_if<Publish>(&packet)) {
            if (p->fit == 0 || p->game == 0 || (as >= 6 && p->rom == 0) || !valid_platform(p->platform) ||
                !valid_listing(p->room, as) || p->room.other_build) return 0;
            w.u8(publish);
            w.u64(p->fit);
            w.u64(p->game);
            if (as >= 6) w.u64(p->rom);
            w.u8((uint8_t)p->platform);
            write_room(w, p->room, as);
        }
        else if (const DesyncReport* p = std::get_if<DesyncReport>(&packet)) {
            if (!valid_report(*p)) return 0;
            w.u8(desync_report);
            w.u32(p->match);
            w.u64(p->build);
            w.u8((uint8_t)p->platform);
            w.u8(p->players);
            w.u8(p->port);
            w.u8(p->other);
            w.u32(p->read);
            w.u8(p->rollback);
            w.u8(p->delay);
            for (uint8_t b : p->regions) w.u8(b);
        }
        else if (const ListRooms* p = std::get_if<ListRooms>(&packet)) {
            if (p->fit == 0 || p->game == 0 || (as >= 6 && p->rom == 0) || !valid_platform(p->platform)) return 0;
            w.u8(list_rooms);
            w.u64(p->fit);
            w.u64(p->game);
            if (as >= 6) w.u64(p->rom);
            w.u8((uint8_t)p->platform);
        }
        else if (const Rooms* p = std::get_if<Rooms>(&packet)) {
            if (p->rooms.size() > max_listed) return 0;
            for (const ListedRoom& room : p->rooms) {
                if (!valid_room(room.room) || !valid_listing(room, as)) return 0;
            }
            w.u8(rooms);
            w.u16(p->others);
            if (as >= 6) w.u16(p->older);
            w.u8((uint8_t)p->rooms.size());
            for (const ListedRoom& room : p->rooms) {
                write_room(w, room, as);
            }
        }
        else {
            const Refused& r = std::get<Refused>(packet);
            if ((uint8_t)r.reason < 1 || r.reason > Refusal::last) return 0;
            w.u8(refused);
            w.u8((uint8_t)r.reason);
        }
        return ok ? (size_t)(w.p - out.data()) : 0;
    }

    std::optional<Packet> decode(std::span<const uint8_t> data, uint16_t as) {
        if (!spoken(as) || data.size() < header_size || data[0] != 'C' || data[1] != 'S' ||
            (uint16_t)(data[2] | (data[3] << 8)) != as || as < since(data[4])) {
            return std::nullopt;
        }
        Reader r{ data.data() + header_size };
        const size_t size = data.size() - header_size;
        // A kind of one u64: the value, if it's the only field and valid.
        const auto one = [&r, size](bool (*valid)(uint64_t)) -> std::optional<uint64_t> {
            if (size != 8) return std::nullopt;
            const uint64_t value = r.u64();
            if (!valid(value)) return std::nullopt;
            return value;
        };
        std::optional<uint64_t> value;
        switch (data[4]) {
            case register_: if ((value = one(valid_id))) return Register{ *value }; return std::nullopt;
            case unreachable: if ((value = one(valid_id))) return Unreachable{ *value }; return std::nullopt;
            case registered: if ((value = one(valid_id))) return Registered{ *value }; return std::nullopt;
            case create_room: if ((value = one(valid_id))) return CreateRoom{ *value }; return std::nullopt;
            case room_created: if ((value = one(valid_room))) return RoomCreated{ *value }; return std::nullopt;
            case find_room: if ((value = one(valid_room))) return FindRoom{ *value }; return std::nullopt;
            case no_room: if ((value = one(valid_room))) return NoRoom{ *value }; return std::nullopt;
            case signal: {
                if (size < 10) return std::nullopt;
                Signal p;
                p.peer = r.u64();
                const uint16_t length = r.u16();
                if (p.peer == 0 || length == 0 || length > max_signal || size != 10u + length) return std::nullopt;
                p.data.assign(r.p, r.p + length);
                return p;
            }
            case refused: {
                if (size != 1) return std::nullopt;
                const uint8_t reason = r.u8();
                if (reason < 1 || reason > (uint8_t)Refusal::last) return std::nullopt;
                return Refused{ (Refusal)reason };
            }
            case room_found: {
                if (size != 24) return std::nullopt;
                RoomFound p;
                p.room = r.u64();
                p.host = r.u64();
                p.key = r.u64();
                if (!valid_room(p.room) || p.host == 0 || p.key == 0) return std::nullopt;
                return p;
            }
            case turn: {
                Turn p;
                const uint8_t* end = data.data() + data.size();
                for (std::string* text : { &p.servers, &p.user, &p.password }) {
                    if (r.p >= end) return std::nullopt;
                    const uint8_t length = r.u8();
                    if (length > end - r.p) return std::nullopt;
                    text->assign((const char*)r.p, length);
                    r.p += length;
                    if (!valid_text(*text)) return std::nullopt;
                }
                if (r.p != end) return std::nullopt;
                return p;
            }
            case publish: {
                const uint8_t* end = data.data() + data.size();
                if (size < (as >= 6 ? 25u : 17u)) return std::nullopt;
                Publish p;
                p.fit = r.u64();
                p.game = r.u64();
                if (as >= 6) p.rom = r.u64();
                p.platform = (Platform)r.u8();
                const std::optional<ListedRoom> room = read_room(r, end, as);
                if (p.fit == 0 || p.game == 0 || (as >= 6 && p.rom == 0) || !valid_platform(p.platform) || !room ||
                    room->other_build || r.p != end) return std::nullopt;
                p.room = *room;
                return p;
            }
            case list_rooms: {
                if (size != (as >= 6 ? 25u : 17u)) return std::nullopt;
                ListRooms p;
                p.fit = r.u64();
                p.game = r.u64();
                if (as >= 6) p.rom = r.u64();
                p.platform = (Platform)r.u8();
                if (p.fit == 0 || p.game == 0 || (as >= 6 && p.rom == 0) || !valid_platform(p.platform)) return std::nullopt;
                return p;
            }
            case desync_report: {
                if (size != 38) return std::nullopt;
                DesyncReport p;
                p.match = r.u32();
                p.build = r.u64();
                p.platform = (Platform)r.u8();
                p.players = r.u8();
                p.port = r.u8();
                p.other = r.u8();
                p.read = r.u32();
                p.rollback = r.u8();
                p.delay = r.u8();
                for (uint8_t& b : p.regions) b = r.u8();
                if (!valid_report(p)) return std::nullopt;
                return p;
            }
            case rooms: {
                const uint8_t* end = data.data() + data.size();
                if (size < (as >= 6 ? 5u : 3u)) return std::nullopt;
                Rooms p;
                p.others = r.u16();
                if (as >= 6) p.older = r.u16();
                const uint8_t count = r.u8();
                if (count > max_listed) return std::nullopt;
                for (int i = 0; i < count; i++) {
                    const std::optional<ListedRoom> room = read_room(r, end, as);
                    if (!room || !valid_room(room->room)) return std::nullopt;
                    p.rooms.push_back(*room);
                }
                if (r.p != end) return std::nullopt;
                return p;
            }
            default:
                return std::nullopt;
        }
    }

    bool other_version(std::span<const uint8_t> data, uint16_t as) {
        return data.size() >= 4 && data[0] == 'C' && data[1] == 'S' && (uint16_t)(data[2] | (data[3] << 8)) != as;
    }

    uint16_t spoken_version(std::span<const uint8_t> data) {
        if (data.size() < 4 || data[0] != 'C' || data[1] != 'S') return 0;
        const uint16_t as = (uint16_t)(data[2] | (data[3] << 8));
        return spoken(as) ? as : 0;
    }
}
