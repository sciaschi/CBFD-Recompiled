// Tests of the netplay packets (packets.cpp): each kind round-trips, its size and layout are as
// documented, and every out-of-range field, size, version and kind is refused. Exits 1 on a failure.

#include <cstdio>
#include <vector>

#include "packets.hpp"

using namespace conker::netplay;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("packets_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::vector<uint8_t> bytes_of(const Packet& packet) {
        std::vector<uint8_t> buffer(max_message_size);
        buffer.resize(encode(packet, buffer));
        return buffer;
    }

    bool decodes(const std::vector<uint8_t>& data) {
        return decode(data).has_value();
    }

    bool round_trips(const Packet& packet, size_t size) {
        const std::vector<uint8_t> data = bytes_of(packet);
        const std::optional<Packet> back = decode(data);
        return data.size() == size && back && *back == packet;
    }

    Inputs some_inputs() {
        Inputs inputs;
        inputs.port = 2;
        inputs.first_read = 1000;
        inputs.count = 3;
        inputs.pads[0] = { 0x8000, -128, 127, 0 };
        inputs.pads[1] = { 0xFF3F, 5, -5, 0 };
        inputs.pads[2] = { 0, 0, 0, err_no_controller };
        inputs.motions[0] = { -32768, 32767, 0x1234, -2, -128, swap_sticks_bit | 0x0305 };
        inputs.motions[2] = { 1, 0, 0, 0, 3, controls_mask };
        inputs.gaps = { -128, 3, 0, 127 };
        return inputs;
    }

    // A Start whose ports all have input delay `delay`.
    Start start_of(int players, int own_view, int delay = 0, int pairs = 0, int rollback = 0, int open = 0) {
        Start start;
        start.players = (uint8_t)players;
        start.own_view = (uint8_t)own_view;
        start.delays.fill((uint8_t)delay);
        start.pairs = (uint8_t)pairs;
        start.rollback = (uint8_t)rollback;
        start.open = (uint8_t)open;
        return start;
    }
}

int main() {
    // Each kind, at its documented size.
    CHECK(round_trips(Hello{ 0x0123456789ABCDEFull, 2, 3, 4, 5, 1, 9, 1 }, 55));
    CHECK(round_trips(Hello{ 1, 2, 3, 4, 5, 3, ~0ull, 3 }, 55));
    CHECK(round_trips(Welcome{ 0x02, 2 }, 12));
    CHECK(round_trips(Welcome{ 0x08, 4 }, 12));
    CHECK(round_trips(Welcome{ 0x0E, 4 }, 12));     // three players on one machine
    CHECK(round_trips(Welcome{ 0x0C, 4, 1, 1, 0x0C, max_rollback }, 12));    // coming back to a match under way
    CHECK(round_trips(Welcome{ 0x08, 4, 2, 0, 0x06, 0 }, 12));      // joining one under way
    CHECK(round_trips(Welcome{ 0, 3 }, 12));                        // a spectator
    CHECK(round_trips(Welcome{ 0, 4, 2, 1, 0x06, 4 }, 12));         // ...of a match under way
    CHECK(round_trips(Welcome{ 0x08, 4, 2, 0, 0, 0, 16 }, 12));     // its characters
    CHECK(round_trips(Welcome{ 0x08, 4, 1, 0, 0, 0, 12 }, 12));
    CHECK(round_trips(Hello{ 1, 2, 3, 4, 5, 0, 1, 1 }, 55));        // nobody playing on the joiner: a spectator
    CHECK(round_trips(Refuse{ Reason::rom }, 47));
    CHECK(round_trips(Refuse{ Reason::build, "v0.1.6-netplay.15" }, 47));
    CHECK(round_trips(Refuse{ Reason::build, std::string(max_release_name, '~') }, 47));
    CHECK(round_trips(some_inputs(), 161));
    CHECK(round_trips(Hash{ 4000000000u, 0xFEDCBA9876543210ull }, 285));
    Hash regions{ 30, 1 };
    for (int i = 0; i < memory_regions; i++) regions.regions[i] = (uint16_t)(0x0101 * i + 7);
    regions.spread = { no_spread, 0, 17, 254 };
    regions.waited = { 0, 255, 3, 1 };
    regions.waited_long = { 9, 0, 2, 255 };
    CHECK(round_trips(regions, 285));
    const std::vector<uint8_t> hash = bytes_of(regions);
    CHECK(hash[17] == 7 && hash[18] == 0 && hash[19] == 8 && hash[20] == 1 && hash[271] == 0x86 && hash[272] == 0x7F);
    CHECK(hash[273] == no_spread && hash[274] == 0 && hash[275] == 17 && hash[276] == 254);       // spreads
    CHECK(hash[277] == 0 && hash[278] == 255 && hash[279] == 3 && hash[280] == 1);                // reads waited
    CHECK(hash[281] == 9 && hash[282] == 0 && hash[283] == 2 && hash[284] == 255);                // ...long
    CHECK(round_trips(Bye{ Reason::desync }, 6));
    CHECK(round_trips(start_of(3, 0), 15));
    CHECK(round_trips(start_of(4, 1, max_delay), 15));
    CHECK(round_trips(start_of(4, 1, 2, 0x0C), 15));
    CHECK(round_trips(start_of(2, 0, 1, 0, max_rollback), 15));
    CHECK(round_trips(start_of(4, 1, 2, 0x06, 0, 0x08), 15));       // an open seat
    Start delays = start_of(4, 0);
    delays.delays = { 0, 3, max_delay, 1 };                        // each port's own
    CHECK(round_trips(delays, 15));
    CHECK(round_trips(Gone{ 3, 4000000000u }, 11));
    CHECK(round_trips(Gone{ 1, 5, 1 }, 11));
    const std::vector<uint8_t> gone = bytes_of(Gone{ 2, 0x01020304, 1 });
    CHECK(gone[4] == 8 && gone[5] == 2 && gone[6] == 4 && gone[9] == 1 && gone[10] == 1);
    CHECK(round_trips(Delay{ 0x01, max_delay, 4000000000u }, 11));
    CHECK(round_trips(Delay{ 0x0E, 0, 30 }, 11));                    // a machine of three players
    const std::vector<uint8_t> delay = bytes_of(Delay{ 0x06, 5, 0x01020304 });
    CHECK(delay[4] == 9 && delay[5] == 0x06 && delay[6] == 5 && delay[7] == 4 && delay[10] == 1);

    // The layout: header, then little-endian fields.
    const std::vector<uint8_t> hello = bytes_of(Hello{ 0x0807060504030201ull, 0, 0, 0x1112131415161718ull, 0x2122232425262728ull, 2,
        0x3132333435363738ull, 2 });
    CHECK(hello[0] == 'C' && hello[1] == 'N' && hello[2] == protocol_version && hello[3] == 0 && hello[4] == 1);
    CHECK(hello[5] == 1 && hello[12] == 8 && hello[29] == 0x18 && hello[36] == 0x11 && hello[37] == 0x28 && hello[44] == 0x21 && hello[45] == 2);
    CHECK(hello[46] == 0x38 && hello[53] == 0x31 && hello[54] == 2);
    const std::vector<uint8_t> inputs = bytes_of(some_inputs());
    CHECK(inputs[4] == 4 && inputs[5] == 2);                                    // kind, port
    CHECK(inputs[6] == 0xE8 && inputs[7] == 0x03 && inputs[8] == 0 && inputs[9] == 0);  // first read 1000
    CHECK(inputs[10] == 3);                                                     // count
    CHECK(inputs[11] == 0x00 && inputs[12] == 0x80 && inputs[13] == 0x80 && inputs[14] == 0x7F);  // pad 0
    CHECK(inputs[51] == 0x00 && inputs[52] == 0x80 && inputs[53] == 0xFF && inputs[54] == 0x7F &&  // motion 0
          inputs[55] == 0x34 && inputs[56] == 0x12 && inputs[57] == 0xFE && inputs[58] == 0xFF && inputs[59] == 0x80 &&
          inputs[60] == 0x05 && inputs[61] == 0x03 && inputs[62] == 0x01 && inputs[63] == 0x00);
    CHECK(inputs.size() == 161 && inputs[155] == 0x80 && inputs[156] == 3 && inputs[157] == 0 && inputs[158] == 0x7F);  // gaps
    Start laid_out = start_of(3, 1, 0, 0x03, 8, 0x04);
    laid_out.delays = { 3, 0, 1, 2 };
    laid_out.characters = 16;
    const std::vector<uint8_t> start = bytes_of(laid_out);
    CHECK(start.size() == 15 && start[5] == 3 && start[6] == 1 && start[7] == 3 && start[8] == 0 && start[9] == 1 && start[10] == 2 &&
        start[11] == 0x03 && start[12] == 8 && start[13] == 0x04 && start[14] == 16);
    CHECK(round_trips(laid_out, 15));
    Start twelve = start_of(4, 0);
    twelve.characters = 12;
    CHECK(round_trips(twelve, 15));

    // Out-of-range fields are neither sent nor accepted.
    CHECK(bytes_of(Welcome{ 0x04, 2 }).empty());    // a port not below players
    CHECK(bytes_of(Welcome{ 0x03, 3 }).empty());    // the host's port 0
    CHECK(bytes_of(Welcome{ 0x02, 1 }).empty());    // players below 2
    CHECK(bytes_of(Welcome{ 0x02, 5 }).empty());    // players above 4
    CHECK(bytes_of(Welcome{ 0x02, 2, 3 }).empty());         // rejoin is 0 to 2
    CHECK(bytes_of(Welcome{ 0x02, 2, 0, 1 }).empty());      // a match's rules only when rejoining
    CHECK(bytes_of(Welcome{ 0x02, 2, 0, 0, 0, 4 }).empty());
    CHECK(bytes_of(Welcome{ 0x02, 2, 1, 2 }).empty());      // own view 0 or 1
    CHECK(bytes_of(Welcome{ 0x02, 2, 1, 0, 0x04 }).empty());        // pairs past the players
    CHECK(bytes_of(Welcome{ 0x02, 3, 1, 0, 0x01 }).empty());        // an odd number of them
    CHECK(bytes_of(Welcome{ 0x02, 2, 1, 0, 0, max_rollback + 1 }).empty());
    CHECK(bytes_of(Welcome{ 0x02, 2, 0, 0, 0, 0, 16 }).empty());     // characters only when rejoining
    CHECK(bytes_of(Welcome{ 0x02, 2, 1, 0, 0, 0, 13 }).empty());     // characters 0, 12 or 16
    CHECK(bytes_of(Hello{ 1, 2, 3, 4, 5, 4, 1, 1 }).empty());  // nobody left for the host
    CHECK(bytes_of(Hello{ 1, 2, 3, 4, 5, 1, 0, 1 }).empty());  // no player id
    CHECK(bytes_of(Hello{ 1, 2, 3, 4, 5, 1, 1, 0 }).empty());  // no platform
    CHECK(bytes_of(Hello{ 1, 2, 3, 4, 5, 1, 1, 4 }).empty());
    CHECK(bytes_of(start_of(1, 0)).empty());
    CHECK(bytes_of(start_of(5, 0)).empty());
    CHECK(bytes_of(start_of(2, 2)).empty());          // own view is 0 or 1
    CHECK(bytes_of(start_of(2, 0, max_delay + 1)).empty());
    Start one_delay = start_of(2, 0);
    one_delay.delays[3] = max_delay + 1;
    CHECK(bytes_of(one_delay).empty());               // any port's
    CHECK(bytes_of(start_of(3, 1, 0, 0x08)).empty()); // a pair past the players
    CHECK(bytes_of(start_of(4, 1, 0, 0x07)).empty()); // an odd number of ports in pairs
    CHECK(bytes_of(start_of(2, 0, 0, 0, max_rollback + 1)).empty());
    CHECK(bytes_of(start_of(3, 0, 0, 0, 0, 0x01)).empty());     // the host's port open
    Start bad_characters = start_of(4, 0);
    bad_characters.characters = 8;
    CHECK(bytes_of(bad_characters).empty());                    // characters 0, 12 or 16
    CHECK(bytes_of(start_of(3, 0, 0, 0, 0, 0x08)).empty());     // an open port past the players
    CHECK(bytes_of(start_of(4, 0, 0, 0x0C, 0, 0x08)).empty());  // an open port in a pair
    CHECK(bytes_of(Delay{ 0x01, max_delay + 1, 30 }).empty());
    CHECK(bytes_of(Delay{ 0, 1, 30 }).empty());                 // no port
    CHECK(bytes_of(Delay{ 0x10, 1, 30 }).empty());              // past the ports
    CHECK(bytes_of(Gone{ 1, 30, 4 }).empty());

    CHECK(bytes_of(Refuse{ (Reason)0 }).empty());
    CHECK(bytes_of(Refuse{ Reason::build, "a release" }).empty());
    CHECK(bytes_of(Refuse{ Reason::build, std::string(max_release_name + 1, 'a') }).empty());
    CHECK(bytes_of(Bye{ (Reason)(last_reason + 1) }).empty());
    Inputs bad = some_inputs();
    bad.port = 4;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.count = 0;
    bad.pads = {};
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.count = input_window + 1;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[0].buttons |= 0x0040;                  // a reserved button
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[1].err_no = 0x04;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[5].stick_x = 1;                        // an unused pad that isn't zero
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[3].wheel = 1;                       // an unused motion that isn't zero
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[1].controls = (fov_codes + 1) << fov_shift;  // a field of view past the codes
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[1].controls = (fov_codes << fov_shift) | controls_mask | swap_sticks_bit;  // the last code, every other bit: fine
    CHECK(!bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[1].controls = swap_sticks_bit << 1;  // a bit past Swap Sticks
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.first_read = UINT32_MAX - 1;                // its last read past 2^32 - 1
    CHECK(bytes_of(bad).empty());
    bad.first_read = UINT32_MAX - 2;                // ...and just fitting
    CHECK(!bytes_of(bad).empty());

    // The same, arriving as bytes.
    std::vector<uint8_t> data = bytes_of(Welcome{ 0x02, 2 });
    data[5] = 0x03;
    CHECK(!decodes(data));
    data = gone;
    data[5] = 0;                                    // the host's port
    CHECK(!decodes(data));
    data = gone;
    data[5] = 4;
    CHECK(!decodes(data));
    data = gone;
    data[10] = 4;                                   // silent is 0 to 3
    CHECK(!decodes(data));
    data = hello;
    data[45] = 4;
    CHECK(!decodes(data));
    data = bytes_of(Bye{ Reason::quit });
    data[5] = 0;
    CHECK(!decodes(data));
    data[5] = last_reason + 1;
    CHECK(!decodes(data));
    data = inputs;
    data[11 + 5 * 6] = 1;                           // pad 6, unused
    CHECK(!decodes(data));
    data = inputs;
    data[11] |= 0x80;                               // pad 0's reserved button
    CHECK(!decodes(data));
    data = inputs;
    data[51 + 13 * 7] = 1;                          // motion 7, unused
    CHECK(!decodes(data));
    data = inputs;
    data[51 + 13 * 1 + 10] = (uint8_t)(((fov_codes + 1) << fov_shift) >> 8);  // motion 1's field of view: past the codes
    CHECK(!decodes(data));
    data = inputs;
    data[51 + 13 * 1 + 12] = 0x01;                  // motion 1's top byte: past Swap Sticks
    CHECK(!decodes(data));
    data = start;
    data[7] = max_delay + 1;
    CHECK(!decodes(data));
    data = start;
    data[8] = 0x0C;                                 // ports 3 and 4 in pairs, in a match of 3
    CHECK(!decodes(data));

    // Sizes, versions, kinds and the magic.
    for (size_t size = 0; size <= max_packet_size; size++) {
        std::vector<uint8_t> resized = inputs;
        resized.resize(size);
        CHECK(decodes(resized) == (size == inputs.size()));
    }
    data = hello;
    data[2]++;
    CHECK(!decodes(data));
    data = hello;
    data[3] = 1;
    CHECK(!decodes(data));
    data = hello;
    data[0] = 'X';
    CHECK(!decodes(data));
    for (int kind : { 0, 14, 255 }) {
        data = hello;
        data[4] = (uint8_t)kind;
        CHECK(!decodes(data));
    }
    data = hello;
    data[4] = 3;                                    // Refuse's kind with Hello's size
    CHECK(!decodes(data));

    // The waiting room: Roster and Ready.
    Roster roster;
    roster.players = 3;
    roster.seats[0] = { 0x0102030405060708ull, 1, 1, 0 };
    roster.seats[2] = { 9, 2, 0, 0xFFFF };
    roster.rollback = max_rollback;
    roster.characters = 16;
    roster.watchers[1] = { 0x1112131415161718ull, 3 };
    CHECK(round_trips(roster, 131));
    Roster settings_only;
    settings_only.delay = max_delay;
    settings_only.spectators = 0;
    CHECK(round_trips(settings_only, 131));
    CHECK(round_trips(Ready{ 1 }, 6));
    CHECK(round_trips(Ready{ 0 }, 6));
    const std::vector<uint8_t> r = bytes_of(roster);
    CHECK(r[4] == 10 && r[5] == 8 && r[12] == 1 && r[13] == 1 && r[14] == 1 && r[15] == 0 && r[29] == 9 && r[37] == 2 &&
        r[39] == 0xFF && r[40] == 0xFF && r[53] == 3 && r[54] == 0 && r[55] == auto_delay && r[56] == max_rollback && r[57] == 1 &&
        r[58] == 16 && r[59] == 0 && r[67] == 0 && r[68] == 0x18 && r[75] == 0x11 && r[76] == 3 && r[130] == 0);
    Roster bad_roster = roster; bad_roster.players = 1; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.players = 2; CHECK(bytes_of(bad_roster).empty());           // a seat past the players
    bad_roster = roster; bad_roster.seats[1].ready = 1; CHECK(bytes_of(bad_roster).empty());    // a free seat that isn't all zero
    bad_roster = roster; bad_roster.seats[0].platform = 0; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.seats[0].ready = 2; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.own_view = 2; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.delay = max_delay + 1; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.rollback = max_rollback + 1; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.spectators = 2; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.characters = 15; CHECK(bytes_of(bad_roster).empty());
    bad_roster = roster; bad_roster.watchers[0].platform = 1; CHECK(bytes_of(bad_roster).empty());    // nobody, but not all zero
    bad_roster = roster; bad_roster.watchers[1].platform = 4; CHECK(bytes_of(bad_roster).empty());
    CHECK(bytes_of(Ready{ 2 }).empty());
    data = r; data[13] = 4; CHECK(!decodes(data));                 // platform past macOS
    data = r; data[17] = 1; CHECK(!decodes(data));                 // seat 1 free, but not all zero
    data = r; data[53] = 5; CHECK(!decodes(data));
    data = r; data[55] = max_delay + 1; CHECK(!decodes(data));
    data = r; data[57] = 2; CHECK(!decodes(data));
    data = r; data[58] = 13; CHECK(!decodes(data));                // characters 0, 12 or 16
    data = r; data[76] = 0; CHECK(!decodes(data));                 // a spectator with no platform
    data = bytes_of(Ready{ 1 }); data[5] = 2; CHECK(!decodes(data));
    data = bytes_of(Refuse{ Reason::watching }); CHECK(data.size() == 47 && data[5] == last_reason && data[6] == 0);

    // Refuse keeps its layout from version 17 on: read in any of them (decode_refusal), a reason this
    // version doesn't know as Reason::version; not in an older one, nor with the padding not zero.
    const std::vector<uint8_t> refusal = bytes_of(Refuse{ Reason::build, "v0.1.6-netplay.15" });
    CHECK(refusal[6] == 17 && refusal[7] == 'v' && refusal[23] == '5' && refusal[24] == 0 && refusal[46] == 0);
    CHECK(decode_refusal(refusal) && *decode_refusal(refusal) == (Refuse{ Reason::build, "v0.1.6-netplay.15" }));
    data = refusal; data[2] = 0x40; data[3] = 0x01;                  // version 320
    CHECK(!decodes(data) && decode_refusal(data) && decode_refusal(data)->release == "v0.1.6-netplay.15");
    data[5] = 200;
    CHECK(decode_refusal(data) && decode_refusal(data)->reason == Reason::version);
    data = refusal; data[2] = 16; CHECK(!decode_refusal(data));
    data = refusal; data[30] = 'x'; CHECK(!decodes(data) && !decode_refusal(data));
    data = refusal; data[6] = 41; CHECK(!decodes(data) && !decode_refusal(data));
    data = refusal; data[10] = ' '; CHECK(!decodes(data) && !decode_refusal(data));
    data = refusal; data[5] = 0; CHECK(!decode_refusal(data) || decode_refusal(data)->reason == Reason::version);
    data = refusal; data.pop_back(); CHECK(!decode_refusal(data));
    CHECK(!decode_refusal(bytes_of(Ready{ 1 })));

    // Desync recovery: Resync, and StatePart, the one kind of no fixed size.
    CHECK(round_trips(Resync{ 0x01020304, 0x0A }, 10));
    data = bytes_of(Resync{ 0x01020304, 0x0A });
    CHECK(data[4] == 12 && data[5] == 4 && data[8] == 1 && data[9] == 0x0A);
    CHECK(round_trips(Resync{ 5, 0 }, 10));                         // to a spectator alone
    CHECK(bytes_of(Resync{ 5, 1 }).empty());                        // the host
    CHECK(bytes_of(Resync{ 5, 0x10 }).empty());                     // past the ports
    StatePart part{ 900, 100, 40, 0x1122334455667788ull, std::vector<uint8_t>(60, 0xAB) };
    CHECK(round_trips(part, 25 + 60));
    data = bytes_of(part);
    CHECK(data[4] == 13 && data[5] == 0x84 && data[6] == 3 && data[9] == 100 && data[13] == 40 && data[17] == 0x88 &&
        data[24] == 0x11 && data[25] == 0xAB && data[84] == 0xAB);
    StatePart whole{ 1, max_state_size, max_state_size - max_state_part, 7, std::vector<uint8_t>(max_state_part, 1) };
    CHECK(round_trips(whole, max_message_size));
    StatePart bad_part = part; bad_part.bytes.push_back(0); CHECK(bytes_of(bad_part).empty());          // past the end
    bad_part = part; bad_part.bytes.clear(); CHECK(bytes_of(bad_part).empty());
    bad_part = part; bad_part.size = 0; bad_part.offset = 0; bad_part.bytes = { 1 }; CHECK(bytes_of(bad_part).empty());
    bad_part = part; bad_part.size = max_state_size + 1; CHECK(bytes_of(bad_part).empty());
    bad_part = part; bad_part.offset = 100; bad_part.bytes = { 1 }; CHECK(bytes_of(bad_part).empty());
    bad_part = whole; bad_part.offset--; bad_part.bytes.push_back(1); CHECK(bytes_of(bad_part).empty());   // more than a part holds
    data = bytes_of(part); data.pop_back(); CHECK(decodes(data));                    // fewer bytes: a smaller part
    data.resize(25); CHECK(!decodes(data));                                          // no bytes
    data = bytes_of(part); data.resize(25 + 61); CHECK(!decodes(data));              // past the state's end

    // Rejoining: Back, and Rejoin.
    CHECK(round_trips(Back{ 0x06, 0x01020304 }, 19));
    CHECK(round_trips(Back{ 0x06, 5, 0x04 }, 19));
    CHECK(round_trips(Back{ 0x04, 5, 0x04, 0x0102030405060708ull }, 19));     // a spectator taking port 2
    CHECK(bytes_of(Back{ 0x06, 5, 0x06, 9 }).empty());              // ...one port only
    CHECK(bytes_of(Back{ 0x04, 5, 0, 9 }).empty());                 // ...as a new player
    CHECK(round_trips(Claim{ 3 }, 6));
    CHECK(round_trips(Unseat{ 0x06 }, 6));                          // a machine of two players
    CHECK(bytes_of(Unseat{ 0 }).empty());
    CHECK(bytes_of(Unseat{ 1 }).empty());                           // the host's port
    CHECK(bytes_of(Claim{ 0 }).empty());                            // the host's port
    CHECK(bytes_of(Unseat{ 0x10 }).empty());
    CHECK(round_trips(Gone{ 2, 30, 2 }, 11));                       // stepped out to watch
    CHECK(round_trips(Gone{ 2, 30, 3 }, 11));                       // let go of, for now
    data = bytes_of(Back{ 0x06, 0x01020304, 0x02 });
    CHECK(data[4] == 14 && data[5] == 0x06 && data[6] == 4 && data[9] == 1 && data[10] == 0x02);
    CHECK(bytes_of(Back{ 0x06, 5, 0x08 }).empty());                 // a new player not among them
    CHECK(bytes_of(Back{ 0, 5 }).empty());                          // nobody
    CHECK(bytes_of(Back{ 0x03, 5 }).empty());                       // the host
    CHECK(bytes_of(Back{ 0x10, 5 }).empty());                       // past the ports
    Rejoin rejoin;
    rejoin.read = 1000;
    rejoin.at = 1004;
    rejoin.delay_from = { 600, 0, 0, 700 };
    rejoin.delay_before = { 2, 0, 0, 1 };
    rejoin.delay_after = { max_delay, 0, 0, 0 };
    rejoin.idle_from = { no_read, 1000, no_read, 1003 };
    rejoin.pads[1] = { 0x8001, -5, 7, 0 };
    rejoin.pads[3] = { 0, 0, 0, err_no_controller };
    rejoin.motions[1] = { -300, 200, 1, -1, -3, 0x2C05 };
    rejoin.motions[2].controls = swap_sticks_bit;
    rejoin.open = 0x08;
    CHECK(round_trips(rejoin, 126));
    data = bytes_of(rejoin);
    CHECK(data[4] == 15 && data[5] == 0xE8 && data[6] == 3 && data[9] == 0xEC && data[13] == 0x58 && data[17] == 2 &&
        data[18] == max_delay && data[31] == 0xBC && data[35] == 1 && data[36] == 0 && data[37] == 0xFF && data[59] == 0xE8 &&
        data[63] == 1 && data[64] == 0x80);
    Rejoin bad_rejoin = rejoin; bad_rejoin.at = 999; CHECK(bytes_of(bad_rejoin).empty());                   // the host before it
    bad_rejoin = rejoin; bad_rejoin.delay_after[2] = max_delay + 1; CHECK(bytes_of(bad_rejoin).empty());
    bad_rejoin = rejoin; bad_rejoin.delay_before[1] = max_delay + 1; CHECK(bytes_of(bad_rejoin).empty());
    bad_rejoin = rejoin; bad_rejoin.idle_from[1] = 999; CHECK(bytes_of(bad_rejoin).empty());          // idle before it
    bad_rejoin = rejoin; bad_rejoin.pads[0].buttons = reserved_buttons; CHECK(bytes_of(bad_rejoin).empty());
    bad_rejoin = rejoin; bad_rejoin.pads[0].err_no = 1; CHECK(bytes_of(bad_rejoin).empty());
    bad_rejoin = rejoin; bad_rejoin.motions[0].controls = swap_sticks_bit << 1; CHECK(bytes_of(bad_rejoin).empty());
    bad_rejoin = rejoin; bad_rejoin.open = 0x04; CHECK(bytes_of(bad_rejoin).empty());           // an open port that plays
    bad_rejoin = rejoin; bad_rejoin.open = 0x01; CHECK(bytes_of(bad_rejoin).empty());           // the host's
    bad_rejoin = rejoin; bad_rejoin.open = 0x10; CHECK(bytes_of(bad_rejoin).empty());           // past the ports
    data = bytes_of(rejoin); data.pop_back(); CHECK(!decodes(data));

    std::vector<uint8_t> small(max_packet_size);
    CHECK(encode(part, small) == 85 && encode(whole, small) == 0);                   // too big for the buffer

    if (failures == 0) {
        std::printf("packets_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
