// Notices: a line in the top-left corner for a few seconds, one at a time, for anything the program has to say
// while the game runs (an achievement unlocked, a save state saved...). Anyone may call conker::notices::show,
// from any thread; the main thread shows them (update, from update_gfx).
//
// It's a recompui context of its own that takes no input and no mouse (as the FPS counter's), so the game and
// the menus work as before. The lines wait for the game to start: recompui shows the launcher only while no
// context is shown, so a line shown at the launcher kept the screen black until it went. And the context is
// only made once the launcher's init has run (on_ui_ready): recompui's UI is made on the render thread as RT64
// starts, and update_gfx can run before that.
//
// (This was RetroAchievements' message line, achievements.cpp, made a part of its own so what's built without
// RetroAchievements can show its lines too.)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;
    constexpr auto notice_time = std::chrono::seconds(5);

    // The lines to show, from any thread, for the main thread. replacing: one to show at once in place of
    // what's shown (and of an earlier one to replace), as a save state's slot is as F6 is pressed again and again.
    std::mutex notices_mutex;
    std::deque<std::string> waiting;
    std::string replacing;

    std::atomic<bool> ui_ready = false;
    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Label* label = nullptr;
    std::string shown_text;
    clock::time_point shown_until;

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* box = context.create_element<recompui::Element>(context.get_root_element());
        box->set_position(recompui::Position::Absolute);
        box->set_top(8.0f);
        box->set_left(8.0f);
        box->set_padding_left(10.0f);
        box->set_padding_right(10.0f);
        box->set_padding_top(4.0f);
        box->set_padding_bottom(4.0f);
        box->set_border_radius(6.0f);
        box->set_background_color(recompui::theme::color::BGOverlay);
        label = context.create_element<recompui::Label>(box, "", recompui::LabelStyle::Normal);
        context.close();
        created = true;
    }
}

void conker::notices::show(const std::string& text, bool replace) {
    std::printf("[notice] %s\n", text.c_str());
    std::fflush(stdout);
    std::lock_guard lock{notices_mutex};
    if (replace) {
        replacing = text;
    }
    else {
        waiting.push_back(text);
    }
}

void conker::notices::on_ui_ready() {
    ui_ready = true;
}

void conker::notices::update() {
    if (!ui_ready || !ultramodern::is_game_started()) {
        return;
    }
    const clock::time_point now = clock::now();
    std::string replacement;
    {
        std::lock_guard lock{notices_mutex};
        replacement.swap(replacing);
    }
    if (!shown_text.empty() && now < shown_until && replacement.empty()) {
        return;
    }
    std::string next = replacement;
    if (next.empty()) {
        std::lock_guard lock{notices_mutex};
        if (!waiting.empty()) {
            next = waiting.front();
            waiting.pop_front();
        }
    }
    if (!created) {
        if (next.empty()) {
            return;
        }
        create();
    }
    if (next.empty()) {
        if (!shown_text.empty()) {
            recompui::hide_context(context);
            shown_text.clear();
        }
        return;
    }
    shown_text = next;
    shown_until = now + notice_time;
    context.open();
    label->set_text(shown_text);
    context.close();
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
}
