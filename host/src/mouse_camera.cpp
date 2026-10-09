// Mouse camera for keyboard and mouse players: a free orbit camera, called from hooks
// in conker.toml.
//
// RecompFrontend's Mouse Sensitivity option (on the Mouse & Gyro tab, look_aim.cpp). Above 0, the cursor is
// captured while the game is played (and released in the menus), and
// recompinput::get_mouse_deltas() gives the mouse's movement since the game last read
// its controllers, scaled by that sensitivity.
//
// The follow camera (struct108: gObjects[0].camera for player 1, D_800DBFF0 the one
// being played) looks at a point (+0x2BC) above its pivot at Conker's feet (+0x2A4),
// from an eye kept at a horizontal distance (+0x374) and height (+0x344) from the
// pivot. It doesn't keep its angle: every frame func_15125330 works it out (+0x37C)
// from where the eye is, and Conker's movement is relative to it. So once the mouse
// moves, the eye the camera wants (+0x2F8) is placed each frame from our own yaw and
// pitch around the look-at point, and the rest of the game follows. The camera stays
// where the mouse leaves it.
//
// Walls: the eye is placed as the game's camera collision (func_1512BB10) starts, the
// way the C-buttons' turning places it earlier in the same update (func_15122C5C). The
// collision moves the camera from where it was drawn last frame (+0x304) toward that eye
// and stops it at walls, sliding along them, and leaves the result in +0x2F8. The view
// (func_151284C4, in func_1512C490) then draws from there (+0x2EC). So the orbit stops at
// walls the way the game's own camera does.
//
// The orbit only runs where the C-buttons turn the camera (func_1512D390 ran this
// frame) and not in the look mode (func_15120158: hold R, aiming), so cutscenes, special
// cameras and aiming are the game's. Turning with C-left or C-right (or the stick) alone
// hands the camera back to the game until the mouse moves again. Turning with both, the
// orbit takes the C-buttons' turn too (issue #60: the stick stopped turning the camera while
// the mouse, or gyro sent as the mouse, moved). The Mouse: Turn the Camera setting
// (look_aim.cpp) turns the orbit off.
//
// Right Stick: Free Camera (look_aim.cpp, issue #65) turns the orbit with player 1's right
// stick too, as a modern third-person camera: left and right turn it, up and down tilt it, at
// a rate (degrees per second) the stick's tilt sets. While the orbit could run last frame,
// the controller's input leaves the right stick off the C-buttons (frontend.cpp), so the
// game's own turning doesn't fight it. Elsewhere (R-Look, aiming, cutscenes, other cameras)
// the stick is the C-buttons as usual. The distance follows the game's own (C-Down zooms)
// until the scroll wheel sets one.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <SDL.h>

#include "recomp.h"
#include "recompinput/input_state.h"

#include "conker.hpp"

namespace {
    // Degrees per pixel of mouse movement at 100% sensitivity.
    constexpr float degrees_per_pixel = 0.2f;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    // How far the camera may look up or down: pitch is the eye's angle above the
    // look-at point.
    constexpr float min_pitch = -25.0f * degrees_to_radians;
    constexpr float max_pitch = 75.0f * degrees_to_radians;
    constexpr uint32_t current_camera = 0x800DBFF0; // D_800DBFF0
    // Scroll wheel zoom: each notch scales the distance by this, between the nearest and
    // farthest of the game's own camera distances (D_800A34B0: the controller's four,
    // each a horizontal distance and a height from the pivot, 530 x 400 the farthest).
    constexpr float zoom_step = 1.12f;
    constexpr uint32_t camera_distances = 0x800A34B0; // D_800A34B0, 4 x { horizontal, height }
    constexpr int camera_distance_count = 4;
    // When the camera is behind the eye the orbit wants (held by a wall, or freed after one),
    // it's sent this fraction of the way each frame, and a gap this many units larger than the
    // orbit's own move counts as behind. The collision only takes the C-buttons' camera a
    // little way each frame; one long move from where a wall held it jumped about.
    constexpr float catch_up = 0.3f;
    constexpr float behind_slack = 8.0f;
    // The right stick at full tilt, at 100% Camera: Turning Speed: degrees per second turned and
    // tilted. Inside the deadzone it's still; past it, the tilt is rescaled from 0 and squared, for
    // fine control near the middle.
    constexpr float stick_yaw_speed = 150.0f;
    constexpr float stick_pitch_speed = 90.0f;
    constexpr float stick_deadzone = 0.2f;
    // A longer gap between two frames' camera updates (a pause, loading) counts as this long.
    constexpr float max_frame_seconds = 0.1f;

    // Set at the end of each camera update, read by the input thread (frontend.cpp).
    std::atomic<bool> stick_owns_camera = false;
    // Set at the end of each camera update: it was the normal camera.
    bool was_normal_camera = false;

    // Scroll wheel notches since the view last read them (SDL event watch: the
    // frontend's own event loop consumes the events).
    std::atomic<int> wheel_notches = 0;
    int SDLCALL watch_wheel(void*, SDL_Event* event) {
        if (event->type == SDL_MOUSEWHEEL) {
            wheel_notches.fetch_add(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y);
        }
        return 1;
    }

    struct Orbit {
        bool engaged = false;
        bool follow_camera_ran = false; // func_1512D390 ran since the last view
        bool look_mode_ran = false;     // func_15120158 (hold R, aiming) ran since the last view
        bool c_turning = false;         // C-left or C-right held when func_1512D390 ran
        bool turned = false;            // the mouse and wheel were read since the last view
        bool moved = false;             // and they moved
        bool has_target = false;        // target and next_target hold eyes the orbit wanted
        float target[3] = {};           // the eye the orbit wanted last frame
        float next_target[3] = {};      // this frame's, kept as target once the frame ends
        float yaw = 0.0f;               // radians, the eye's direction from the look-at point
        float pitch = 0.0f;
        float wanted = 0.0f;            // the distance from the look-at point
        bool wheel_zoomed = false;      // the scroll wheel set it (else it follows the game's)
        bool has_stick_time = false;    // stick_time holds the last read of the stick
        std::chrono::steady_clock::time_point stick_time;
    } orbit;

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

    // The yaw of the eye at camera + offset around the look-at point (cx, cz), as orbit.yaw.
    float eye_yaw(uint8_t* rdram, gpr camera, int32_t offset, float cx, float cz) {
        return std::atan2(read_float(rdram, camera, offset + 8) - cz, read_float(rdram, camera, offset) - cx);
    }

    bool mouse_turns_camera() {
#if defined(CONKER_RT64)
        return conker::look_aim::mouse_turns_camera();
#else
        return true;
#endif
    }

    bool stick_turns_camera_setting() {
#if defined(CONKER_RT64)
        return conker::look_aim::stick_free_camera();
#else
        return false;
#endif
    }

    // The orbit's turn (yaw) and tilt (pitch) from player 1's right stick since the last read, in
    // radians: its tilt past the deadzone, at Camera: Turning Speed and Invert Turning, for the time
    // since the last read. Right turns the way the mouse moving right does; up tilts to look up.
    void read_stick(float& yaw, float& pitch) {
        yaw = 0.0f;
        pitch = 0.0f;
        const auto now = std::chrono::steady_clock::now();
        const float seconds = orbit.has_stick_time ?
            std::min(std::chrono::duration<float>(now - orbit.stick_time).count(), max_frame_seconds) : 0.0f;
        orbit.stick_time = now;
        orbit.has_stick_time = true;
#if defined(CONKER_RT64)
        std::array<SDL_GameController*, 4> controllers{};
        if (conker::frontend::port_controllers(controllers) < 1 || controllers[0] == nullptr) {
            return;
        }
        const float x = SDL_GameControllerGetAxis(controllers[0], SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        const float y = SDL_GameControllerGetAxis(controllers[0], SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        const float tilt = std::sqrt(x * x + y * y);
        if (tilt <= stick_deadzone) {
            return;
        }
        const float scaled = std::min((tilt - stick_deadzone) / (1.0f - stick_deadzone), 1.0f);
        const float strength = scaled * scaled / tilt;
        bool invert_x = false, invert_y = false;
        conker::look_aim::free_camera_invert(invert_x, invert_y);
        const float speed = conker::look_aim::camera_turn_speed() * seconds * degrees_to_radians;
        yaw = x * strength * stick_yaw_speed * speed * (invert_x ? -1.0f : 1.0f);
        pitch = y * strength * stick_pitch_speed * speed * (invert_y ? -1.0f : 1.0f);
#endif
    }
}

bool conker::mouse_camera::stick_turns_camera() {
    return stick_owns_camera.load(std::memory_order_relaxed);
}

bool conker::mouse_camera::normal_camera() {
    return was_normal_camera;
}

// func_1512D390 (the C-buttons' turning), before its last restore: $s0 is the camera.
// Marks that the follow camera is running this frame, and whether C-left or C-right is held
// (+0x36C points at the buttons held).
extern "C" void conker_mouse_camera_follow(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    orbit.follow_camera_ran = true;
    const gpr buttons = (gpr)(int32_t)MEM_W(0x36C, camera);
    orbit.c_turning = ((uint32_t)MEM_HU(0, buttons) & 0x3) != 0;
}

// func_15120158 (the look mode: hold R, and aiming such as the slingshot on a B pad), after
// its first instruction. The mouse aims there (look_aim.cpp), so the orbit leaves the
// camera to it: otherwise both turned with the mouse, and the view ran ahead of the aim.
extern "C" void conker_mouse_camera_look_mode(uint8_t* rdram, recomp_context* ctx) {
    orbit.look_mode_ran = true;
}

// func_1512BB10 (the camera's collision), after its first instruction: $a0 is the camera.
// Places the eye the orbit wants, for the collision to move the camera toward. The game
// calls it a second time in some frames (camera +0x23C set): the mouse and wheel are read
// only the first time, and the second places the same eye. The C-buttons' turning has
// placed the eye the game wants by now (+0x2F8), turned from last frame's (+0x304).
extern "C" void conker_mouse_camera_collide(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    const bool use_mouse = mouse_turns_camera();
    const bool use_stick = stick_turns_camera_setting();
    if (!orbit.follow_camera_ran || orbit.look_mode_ran || (!use_mouse && !use_stick)) {
        orbit.engaged = false;
        orbit.has_stick_time = false;
        wheel_notches = 0;
        return;
    }

    float mouse_x = 0.0f, mouse_y = 0.0f;
    float stick_yaw = 0.0f, stick_pitch = 0.0f;
    int notches = 0;
    const bool first = !orbit.turned;
    if (first) {
        if (use_mouse) {
            recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
            notches = wheel_notches.exchange(0);
        }
        else {
            wheel_notches = 0;
        }
        if (use_stick) {
            read_stick(stick_yaw, stick_pitch);
        }
        orbit.turned = true;
        orbit.moved = mouse_x != 0.0f || mouse_y != 0.0f || notches != 0 || stick_yaw != 0.0f || stick_pitch != 0.0f;
        orbit.wheel_zoomed |= notches != 0;
    }
    // The C-buttons alone turn the game's camera.
    if (orbit.c_turning && !orbit.moved) {
        orbit.engaged = false;
        return;
    }

    const float cx = read_float(rdram, camera, 0x2BC);
    const float cy = read_float(rdram, camera, 0x2C0);
    const float cz = read_float(rdram, camera, 0x2C4);
    // The distance the game keeps: its eye's horizontal distance and height from the
    // pivot, measured from the look-at point.
    const float horizontal = read_float(rdram, camera, 0x374);
    const float height = read_float(rdram, camera, 0x344) - (cy - read_float(rdram, camera, 0x2A8));
    const float wanted_distance = std::sqrt(horizontal * horizontal + height * height);

    if (!orbit.engaged) {
        if (!orbit.moved) {
            return;
        }
        // Take over from where the game's camera was drawn.
        const float ex = read_float(rdram, camera, 0x2EC) - cx;
        const float ey = read_float(rdram, camera, 0x2F0) - cy;
        const float ez = read_float(rdram, camera, 0x2F4) - cz;
        orbit.yaw = std::atan2(ez, ex);
        orbit.pitch = std::atan2(ey, std::sqrt(ex * ex + ez * ez));
        if (orbit.wanted == 0.0f) {
            orbit.wanted = wanted_distance;
        }
        orbit.engaged = true;
    }
    // Until the scroll wheel sets a distance, the orbit keeps the game's, so C-Down zooms it.
    if (!orbit.wheel_zoomed) {
        orbit.wanted = wanted_distance;
    }

    // The controller's nearest and farthest distances from the look-at point.
    const float look_height = cy - read_float(rdram, camera, 0x2A8);
    float nearest = 0.0f, farthest = 0.0f;
    for (int i = 0; i < camera_distance_count; i++) {
        const gpr preset = (gpr)(int32_t)(camera_distances + i * 8);
        const float h = read_float(rdram, preset, 0), v = read_float(rdram, preset, 4) - look_height;
        const float d = std::sqrt(h * h + v * v);
        nearest = (i == 0) ? d : std::min(nearest, d);
        farthest = (i == 0) ? d : std::max(farthest, d);
    }
    orbit.wanted = std::clamp(orbit.wanted * std::pow(zoom_step, (float)-notches), nearest, farthest);
    orbit.yaw += mouse_x * degrees_per_pixel * degrees_to_radians + stick_yaw;
    if (first && orbit.c_turning) {
        // The C-buttons' turn this frame: as far as the game turned its own eye.
        constexpr float two_pi = 2.0f * 3.14159265358979f;
        orbit.yaw += std::remainder(eye_yaw(rdram, camera, 0x2F8, cx, cz) - eye_yaw(rdram, camera, 0x304, cx, cz), two_pi);
    }
    orbit.pitch = std::clamp(orbit.pitch + mouse_y * degrees_per_pixel * degrees_to_radians + stick_pitch, min_pitch, max_pitch);

    // The eye's direction from the look-at point.
    const float dx = std::cos(orbit.pitch) * std::cos(orbit.yaw);
    const float dy = std::sin(orbit.pitch);
    const float dz = std::cos(orbit.pitch) * std::sin(orbit.yaw);

    // Where the game pulls its own camera in closer than the controller can (tight spots),
    // so does the orbit.
    const float zoomed = (wanted_distance < nearest) ? std::min(orbit.wanted, wanted_distance) : orbit.wanted;
    const float target[3] = { cx + dx * zoomed, cy + dy * zoomed, cz + dz * zoomed };

    // The collision moves the camera from last frame's eye (+0x304). If that's further from
    // the eye wanted now than the orbit itself moved since last frame, the camera is behind:
    // ease it there rather than sending it the whole way at once.
    float gap = 0.0f, moved = 0.0f;
    float from[3];
    for (int i = 0; i < 3; i++) {
        from[i] = read_float(rdram, camera, 0x304 + i * 4);
        gap += (target[i] - from[i]) * (target[i] - from[i]);
        const float step = orbit.has_target ? (target[i] - orbit.target[i]) : 0.0f;
        moved += step * step;
    }
    const bool behind = orbit.has_target && (std::sqrt(gap) > std::sqrt(moved) + behind_slack);
    for (int i = 0; i < 3; i++) {
        const float eye = behind ? from[i] + (target[i] - from[i]) * catch_up : target[i];
        write_float(rdram, camera, 0x2F8 + i * 4, eye);
        orbit.next_target[i] = target[i];
    }
}

// func_151284C4 (builds the view), after its first instruction: $a0 is the camera. The
// frame's camera update is done: start over for the next one.
extern "C" void conker_mouse_camera(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
    }
    // For the controller's input until the next update: whether the stick turns this camera.
    was_normal_camera = orbit.follow_camera_ran && !orbit.look_mode_ran;
    stick_owns_camera = was_normal_camera && stick_turns_camera_setting();
    // The eye wanted this frame, to tell next frame how far the orbit itself moved.
    orbit.has_target = orbit.engaged;
    std::memcpy(orbit.target, orbit.next_target, sizeof(orbit.target));
    orbit.follow_camera_ran = false;
    orbit.look_mode_ran = false;
    orbit.c_turning = false;
    orbit.turned = false;
    orbit.moved = false;
}

// frontend.cpp, once SDL is up: listen for the scroll wheel.
void conker_mouse_camera_init() {
    SDL_AddEventWatch(watch_wheel, nullptr);
}
