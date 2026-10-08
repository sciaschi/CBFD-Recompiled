// Frame interpolation help, called from hooks in conker.toml.
//
// RT64 draws frames between the game's by pairing each transform with one from the
// previous frame. Without help it guesses, from draw calls that look alike, and the
// game batches a character's triangles differently from frame to frame, so some of
// Conker's parts were paired with others and jumped about. func_1502CCFC draws a game
// object (from gObjects) into a display list: its commands are wrapped in an RT64
// matrix group whose ID names the object, with linear ordering, so the n-th matrix
// of an object is always paired with the n-th of the same object last frame. An
// object whose count of matrices changed (a character's mesh is drawn in chunks, one
// bone's matrix each, cut differently as its animation changes) has each matrix paired
// with last frame's closest instead, its vertices left as they are (rt64.patch,
// GameFrame::alignTransforms; pairing none of them made Conker snap, issue #76).
//
// A character's shadow (func_15186794) is the ground under it, clipped anew every
// frame and drawn with the shadow's texture projected onto it: its own group asks RT64
// to interpolate its texture coordinates from last frame's projection (rt64.patch),
// so the shadow slides with the character instead of stepping at the game's rate.
// The silhouette in the shadow's texture is drawn from a camera the game aims at the character
// anew every frame, its distance and zoom changing together; RT64 interpolates that camera whole
// (rt64.patch, ProjectionProcessor), as taken apart and put together halfway it drew the shadow
// larger (issue #76).
//
// A shadow isn't drawn on the first game frame it's back after frames without it (issue
// #75). Walking into a room, the game draws no shadow while the room loads, then for
// one frame lays the shadow from where Conker was before the room switched over onto
// the new room's ground: a big patch of it, only the dark middle of the shadow's texture
// on it. Measured (RT64's shadows logged, frame by frame): the shadow back after the
// gap was a 90-vertex patch at the wrong height, the next one a normal 28-vertex patch
// on the floor 160 units away. The game draws it so at 30 FPS too, so it isn't the
// smoothing; on the frame after, all is right. Anywhere else a shadow shows up a frame
// late, 1/30 of a second, after a cutscene or a load.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "recomp.h"

#include "conker.hpp"

namespace {
    // RT64's extended GBI (tools/rt64/include/rt64_extended_gbi.h) for F3DEX2,
    // whose no-op (0xE0) carries RT64's hooks.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_matrixgroup_v1 = 0x00000C;
    constexpr uint32_t g_ex_popmatrixgroup_v1 = 0x00000D;

    constexpr uint32_t g_ex_push = 1;
    constexpr uint32_t g_ex_interpolate_simple = 0;
    constexpr uint32_t g_ex_interpolate_decompose = 1;
    constexpr uint32_t g_ex_component_skip = 0;
    constexpr uint32_t g_ex_component_interpolate = 1;
    constexpr uint32_t g_ex_component_auto = 2;
    constexpr uint32_t g_ex_order_linear = 0;

    // gEXMatrixGroup's parameters for the model matrix, matched in the order drawn:
    // mode (bit 2), the matrices' position, rotation, scale, skew and perspective,
    // vertices, tile, ordering (bit 17), texture coordinates (bit 22) and look-at.
    constexpr uint32_t group_params(uint32_t mode, uint32_t matrices, uint32_t vertices, uint32_t texcoords) {
        return (g_ex_push << 0) | (mode << 2) |
            (matrices << 3) | (matrices << 5) | (matrices << 7) |
            (matrices << 9) | (matrices << 11) | (vertices << 13) |
            (g_ex_component_auto << 15) | (g_ex_order_linear << 17) | (texcoords << 22) |
            (g_ex_component_auto << 24);
    }

    // An object: every component interpolated automatically, vertices too (the game
    // bends joints on the CPU), texture coordinates as they are.
    constexpr uint32_t object_group_params = group_params(g_ex_interpolate_decompose, g_ex_component_auto, g_ex_component_auto, g_ex_component_skip);
    // A shadow: the ground it's drawn on stays where it is (its matrices and vertices as they
    // are), and only its texture coordinates are interpolated.
    constexpr uint32_t shadow_group_params = group_params(g_ex_interpolate_simple, g_ex_component_skip, g_ex_component_skip, g_ex_component_interpolate);

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }

    // Opens a matrix group at dl, with RT64's extended GBI enabled (RT64 turns it off
    // at the start of every display list).
    void put_group(uint8_t* rdram, gpr& dl, uint32_t id, uint32_t params) {
        put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_matrixgroup_v1, id);
        put_command(rdram, dl, params, 0);
    }

    void put_pop(uint8_t* rdram, gpr& dl) {
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1);
    }

    // Game frames so far (conker::shadows::game_frame), and the last one each shadow (by its
    // index) was drawn in. The shadow being drawn: its index, and where its commands start.
    uint32_t game_frames = 0;
    std::unordered_map<uint32_t, uint32_t> shadow_last_drawn;
    uint32_t shadow_index = 0;
    gpr shadow_dl_start = 0;
}

void conker::shadows::game_frame() {
    game_frames++;
}

// func_1502CCFC, after its first instruction (the stack frame): $a0 is where it
// writes its first command, $a1 the object's index, and its 7th argument (0x168 on
// the new stack) tells its callers apart (the object, its shadow...). The group's
// commands go first, and the function writes after them. IDs are neither 0
// (G_EX_ID_IGNORE) nor 0xFFFFFFFF (G_EX_ID_AUTO).
extern "C" void conker_object_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t object = (uint32_t)ctx->r5 & 0xFFFF;
    const uint32_t kind = (uint32_t)MEM_W(0x168, ctx->r29) & 0xFF;
    gpr dl = ctx->r4;
    put_group(rdram, dl, 0x43000000u | (kind << 16) | object, object_group_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote. The group is popped after it.
extern "C" void conker_object_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_pop(rdram, dl);
    ctx->r2 = dl;
}

// func_15186794, after its first instruction: $a0 is where it writes its first
// command, $a1 the shadow's index.
extern "C" void conker_shadow_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t shadow = (uint32_t)ctx->r5 & 0xFFFF;
    gpr dl = ctx->r4;
    shadow_index = shadow;
    shadow_dl_start = dl;
    put_group(rdram, dl, 0x53480000u | shadow, shadow_group_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote. A shadow not drawn last game frame isn't
// drawn in this one either (see the top): handing back where its commands started drops
// everything it wrote, the group's commands too.
extern "C" void conker_shadow_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    // TEMP-DEBUG (issues #73 and #75: a thin dark line next to Conker, only on the reporter's AMD
    // GPU under Linux, only at high resolutions). A shadow is a texture the game draws every frame:
    // it points its drawing at a 64 by 64 8-bit buffer (G_SETCIMG to 0x800DE080), draws the
    // character's silhouette into it, then loads that buffer as a texture (G_SETTIMG, the same
    // address) and lays it over the patch of ground under the character. Its tile wraps at 64 both
    // ways (cms/cmt 0, masks 6), so a sample just past an edge of the patch reads the buffer's
    // other side, and RT64 draws the buffer at the resolution's scale, so its edges change with it.
    // CONKER_LINE_TEST picks a test, to tell what the line is on the reporter's machine:
    //   noshadow  no shadows: does the line go with them?
    //   clamp     the shadow's texture clamped at its edges instead of wrapped
    //   noinset   without the inset that took the line away (rt64.patch, rt64_rsp.cpp: the shadow's texture
    //             coordinates pulled in 2 texels from each edge, now always), to compare
    //   outline   the patch's edges shown as a hard border (rt64.patch), to compare with the line
    // CONKER_SHADOW_DUMP prints the commands of the first few shadows drawn.
    static const char* line_test = std::getenv("CONKER_LINE_TEST");
    static const bool no_shadow_test = line_test != nullptr && std::strcmp(line_test, "noshadow") == 0;
    static const bool clamp_test = line_test != nullptr && std::strcmp(line_test, "clamp") == 0;
    static const bool shadow_dump = std::getenv("CONKER_SHADOW_DUMP") != nullptr;
    static int shadow_dumps = 0;
    if (shadow_dump && shadow_dumps < 3 && ctx->r2 - shadow_dl_start > 6 * 8) {
        shadow_dumps++;
        std::fprintf(stderr, "[shadow dump] index %u\n", shadow_index);
        for (gpr at = shadow_dl_start; at < ctx->r2; at += 8) {
            std::fprintf(stderr, "  %08X %08X\n", (uint32_t)MEM_W(0, at), (uint32_t)MEM_W(4, at));
        }
    }
    if (clamp_test) {
        // The shadow's tiles: G_SETTILE (0xF5) of an I 8-bit texture (format 4, size 1), wrapping
        // at 64 both ways. G_TX_CLAMP (2) is set in cmt (bits 18-19 of w1) and cms (bits 8-9).
        for (gpr at = shadow_dl_start; at < ctx->r2; at += 8) {
            const uint32_t w0 = (uint32_t)MEM_W(0, at);
            const uint32_t w1 = (uint32_t)MEM_W(4, at);
            if ((w0 >> 24) == 0xF5 && ((w0 >> 21) & 7) == 4 && ((w0 >> 19) & 3) == 1 && (w1 & 0x000FFFFF) == 0x00018060) {
                MEM_W(4, at) = (int32_t)(w1 | (2u << 18) | (2u << 8));
            }
        }
    }
    if (no_shadow_test) {
        shadow_last_drawn[shadow_index] = game_frames;
        ctx->r2 = shadow_dl_start;
        return;
    }
    const auto last = shadow_last_drawn.find(shadow_index);
    const bool back_after_gap = last == shadow_last_drawn.end() || game_frames - last->second > 1;
    shadow_last_drawn[shadow_index] = game_frames;
    if (back_after_gap) {
        ctx->r2 = shadow_dl_start;
        return;
    }
    gpr dl = ctx->r2;
    put_pop(rdram, dl);
    ctx->r2 = dl;
}
