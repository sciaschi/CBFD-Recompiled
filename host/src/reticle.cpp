// Aiming: Reticle (Camera tab, issue #74): a green ring with a dot in the middle of the screen while
// Conker aims, like the one Conker: Live & Reloaded shows. The original game shows nothing, so
// throwing is aimed by eye; the setting is Off by default, as the original.
//
// Conker aims in one of two modes, each run once a game frame for each camera while it's in it:
// - the look mode (func_15120158), which R-Look uses too. Its state (saved at 0xA0($sp), what picks
//   its paths) tells what he's doing: logged while playing, 0x0 is plain R-Look, 0x2 the slingshot
//   (the B pad on the hill's bugs) and 0xA the throwing knives (the B pad at the top of the barn).
//   The reticle is shown for those two (conker_look_targets tells this file, look_frame).
// - the second aiming mode (func_15126378: the sniper scope, the magnum), conker_aim_stick
//   (aim_frame).
// Both turn the camera itself, but the slingshot's shots and the knives leave Conker's hand below
// it, pitched up: they end up above the middle of the view, by the same part of the screen however
// far (the angle's the same), 20% of its height, found by playing (thrown_raise_percent). Close
// by, where a shot hasn't climbed from his hand yet, it lands a little lower than the reticle: that
// would need the distance to what's aimed at, traced through the level.
//
// Each frame either mode tells this file which camera it is, whether it's zoomed in, and where its
// view is on the N64's screen. The reticle is shown while that keeps coming, and hidden a moment
// after it stops (the aim let go, a cutscene, the pause menu: the game stops running the mode).
//
// It isn't shown:
// - zoomed in (the sniper scope): the scope draws its own crosshair.
// - for a camera that isn't full screen (multiplayer's split screen): the middle of the window isn't
//   the middle of its view. Only player 1's camera counts, as every player's aims in turn.
//
// Like the FPS counter (fps_counter.cpp) it's a recompui context of its own that takes no input and
// no mouse, made once the launcher's init has run (on_ui_ready), and updated on the main thread.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"

#include <SDL.h>

#include "conker.hpp"

// The game window (frontend.cpp).
extern SDL_Window* window;

namespace {
    using clock = std::chrono::steady_clock;

    // How long after the last aiming frame the reticle stays: a little over two game frames at 30,
    // so a frame the game takes longer over doesn't make it blink.
    constexpr auto linger = std::chrono::milliseconds(80);

    // The reticle's size, as a share of the window's height: a little smaller than Live & Reloaded's.
    constexpr float size_share = 0.1f;

    // How far above the middle the slingshot's shots and the knives end up, in percent of the
    // window's height: the same for both, matched to where they land in the distance while playing.
    // The second aiming mode (the magnum) is left in the middle.
    constexpr float thrown_raise_percent = 20.0f;

    // The cameras (D_800BE628, 0x180 bytes each, the same as widescreen.cpp's): their field of view
    // in use (+ 0x74) and unzoomed (+ 0x6C), degrees, and the edges of their view on the N64's
    // 292 by 216 screen (+ 0x24 top, + 0x28 bottom, + 0x2C left, + 0x30 right), floats.
    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr uint32_t camera_size = 0x180;
    constexpr int32_t camera_fov = 0x74, camera_unzoomed_fov = 0x6C;
    constexpr int32_t camera_top = 0x24, camera_bottom = 0x28, camera_left = 0x2C, camera_right = 0x30;

    // The last aiming frame, from the game thread: when (clock ticks), and whether the reticle
    // belongs on it (player 1's camera, full screen, not zoomed in).
    std::atomic<int64_t> last_aim_ticks = 0;
    std::atomic<bool> last_aim_shown = false;
    // The look mode's state of the last aiming frame (no_state for the second aiming mode).
    constexpr uint32_t no_state = 0xFFFFFFFFu;
    std::atomic<uint32_t> last_aim_state = no_state;

    std::atomic<bool> ui_ready = false;
    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* holder = nullptr;
    float holder_aspect = 0.0f;
    float holder_raise = -1.0f;

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    // A bar or dot of the reticle: a rectangle placed in the holder, in shares of its size
    // (the holder is square on screen, see place()).
    recompui::Element* add_piece(recompui::Element* parent, float left, float top, float width, float height,
        recompui::Color color) {
        recompui::Element* piece = context.create_element<recompui::Element>(parent);
        piece->set_position(recompui::Position::Absolute);
        piece->set_left(left * 100.0f, recompui::Unit::Percent);
        piece->set_top(top * 100.0f, recompui::Unit::Percent);
        piece->set_width(width * 100.0f, recompui::Unit::Percent);
        piece->set_height(height * 100.0f, recompui::Unit::Percent);
        piece->set_background_color(color);
        return piece;
    }

    // Live & Reloaded's: a thin green ring, short ticks pointing in from it at the four sides, and a
    // dot in the middle; translucent, so what's aimed at shows through.
    void create() {
        const recompui::Color green{ 110, 230, 110, 210 };
        const recompui::Color dim_green{ 110, 230, 110, 150 };
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        holder = context.create_element<recompui::Element>(context.get_root_element());
        holder->set_position(recompui::Position::Absolute);

        recompui::Element* ring = context.create_element<recompui::Element>(holder);
        ring->set_position(recompui::Position::Absolute);
        ring->set_inset(0.0f);
        ring->set_border_width(3.0f);
        ring->set_border_color(green);
        // RmlUi takes no percentage for a radius; one past the reticle's size is scaled down to fit, a circle.
        ring->set_border_radius(4096.0f, recompui::Unit::Px);

        // The ticks: from the ring to a third of the way in. The bars are about 2 of the ring's
        // 100 thick (a share of the holder, so they scale with it).
        constexpr float thick = 0.03f, length = 0.2f, start = 0.04f;
        add_piece(holder, 0.5f - thick / 2, start, thick, length, dim_green);
        add_piece(holder, 0.5f - thick / 2, 1.0f - start - length, thick, length, dim_green);
        add_piece(holder, start, 0.5f - thick / 2, length, thick, dim_green);
        add_piece(holder, 1.0f - start - length, 0.5f - thick / 2, length, thick, dim_green);
        constexpr float dot = 0.07f;
        recompui::Element* centre = add_piece(holder, 0.5f - dot / 2, 0.5f - dot / 2, dot, dot, green);
        centre->set_border_radius(4096.0f, recompui::Unit::Px);

        context.close();
        created = true;
    }

    // Puts the holder in the middle of the window, raised by raise_percent of its height, square: its
    // height a share of the window's, and its width the same share of the height in terms of the
    // window's width.
    void place(float raise_percent) {
        int width = 0, height = 0;
        if (window != nullptr) {
            SDL_GetWindowSize(window, &width, &height);
        }
        const float aspect = (width > 0 && height > 0) ? (float)width / (float)height : 16.0f / 9.0f;
        if (aspect == holder_aspect && raise_percent == holder_raise) {
            return;
        }
        holder_aspect = aspect;
        holder_raise = raise_percent;
        const float h = size_share * 100.0f;
        const float w = h / aspect;
        context.open();
        holder->set_width(w, recompui::Unit::Percent);
        holder->set_height(h, recompui::Unit::Percent);
        holder->set_left(50.0f - w / 2, recompui::Unit::Percent);
        holder->set_top(50.0f - raise_percent - h / 2, recompui::Unit::Percent);
        context.close();
    }
}

namespace {
    // A frame aiming with camera (its state's + 0x23D is the camera's index): player 1's, full
    // screen and not zoomed in, shows the reticle.
    void aiming_frame(uint8_t* rdram, gpr camera, uint32_t state) {
        const uint32_t index = MEM_BU(0x23D, camera);
        if (index != 0) {
            return;
        }
        const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, cameras_pointer) + index * camera_size);
        const float fov = read_float(rdram, view, camera_fov);
        const float unzoomed = read_float(rdram, view, camera_unzoomed_fov);
        const bool zoomed = fov > 0.0f && unzoomed > 0.0f && fov < unzoomed - 0.5f;
        // A full-screen camera's view is clipped to 2..290 across (widescreen.cpp): split-screen ones
        // stop in the middle.
        const bool full_screen = read_float(rdram, view, camera_left) <= 4.0f && read_float(rdram, view, camera_right) >= 286.0f &&
            read_float(rdram, view, camera_top) <= 4.0f && read_float(rdram, view, camera_bottom) >= 210.0f;
        last_aim_shown = !zoomed && full_screen;
        last_aim_state = state;
        last_aim_ticks = clock::now().time_since_epoch().count();
    }
}

void conker::reticle::aim_frame(uint8_t* rdram, gpr camera) {
    aiming_frame(rdram, camera, no_state);
}

void conker::reticle::look_frame(uint8_t* rdram, gpr camera, uint32_t state) {
    // The slingshot and the throwing knives; plain R-Look (0x0) and the rest aren't aiming anything.
    constexpr uint32_t slingshot = 0x2, throwing_knives = 0xA;
    if (state == slingshot || state == throwing_knives) {
        aiming_frame(rdram, camera, state);
    }
}

void conker::reticle::on_ui_ready() {
    ui_ready = true;
}

void conker::reticle::update() {
    const int64_t since = clock::now().time_since_epoch().count() - last_aim_ticks.load();
    const bool aiming = since >= 0 && clock::duration(since) < linger && last_aim_shown.load();
    const bool wanted = ui_ready && conker::reticle::enabled() && ultramodern::is_game_started() && aiming;
    if (!wanted) {
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    if (!created) {
        create();
    }
    place(last_aim_state.load() == no_state ? 0.0f : thrown_raise_percent);
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
}
