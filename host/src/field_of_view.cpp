// Camera: Field of View (General tab, issue #65): widens or narrows the normal third-person
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
// are the game's). Changing between them eases the change over a few frames. And only while its
// view fills the screen: in multiplayer's split screen, camera 0 is player 1's part of it, with a
// base field of view of its own, and the setting is for single player (issue #78: player 1's view
// showed no level in split screen).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t cameras = 0x800BE628; // D_800BE628, a pointer to the cameras
    constexpr uint32_t camera_size = 0x180;
    constexpr uint32_t cull_scales = 0x800D35E0; // cullScaleX_800D35E0, then y
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    constexpr float max_fov = 150.0f;
    // Each frame, the widening moves this fraction of the way to the setting's.
    constexpr float ease = 0.25f;

    float widening = 1.0f;      // how much player 1's view is widened now (tan of the half angle)
    float game_fov[2] = {};     // the game's own fields of view (x, y) before them
    float widened[2] = { 1.0f, 1.0f }; // how much wider (tan of the half angle) they made them

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
        return std::tan(degrees * 0.5f * degrees_to_radians);
    }

    // The degrees whose half angle's tangent is t.
    float from_half_tan(float t) {
        return 2.0f * std::atan(t) / degrees_to_radians;
    }

    // A camera's view fills the screen: its edges on the N64's 292 by 216 screen (+ 0x24 top,
    // + 0x28 bottom, + 0x2C left, + 0x30 right) are a full-screen camera's, 2..290 across (as
    // widescreen.cpp clips it). Split-screen views stop in the middle.
    bool full_screen(uint8_t* rdram, gpr camera) {
        return read_float(rdram, camera, 0x2C) <= 4.0f && read_float(rdram, camera, 0x30) >= 286.0f &&
            read_float(rdram, camera, 0x24) <= 4.0f && read_float(rdram, camera, 0x28) >= 210.0f;
    }

    // TEMP-DEBUG (issue #78): with CONKER_FOV_LOG set, camera 0's fields of view, edges and cull
    // scales each frame, to fov_log.txt in the working directory.
    FILE* fov_log() {
        static FILE* file = (std::getenv("CONKER_FOV_LOG") != nullptr) ? std::fopen("fov_log.txt", "w") : nullptr;
        return file;
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
    if ((uint32_t)ctx->r16 != 0) {
        return;
    }
    const gpr camera = ctx->r2;
    const float base_y = read_float(rdram, camera, 0x70);
    const float setting = setting_degrees();
    const bool whole_screen = full_screen(rdram, camera);
    float target = 1.0f;
    if (whole_screen && conker::mouse_camera::normal_camera() && setting > 0.0f && base_y > 0.0f && base_y < 180.0f) {
        target = half_tan(setting) / half_tan(base_y);
    }
    if (FILE* log = fov_log()) {
        std::fprintf(log, "fov edges=(%.0f,%.0f,%.0f,%.0f) full=%d normal=%d base=(%.2f,%.2f) in_use=(%.2f,%.2f) base_scale=(%.3f,%.3f) setting=%.1f target=%.3f widening=%.3f" "\n",
            read_float(rdram, camera, 0x2C), read_float(rdram, camera, 0x30), read_float(rdram, camera, 0x24), read_float(rdram, camera, 0x28),
            whole_screen ? 1 : 0, conker::mouse_camera::normal_camera() ? 1 : 0, read_float(rdram, camera, 0x6C), base_y,
            read_float(rdram, camera, 0x74), read_float(rdram, camera, 0x78), read_float(rdram, camera, 0x64), read_float(rdram, camera, 0x68),
            setting, target, widening);
        std::fflush(log);
    }
    widening += (target - widening) * ease;
    if (std::fabs(widening - target) < 0.001f) {
        widening = target;
    }
    const float x = read_float(rdram, camera, 0x74);
    const float y = read_float(rdram, camera, 0x78);
    game_fov[0] = x;
    game_fov[1] = y;
    widened[0] = widened[1] = 1.0f;
    if (widening == 1.0f || x <= 0.0f || y <= 0.0f || x >= 180.0f || y >= 180.0f) {
        return;
    }
    const float wide_x = std::min(from_half_tan(half_tan(x) * widening), max_fov);
    const float wide_y = std::min(from_half_tan(half_tan(y) * widening), max_fov);
    write_float(rdram, camera, 0x74, wide_x);
    write_float(rdram, camera, 0x78, wide_y);
    widened[0] = half_tan(wide_x) / half_tan(x);
    widened[1] = half_tan(wide_y) / half_tan(y);
}

void conker::field_of_view::adjust_cull_scales(uint8_t* rdram, uint64_t camera_address) {
    const gpr camera = (gpr)camera_address;
    const gpr first = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)cameras);
    if ((uint32_t)camera != (uint32_t)first || (widened[0] == 1.0f && widened[1] == 1.0f)) {
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
        const float scale = base_scale - (game_fov[i] / base_fov - 1.0f);
        write_float(rdram, scales, i * 4, scale / widened[i]);
        if (FILE* log = fov_log()) {
            std::fprintf(log, "cull axis=%d base_fov=%.2f base_scale=%.3f game_fov=%.2f widened=%.3f scale=%.3f" "\n",
                i, base_fov, base_scale, game_fov[i], widened[i], scale / widened[i]);
            std::fflush(log);
        }
    }
}
