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
//
// Looking up, the eye swings down under the look-at point only as far as it can stay above
// Conker's feet and outside him. Tilted further, the eye stays there and the view aims up
// from it instead, to 80 degrees above the horizon (conker_mouse_camera_aim, issue #70):
// otherwise the view always looked at the look-at point, and beside anything tall its top
// stayed out of view.

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

// The game's floor height under a point (game_13BB20.c), called by floor_below.
extern "C" void func_1510E8BC(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Degrees per pixel of mouse movement at 100% sensitivity.
    constexpr float degrees_per_pixel = 0.2f;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    // How far the camera may look up or down: pitch is the eye's angle above the
    // look-at point. Below it, the eye comes in toward Conker's feet (see ground_clearance),
    // as far as body_clearance lets it.
    constexpr float min_pitch = -70.0f * degrees_to_radians;
    constexpr float max_pitch = 75.0f * degrees_to_radians;
    // Looking up (the eye below the look-at point), the camera comes in closer so the eye
    // stays this share of the look-at point's height above Conker's feet (the pivot), the
    // way modern third-person cameras slide in along the ground. Otherwise the eye went into
    // the floor, the collision pushed it back out each frame and the view shook (issue #70).
    constexpr float ground_clearance = 0.13f;
    // Coming in, the eye stays at least this far from the look-at point across the ground
    // (horizontally), outside Conker's body. With ground_clearance it sets how far the camera
    // can tilt to look up: past that, it couldn't stay both above his feet and outside him.
    constexpr float body_clearance = 90.0f;
    // Tilting up past where the eye stops (the lowest pitch), the view aims up from the eye, up to
    // this angle above the horizon (issue #70: to see the top of anything tall from beside it).
    constexpr float highest_view = 80.0f * degrees_to_radians;
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
    // Where the game pulls its own camera in (tight spots, and for a frame here and there as its
    // collision meets a wall), the orbit's distance eases in toward it by this share a frame, keeps
    // it tight_hold frames after the game lets go, then eases back out by tight_release a frame.
    // Following the game's distance as it was, the orbit jumped in and out every other frame (#70).
    constexpr float tight_ease = 0.35f;
    constexpr float tight_release = 0.12f;
    constexpr int tight_hold = 15;
    // Easing back out no faster than this a frame: from where a wall held it far in (the distance wanted is far
    // further), a share of the way sent the eye straight back into the wall, which caught it again (issue #70).
    constexpr float release_step = 6.0f;
    // When the game's wall collision stops the orbit's eye short of where it was placed, the orbit comes in to
    // where it was held (its distance from the look-at point) plus wall_margin, for tight_hold frames, as the
    // game's own camera does. Placed far into the wall instead (the distance it wants is beyond it), the eye was
    // stopped at one of two points along the wall in turn, each start giving the other, and the view shook
    // (issue #70: a wall behind Conker).
    constexpr float wall_margin = 10.0f;
    constexpr float wall_held_slack = 1.0f;
    // At rest against something (the collision held or lifted the eye last frame) with nothing the orbit goes by
    // changed since (the look-at point, the turn and tilt, the distance wanted), the eye stays where it was drawn:
    // moved from there to there, the collision has nothing to change. Its answers otherwise differ with where the
    // camera starts, and at a wall, or more so where walls and the floor meet, the view shook with nothing moving
    // (issue #70). It's kept until something changes.
    constexpr float rest_move = 0.5f;
    constexpr float rest_turn = 1e-4f;
    // The game's own distance (which the orbit's follows) wavers by a unit or two from frame to frame, still.
    constexpr float rest_distance = 5.0f;
    // Touching something (the collision held or lifted the eye), the eye drawn goes only this share of the way
    // from where it was last frame to where the collision left it. Walls, ledges and corners can each answer a
    // little differently from frame to frame however the eye is placed (the start it's moved from changes the
    // end): smoothed, an eye sent back and forth between two places moves a few units instead (issue #70). In
    // the open it's drawn where it's placed.
    constexpr float contact_smooth = 0.35f;
    // And for this many frames after, so an eye let go of (behind where it was placed, smoothed) doesn't jump the
    // rest of the way at once.
    constexpr int contact_after = 8;
    // When the game's collision meets the floor, it lifts the eye to 40 above it (func_1512BB10,
    // the floor from func_1510E8BC), straight up. Near the ground the orbit's eye then bobbed: lifted,
    // eased back down, lifted again; and so high a floor kept it from looking up (issue #70). For the
    // orbit's eye the lift is to this much above the floor instead, and the orbit places its eye
    // no lower than that above the floor under it, asked of the game each frame (floor_below), so
    // it doesn't sink into it.
    constexpr float game_floor_lift = 40.0f;
    constexpr float floor_clearance = 20.0f;
    // floor_below: what func_1510E8BC gives when it finds no floor (D_800A3690, its lowest).
    constexpr float no_floor = -9000.0f;
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
        float aim = 0.0f;               // radians the view aims up past the lowest pitch (0: at the look-at point)
        float wanted = 0.0f;            // the distance from the look-at point
        bool wheel_zoomed = false;      // the scroll wheel set it (else it follows the game's)
        bool has_stick_time = false;    // stick_time holds the last read of the stick
        float tight = 0.0f;             // the game's pulled-in distance kept (0: none)
        int tight_frames = 0;           // frames left to keep it
        float shown = 0.0f;             // the distance the orbit places the eye at, eased
        float wall = 0.0f;              // the distance a wall held the eye at (0: none)
        float floor = no_floor;         // the floor found under the eye this frame (floor_below)
        bool contact = false;           // the collision held or lifted the eye last frame
        int smooth_frames = 0;          // frames left to smooth the eye drawn (contact_smooth)
        bool resting = false;           // the eye is kept where it was drawn (rest_move)
        bool has_last = false;          // last_* hold last frame's look-at point, turn, tilt and distance wanted
        float last_look[3] = {}, last_yaw = 0.0f, last_pitch = 0.0f, last_wanted = 0.0f;
        int wall_frames = 0;            // frames left to keep to it
        bool placed = false;            // the orbit placed the eye this frame, at placed_eye
        float placed_eye[3] = {};
        std::chrono::steady_clock::time_point stick_time;
    } orbit;

    float read_float(uint8_t* rdram, gpr base, int32_t offset);
    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value);

    // The floor's height below (x, from_y, z), or no_floor: func_1510E8BC as func_1512BB10 calls it
    // to lift its eye off the floor ($a2 the result's address, the point and the camera's last eye
    // height on the stack). Called from a hook in func_1512BB10 (ctx its context), with a stack of
    // its own below that function's.
    float floor_below(uint8_t* rdram, recomp_context* ctx, gpr camera, float x, float from_y, float z) {
        recomp_context call = *ctx;
        call.r29 = ADD32(ctx->r29, -0x80);
        const gpr sp = call.r29;
        const gpr result = ADD32(sp, 0x40);
        write_float(rdram, result, 0, -10000.0f);
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        write_float(rdram, sp, 0x18, x);
        write_float(rdram, sp, 0x1C, from_y);
        write_float(rdram, sp, 0x20, z);
        MEM_W(0x24, sp) = MEM_W(0x308, camera);
        MEM_W(0x28, sp) = 0;
        MEM_W(0x2C, sp) = 0;
        write_float(rdram, sp, 0x30, -10000.0f);
        write_float(rdram, sp, 0x34, from_y);
        MEM_W(0x38, sp) = 1;
        call.r4 = 0;
        call.r5 = 0;
        call.r6 = result;
        call.r7 = 0;
        func_1510E8BC(rdram, &call);
        return read_float(rdram, result, 0);
    }

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
        orbit.aim = 0.0f;
        orbit.resting = false;
        orbit.has_last = false;
        orbit.smooth_frames = 0;
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
        orbit.aim = 0.0f;
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
        orbit.shown = std::sqrt(ex * ex + ey * ey + ez * ez);
        orbit.tight = 0.0f;
        orbit.tight_frames = 0;
        orbit.wall = 0.0f;
        orbit.wall_frames = 0;
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
    // Looking up: the eye no further than it can stay above Conker's feet and outside him (issue #70).
    // Tilting further up than that, the eye stays there and the view aims up instead (orbit.aim, see
    // conker_mouse_camera_aim), as Ocarina of Time's free camera does: from close to a tower its top
    // was out of view, as the eye always looked at the look-at point. Tilting back down takes the aim
    // off first, then the eye comes back up.
    const float drop = std::max(look_height * (1.0f - ground_clearance), 0.0f);
    const float lowest_pitch = std::max(min_pitch, -std::atan2(drop, body_clearance));
    const float tilt = orbit.pitch - orbit.aim + mouse_y * degrees_per_pixel * degrees_to_radians + stick_pitch;
    if (tilt >= lowest_pitch) {
        orbit.pitch = std::min(tilt, max_pitch);
        orbit.aim = 0.0f;
    }
    else {
        orbit.pitch = lowest_pitch;
        // No more aim than takes the view to highest_view (the eye looks up -lowest_pitch already).
        orbit.aim = std::clamp(lowest_pitch - tilt, 0.0f, std::max(highest_view + lowest_pitch, 0.0f));
    }

    // The eye's direction from the look-at point.
    const float dx = std::cos(orbit.pitch) * std::cos(orbit.yaw);
    const float dy = std::sin(orbit.pitch);
    const float dz = std::cos(orbit.pitch) * std::sin(orbit.yaw);

    // At rest: nothing the orbit goes by changed since last frame, against something (rest_move).
    if (first) {
        const bool unchanged = orbit.has_last && (notches == 0) &&
            (std::fabs(cx - orbit.last_look[0]) < rest_move) && (std::fabs(cy - orbit.last_look[1]) < rest_move) && (std::fabs(cz - orbit.last_look[2]) < rest_move) &&
            (std::fabs(orbit.yaw - orbit.last_yaw) < rest_turn) && (std::fabs(orbit.pitch - orbit.last_pitch) < rest_turn) && (std::fabs(orbit.wanted - orbit.last_wanted) < rest_distance);
        orbit.resting = unchanged && (orbit.resting || orbit.contact);
        orbit.last_look[0] = cx; orbit.last_look[1] = cy; orbit.last_look[2] = cz;
        orbit.last_yaw = orbit.yaw; orbit.last_pitch = orbit.pitch; orbit.last_wanted = orbit.wanted;
        orbit.has_last = true;
    }

    // The distance the orbit eases toward (only the first time a frame, see tight_ease), held while at rest.
    if (first && !orbit.resting) {
        // Where the game pulls its own camera in closer than the controller can (tight spots),
        // so does the orbit.
        if (wanted_distance < nearest) {
            orbit.tight = wanted_distance;
            orbit.tight_frames = tight_hold;
        }
        else if (orbit.tight_frames > 0) {
            orbit.tight_frames--;
        }
        else {
            orbit.tight = 0.0f;
        }
        float goal = (orbit.tight > 0.0f) ? std::min(orbit.wanted, orbit.tight) : orbit.wanted;
        // No further than just beyond a wall that held the eye.
        if (orbit.wall_frames > 0) {
            orbit.wall_frames--;
            goal = std::min(goal, orbit.wall + wall_margin);
        }
        // Looking up, in close enough that the eye stays above Conker's feet, but not into him
        // (issue #70). Eased too: far out, a small tilt here moves the eye a long way in.
        if (dy < 0.0f && -dy * goal > drop) {
            goal = std::min(goal, std::max(drop / -dy, body_clearance / std::cos(orbit.pitch)));
        }
        if (orbit.shown <= 0.0f) {
            orbit.shown = goal;
        }
        orbit.shown += (goal < orbit.shown) ? (goal - orbit.shown) * tight_ease : std::min((goal - orbit.shown) * tight_release, release_step);
    }
    const float zoomed = orbit.shown;
    float target[3] = { cx + dx * zoomed, cy + dy * zoomed, cz + dz * zoomed };
    // Above the floor under it (floor_clearance), looked for down from the look-at point. func_1510E8BC can
    // answer with a surface above where it looked from: with a wall behind Conker the eye is behind or in it,
    // and it gives the wall's top. That isn't ground under the camera: going by it lifted the eye high up the
    // wall, and dropped it back each time the eye crossed the wall's edge (the jitter of issue #70).
    const float search_from = std::max(target[1], cy);
    const float floor = floor_below(rdram, ctx, camera, target[0], search_from, target[2]);
    orbit.floor = (floor > no_floor && floor <= search_from) ? floor : no_floor;
    if (floor > no_floor && floor <= search_from) {
        target[1] = std::max(target[1], floor + floor_clearance);
    }

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
        const float eye = orbit.resting ? from[i] : (behind ? from[i] + (target[i] - from[i]) * catch_up : target[i]);
        orbit.placed_eye[i] = eye;
        orbit.placed = true;
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
        orbit.aim = 0.0f;
    }
    // The game's collision lifted the orbit's eye off the floor (to game_floor_lift above it): to floor_clearance
    // above it instead, unless the orbit placed it higher than that. It's the floor's lift when the eye was lifted
    // straight up, or to just game_floor_lift above the floor found under it (a wall may move it across as well).
    if (orbit.engaged && orbit.placed) {
        const float ex = read_float(rdram, camera, 0x2F8), ey = read_float(rdram, camera, 0x2FC), ez = read_float(rdram, camera, 0x300);
        orbit.contact = (std::fabs(ex - orbit.placed_eye[0]) + std::fabs(ey - orbit.placed_eye[1]) + std::fabs(ez - orbit.placed_eye[2])) > rest_move;
        const bool straight_up = std::fabs(ex - orbit.placed_eye[0]) < 1.5f && std::fabs(ez - orbit.placed_eye[2]) < 1.5f;
        const bool to_floor = (orbit.floor > no_floor) && (std::fabs(ey - (orbit.floor + game_floor_lift)) < 2.0f);
        if ((straight_up || to_floor) && ey > orbit.placed_eye[1] + 1.0f) {
            write_float(rdram, camera, 0x2FC, std::max(orbit.placed_eye[1], ey - game_floor_lift + floor_clearance));
        }
        // Held across by a wall: how far from the look-at point, to come in to (wall_margin).
        if (std::hypot(ex - orbit.placed_eye[0], ez - orbit.placed_eye[2]) > wall_held_slack) {
            const float lx = read_float(rdram, camera, 0x2BC), ly = read_float(rdram, camera, 0x2C0), lz = read_float(rdram, camera, 0x2C4);
            orbit.wall = std::sqrt((ex - lx) * (ex - lx) + (ey - ly) * (ey - ly) + (ez - lz) * (ez - lz));
            orbit.wall_frames = tight_hold;
        }
        // Touching, or just let go: part of the way from last frame's eye (+0x304) to this one (contact_smooth).
        if (orbit.contact) {
            orbit.smooth_frames = contact_after;
        }
        else if (orbit.smooth_frames > 0) {
            orbit.smooth_frames--;
        }
        if (orbit.smooth_frames > 0) {
            for (int i = 0; i < 3; i++) {
                const float was = read_float(rdram, camera, 0x304 + i * 4);
                const float now = read_float(rdram, camera, 0x2F8 + i * 4);
                write_float(rdram, camera, 0x2F8 + i * 4, was + (now - was) * contact_smooth);
            }
        }
    }
    orbit.placed = false;
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

// func_1512C490 (builds the view, called by func_151284C4), before 0x1512C640: $s0 is the camera,
// and the view's look-at point (+0x2E0) and eye (+0x2EC) have just been copied from the look-at
// point (+0x2BC) and the eye the collision left (+0x2F8). The view (func_15128CB0) is made from
// these two. Tilted up past the lowest pitch (orbit.aim), the view's look-at point is raised so
// the view aims that much higher from the same eye, its direction across the ground the same:
// Conker drops toward the bottom of the screen and what's above comes into view (issue #70). Only
// the view's copy changes: the look-at point the game follows with, the eye and its collision,
// and Conker's movement (by the eye's direction across the ground) are as they were.
extern "C" void conker_mouse_camera_aim(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    if (!orbit.engaged || orbit.aim <= 0.0f || (uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    // The eye from where it's copied (+0x2F8): the copy's z (+0x2F4) is stored after this, in the branch's delay slot.
    const float ex = read_float(rdram, camera, 0x2F8), ey = read_float(rdram, camera, 0x2FC), ez = read_float(rdram, camera, 0x300);
    const float lx = read_float(rdram, camera, 0x2E0) - ex, ly = read_float(rdram, camera, 0x2E4) - ey, lz = read_float(rdram, camera, 0x2E8) - ez;
    const float across = std::sqrt(lx * lx + lz * lz);
    if (across < 1.0f) {
        return;
    }
    const float distance = std::sqrt(across * across + ly * ly);
    const float view = std::min(std::atan2(ly, across) + orbit.aim, highest_view);
    const float scale = distance * std::cos(view) / across;
    write_float(rdram, camera, 0x2E0, ex + lx * scale);
    write_float(rdram, camera, 0x2E4, ey + distance * std::sin(view));
    write_float(rdram, camera, 0x2E8, ez + lz * scale);
}

// frontend.cpp, once SDL is up: listen for the scroll wheel.
void conker_mouse_camera_init() {
    SDL_AddEventWatch(watch_wheel, nullptr);
}
