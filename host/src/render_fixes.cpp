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

#include "conker.hpp"

// func_151408A4 at 0x15140DB4, just after it compared the depths: $t3 is the light's depth
// ($t8) less the sample's ($a2), $a0 the raw sample. Zero passes the test.
extern "C" void conker_light_glow_depth(uint8_t* rdram, recomp_context* ctx) {
    ctx->r11 = 0;
}

// The follow camera's view of its player, in netplay (and anything else on the harness: recordings, replays).
//
// Each camera keeps a row of the depth buffer, at its player's height on the screen (+0x8BC points at it, 2 bytes
// a pixel across the whole frame), which the RDP copies there mid-frame (func_1512E5F0). func_1512AD54 reads it
// around the player (func_1512B1B8, indexed by the pixel's x on the frame) and, where something nearer than the
// player hides them, turns the camera and pulls it in (func_1512B730, the camera's +0x3EC), so the player stays in
// view. The game clears the row to the farthest depth (0xFFFC) itself as a camera starts (func_1512B53C), but only
// as many pixels as a view is wide (the camera's view's +0x4), from the left edge of the frame.
//
// On the harness the renderer writes nothing back into game memory (netplay/profile.cpp: what a GPU draws isn't
// the same on every machine, and the games must be), so the copies never arrive: the row is what the clear left.
// Player 1's view (the left of the frame) reads only cleared pixels, nothing in front, and its camera stays put.
// The others' views reach past them, into pixels that were never written and read as something just in front of
// the player: players 2 to 4's cameras turned and pulled in on their own all match, the same on every machine,
// with nobody touching anything. So on the harness the whole row is the farthest depth whenever the check is about
// to read it: every player's camera sees what player 1's did. (The game's other ways of keeping its player in
// view, its collision with walls and floors, don't use the depth buffer and are as they were.)
//
// func_1512AD54 at 0x1512AD58: $a0 is the camera.
extern "C" void conker_camera_depth_row(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netplay_profile::active()) {
        return;
    }
    const gpr row = (gpr)(int32_t)MEM_W(0x8BC, ctx->r4);
    if (((uint32_t)row & 0xFF800000u) != 0x80000000u) {
        return;
    }
    // As many pixels as the game made the row: func_15005290 allocates each camera's (D_800BE2B0, copied to
    // +0x8BC as the camera starts) as the frame's width (D_800BE620, 292, set once) times 2 bytes, and the RDP's
    // copy fills that much. (The 640 only keeps a broken width from writing far.)
    const int32_t width = std::clamp((int32_t)MEM_W(0, (gpr)(int32_t)0x800BE620), (int32_t)0, (int32_t)640);
    for (int32_t x = 0; x < width; x++) {
        MEM_H(x * 2, row) = (int16_t)0xFFFC;
    }
}

// Sprite pieces less than a pixel across (the smoke puffs over Windy's beehive, as one starts).
//
// func_15095D34 draws a piece of a sprite (speech bubbles, the tickly bees, puffs of smoke...) as a
// rectangle: its width and height (D_800D2C70 and D_800D2C74, floats the caller sets) are truncated to
// whole pixels, and the texture's step across it is divided by them. The compiler guards each division
// with a break (break 7, divide by zero), and the N64 stops there as the recompiled game does: a piece
// under a pixel wide crashed the game (do_break from func_15095D34, with func_1516D738's puffs), on a
// build without #101's cull for the tickly bees. Where the piece comes from isn't known: the sprite culls
// that draw what's beside the 4:3 frame in widescreen (widescreen.cpp) may let it through, or the
// original game may send one too, as rarely. A size that truncates to 0 (between -1 and 1) is made a
// pixel, drawn as one, not a crash; others, negative ones among them, are left as they are.
// func_15095D34 at 0x15095D58, with the width in $f0 and the height in $f2, before they're truncated.
extern "C" void conker_sprite_piece_size(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->f0.fl > -1.0f && ctx->f0.fl < 1.0f) {
        ctx->f0.fl = 1.0f;
    }
    if (ctx->f2.fl > -1.0f && ctx->f2.fl < 1.0f) {
        ctx->f2.fl = 1.0f;
    }
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
// So each list gets half the budget again, at least 1000 commands and at most 4000 (8-32KB each of the
// heap, which has some 850KB free in a story level), and the limit is the list's own size less 800
// (twice the game's room for what's drawn unchecked, for the interpolation's groups among it). The
// multiplayer arenas' budgets are far larger (14400 to 26400 commands, against 6400 to 8400 in the
// story), and their heap is tighter: half again of those, some 200KB for the two lists, ran the heap
// out in Tank and Heist, which stopped the game (its allocator's fatal error, 0x0C000042, a syscall at
// 0x150AD770), hence the cap.

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
        return game + std::clamp<uint32_t>(budget / 2, 1000, 4000);
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
