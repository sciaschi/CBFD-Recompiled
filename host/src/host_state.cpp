// What the host keeps outside game memory that decides what it writes into it (conker::host_state, in
// conker.hpp), as bytes for save states: so a state loaded goes on exactly as it did when it was taken,
// as a replay (and netplay) needs.
//
// The bytes: a version (u32, 1), then each part as its count of 32-bit words (u32) and the words, all
// little-endian: the song marks, the widescreen ring, the cutscene bars' ring, the shadows, the sound
// buffers on the timeline (the rate, then each one's end as two words, low first), the mouse aiming's
// remainders (look_aim.cpp), the orbits (mouse_camera.cpp), and each player's movement for the current read
// (netplay/motion.cpp: empty without netplay).

#include <cstdint>
#include <cstring>
#include <vector>

#include "conker.hpp"

namespace {
    constexpr uint32_t version = 1;

    void put(std::vector<uint8_t>& out, uint32_t word) {
        for (int i = 0; i < 4; i++) {
            out.push_back((uint8_t)(word >> (8 * i)));
        }
    }

    void put_part(std::vector<uint8_t>& out, const std::vector<uint32_t>& words) {
        put(out, (uint32_t)words.size());
        for (uint32_t word : words) {
            put(out, word);
        }
    }

    struct Reader {
        const std::vector<uint8_t>& in;
        size_t at = 0;
        bool get(uint32_t& word) {
            if (at + 4 > in.size()) {
                return false;
            }
            word = (uint32_t)in[at] | ((uint32_t)in[at + 1] << 8) | ((uint32_t)in[at + 2] << 16) | ((uint32_t)in[at + 3] << 24);
            at += 4;
            return true;
        }
        // Empty if it's missing.
        std::vector<uint32_t> get_part() {
            uint32_t count = 0;
            if (!get(count) || count > (in.size() - at) / 4) {
                at = in.size();
                return {};
            }
            std::vector<uint32_t> words(count);
            for (uint32_t& word : words) {
                get(word);
            }
            return words;
        }
    };
}

std::vector<uint8_t> conker::host_state::save() {
    std::vector<uint8_t> out;
    put(out, version);
    put_part(out, conker::songs::marks());
    put_part(out, { conker::widescreen::dl_ring() });
    put_part(out, { conker::cutscene_aspect::dl_ring() });
    put_part(out, conker::shadows::drawn());
    put_part(out, conker::audio_clock::state());
    put_part(out, conker::look_aim::state());
    put_part(out, conker::mouse_camera::state());
#if defined(CONKER_NETPLAY)
    put_part(out, conker::motion::state());
#else
    put_part(out, {});
#endif
    put_part(out, conker::field_of_view::state());
    return out;
}

void conker::host_state::load(const std::vector<uint8_t>& bytes) {
    Reader in{ bytes };
    uint32_t got_version = 0;
    if (!in.get(got_version) || got_version != version) {
        in.at = bytes.size();
    }
    conker::songs::set_marks(in.get_part());
    const std::vector<uint32_t> wide = in.get_part();
    conker::widescreen::set_dl_ring(wide.empty() ? 0 : wide[0]);
    const std::vector<uint32_t> bars = in.get_part();
    conker::cutscene_aspect::set_dl_ring(bars.empty() ? 0 : bars[0]);
    conker::shadows::set_drawn(in.get_part());
    conker::audio_clock::set_state(in.get_part());
    conker::look_aim::set_state(in.get_part());
    conker::mouse_camera::set_state(in.get_part());
#if defined(CONKER_NETPLAY)
    conker::motion::set_state(in.get_part());
#else
    in.get_part();
#endif
    conker::field_of_view::set_state(in.get_part());
}
