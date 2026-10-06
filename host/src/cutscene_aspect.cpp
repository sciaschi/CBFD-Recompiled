// The Cutscene Aspect Ratio setting (Graphics tab): cutscenes in 4:3, with black bars at the
// sides, and the rest of the game in the window's aspect ratio. Cutscenes are framed for the N64's
// 4:3 screen, and in widescreen characters waiting for their cue can be seen beside the picture.
//
// Every cutscene counts: the story scenes, conversations, B pads' hints and Conker's thought-bubble
// tips, anything the cutscene system plays (a slot's byte at D_800C35EA is 1), during which the
// player can't move. The game marks no difference between a story scene and a hint (issue #21:
// most story scenes' scripts mark them unskippable, command 0x0E, but meeting Birdy, the gargoyle
// and others don't, and the same trigger events start them all), and fixed cameras in play aren't
// cutscenes, so this never switches while the player is in control.
//
// The picture stays widescreen while one plays, and black bars are drawn over its sides, beyond the
// 4:3 frame: RT64 widens the view by showing more at the sides at the same height, so the 4:3
// frame in the middle is the picture the N64 shows. At the end of each frame's display list
// (func_1501878C, just before its full sync; conker.toml), the game's list calls a list of our
// own that draws two of RT64's extended fill rectangles, from far past the frame's left edge to
// it and from its right edge on. The first version switched the renderer's aspect ratio to 4:3
// for the cutscene instead, and back after: RT64 then discards and remakes every framebuffer at
// the new size, a moment with both sets in video memory, and in fullscreen at 4K a tester's game
// crashed at those switches (issue #21). Drawing the bars changes nothing in the renderer.

#include <cstdint>

#include "recomp.h"
#include "ultramodern/config.hpp"

#include "conker.hpp"

#if defined(CONKER_RT64)
#include "recompui/config.h"
#endif

namespace {
    constexpr uint32_t cutscene_playing = 0x800C35EA;   // D_800C35EA, a byte per slot
    constexpr uint32_t frame_width = 0x800BE620;        // D_800BE620, pixels (292 in play)
    constexpr uint32_t frame_height = 0x800BE624;       // D_800BE624, pixels (216 in play)

    // RT64's extended GBI for F3DEX2 (as in widescreen.cpp): the no-op carries its enable.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_fillrect_v1 = 0x000003;
    constexpr uint32_t g_ex_origin_none = 0x800;
    // How far past the frame's sides the bars reach, in N64 pixels: past any window's edge.
    constexpr int32_t bar_reach = 2048;
    // And how far into the frame: the game's cameras draw 2 pixels short of each side (the 3D's
    // scissor, and the black of a closed iris wipe), a border the N64 shows black. RT64 draws the
    // widened scene into it, which showed as thin lines at the 4:3 edges between scenes.
    constexpr int32_t bar_overlap = 2;

    // Our display lists, in RDRAM past the game's 8 MB (next to widescreen.cpp's, from
    // 0x00F00000): a ring, as RT64 has long finished with one when it comes round again.
    constexpr uint32_t bars_dl_start = 0x00F10000;
    constexpr uint32_t bars_dl_slot = 0x80;
    constexpr uint32_t bars_dl_slots = 32;
    uint32_t bars_dl_next = 0;

    bool pillarbox = false; // this frame is a cutscene shown in 4:3

    bool cutscene_playing_now(uint8_t* rdram) {
        return MEM_BU(0, (gpr)(int32_t)cutscene_playing) == 1 || MEM_BU(1, (gpr)(int32_t)cutscene_playing) == 1;
    }

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }

    // An extended fill rectangle, corners in pixels (may be negative), no origin: its coordinates are
    // the frame's, which RT64 extends past the 4:3 edges in widescreen.
    void put_fill(uint8_t* rdram, gpr& dl, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_fillrect_v1, g_ex_origin_none | (g_ex_origin_none << 12));
        put_command(rdram, dl, ((uint32_t)(ulx * 4) << 16) | ((uint32_t)(uly * 4) & 0xFFFF),
            ((uint32_t)(lrx * 4) << 16) | ((uint32_t)(lry * 4) & 0xFFFF));
    }
}

void conker::cutscene_aspect::update(uint8_t* rdram) {
#if defined(CONKER_RT64)
    if (!conker::graphics_config_ready()) {
        pillarbox = false;
        return;
    }
    const auto chosen = static_cast<ultramodern::renderer::AspectRatio>(std::get<uint32_t>(
        recompui::config::get_graphics_config().get_option_value(recompui::config::graphics::options::ar_option)));
    pillarbox = conker::cutscene_aspect::in_4x3() && chosen != ultramodern::renderer::AspectRatio::Original &&
        cutscene_playing_now(rdram);
#else
    (void)rdram;
    pillarbox = false;
#endif
}

// func_1501878C at 0x15018C8C: the frame's display list is drawn; its full sync and end come next,
// written at the pointer $s1 points to.
extern "C" void conker_frame_dl_end(uint8_t* rdram, recomp_context* ctx) {
    if (!pillarbox) {
        return;
    }
    const int32_t width = MEM_W(0, (gpr)(int32_t)frame_width);
    const int32_t height = MEM_W(0, (gpr)(int32_t)frame_height);
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return;
    }

    const uint32_t bars_dl = bars_dl_start + bars_dl_next * bars_dl_slot;
    bars_dl_next = (bars_dl_next + 1) % bars_dl_slots;
    gpr dl = (gpr)(int32_t)(0x80000000u | bars_dl);
    put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
    put_command(rdram, dl, 0xE7000000, 0);                          // G_RDPPIPESYNC
    // The whole frame as the scissor: RT64 widens a scissor that spans the frame to the window.
    put_command(rdram, dl, 0xED000000, ((uint32_t)(width * 4) << 12) | (uint32_t)(height * 4));
    put_command(rdram, dl, 0xE3000A01, 0x00300000);                 // G_SETOTHERMODE_H: cycle type fill
    put_command(rdram, dl, 0xF7000000, 0);                          // G_SETFILLCOLOR: black
    put_fill(rdram, dl, -bar_reach, 0, bar_overlap, height);
    put_fill(rdram, dl, width - bar_overlap, 0, width + bar_reach, height);
    put_command(rdram, dl, 0xDF000000, 0);                          // G_ENDDL

    // The call, in place of where the game's full sync goes (which then follows it).
    const gpr list_pointer = ctx->r17;
    gpr game_dl = (gpr)(int32_t)MEM_W(0, list_pointer);
    put_command(rdram, game_dl, 0xDE000000, bars_dl);               // G_DL: call, and come back
    MEM_W(0, list_pointer) = (int32_t)game_dl;
}
