// Tests of signalling's packets (signal_packets.cpp): each kind round-trips, its size and layout are as
// documented, and every out-of-range field, size, version and kind is refused. Exits 1 on a failure.

#include <cstdio>
#include <vector>

#include "signal_packets.hpp"
#include "join_code.hpp"
#include "packets.hpp"

using namespace conker::netplay::signal;
using conker::netplay::max_room;
using conker::netplay::max_players;
using conker::netplay::max_rollback;

namespace {
    int failures = 0;
    // The version the helpers below speak.
    uint16_t testing_as = version;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("signal_packets_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::vector<uint8_t> bytes_of(const Packet& packet) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(packet, buffer, testing_as);
        return std::vector<uint8_t>(buffer, buffer + size);
    }

    bool decodes(const std::vector<uint8_t>& data) {
        return decode(data, testing_as).has_value();
    }

    bool round_trips(const Packet& packet, size_t size) {
        const std::vector<uint8_t> data = bytes_of(packet);
        const std::optional<Packet> back = decode(data, testing_as);
        return data.size() == size && back && *back == packet;
    }
}

int main() {
    // Each kind, at its documented size.
    CHECK(round_trips(Register{ 0x0123456789ABCDEFull }, 13));
    CHECK(round_trips(Signal{ 7, { 1, 2, 3 } }, 18));
    CHECK(round_trips(Signal{ 7, std::vector<uint8_t>(max_signal, 0xAB) }, 15 + max_signal));
    CHECK(round_trips(Unreachable{ 9 }, 13));
    CHECK(round_trips(Registered{ 9 }, 13));
    CHECK(round_trips(Refused{ Refusal::taken }, 6));
    CHECK(round_trips(Refused{ Refusal::last }, 6));
    CHECK(round_trips(CreateRoom{ 0xFEDCBA9876543210ull }, 13));
    CHECK(round_trips(RoomCreated{ max_room }, 13));
    CHECK(round_trips(FindRoom{ 1 }, 13));
    CHECK(round_trips(RoomFound{ 5, 6, 7 }, 29));
    CHECK(round_trips(NoRoom{ 5 }, 13));
    CHECK(round_trips(Turn{ "203.0.113.7:3478", "1700000000:0123456789abcdef", "pass/word+=" }, 8 + 16 + 27 + 11));
    CHECK(round_trips(Turn{ std::string(max_text, 'a'), "u", std::string(max_text, '~') }, 8 + 2 * max_text + 1));

    // Listing as version 5 has it (6's below).
    testing_as = 5;
    const ListedRoom room{ 77, 4, 2, false, true, 8 };
    CHECK(round_trips(Publish{ 0xABCDull, 0x1234ull, 0, Platform::windows, room }, 42));
    CHECK(round_trips(Publish{ 1, 1, 0, Platform::last, { 0, 2, 2, true, false, max_rollback, ~0ull } }, 42));
    CHECK(round_trips(ListRooms{ 5, 6, 0, Platform::gnu_linux }, 22));
    CHECK(round_trips(Rooms{}, 8));
    CHECK(round_trips(Rooms{ 65535, 0, {} }, 8));
    CHECK(round_trips(Rooms{ 3, 0, { room, { max_room, (uint8_t)max_players, 1, true, true, 0, 0x55 } } }, 8 + 2 * 20));
    CHECK(round_trips(Rooms{ 0, 0, std::vector<ListedRoom>(max_listed, room) }, 8 + 20 * max_listed));
    testing_as = version;

    // The layout: "CS", the version, the kind, little-endian fields.
    const std::vector<uint8_t> reg = bytes_of(Register{ 0x0102030405060708ull });
    CHECK(reg == (std::vector<uint8_t>{ 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8), 1, 8, 7, 6, 5, 4, 3, 2, 1 }));
    const std::vector<uint8_t> sig = bytes_of(Signal{ 1, { 0xEE } });
    CHECK(sig.size() == 16 && sig[4] == 2 && sig[13] == 1 && sig[14] == 0 && sig[15] == 0xEE);

    // Not encoded: id 0, an empty or oversized signal, an unknown reason.
    CHECK(bytes_of(Register{ 0 }).empty());
    CHECK(bytes_of(Signal{ 0, { 1 } }).empty());
    CHECK(bytes_of(Signal{ 1, {} }).empty());
    CHECK(bytes_of(Signal{ 1, std::vector<uint8_t>(max_signal + 1) }).empty());
    CHECK(bytes_of(Unreachable{ 0 }).empty());
    CHECK(bytes_of(Registered{ 0 }).empty());
    CHECK(bytes_of(Refused{ (Refusal)0 }).empty());
    CHECK(bytes_of(Refused{ (Refusal)((int)Refusal::last + 1) }).empty());
    CHECK(bytes_of(CreateRoom{ 0 }).empty());
    CHECK(bytes_of(RoomCreated{ 0 }).empty());
    CHECK(bytes_of(RoomCreated{ max_room + 1 }).empty());
    CHECK(bytes_of(FindRoom{ max_room + 1 }).empty());
    CHECK(bytes_of(NoRoom{ 0 }).empty());
    CHECK(bytes_of(RoomFound{ 1, 0, 1 }).empty());
    CHECK(bytes_of(RoomFound{ 1, 1, 0 }).empty());
    CHECK(bytes_of(RoomFound{ 0, 1, 1 }).empty());
    CHECK(bytes_of(Turn{ "", "u", "p" }).empty());
    CHECK(bytes_of(Turn{ "s", "u", std::string(max_text + 1, 'p') }).empty());
    CHECK(bytes_of(Turn{ "s", "a user", "p" }).empty());
    CHECK(bytes_of(Turn{ "s", "u", "p\x7F" }).empty());

    // Refused: another version, magic or kind; a size other than the kind's.
    std::vector<uint8_t> bad = reg;
    bad[2]++;
    CHECK(!decodes(bad) && other_version(bad));
    CHECK(!other_version(reg));
    bad = reg; bad[0] = 'X'; CHECK(!decodes(bad));
    bad = reg; bad[4] = 0; CHECK(!decodes(bad));
    bad = reg; bad[4] = 15; CHECK(!decodes(bad));
    bad = reg; bad.push_back(0); CHECK(!decodes(bad));
    bad = reg; bad.pop_back(); CHECK(!decodes(bad));
    CHECK(!decodes({}));
    CHECK(!decodes({ 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8) }));
    // Id 0.
    bad = reg; for (int i = 5; i < 13; i++) bad[i] = 0; CHECK(!decodes(bad));
    bad = bytes_of(Unreachable{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    bad = bytes_of(Registered{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    // A signal whose size isn't what follows, is 0, or past max_signal.
    bad = sig; bad.push_back(0); CHECK(!decodes(bad));
    bad = sig; bad[13] = 2; CHECK(!decodes(bad));
    bad = sig; bad[13] = 0; bad.pop_back(); CHECK(!decodes(bad));
    bad = bytes_of(Signal{ 1, std::vector<uint8_t>(max_signal, 1) });
    bad[13] = (uint8_t)(max_signal + 1); bad[14] = (uint8_t)((max_signal + 1) >> 8); bad.push_back(1); CHECK(!decodes(bad));
    bad = sig; bad[5] = 0; CHECK(!decodes(bad));
    // A reason out of range.
    bad = bytes_of(Refused{ Refusal::busy }); bad[5] = 0; CHECK(!decodes(bad));
    bad[5] = (uint8_t)Refusal::last + 1; CHECK(!decodes(bad));

    // Rooms out of range.
    bad = bytes_of(FindRoom{ 1 }); bad[10] = 1; CHECK(!decodes(bad));   // past max_room
    bad = bytes_of(RoomCreated{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    const std::vector<uint8_t> found = bytes_of(RoomFound{ 1, 2, 3 });
    bad = found; bad[13] = 0; CHECK(!decodes(bad));                      // host 0
    bad = found; bad[21] = 0; CHECK(!decodes(bad));                      // key 0
    bad = found; bad.pop_back(); CHECK(!decodes(bad));
    bad = bytes_of(CreateRoom{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    // Turn: a size past what follows, 0 or past max_text, a character out of range, anything after.
    const std::vector<uint8_t> turn = bytes_of(Turn{ "s:1", "u", "p" });
    CHECK(turn.size() == 13 && turn[4] == 11 && turn[5] == 3 && turn[9] == 1 && turn[11] == 1);
    bad = turn; bad.push_back('x'); CHECK(!decodes(bad));
    bad = turn; bad.pop_back(); CHECK(!decodes(bad));
    bad = turn; bad[11] = 2; CHECK(!decodes(bad));
    bad = turn; bad[9] = 0; CHECK(!decodes(bad));
    bad = turn; bad[6] = ' '; CHECK(!decodes(bad));
    bad = turn; bad[6] = 0x80; CHECK(!decodes(bad));
    bad = turn; bad.resize(6); CHECK(!decodes(bad));
    bad = { 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8), 11 }; CHECK(!decodes(bad));
    bad = bytes_of(Turn{ std::string(max_text, 'a'), "u", "p" }); bad[5] = (uint8_t)(max_text + 1); bad.insert(bad.begin() + 6, 'a');
    CHECK(!decodes(bad));

    // Listing (version 5): the layout, and every field out of range.
    testing_as = 5;
    const std::vector<uint8_t> pub = bytes_of(Publish{ 0x0102030405060708ull, 0x1112131415161718ull, 0, Platform::macos, room });
    CHECK(pub.size() == 42 && pub[4] == 12 && pub[5] == 8 && pub[13] == 0x18 && pub[21] == 3 && pub[22] == 77 && pub[30] == 4 &&
        pub[31] == 2 && pub[32] == 2 && pub[33] == 8);
    CHECK(bytes_of(Publish{ 0, 1, 0, Platform::windows, room }).empty());
    CHECK(bytes_of(Publish{ 1, 0, 0, Platform::windows, room }).empty());
    CHECK(bytes_of(Publish{ 1, 1, 0, (Platform)0, room }).empty());
    CHECK(bytes_of(Publish{ 1, 1, 0, (Platform)((int)Platform::last + 1), room }).empty());
    CHECK(bytes_of(Publish{ 1, 1, 0, Platform::windows, { 0, 1, 1, false, false, 0 } }).empty());          // 1 player
    CHECK(bytes_of(Publish{ 1, 1, 0, Platform::windows, { 0, max_players + 1, 1, false, false, 0 } }).empty());
    CHECK(bytes_of(Publish{ 1, 1, 0, Platform::windows, { 0, 2, 0, false, false, 0 } }).empty());          // nobody in
    CHECK(bytes_of(Publish{ 1, 1, 0, Platform::windows, { 0, 2, 3, false, false, 0 } }).empty());          // more in than play
    CHECK(bytes_of(Publish{ 1, 1, 0, Platform::windows, { 0, 2, 1, false, false, max_rollback + 1 } }).empty());
    CHECK(bytes_of(ListRooms{ 0, 1, 0, Platform::windows }).empty());
    CHECK(bytes_of(ListRooms{ 1, 0, 0, Platform::windows }).empty());
    CHECK(bytes_of(ListRooms{ 1, 1, 0, (Platform)4 }).empty());
    CHECK(bytes_of(Rooms{ 0, 0, { { 0, 2, 1, false, false, 0 } } }).empty());                             // room 0
    CHECK(bytes_of(Rooms{ 0, 0, { { max_room + 1, 2, 1, false, false, 0 } } }).empty());
    CHECK(bytes_of(Rooms{ 0, 0, std::vector<ListedRoom>(max_listed + 1, room) }).empty());
    bad = pub; for (int i = 5; i < 13; i++) bad[i] = 0; CHECK(!decodes(bad));                     // fit 0
    bad = pub; for (int i = 13; i < 21; i++) bad[i] = 0; CHECK(!decodes(bad));                    // game 0
    bad = pub; bad[21] = 0; CHECK(!decodes(bad));
    bad = pub; bad[21] = 4; CHECK(!decodes(bad));
    bad = pub; bad[30] = 1; bad[31] = 1; CHECK(!decodes(bad));
    bad = pub; bad[30] = max_players + 1; CHECK(!decodes(bad));
    bad = pub; bad[31] = 0; CHECK(!decodes(bad));
    bad = pub; bad[31] = 5; CHECK(!decodes(bad));
    bad = pub; bad[32] = 4; CHECK(!decodes(bad));                                                  // an unknown flag
    bad = pub; bad[33] = max_rollback + 1; CHECK(!decodes(bad));
    bad = pub; bad.push_back(0); CHECK(!decodes(bad));
    bad = pub; bad.pop_back(); CHECK(!decodes(bad));
    // Publish's room number is anything: the server fills it in.
    bad = pub; for (int i = 22; i < 30; i++) bad[i] = 0xFF; CHECK(decodes(bad));
    const std::vector<uint8_t> ask = bytes_of(ListRooms{ 9, 10, 0, Platform::windows });
    CHECK(ask.size() == 22 && ask[5] == 9 && ask[13] == 10 && ask[21] == 2);
    bad = ask; bad[21] = 0; CHECK(!decodes(bad));
    bad = ask; bad[5] = 0; CHECK(!decodes(bad));
    bad = ask; bad[13] = 0; CHECK(!decodes(bad));
    bad = ask; bad.push_back(0); CHECK(!decodes(bad));
    ListedRoom hosted = room;
    hosted.host = 0x1112131415161718ull;
    const std::vector<uint8_t> list = bytes_of(Rooms{ 0x0102, 0, { room, hosted } });
    CHECK(list.size() == 48 && list[4] == 14 && list[5] == 2 && list[6] == 1 && list[7] == 2 && list[8] == 77 && list[28] == 77 &&
        list[40] == 0x18 && list[47] == 0x11);                                                     // the second's host
    bad = list; bad[7] = 3; CHECK(!decodes(bad));                                                  // a count past what follows
    bad = list; bad[7] = 1; CHECK(!decodes(bad));
    bad = list; bad.pop_back(); CHECK(!decodes(bad));
    bad = list; for (int i = 28; i < 36; i++) bad[i] = 0; CHECK(!decodes(bad));                   // the second's room 0
    bad = list; bad[33] = 0x10; CHECK(!decodes(bad));                                              // past max_room (byte 5 of 8)
    bad = list; bad[38] = 4; CHECK(!decodes(bad));                                                 // an unknown flag (6's other_build)
    bad = { 'C', 'S', 5, 0, 14, 0 }; CHECK(!decodes(bad));
    bad = bytes_of(Rooms{ 0, 0, std::vector<ListedRoom>(max_listed, room) }); bad[7] = max_listed + 1;
    bad.insert(bad.end(), list.begin() + 8, list.begin() + 28); CHECK(!decodes(bad));
    testing_as = version;
    // Versions 3 and 4: rooms of 12 bytes, without the host (it's dropped going to them).
    for (uint16_t as : { (uint16_t)3, (uint16_t)4 }) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(Rooms{ 0x0102, 0, { room, hosted } }, buffer, as);
        CHECK(size == 8 + 2 * 12);
        const std::optional<Packet> back = decode({ buffer, size }, as);
        CHECK(back && std::get<Rooms>(*back).rooms[1].host == 0 && std::get<Rooms>(*back).rooms[1].room == 77);
        CHECK(encode(Publish{ 1, 1, 0, Platform::windows, hosted }, buffer, as) == 34);
    }

    // Version 6: a room's release (and other_build, in a list), the ROM's hash in Publish and ListRooms, Rooms' older.
    {
        testing_as = 6;
        ListedRoom r6 = room;
        r6.release = "v0.1.6-netplay.15";
        CHECK(round_trips(Publish{ 1, 2, 3, Platform::windows, r6 }, 30 + 21 + 17));
        const std::vector<uint8_t> p6 = bytes_of(Publish{ 1, 2, 3, Platform::windows, r6 });
        CHECK(p6[21] == 3 && p6[29] == 2 && p6[30] == 77 && p6[50] == 17 && p6[51] == 'v' && p6.back() == '5');
        CHECK(bytes_of(Publish{ 1, 2, 0, Platform::windows, r6 }).empty());                       // rom 0
        CHECK(bytes_of(Publish{ 1, 2, 3, Platform::windows, room }).empty());                     // no release
        ListedRoom r = r6;
        r.release = "a release"; CHECK(bytes_of(Publish{ 1, 2, 3, Platform::windows, r }).empty());
        r.release = std::string(max_release + 1, 'a'); CHECK(bytes_of(Publish{ 1, 2, 3, Platform::windows, r }).empty());
        r.release = std::string(max_release, '~'); CHECK(round_trips(Publish{ 1, 2, 3, Platform::windows, r }, 30 + 21 + max_release));
        r = r6; r.other_build = true; CHECK(bytes_of(Publish{ 1, 2, 3, Platform::windows, r }).empty());
        bad = p6; bad[40] |= 4; CHECK(!decodes(bad));                                            // other_build in Publish
        bad = p6; bad[50] = 18; CHECK(!decodes(bad));                                             // a size past what follows
        bad = p6; bad[50] = 0; CHECK(!decodes(bad));
        bad = p6; bad.push_back('x'); CHECK(!decodes(bad));
        bad = p6; bad.pop_back(); CHECK(!decodes(bad));
        bad = p6; bad[55] = ' '; CHECK(!decodes(bad));
        bad = p6; for (int i = 21; i < 29; i++) bad[i] = 0; CHECK(!decodes(bad));                // rom 0
        CHECK(round_trips(ListRooms{ 1, 2, 3, Platform::gnu_linux }, 30));
        CHECK(bytes_of(ListRooms{ 1, 2, 0, Platform::gnu_linux }).empty());
        bad = bytes_of(ListRooms{ 1, 2, 3, Platform::gnu_linux }); bad.pop_back(); CHECK(!decodes(bad));
        ListedRoom other = r6;
        other.room = 78;
        other.release = "v0.1.6-netplay.14";
        other.other_build = true;
        CHECK(round_trips(Rooms{ 4, 5, { r6, other } }, 10 + 2 * 38));
        const std::vector<uint8_t> l6 = bytes_of(Rooms{ 4, 5, { r6, other } });
        CHECK(l6[5] == 4 && l6[7] == 5 && l6[9] == 2 && l6[10] == 77 && l6[48] == 78 && l6[58] == 6);  // the second's flags: direct, other_build
        bad = l6; bad[58] = 8 | 2; CHECK(!decodes(bad));                                         // an unknown flag
        bad = l6; bad[9] = 3; CHECK(!decodes(bad));
        bad = l6; bad.push_back(0); CHECK(!decodes(bad));
        bad = l6; bad.pop_back(); CHECK(!decodes(bad));
        bad = l6; bad[68] = 0; CHECK(!decodes(bad));                                             // the second's release empty
        CHECK(bytes_of(Rooms{ 0, 0, { room } }).empty());                                         // no release
        r.release = std::string(max_release, 'a');
        CHECK(!bytes_of(Rooms{ 0, 0, std::vector<ListedRoom>(max_listed, r) }).empty());
        // Going to version 5: no release, no other_build, no older.
        uint8_t buffer[max_packet_size];
        const size_t size = encode(Rooms{ 4, 5, { r6, other } }, buffer, 5);
        const std::optional<Packet> back = decode({ buffer, size }, 5);
        CHECK(size == 8 + 2 * 20 && back && std::get<Rooms>(*back).older == 0 && std::get<Rooms>(*back).rooms[1].release.empty() &&
            !std::get<Rooms>(*back).rooms[1].other_build);
        CHECK(encode(Publish{ 1, 2, 3, Platform::windows, r6 }, buffer, 5) == 42);
        testing_as = version;
    }

    // Version 7: how many watch a room, and whether it takes spectators (flags' bit 3); dropped going to 6.
    {
        ListedRoom r7 = room;
        r7.release = "v0.1.6-netplay.22";
        r7.watching = 3;
        r7.spectators = true;
        CHECK(round_trips(Publish{ 1, 2, 3, Platform::windows, r7 }, 30 + 22 + 17));
        const std::vector<uint8_t> p7 = bytes_of(Publish{ 1, 2, 3, Platform::windows, r7 });
        CHECK((p7[40] & 8) != 0 && p7.back() == 3);
        CHECK(round_trips(Rooms{ 4, 5, { r7, r7 } }, 10 + 2 * 39));
        ListedRoom bad7 = r7;
        bad7.watching = conker::netplay::max_spectators + 1; CHECK(bytes_of(Publish{ 1, 2, 3, Platform::windows, bad7 }).empty());
        std::vector<uint8_t> bad = p7; bad.back() = conker::netplay::max_spectators + 1; CHECK(!decodes(bad));
        bad = p7; bad[40] |= 16; CHECK(!decodes(bad));                                             // an unknown flag
        bad = p7; bad.pop_back(); CHECK(!decodes(bad));
        uint8_t buffer[max_packet_size];
        const size_t size = encode(Rooms{ 0, 0, { r7 } }, buffer, 6);
        const std::optional<Packet> back = decode({ buffer, size }, 6);
        CHECK(size == 10 + 21 + 17 && back && std::get<Rooms>(*back).rooms[0].watching == 0 && !std::get<Rooms>(*back).rooms[0].spectators);
    }

    // Older versions: the same kinds as they were, but none that came later; a version not spoken is refused.
    {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(Register{ 5 }, buffer, 2);
        const std::vector<uint8_t> old(buffer, buffer + size);
        CHECK(size == 13 && old[2] == 2 && old[3] == 0);
        CHECK(decode(old, 2) && std::get<Register>(*decode(old, 2)).id == 5);
        CHECK(!decodes(old) && other_version(old) && !other_version(old, 2));
        CHECK(spoken_version(old) == 2 && spoken_version(reg) == version);
        CHECK(encode(Publish{ 1, 1, 0, Platform::windows, room }, buffer, 2) == 0);
        CHECK(encode(ListRooms{ 1, 1, 0, Platform::windows }, buffer, 2) == 0);
        CHECK(encode(Rooms{}, buffer, 2) == 0);
        CHECK(encode(Turn{ "s", "u", "p" }, buffer, 2) != 0);
        CHECK(encode(Register{ 5 }, buffer, oldest_version - 1) == 0);
        CHECK(encode(Register{ 5 }, buffer, version + 1) == 0);
        std::vector<uint8_t> old_publish = pub;
        old_publish[2] = 2;
        CHECK(!decode(old_publish, 2));
        std::vector<uint8_t> ancient = old;
        ancient[2] = oldest_version - 1;
        CHECK(spoken_version(ancient) == 0 && !decode(ancient, oldest_version - 1));
        ancient[2] = version + 1;
        CHECK(spoken_version(ancient) == 0);
    }

    // A desync summary (version 4): its layout, every field out of range, and none in version 3.
    {
        DesyncReport report{ 0xA1B2C3D4u, 0x0102030405060708ull, Platform::windows, 3, 1, 0, 300, 8, 2, {} };
        report.regions[8] = 1;   // region 64
        CHECK(round_trips(report, 43));
        const std::vector<uint8_t> d = bytes_of(report);
        CHECK(d[4] == 15 && d[5] == 0xD4 && d[9] == 8 && d[17] == 2 && d[18] == 3 && d[19] == 1 && d[20] == 0 && d[21] == 44 &&
            d[22] == 1 && d[25] == 8 && d[26] == 2 && d[27 + 8] == 1);
        uint8_t buffer[max_packet_size];
        CHECK(encode(report, buffer, 3) == 0);
        bad = d; bad[2] = 3; CHECK(!decode(bad, 3));
        DesyncReport r = report; r.match = 0; CHECK(bytes_of(r).empty());
        r = report; r.build = 0; CHECK(bytes_of(r).empty());
        r = report; r.platform = (Platform)0; CHECK(bytes_of(r).empty());
        r = report; r.players = 1; r.port = 0; r.other = 0; CHECK(bytes_of(r).empty());
        r = report; r.players = max_players + 1; CHECK(bytes_of(r).empty());
        r = report; r.port = 3; CHECK(bytes_of(r).empty());                  // past players
        r = report; r.other = 1; CHECK(bytes_of(r).empty());                 // itself
        r = report; r.rollback = max_rollback + 1; CHECK(bytes_of(r).empty());
        r = report; r.delay = conker::netplay::max_delay + 1; CHECK(bytes_of(r).empty());
        bad = d; bad[18] = 1; CHECK(!decodes(bad));
        bad = d; bad[19] = 3; CHECK(!decodes(bad));
        bad = d; bad[20] = 1; CHECK(!decodes(bad));
        bad = d; bad[25] = max_rollback + 1; CHECK(!decodes(bad));
        bad = d; bad[26] = conker::netplay::max_delay + 1; CHECK(!decodes(bad));
        bad = d; bad.push_back(0); CHECK(!decodes(bad));
        bad = d; bad.pop_back(); CHECK(!decodes(bad));
    }

    if (failures == 0) {
        std::printf("signal_packets_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
