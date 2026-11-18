// The black and white newspaper of It's War's intro (issue #162). The N64 draws it in colour and greys the frame on
// the CPU: once the RDP has drawn a frame, before it's shown, func_10005020 calls func_1515FDA0, which runs the frame
// effects on two lists (heads at D_800DCF20 and 0x1A0 on, linked through +8), each by its type (+0x10, a byte) from a
// table of functions (D_8008B0D8). Type 2 is func_150AF790, which has func_150B1DB0 put each pixel's green in all
// three channels, in RDRAM (292 x 216 pixels from the colour image). RT64 draws the frame on the GPU and doesn't
// write it back to RDRAM, so that greyed nothing and the newspaper stayed in colour.
//
// So with RT64 the CPU's grey is skipped, and while the effect is on a list, the frame's display list asks RT64 to
// grey its picture once drawn (gEXConkerGreyFramebuffer, rt64.patch), from the list of our own the frame's list
// calls at its end (cutscene_aspect.cpp).

#include <cstdint>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t effect_lists = 0x800DCF20;   // D_800DCF20: the two lists' heads
    constexpr uint32_t effect_list_stride = 0x1A0;
    constexpr uint32_t cutscene_playing = 0x800C35EA; // D_800C35EA
    constexpr int8_t effect_grey = 2;              // func_150AF790
    // An effect's flags (+0xE): 4, skipped (and cleared); 2, skipped while a cutscene plays.
    constexpr uint8_t effect_skip_once = 0x4;
    constexpr uint8_t effect_not_in_cutscene = 0x2;
}

// Whether func_1515FDA0 greys the frame this display list draws, as far as can be told while it's made: an effect
// of type 2 on either list, that it would run.
bool conker::grey_frame::this_frame(uint8_t* rdram) {
    const bool in_cutscene = MEM_BU(0, (gpr)(int32_t)cutscene_playing) == 1;
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t node = (uint32_t)MEM_W(0, (gpr)(int32_t)(effect_lists + list * effect_list_stride));
        for (int count = 0; node != 0 && count < 256; count++) {
            if ((node & 0xFF800000u) != 0x80000000u) {
                break; // not in RDRAM: not a list
            }
            const gpr effect = (gpr)(int32_t)node;
            const uint8_t flags = MEM_BU(0xE, effect);
            const int8_t type = (int8_t)MEM_BU(0x10, effect);
            if (type == effect_grey && (flags & effect_skip_once) == 0 &&
                !((flags & effect_not_in_cutscene) != 0 && in_cutscene)) {
                return true;
            }
            node = (uint32_t)MEM_W(8, effect);
        }
    }
    return false;
}

// func_150AF790, at its first instruction ($a1, the colour image). Returns 1 to skip the CPU's grey, which RT64 does.
extern "C" int conker_grey_skip_cpu(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    (void)rdram;
    (void)ctx;
    return 1;
#else
    (void)rdram;
    (void)ctx;
    return 0;
#endif
}
