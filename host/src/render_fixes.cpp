// Rendering fixes for things the game did that RT64 draws differently, and room for what the
// recompilation draws more of, called from hooks in conker.toml.

#include <algorithm>
#include <cstdint>

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

// Room in the frame's display lists (issue #98: a crash leaving Spooky, after the barrel ride).
//
// func_15015E80 (as a level loads) allocates the two display lists the frames are drawn into, one
// for each frame in turn, from the game's heap: as many commands as the scene's budget (its data's
// +0x1C, 8000 if none; 4000 in the lava level) and 400 more, 8 bytes each. It stores the budget as
// a limit (D_800BEBA4): while drawing, the game stops adding objects once a list holds more commands
// than that, and the 400 more are for what's drawn after, unchecked (the HUD, the frame's end...).
//
// Two things made a frame need more than the original did: the frame interpolation's matrix groups
// (interpolation.cpp: 4 commands for each object and each shadow drawn), and widescreen, whose wider
// view draws the objects beside the 4:3 frame too. In a busy scene the budget ran out: the game
// stopped drawing objects (in the report, the water and every object vanished), and what came after
// ran past the list's end, over the next heap block's header. The game frees the level's blocks as it
// leaves the level (func_15019130, by func_10004250, which walks the heap's blocks), and walking
// into the overwritten header crashed (a segfault in func_10004250). And the limit isn't the list's
// size: it's worked out from how far apart the two lists are, which is their size only if the heap
// put them side by side. At the title it hadn't (another block between them): the limit was 11830 for
// lists of 6400 commands, so a list could run past its end with nothing to stop it.
//
// So each list gets half the budget again (at least 1000 commands: 32-64KB of the heap, which has
// some 850KB free in a level), and the limit is the list's own size less 800 (twice the game's room
// for what's drawn unchecked, for the interpolation's groups among it).

namespace {
    constexpr uint32_t scene_data = 0x800B0DF0;      // D_800B0DF0: the scene's data
    constexpr uint32_t display_list_room = 400;      // the game's, for what's drawn unchecked

    // The commands the game allocates a list for: the budget and its 400 (the scene's data at +0x1C,
    // once func_15015E80 has added them).
    uint32_t game_list_commands(uint8_t* rdram) {
        const gpr scene = (gpr)MEM_W(0, (gpr)(int32_t)scene_data);
        return (uint32_t)MEM_HU(0x1C, scene);
    }

    uint32_t list_commands(uint8_t* rdram) {
        const uint32_t game = game_list_commands(rdram);
        const uint32_t budget = game > display_list_room ? game - display_list_room : 0;
        return game + std::max<uint32_t>(budget / 2, 1000);
    }
}

// func_15015E80 at 0x15015ED0 and 0x15015F00, as it allocates each list: $t9 (the first) and $t2
// (the second) are its size in bytes, passed to the allocator in the call's delay slot.
extern "C" void conker_display_list_size_first(uint8_t* rdram, recomp_context* ctx) {
    ctx->r25 = (gpr)(int32_t)(list_commands(rdram) * 8);
}

extern "C" void conker_display_list_size_second(uint8_t* rdram, recomp_context* ctx) {
    ctx->r10 = (gpr)(int32_t)(list_commands(rdram) * 8);
}

// At 0x15015F28, as it stores the limit (D_800BEBA4) from $t7.
extern "C" void conker_display_list_limit(uint8_t* rdram, recomp_context* ctx) {
    ctx->r15 = (gpr)(int32_t)(list_commands(rdram) - 2 * display_list_room);
}
