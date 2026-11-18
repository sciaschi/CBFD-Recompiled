// Camera: Field of View (Camera tab, issue #65): widens or narrows the normal third-person
// camera's view, since with Right Stick: Free Camera the stick no longer zooms.
//
// func_1510B128 sets a camera's field of view each frame (D_800BE628, 0x180-byte cameras): the
// horizontal one in use (+0x74) and the vertical (+0x78), from the base ones (+0x6C, +0x70) plus
// its zoom and a global offset (D_800C3648). It then works out the projection (func_1510B5F8)
// and more from them, and the frustum the world is culled against (func_1501B22C) and the sky's
// sectors (widescreen.cpp) read them later. conker.toml calls conker_field_of_view just after
// both are stored: both are widened there by as much as the setting's vertical field of view is
// wider than the camera's base one (+0x70, 50 degrees; the horizontal base is 60.6 at 4:3), so
// the game's own zooming carries on around it, and everything after follows. The setting is the
// vertical field of view as it stays the same whatever the window's aspect ratio.
//
// The one other thing that depends on them is the scale the level's culls compare view-space
// x and y with (updateCullScales_1510B958): the game moves it linearly with the field of view,
// close enough for its own zooms but not for a much wider view, where the level's pieces at the
// sides were culled while in view. So at its return, with the setting in use, it's worked out
// from the game's own field of view instead, divided by how much wider the view is
// (conker::field_of_view::adjust_cull_scales, before widescreen.cpp widens it for the window).
//
// Only player 1's camera (index 0) in the normal camera (where the C-buttons turn it: not
// R-Look, aiming, cutscenes or other special cameras, so the scope's zoom and cutscenes' framing
// are the game's). Changing between them eases the change over a few frames.
//
// While the harness is used (netplay, recordings, replays), every player's camera is widened by that
// player's own setting instead, sent with their input for each read (their Controls, netplay/motion.cpp):
// it changes the game's memory (its culls, the aiming's zoom), so every machine applies every player's
// the same way. The settings menus' own value isn't read then (the netplay profile, look_aim.cpp).
// How far each camera has eased, and what its last update widened, are kept in save states (host_state.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "recomp.h"

#include "conker.hpp"
#include "game_trig.hpp"

namespace {
    constexpr uint32_t cameras = 0x800BE628; // D_800BE628, a pointer to the cameras
    constexpr uint32_t camera_size = 0x180;
    constexpr uint32_t cull_scales = 0x800D35E0; // cullScaleX_800D35E0, then y
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    constexpr float max_fov = 150.0f;
    // Each frame, the widening moves this fraction of the way to the setting's.
    constexpr float ease = 0.25f;

    // Player 1's camera, or with the harness each player's (the cameras' index in D_800BE628).
    constexpr int fov_cameras = 4;
    float widening[fov_cameras] = { 1.0f, 1.0f, 1.0f, 1.0f };  // how much each view is widened now (tan of the half angle)
    float game_fov[fov_cameras][2] = {};    // the game's own fields of view (x, y) before them
    float widened[fov_cameras][2] = { { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f } };  // how much wider they made them

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }

    float half_tan(float degrees) {
        return conker::game_trig::tan(degrees * 0.5f * degrees_to_radians);
    }

    // The degrees whose half angle's tangent is t.
    float from_half_tan(float t) {
        return 2.0f * conker::game_trig::atan(t) / degrees_to_radians;
    }

    // The setting's vertical field of view, or 0 without one (the build without the menus).
    float setting_degrees() {
#if defined(CONKER_RT64)
        return conker::look_aim::camera_field_of_view();
#else
        return 0.0f;
#endif
    }
}

// func_1510B128, at 0x1510B1A8: both fields of view are stored, $v0 is the camera and $s0 its
// offset in the array.
extern "C" void conker_field_of_view(uint8_t* rdram, recomp_context* ctx) {
    const bool harness = conker::netplay_profile::active();
    const uint32_t offset = (uint32_t)ctx->r16;
    const int index = (int)(offset / camera_size);
    if (offset % camera_size != 0 || index >= (harness ? fov_cameras : 1)) {
        return;
    }
    const gpr camera = ctx->r2;
    const float base_y = read_float(rdram, camera, 0x70);
    const float setting = harness ? conker::motion::field_of_view(conker::motion::controls(index)) : setting_degrees();
    const bool normal = harness ? conker::mouse_camera::normal_camera_of(index) : conker::mouse_camera::normal_camera();
    float target = 1.0f;
    if (normal && setting > 0.0f && base_y > 0.0f && base_y < 180.0f) {
        target = half_tan(setting) / half_tan(base_y);
    }
    float& w = widening[index];
    w += (target - w) * ease;
    if (std::fabs(w - target) < 0.001f) {
        w = target;
    }
    const float x = read_float(rdram, camera, 0x74);
    const float y = read_float(rdram, camera, 0x78);
    game_fov[index][0] = x;
    game_fov[index][1] = y;
    widened[index][0] = widened[index][1] = 1.0f;
    if (w == 1.0f || x <= 0.0f || y <= 0.0f || x >= 180.0f || y >= 180.0f) {
        return;
    }
    const float wide_x = std::min(from_half_tan(half_tan(x) * w), max_fov);
    const float wide_y = std::min(from_half_tan(half_tan(y) * w), max_fov);
    write_float(rdram, camera, 0x74, wide_x);
    write_float(rdram, camera, 0x78, wide_y);
    widened[index][0] = half_tan(wide_x) / half_tan(x);
    widened[index][1] = half_tan(wide_y) / half_tan(y);
}

void conker::field_of_view::adjust_cull_scales(uint8_t* rdram, uint64_t camera_address) {
    const gpr camera = (gpr)camera_address;
    const gpr first = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)cameras);
    const uint32_t offset = (uint32_t)camera - (uint32_t)first;
    const int index = (int)(offset / camera_size);
    if ((uint32_t)camera < (uint32_t)first || offset % camera_size != 0 ||
        index >= (conker::netplay_profile::active() ? fov_cameras : 1) ||
        (widened[index][0] == 1.0f && widened[index][1] == 1.0f)) {
        return;
    }
    // The game's scales at its own fields of view (as updateCullScales_1510B958 works them out),
    // then as much smaller as the view is wider.
    const gpr scales = (gpr)(int32_t)cull_scales;
    for (int i = 0; i < 2; i++) {
        const float base_fov = read_float(rdram, camera, 0x6C + i * 4);
        const float base_scale = read_float(rdram, camera, 0x64 + i * 4);
        if (base_fov <= 0.0f) {
            continue;
        }
        const float scale = base_scale - (game_fov[index][i] / base_fov - 1.0f);
        write_float(rdram, scales, i * 4, scale / widened[index][i]);
    }
}

// Each camera's easing, and its fields of view and widening as its last update left them: the cull scales are
// worked out from those when the camera is drawn (adjust_cull_scales), which a state can fall between.
namespace {
    constexpr size_t fov_state_words = (sizeof(widening) + sizeof(game_fov) + sizeof(widened)) / 4;
}

std::vector<uint32_t> conker::field_of_view::state() {
    std::vector<uint32_t> words(fov_state_words);
    uint8_t* at = (uint8_t*)words.data();
    std::memcpy(at, widening, sizeof(widening));
    std::memcpy(at + sizeof(widening), game_fov, sizeof(game_fov));
    std::memcpy(at + sizeof(widening) + sizeof(game_fov), widened, sizeof(widened));
    return words;
}

void conker::field_of_view::set_state(const std::vector<uint32_t>& words) {
    if (words.size() != fov_state_words) {
        std::fill(std::begin(widening), std::end(widening), 1.0f);
        for (auto& fov : game_fov) fov[0] = fov[1] = 0.0f;
        for (auto& w : widened) w[0] = w[1] = 1.0f;
        return;
    }
    const uint8_t* at = (const uint8_t*)words.data();
    std::memcpy(widening, at, sizeof(widening));
    std::memcpy(game_fov, at + sizeof(widening), sizeof(game_fov));
    std::memcpy(widened, at + sizeof(widening) + sizeof(game_fov), sizeof(widened));
}
