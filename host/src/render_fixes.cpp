// Rendering fixes for things the game did that RT64 draws differently, called from hooks in
// conker.toml.

#include <SDL.h>

// Light glows (such as the two lights over the Feral Reserve's doors).
//
// func_151408A4 draws a glow over a light, fading it out while something stands in front
// of it. It finds that by having the RDP copy the one pixel of the depth buffer under the
// light into memory (its display list gets the pixel's x and y), then, the next frame,
// decoding that sample (D_80089630: the N64's depth format) and comparing it with the
// light's own depth: more than 31 behind the sample, and the glow fades out. RT64 draws
// depth on the GPU, and that sample doesn't come back as the N64's depth, so glows faded
// out wrongly (lit near the light, gone a little way off). The comparison is skipped: a
// glow on screen is shown. (The same function's 4:3 bounds check is widened for widescreen in
// widescreen.cpp.)

#include "recomp.h"

// func_151408A4 at 0x15140DB4, just after it compared the depths: $t3 is the light's depth
// ($t8) less the sample's ($a2), $a0 the raw sample. Zero passes the test.
extern "C" void conker_light_glow_depth(uint8_t* rdram, recomp_context* ctx) {
    ctx->r11 = 0;
}

// The tickly bees in front of the sunflower (issue #101; Windy's pollination quest).
//
// func_15168118 draws each bee by calling func_15095760 (0x15168640), which gives the bee a depth from its place in
// the scene (G_SETPRIMDEPTH) and draws it as a rectangle (func_15095D34), with a render mode (func_15142C10's) that
// blends it over what's there, depth-tested against that depth but not writing it. The sunflower the bees circle is
// drawn after them, so nothing stopped it from being drawn over a bee nearer the camera than it (measured: with the
// bees' depth made the nearest, the flower was still drawn over them). The N64 drew it so too; here a bee writes its
// depth, so what's drawn after it, the flower among it, is hidden behind it where the bee is nearer and drawn over it
// where it's further. Only the pixels the bee draws: the alpha compare's threshold (the blend colour's alpha, 0) drops
// its rectangle's transparent ones, which would otherwise hide what's behind the bee as a rectangle.
namespace {
    bool drawing_bee = false;
}

// func_15168118 at 0x15168640, about to call func_15095760 for a bee, and at 0x15168648, back from it.
extern "C" void conker_bee_begin(uint8_t* rdram, recomp_context* ctx) {
    drawing_bee = true;
}

extern "C" void conker_bee_end(uint8_t* rdram, recomp_context* ctx) {
    drawing_bee = false;
}

// func_15095760 at 0x15095878, about to store the depth of a G_SETPRIMDEPTH at $v1: for a bee, its render mode
// (the G_RDPSETOTHERMODE, 0xEF, written a few commands before) gets Z_UPD (0x20) and the alpha compare's threshold (0x1).
extern "C" void conker_bee_depth(uint8_t* rdram, recomp_context* ctx) {
    if (!drawing_bee) {
        return;
    }
    for (int i = 1; i <= 8; i++) {
        const gpr command = ctx->r3 - i * 8;
        if (((uint32_t)MEM_W(0, command) >> 24) == 0xEF) {
            MEM_W(4, command) = (int32_t)((uint32_t)MEM_W(4, command) | 0x21);
            break;
        }
    }
}

// func_1510FEA0 at 0x1510FFA4 (it starts each frame's display list): $t8 is D_800BE635, which
// decides whether the frame is cleared to black before it is drawn. Level setup clears the
// flag, since the level and sky are meant to cover the screen; where they don't (such as with
// the mouse camera in a wall), the last frame's picture repeated. Clear every frame.
extern "C" void conker_frame_clear(uint8_t* rdram, recomp_context* ctx) {
    // TEMP-DEBUG: CONKER_NO_FRAME_CLEAR leaves the game's own choice, to compare.
    static const bool disabled = SDL_getenv("CONKER_NO_FRAME_CLEAR") != nullptr;
    if (!disabled) {
        ctx->r24 = 1;
    }
}

// TEMP-DEBUG: the game copies pixels of the depth buffer into small images mid-frame, then
// reads them with the CPU (switching the colour image to them and back). In widescreen,
// what RT64 drew after those copies lost pieces (doors among them). These switch the copies
// off, to find which one does it.
// func_1510B9D0 at 0x1510BE18: $t2 (camera +0x84 & 8) decides whether func_1512E5F0 samples.
extern "C" void conker_depth_copy_camera(uint8_t* rdram, recomp_context* ctx) {
    static const bool skip = SDL_getenv("CONKER_SKIP_DEPTH_CAMERA") != nullptr;
    if (skip) {
        ctx->r10 = 0;
    }
}
// func_151742EC at 0x1517432C: $t6 (its camera argument) nonzero returns before any copy.
extern "C" void conker_depth_copy_probes(uint8_t* rdram, recomp_context* ctx) {
    static const bool skip = SDL_getenv("CONKER_SKIP_DEPTH_PROBES") != nullptr;
    if (skip) {
        ctx->r14 = 1;
    }
}
