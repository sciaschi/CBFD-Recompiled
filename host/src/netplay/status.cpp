// Netplay's status on screen (#19), from what lockstep keeps (lockstep::status, take_news), so the players can
// see what the match is doing instead of only the log:
// - at the top, in the middle: "Waiting for Player 2... 4 s" once a read has waited a moment for the others
//   (a read waits a few ms most of the time, which isn't worth a word), "Catching up..." once that has gone on
//   a moment, and for a few seconds each, what happened: "Player 3 left.", "Player 3 stopped answering (they
//   may have quit).", "Player 3 lost connection: the others play on.", "Everyone else has left: the match goes on
//   here."; "Getting the host's game... 40%" while a
//   desync is set right here (desync recovery)
// - with Netplay Details on (the in-game menu's Netplay tab), in the bottom-right corner: the input delay (a spectator: Watching), how far ahead of the others
//   this machine is (or behind), and each player's round trip (a joiner knows only the host's: the others'
//   pads come through it) and input delay, or that they're on this machine or left.
//
// It's drawn by recompui, as notices.cpp's lines and the FPS counter are: a context of its own that takes no
// input and no mouse, nothing in the game's memory. It waits for the game to start and for the launcher's init
// (on_ui_ready), for the same reasons as theirs.

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <deque>
#include <string>
#include <variant>
#include <vector>

#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"

#include "../conker.hpp"
#include "lockstep.hpp"

namespace {
    // A read waiting this long is said on screen; catching up, this long.
    constexpr double waiting_grace = 1.0;
    constexpr double catching_up_grace = 0.5;
    // How long each line of what happened stays, and how many at once.
    constexpr double news_seconds = 6.0;
    constexpr size_t most_news = 3;
    // How often the corner's numbers change.
    constexpr double details_every = 0.25;

    // Netplay Details, Off or On (kept with General's options, shown on the Netplay tab).
    const std::string details_option = "netplay_details";
    enum class Details : uint32_t { Off, On };

    std::atomic<bool> ui_ready = false;
    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* banner = nullptr;
    recompui::Element* details = nullptr;
    // The banner's lines: what's going on now, then what happened.
    std::array<recompui::Label*, 1 + most_news> banner_labels{};
    // The corner's: the delay and how far ahead, then a line for each port.
    std::array<recompui::Label*, 1 + conker::netplay::max_players> detail_labels{};
    std::array<std::string, 1 + most_news> banner_texts;
    std::array<std::string, 1 + conker::netplay::max_players> detail_texts;
    bool banner_shown = false;
    bool details_shown = false;
    double details_at = 0;

    struct News {
        std::string text;
        double until = 0;
    };
    std::deque<News> recent;

    recompui::Element* box(recompui::Element* parent) {
        recompui::Element* b = context.create_element<recompui::Element>(parent);
        b->set_display(recompui::Display::Flex);
        b->set_flex_direction(recompui::FlexDirection::Column);
        b->set_padding_left(10.0f);
        b->set_padding_right(10.0f);
        b->set_padding_top(4.0f);
        b->set_padding_bottom(4.0f);
        b->set_border_radius(6.0f);
        b->set_background_color(recompui::theme::color::BGOverlay);
        return b;
    }

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        // The banner, across the top, in the middle.
        recompui::Element* top = context.create_element<recompui::Element>(context.get_root_element());
        top->set_position(recompui::Position::Absolute);
        top->set_top(8.0f);
        top->set_left(0.0f);
        top->set_right(0.0f);
        top->set_display(recompui::Display::Flex);
        top->set_flex_direction(recompui::FlexDirection::Column);
        top->set_align_items(recompui::AlignItems::Center);
        banner = box(top);
        banner->set_align_items(recompui::AlignItems::Center);
        for (recompui::Label*& label : banner_labels) {
            label = context.create_element<recompui::Label>(banner, "", recompui::LabelStyle::Normal);
            label->set_display(recompui::Display::None);
        }
        banner->set_display(recompui::Display::None);
        // The details, in the bottom-right corner.
        details = box(context.get_root_element());
        details->set_position(recompui::Position::Absolute);
        details->set_bottom(8.0f);
        details->set_right(8.0f);
        for (recompui::Label*& label : detail_labels) {
            label = context.create_element<recompui::Label>(details, "", recompui::LabelStyle::Small);
            label->set_display(recompui::Display::None);
        }
        details->set_display(recompui::Display::None);
        context.close();
        created = true;
    }

    bool details_on() {
        recomp::config::Config* config = recompui::config::find_option_config(details_option);
        return config != nullptr &&
            static_cast<Details>(std::get<uint32_t>(config->get_option_value(details_option))) == Details::On;
    }

    // "Player 2", or "Players 2 and 3".
    std::string players_text(uint8_t ports) {
        std::string numbers;
        const int count = std::popcount(ports);
        for (int port = 0, n = 0; port < conker::netplay::max_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            numbers += (n == 0 ? "" : n == count - 1 ? " and " : ", ") + std::to_string(port + 1);
            n++;
        }
        return (count == 1 ? "Player " : "Players ") + numbers;
    }

    std::string frames_text(int64_t frames) {
        return std::to_string(frames) + (frames == 1 ? " frame" : " frames");
    }

    // Sets `label`'s text (and shows it), or hides it for none; only what changed is sent to recompui.
    void set_line(recompui::Label* label, std::string& shown, const std::string& text) {
        if (text == shown) return;
        if (text.empty()) {
            label->set_display(recompui::Display::None);
        }
        else {
            if (shown.empty()) label->set_display(recompui::Display::Block);
            label->set_text(text);
        }
        shown = text;
    }

    void set_box(recompui::Element* element, bool& shown, bool show) {
        if (show == shown) return;
        element->set_display(show ? recompui::Display::Flex : recompui::Display::None);
        shown = show;
    }

    // The banner's lines for `status` at `t`.
    std::array<std::string, 1 + most_news> banner_lines(const conker::lockstep::Status& status, double t) {
        std::array<std::string, 1 + most_news> lines;
        if (status.getting_state) {
            lines[0] = "Getting the host's game... " + std::to_string((int)(status.state_progress * 100)) + "%";
        }
        else if (status.playing && status.waiting_for != 0 && t - status.waiting_since >= waiting_grace) {
            lines[0] = "Waiting for " + players_text(status.waiting_for) + "... " +
                std::to_string((int)(t - status.waiting_since)) + " s";
        }
        else if (status.playing && status.catching_up && t - status.catching_up_since >= catching_up_grace) {
            lines[0] = "Catching up...";
        }
        for (size_t i = 0; i < recent.size(); i++) {
            lines[1 + i] = recent[i].text;
        }
        return lines;
    }

    // The corner's lines for `status`.
    std::array<std::string, 1 + conker::netplay::max_players> detail_lines(const conker::lockstep::Status& status) {
        std::array<std::string, 1 + conker::netplay::max_players> lines;
        std::string where = "nobody else plays";
        if (status.others_playing) {
            where = status.ahead > 0 ? frames_text(status.ahead) + " ahead"
                : status.ahead < 0 ? frames_text(-status.ahead) + " behind" : "level with the others";
        }
        lines[0] = (status.watching ? std::string("Watching") : "Input delay " + std::to_string(status.input_delay)) + " | " + where;
        for (int port = 0; port < status.players && port < conker::netplay::max_players; port++) {
            std::string what;
            if ((status.own_ports & (1u << port)) != 0) what = "here";
            else if ((status.open & (1u << port)) != 0) what = "open";
            else if ((status.lost & (1u << port)) != 0) what = "lost connection";
            else if ((status.left_silently & (1u << port)) != 0) what = "stopped answering";
            else if ((status.left & (1u << port)) != 0) what = "left";
            else if (status.round_trip_ms[port] >= 0) what = std::to_string(status.round_trip_ms[port]) + " ms";
            else what = status.hosting ? "..." : "through the host";
            // (Each player's input delay is their own: one with a bad link has a higher one.)
            if (((status.own_ports | status.open | status.left | status.lost) & (1u << port)) == 0) {
                what += ", delay " + std::to_string(status.delays[port]);
            }
            lines[1 + port] = "Player " + std::to_string(port + 1) + (port == 0 && !status.hosting ? " (host)" : "") + ": " + what;
        }
        return lines;
    }
}

const char* const conker::netplay_status::details_description =
    "In a netplay match, shows in the bottom-right corner the input delay, how far ahead of the others (or "
    "behind) this computer is, and each player's round trip and input delay. Waiting for a player, and a player "
    "leaving, are shown at the top whatever this is set to.";

void conker::netplay_status::add_options(recomp::config::Config& config) {
    static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
        {Details::Off, "Off", "Off"},
        {Details::On, "On", "On"},
    };
    // (Hidden: the Netplay tab shows it.)
    config.add_enum_option(details_option, "Netplay Details", details_description, choices, Details::Off, true);
}

bool conker::netplay_status::details_wanted() {
    return details_on();
}

void conker::netplay_status::set_details_wanted(bool on) {
    recomp::config::Config* config = recompui::config::find_option_config(details_option);
    if (config == nullptr) {
        return;
    }
    config->set_option_value(details_option, (uint32_t)(on ? Details::On : Details::Off));
    config->save_config();
}

void conker::netplay_status::on_ui_ready() {
    ui_ready = true;
}

void conker::netplay_status::update() {
    if (!ui_ready || !ultramodern::is_game_started() || !conker::lockstep::active()) {
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    const double t = conker::lockstep::now();
    for (std::string& text : conker::lockstep::take_news()) {
        recent.push_back({ std::move(text), t + news_seconds });
        if (recent.size() > most_news) recent.pop_front();
    }
    while (!recent.empty() && recent.front().until <= t) {
        recent.pop_front();
    }
    const conker::lockstep::Status status = conker::lockstep::status();
    const std::array<std::string, 1 + most_news> lines = banner_lines(status, t);
    bool any_line = false;
    for (const std::string& line : lines) any_line = any_line || !line.empty();
    const bool want_details = status.playing && details_on();
    if (!created) {
        if (!any_line && !want_details) {
            return;
        }
        create();
    }

    const bool details_due = want_details && t - details_at >= details_every;
    std::array<std::string, 1 + conker::netplay::max_players> rows = detail_texts;
    if (details_due) {
        details_at = t;
        rows = detail_lines(status);
    }
    if (lines != banner_texts || rows != detail_texts || any_line != banner_shown || want_details != details_shown) {
        context.open();
        for (size_t i = 0; i < lines.size(); i++) {
            set_line(banner_labels[i], banner_texts[i], lines[i]);
        }
        set_box(banner, banner_shown, any_line);
        for (size_t i = 0; i < rows.size(); i++) {
            set_line(detail_labels[i], detail_texts[i], rows[i]);
        }
        set_box(details, details_shown, want_details);
        context.close();
    }

    const bool want_context = any_line || want_details;
    if (want_context != recompui::is_context_shown(context)) {
        if (want_context) recompui::show_context(context, "");
        else recompui::hide_context(context);
    }
}
