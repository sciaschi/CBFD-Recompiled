// Netplay's "my view only": in a match, each machine still runs the whole game, split
// screen and all (every player's view is drawn on every machine, so their games stay the same), but the
// window can show only this machine's players' views. RT64 draws them anew (rt64.patch, RT64::setViewFocusRedraw):
// one player's view across the whole frame, two players' across its top and bottom halves (in the order of their
// ports), each with its projection scaled to match so it isn't stretched, and leaves the other views out
// (widescreen then widens each as any view across the frame's width). With three players on one machine the whole
// split screen is shown. Only the renderer changes, not game memory, so it can't desync. It's on when the player
// chooses it, or when the host's rule for the match is that everyone sees only their own view (lockstep.hpp: the
// default; a replay's recording says whether its match had it). That rule can't be enforced against a modified
// build (every machine draws every view); it keeps honest players on the same footing.
//
// A spectator (no ports of its own) sees the whole split screen, or one player's view across the window, as it
// chooses (lockstep::watched_ports: L and R on its controller go from one to the next), whatever the match's rule.
//
// A view drawn this way shows more of the world than the game culled it for: above and below for a view made
// taller, at the sides for one made wider. So while the rule is on, the game's culls for every view are made as
// tall and as wide on every machine (widescreen.cpp's own_view_scale: game memory, so by the match's rule and its
// layout, which every machine has, never by a player's own choice alone).
//
// While the game is paused its menu is drawn over the whole split screen, so the picture is left as the game
// draws it (the pause counter D_80082F90, widescreen.cpp). The menu's background is the game's copy of the last
// frame (D_800BE9C4, made as the pause starts, widescreen.cpp), drawn into an image as wide as the frame: RT64 is
// told it isn't the frame, so the copy is left alone, and shows this machine's views as they were drawn. (Taken
// for the frame, the copy's tiles moved with the views or were left out, and the picture behind the menu was
// what was in memory: green and pink stripes.) The motion blur's copy (D_800BE570) likewise. D_800BE9C4 is the
// depth image too (func_1501A39C's G_SETZIMG), cleared each frame across the whole frame (func_151106A8), which
// the focus left as it was anyway.
//
// Each frame, the game's cameras (D_800BE628, 0x180 bytes each, the same as reticle.cpp's and
// widescreen.cpp's) say where each player's view is on the N64's screen: + 0x24 top, + 0x28 bottom,
// + 0x2C left, + 0x30 right, floats, on the frame (D_800BE620 wide, 216 high). While player 1's camera
// isn't full screen the split is on, and this machine's ports' cameras are the parts shown. The game's
// layouts (292 by 216): 2 players, top and bottom halves (30 to 262 across); 3, the top two quarters and the
// bottom middle (74 to 218); 4, the quarters.

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "recomp.h"

#include "../conker.hpp"
#include "lockstep.hpp"

namespace RT64 {
    // rt64.patch: `count` views of the split screen (1 or 2) drawn anew elsewhere in the frame, the others not
    // drawn. Each 8 floats of `rects`: where the game draws it, then where it's drawn (left, top, right, bottom, in
    // pixels of the frame); not in the two images given, the game's own copies of the frame.
    void setViewFocusRedraw(bool on, int count, const float* rects, float frameWidth, float frameHeight,
        uint32_t ownImage0, uint32_t ownImage1);
}

namespace {
    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr gpr frame_width_address = (gpr)(int32_t)0x800BE620;
    constexpr gpr pause_counter_address = (gpr)(int32_t)0x80082F90;
    constexpr gpr pause_picture_address = (gpr)(int32_t)0x800BE9C4;   // D_800BE9C4, the pause's copy of the frame
    constexpr gpr motion_blur_copy_address = (gpr)(int32_t)0x800BE570; // D_800BE570, the motion blur's
    constexpr uint32_t camera_size = 0x180;
    constexpr int32_t camera_top = 0x24, camera_bottom = 0x28, camera_left = 0x2C, camera_right = 0x30;
    constexpr float frame_height = 216.0f;

    std::atomic<bool> mine_only = false;
    // Replaying (to try it): the players whose views are shown (bit n: port n), as on a machine with them.
    std::atomic<uint8_t> replay_ports = 1;
    // What's shown: the views' rectangles (shares of the frame), none for the whole picture as it is.
    using Rect = std::array<float, 4>;
    std::vector<Rect> shown;

    float read_float(uint8_t* rdram, gpr address) {
        const uint32_t bits = (uint32_t)MEM_W(0, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Draws the views given (shares of the frame), each across a part of the frame of its own: one across the whole
    // frame, two across its top and bottom halves (the line between them left as the game's 2-player split leaves
    // it, for the timer and scores the views share). None draws the picture as it is.
    void show(uint8_t* rdram, const std::vector<Rect>& views, int32_t frame_width) {
        const float w = (float)frame_width;
        const float h = frame_height;
        const Rect whole = { 0, 0, w, h };
        const Rect halves[2] = { { 0, 0, w, h / 2 - 1 }, { 0, h / 2 + 1, w, h } };
        std::vector<float> rects;
        for (size_t i = 0; i < views.size(); i++) {
            const Rect& v = views[i];
            const Rect& to = views.size() == 1 ? whole : halves[i];
            rects.insert(rects.end(), { v[0] * w, v[1] * h, v[2] * w, v[3] * h, to[0], to[1], to[2], to[3] });
        }
        RT64::setViewFocusRedraw(!views.empty(), (int)views.size(), rects.data(), w, h,
            (uint32_t)MEM_W(0, pause_picture_address), (uint32_t)MEM_W(0, motion_blur_copy_address));
        if (views != shown) {
            shown = views;
            if (views.empty()) {
                std::printf("[netplay] drawing the whole picture\n");
            }
            for (size_t i = 0; i < views.size(); i++) {
                std::printf("[netplay] drawing %.3f %.3f %.3f %.3f of the picture across %s\n", views[i][0], views[i][1],
                    views[i][2], views[i][3], views.size() == 1 ? "it" : i == 0 ? "its top half" : "its bottom half");
            }
        }
    }
}

void conker::netplay_view::set_mine_only(bool on) {
    mine_only = on;
}

bool conker::netplay_view::is_mine_only() {
    return mine_only;
}

void conker::netplay_view::set_replay_ports(uint8_t ports) {
    replay_ports = ports == 0 ? 1 : ports;
}

void conker::netplay_view::game_frame(uint8_t* rdram) {
    // In a match, this machine's ports (a spectator's, the one it watches: none, everyone's); replaying a recording (to
    // try it), --my-view-ports' (player 1's).
    const bool in_match = conker::lockstep::connected();
    const bool watching = in_match && conker::lockstep::spectating();
    const uint8_t ports = watching ? conker::lockstep::watched_ports() : in_match ? conker::lockstep::ports() : replay_ports.load();
    const int count = std::popcount(ports);
    const uint32_t cameras = (uint32_t)MEM_W(0, cameras_pointer);
    const int32_t frame_width = MEM_W(0, frame_width_address);
    // The player's choice, or the match's rule (the host's) that everyone sees only their own view; a spectator's.
    const bool wanted = watching || mine_only || conker::harness::own_view_rule();
    if (!wanted || !(in_match || conker::harness::replaying()) || count < 1 || count > 2 || cameras < 0x80000000u ||
        cameras >= 0x80800000u || frame_width < 16 || frame_width > 640) {
        show(rdram, {}, frame_width);
        return;
    }
    auto rect = [&](int index) {
        const gpr camera = (gpr)(int32_t)(cameras + index * camera_size);
        Rect out = { read_float(rdram, camera + camera_left) / (float)frame_width, read_float(rdram, camera + camera_top) / frame_height,
            read_float(rdram, camera + camera_right) / (float)frame_width, read_float(rdram, camera + camera_bottom) / frame_height };
        for (float& v : out) {
            if (!std::isfinite(v)) v = 0;
        }
        return out;
    };
    // Paused: the pause menu is drawn over the whole split screen, so it's shown as the game draws it.
    // (D_80082F90 counts the frames since the pause, widescreen.cpp; 0 while not paused.)
    const uint32_t pause_counter = (uint32_t)MEM_W(0, pause_counter_address);
    if (pause_counter != 0) {
        show(rdram, {}, frame_width);
        return;
    }
    const Rect first = rect(0);
    // Player 1's view reaches every side (to within the 2-pixel border): not split.
    const bool split = !(first[0] < 0.03f && first[1] < 0.03f && first[2] > 0.97f && first[3] > 0.97f);
    if (!split) {
        show(rdram, {}, frame_width);
        return;
    }
    // This machine's players' views, in the order of their ports; any that isn't a real part of the split (Race's
    // third and fourth players have none) shows the whole picture.
    std::vector<Rect> views;
    for (int port = 0; port < 4; port++) {
        if ((ports & (1u << port)) == 0) {
            continue;
        }
        const Rect own = rect(port);
        if (own[2] - own[0] < 0.05f || own[3] - own[1] < 0.05f || (own[2] - own[0] > 0.97f && own[3] - own[1] > 0.97f)) {
            show(rdram, {}, frame_width);
            return;
        }
        views.push_back(own);
    }
    show(rdram, views, frame_width);
}
