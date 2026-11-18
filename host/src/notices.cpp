// Notices: a line in the top-left corner for a few seconds, one at a time, for anything the program has to say
// while the game runs (an achievement unlocked, a save state saved...). Anyone may call conker::notices::show,
// from any thread; the main thread shows them (update, from update_gfx).
//
// General: Notifications (add_options) turns them all off; each kind has its own setting too (Achievement,
// Save State and Texture Pack Notifications), which its sender checks. A line saying something couldn't be
// done (show_failure: a save state not saved or loaded) is shown whatever they're set to, as nothing else
// would tell.
//
// It's a recompui context of its own that takes no input and no mouse (as the FPS counter's), so the game and
// the menus work as before. The lines wait for the game to start: recompui shows the launcher only while no
// context is shown, so a line shown at the launcher kept the screen black until it went. And the context is
// only made once the launcher's init has run (on_ui_ready): recompui's UI is made on the render thread as RT64
// starts, and update_gfx can run before that.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "librecomp/config.hpp"
#include "recompui/config.h"
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

namespace {
    // General: Notifications, On or Off.
    const std::string notifications_option = "notifications";
    enum class Notifications : uint32_t { On, Off };

    // A line for the main thread to show.
    void queue(const std::string& text, bool replace) {
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
}

void conker::notices::add_options(recomp::config::Config& config) {
    static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
        {Notifications::On, "On", "On"},
        {Notifications::Off, "Off", "Off"},
    };
    config.add_enum_option(notifications_option, "Notifications",
        "Shows lines in the corner for what the program does while you play: an achievement unlocked, a save state "
        "saved or loaded, the texture pack switched with F8. <recomp-color primary>Off</recomp-color> turns them all "
        "off; the settings below turn each kind off on its own. A line saying a save state couldn't be saved or loaded "
        "is still shown.",
        choices, Notifications::On);
}

void conker::notices::show(const std::string& text, bool replace) {
    recomp::config::Config* config = recompui::config::find_option_config(notifications_option);
    if (config != nullptr && static_cast<Notifications>(std::get<uint32_t>(config->get_option_value(notifications_option))) == Notifications::Off) {
        std::printf("[notice] (not shown: Notifications is off) %s\n", text.c_str());
        std::fflush(stdout);
        return;
    }
    queue(text, replace);
}

void conker::notices::show_failure(const std::string& text) {
    queue(text, false);
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
