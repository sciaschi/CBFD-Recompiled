// Aiming: Reticle (Camera tab, issue #74): a green ring with a dot in the middle of the screen while
// Conker aims, like the one Conker: Live & Reloaded shows, or a crosshair or a dot instead, in three
// sizes (Aiming: Reticle Size, issue #124). The original game shows nothing, so throwing is aimed by
// eye; the setting is Off by default, as the original.
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

    // The reticle's size, as a share of the window's height, by Aiming: Reticle Size: Medium a little
    // smaller than Live & Reloaded's.
    float size_share(conker::reticle::Size size) {
        switch (size) {
            case conker::reticle::Size::Small: return 0.06f;
            case conker::reticle::Size::Large: return 0.14f;
            default: return 0.1f;
        }
    }

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
    // Each style's pieces, in a group of its own the size of the holder; only the style chosen shows.
    recompui::Element* ring_group = nullptr;
    recompui::Element* crosshair_group = nullptr;
    recompui::Element* dot_group = nullptr;
    conker::reticle::Style shown_style = conker::reticle::Style::Off;
    float holder_aspect = 0.0f;
    float holder_raise = -1.0f;
    float holder_share = 0.0f;

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

    // A style's group: the holder's size, at its corner, hidden until chosen.
    recompui::Element* add_group() {
        recompui::Element* group = context.create_element<recompui::Element>(holder);
        group->set_position(recompui::Position::Absolute);
        group->set_inset(0.0f);
        group->set_display(recompui::Display::None);
        return group;
    }

    // A round dot in the middle of a group, size a share of the holder.
    recompui::Element* add_dot(recompui::Element* group, float size, recompui::Color color) {
        recompui::Element* dot = add_piece(group, 0.5f - size / 2, 0.5f - size / 2, size, size, color);
        // RmlUi takes no percentage for a radius; one past the reticle's size is scaled down to fit, a circle.
        dot->set_border_radius(4096.0f, recompui::Unit::Px);
        return dot;
    }

    // The three styles, all green and translucent, so what's aimed at shows through:
    // - the ring, Live & Reloaded's: a thin ring, short ticks pointing in from it at the four sides,
    //   and a dot in the middle;
    // - the crosshair: four bars pointing in at the middle, which they stop short of, and a dot;
    // - the dot: a dot alone, with a dark edge so it shows on light ground too.
    void create() {
        const recompui::Color green{ 110, 230, 110, 210 };
        const recompui::Color dim_green{ 110, 230, 110, 150 };
        const recompui::Color edge{ 0, 0, 0, 140 };
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        holder = context.create_element<recompui::Element>(context.get_root_element());
        holder->set_position(recompui::Position::Absolute);

        ring_group = add_group();
        recompui::Element* ring = context.create_element<recompui::Element>(ring_group);
        ring->set_position(recompui::Position::Absolute);
        ring->set_inset(0.0f);
        ring->set_border_width(3.0f);
        ring->set_border_color(green);
        ring->set_border_radius(4096.0f, recompui::Unit::Px);
        // The ticks: from the ring to a third of the way in. The bars are about 2 of the ring's
        // 100 thick (a share of the holder, so they scale with it).
        {
            constexpr float thick = 0.03f, length = 0.2f, start = 0.04f;
            add_piece(ring_group, 0.5f - thick / 2, start, thick, length, dim_green);
            add_piece(ring_group, 0.5f - thick / 2, 1.0f - start - length, thick, length, dim_green);
            add_piece(ring_group, start, 0.5f - thick / 2, length, thick, dim_green);
            add_piece(ring_group, 1.0f - start - length, 0.5f - thick / 2, length, thick, dim_green);
            add_dot(ring_group, 0.07f, green);
        }

        crosshair_group = add_group();
        {
            // From the holder's edge to a gap round the middle. The dot is as thick as the bars: each
            // piece's edges are rounded to whole pixels on their own, and a dot of another size could
            // round a pixel off the line the bars meet on (it did, at the small size).
            constexpr float thick = 0.06f, gap = 0.12f, length = 0.5f - gap;
            add_piece(crosshair_group, 0.5f - thick / 2, 0.0f, thick, length, green);
            add_piece(crosshair_group, 0.5f - thick / 2, 0.5f + gap, thick, length, green);
            add_piece(crosshair_group, 0.0f, 0.5f - thick / 2, length, thick, green);
            add_piece(crosshair_group, 0.5f + gap, 0.5f - thick / 2, length, thick, green);
            add_dot(crosshair_group, thick, green);
        }

        dot_group = add_group();
        recompui::Element* dot = add_dot(dot_group, 0.16f, green);
        dot->set_border_width(2.0f);
        dot->set_border_color(edge);

        context.close();
        shown_style = conker::reticle::Style::Off;
        created = true;
    }

    // Shows the chosen style's group, the others hidden.
    void show_style(conker::reticle::Style style) {
        if (style == shown_style) {
            return;
        }
        shown_style = style;
        context.open();
        ring_group->set_display(style == conker::reticle::Style::Ring ? recompui::Display::Block : recompui::Display::None);
        crosshair_group->set_display(style == conker::reticle::Style::Crosshair ? recompui::Display::Block : recompui::Display::None);
        dot_group->set_display(style == conker::reticle::Style::Dot ? recompui::Display::Block : recompui::Display::None);
        context.close();
    }

    // Puts the holder in the middle of the window, raised by raise_percent of its height, square: its
    // height a share of the window's, and its width the same share of the height in terms of the
    // window's width.
    void place(float raise_percent, float share) {
        int width = 0, height = 0;
        if (window != nullptr) {
            SDL_GetWindowSize(window, &width, &height);
        }
        const float aspect = (width > 0 && height > 0) ? (float)width / (float)height : 16.0f / 9.0f;
        if (aspect == holder_aspect && raise_percent == holder_raise && share == holder_share) {
            return;
        }
        holder_aspect = aspect;
        holder_raise = raise_percent;
        holder_share = share;
        const float h = share * 100.0f;
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
    const conker::reticle::Style style = ui_ready ? conker::reticle::style() : conker::reticle::Style::Off;
    const bool wanted = ui_ready && style != conker::reticle::Style::Off && ultramodern::is_game_started() && aiming;
    if (!wanted) {
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    if (!created) {
        create();
    }
    show_style(style);
    place(last_aim_state.load() == no_state ? 0.0f : thrown_raise_percent, size_share(conker::reticle::size()));
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
}
