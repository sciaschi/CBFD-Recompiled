// The launcher's Netplay menu: host a match or join one by its code, wait for
// every player there, then the game starts on every machine at once. It's a modal of its own over the
// launcher; the connecting is lockstep's (lockstep.hpp), a step at each of the menu's updates, so a
// refusal (another build, ROM or settings, a full match) is said here and the player can try again.
//
// A match started from here doesn't quit when it ends (a desync, the host leaving): the game stops taking
// input and a prompt says why, with Quit.
//
// The host's join code is made, once hosting, from the address the host gives and the match's random key:
// the game can't know its public address without a server to ask, so the host looks it up (or gives its
// local one, for a match at home). The code is the host's address, so the menu says to share it only with
// people the host trusts. The input delay is lockstep's default, the same on every machine.

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <SDL.h>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "elements/ui_radio.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_tab_set.h"
#include "elements/ui_text_input.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../conker.hpp"
#include "hello.hpp"
#include "lockstep.hpp"
#include "net/session.hpp"

namespace {
    constexpr int default_port = 27015;

    std::u8string game_id;
    std::filesystem::path stored_rom;
    recompui::Modal* modal = nullptr;

    // From the game thread (the harness), for the main thread: why the match is over.
    std::mutex over_mutex;
    std::string over_text;
    bool over_pending = false;

    class NetplayPage : public recompui::Element {
        enum class State { idle, connecting, starting };
        State state = State::idle;
        recompui::TextInput* port_input = nullptr;
        recompui::TextInput* address_input = nullptr;
        recompui::TextInput* join_input = nullptr;
        recompui::Label* code_label = nullptr;
        recompui::Button* copy_button = nullptr;
        std::string shown_code;             // the join code on show, for Copy
        recompui::Element* status_row = nullptr;   // hidden while there's nothing to say
        recompui::Label* status_label = nullptr;
        recompui::Element* host_section = nullptr; // one at a time, by the Host and Join tabs
        recompui::Element* join_section = nullptr;
        recompui::Button* host_button = nullptr;
        recompui::Button* join_button = nullptr;
        recompui::Button* cancel_button = nullptr;
        int players = 2;
        int local_players = 1;              // the players on this computer, hosting or joining
        bool hosting = false;               // a match is hosted (connecting, as the host)
        bool own_view_for_everyone = true;  // hosting: the match's screens rule
        int input_delay = -1;               // hosting: reads, or -1 for auto
        std::string port_text = std::to_string(default_port);
        std::string address_text;
        std::string join_text;

        recompui::Label* text(recompui::Element* parent, const std::string& words, recompui::theme::Typography typography,
            recompui::theme::color color = recompui::theme::color::Text) {
            recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
            label->set_color(color);
            label->set_flex_shrink(0.0f);
            return label;
        }

        recompui::Element* column(recompui::Element* parent, float gap) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Column);
            box->set_gap(gap);
            box->set_width(100.0f, recompui::Unit::Percent);
            box->set_align_items(recompui::AlignItems::FlexStart);
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::Element* row(recompui::Element* parent) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Row);
            box->set_align_items(recompui::AlignItems::Center);
            box->set_gap(16.0f);
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::TextInput* input(recompui::Element* parent, const std::string& value, float width) {
            recompui::TextInput* field = recompui::get_current_context().create_element<recompui::TextInput>(parent);
            field->set_width(width);
            field->set_min_width(width);
            field->set_flex_shrink(0.0f);
            field->set_text(value);
            return field;
        }

        // The join code, once hosting (it holds the match's key, new each time).
        void show_code() {
            const bool blank = address_text.find_first_not_of(' ') == std::string::npos;
            const std::string code = hosting ? conker::lockstep::join_code(address_text) : std::string();
            if (code != shown_code) {
                copy_button->set_text("Copy");
            }
            shown_code = code;
            copy_button->set_display(code.empty() ? recompui::Display::None : recompui::Display::Flex);
            if (blank) {
                code_label->set_text("Enter your address to get a join code.");
            }
            else if (!hosting) {
                code_label->set_text("Press Host: the join code to send the others appears here.");
            }
            else if (code.empty()) {
                code_label->set_text("That isn't an address: four numbers such as 203.0.113.7, or an IPv6 one.");
            }
            else {
                code_label->set_text("Join code: " + code);
            }
        }

        void set_state(State next, const std::string& status) {
            state = next;
            if (state == State::idle) {
                hosting = false;
            }
            show_code();
            status_label->set_text(status);
            status_row->set_display(status.empty() && state != State::connecting ? recompui::Display::None : recompui::Display::Flex);
            host_button->set_enabled(state == State::idle);
            join_button->set_enabled(state == State::idle);
            cancel_button->set_display(state == State::connecting ? recompui::Display::Flex : recompui::Display::None);
            // An update applies the text (and, while connecting, polls).
            queue_update();
        }

        // Hosting or joining: the Hello from the stored ROM, then lockstep's begin.
        void begin() {
            std::ifstream file(stored_rom, std::ios::binary);
            const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (rom.empty()) {
                set_state(State::idle, "Load the ROM first (Start Game on the launcher).");
                return;
            }
            std::string error;
            if (!conker::lockstep::begin(conker::netplay::hello_for_rom(rom), error)) {
                set_state(State::idle, "Couldn't: " + error);
                return;
            }
            set_state(State::connecting, "Connecting...");
        }

        void on_host() {
            const int port = std::atoi(port_text.c_str());
            if (port < 1 || port > 65535) {
                set_state(State::idle, "The port is a number from 1 to 65535.");
                return;
            }
            if (address_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Give your address first: the join code is made from it.");
                return;
            }
            if (local_players >= players) {
                set_state(State::idle, "With " + std::to_string(local_players) + " players on this computer, the match needs " +
                    std::to_string(local_players + 1) + " or more players: someone has to join.");
                return;
            }
            conker::lockstep::set_host(port, players, own_view_for_everyone, input_delay, local_players);
            hosting = true;
            begin();
            show_code();
        }

        void on_join() {
            if (join_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Type the host's join code first.");
                return;
            }
            conker::lockstep::set_join(join_text, local_players);
            begin();
        }

        void on_cancel() {
            conker::lockstep::cancel();
            conker::lockstep::set_host(-1, players);
            set_state(State::idle, "Cancelled.");
        }

    protected:
        std::string_view get_type_name() override { return "NetplayPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) {
                return;
            }
            if (state == State::connecting) {
                std::string status;
                switch (conker::lockstep::poll_connect(status)) {
                    case conker::lockstep::Progress::waiting:
                        status_label->set_text(status);
                        break;
                    case conker::lockstep::Progress::failed:
                        conker::lockstep::set_host(-1, players);
                        set_state(State::idle, status);
                        break;
                    case conker::lockstep::Progress::started:
                        set_state(State::starting, "Starting the game...");
                        conker::harness::begin_menu_match([](const std::string& why) {
                            std::lock_guard lock(over_mutex);
                            over_text = why;
                            over_pending = true;
                        });
                        recomp::start_game(game_id, {});
                        recompui::hide_all_contexts();
                        return;
                }
                // Polled again next frame while connecting (set_state starts it).
                queue_update();
            }
        }

    public:
        NetplayPage(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update)) {
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            set_display(recompui::Display::Flex);
            set_flex_direction(recompui::FlexDirection::Column);
            set_gap(28.0f);
            set_padding(32.0f);
            set_width(100.0f, recompui::Unit::Percent);
            set_background_color(recompui::theme::color::Background1);
            set_as_navigation_container(recompui::NavigationType::Vertical);

            recompui::Element* intro = column(this, 8.0f);
            text(intro, "Netplay", Typography::Header3);
            text(intro, "Play multiplayer online. Everyone needs this same build and the same ROM, with no mods on "
                "(texture packs are fine). The game starts from the beginning.",
                Typography::Body, color::TextDim);

            // How it goes: at the top, where it's seen whatever part of the page is scrolled to.
            status_row = row(this);
            status_label = text(status_row, "", Typography::LabelLG);
            cancel_button = context.create_element<recompui::Button>(status_row, "Cancel", recompui::ButtonStyle::Tertiary);
            cancel_button->add_pressed_callback([this]() { on_cancel(); });

            // Who plays on this computer, hosting or joining.
            recompui::Element* here = column(this, 12.0f);
            recompui::Element* here_row = row(here);
            text(here_row, "Players on this computer", Typography::LabelMD);
            recompui::Radio* here_radio = context.create_element<recompui::Radio>(here_row);
            for (int n = 1; n < conker::netplay::max_players; n++) {
                here_radio->add_option(std::to_string(n));
            }
            here_radio->set_index(0);
            here_radio->add_index_changed_callback([this](uint32_t index) { local_players = 1 + (int)index; });
            text(here, "One controller each, in the order they first press a button. The first player also has the "
                "keyboard, mouse and gyro.", Typography::Body, color::TextDim);

            // Hosting or joining: a tab each, so only the one chosen's settings show.
            recompui::TabSet* tabs = context.create_element<recompui::TabSet>(this);
            tabs->set_flex_shrink(0.0f);
            tabs->add_tab("Host a match");
            tabs->add_tab("Join a match");

            // Hosting.
            recompui::Element* host = column(this, 12.0f);
            host_section = host;
            recompui::Element* host_row = row(host);
            text(host_row, "Players", Typography::LabelMD);
            recompui::Radio* count_radio = context.create_element<recompui::Radio>(host_row);
            for (int n = 2; n <= conker::netplay::max_players; n++) {
                count_radio->add_option(std::to_string(n));
            }
            count_radio->set_index(0);
            count_radio->add_index_changed_callback([this](uint32_t index) { players = 2 + (int)index; });
            text(host_row, "UDP port", Typography::LabelMD);
            port_input = input(host_row, port_text, 120.0f);
            port_input->add_text_changed_callback([this](const std::string& value) { port_text = value; });
            text(host_row, "Your address", Typography::LabelMD);
            address_input = input(host_row, "", 320.0f);
            address_input->add_text_changed_callback([this](const std::string& value) {
                address_text = value;
                show_code();
            });
            recompui::Element* screens_row = row(host);
            text(screens_row, "Screens", Typography::LabelMD);
            recompui::Radio* screens_radio = context.create_element<recompui::Radio>(screens_row);
            screens_radio->add_option("Everyone sees only their own view");
            screens_radio->add_option("Players choose");
            screens_radio->set_index(0);
            screens_radio->add_index_changed_callback([this](uint32_t index) { own_view_for_everyone = index == 0; });
            recompui::Element* delay_row = row(host);
            text(delay_row, "Input delay", Typography::LabelMD);
            recompui::Radio* delay_radio = context.create_element<recompui::Radio>(delay_row);
            delay_radio->add_option("Auto");
            for (int reads = 0; reads <= 4; reads++) {
                delay_radio->add_option(std::to_string(reads));
            }
            delay_radio->set_index(0);
            delay_radio->add_index_changed_callback([this](uint32_t index) { input_delay = (int)index - 1; });
            text(host, "Auto suits most matches (0 at home). Raise it if the game stutters waiting for a player.",
                Typography::Body, color::TextDim);
            recompui::Element* code_row = row(host);
            code_label = text(code_row, "", Typography::LabelMD);
            copy_button = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Tertiary);
            copy_button->add_pressed_callback([this]() {
                // Said on the button: the status line is busy while waiting for players.
                if (!shown_code.empty() && SDL_SetClipboardText(shown_code.c_str()) == 0) {
                    copy_button->set_text("Copied");
                    queue_update();
                }
            });
            text(host, "For players outside your home, forward the UDP port on your router and use your public "
                "address (any \"what is my IP\" site). The join code contains your IP address: share it only with "
                "people you trust.", Typography::Body, color::TextDim);
            host_button = context.create_element<recompui::Button>(host, "Host", recompui::ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { on_host(); });

            // Joining.
            recompui::Element* join = column(this, 12.0f);
            join_section = join;
            recompui::Element* join_row = row(join);
            text(join_row, "Join code", Typography::LabelMD);
            join_input = input(join_row, "", 520.0f);
            join_input->add_text_changed_callback([this](const std::string& value) { join_text = value; });
            recompui::Button* paste_button = context.create_element<recompui::Button>(join_row, "Paste", recompui::ButtonStyle::Tertiary);
            paste_button->add_pressed_callback([this]() {
                char* pasted = SDL_GetClipboardText();
                if (pasted != nullptr) {
                    // One line, without the spaces around it (the code reads dashes and spaces anyway).
                    std::string text = pasted;
                    SDL_free(pasted);
                    text = text.substr(0, text.find_first_of("\r\n"));
                    while (!text.empty() && text.back() == ' ') text.pop_back();
                    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
                    join_text = text.substr(0, 64);
                    join_input->set_text(join_text);
                    queue_update();     // which applies the field's new value
                }
            });
            join_button = context.create_element<recompui::Button>(join, "Join", recompui::ButtonStyle::Primary);
            join_button->add_pressed_callback([this]() { on_join(); });

            tabs->set_change_tab_callback([this](int index) {
                host_section->set_display(index == 0 ? recompui::Display::Flex : recompui::Display::None);
                join_section->set_display(index == 1 ? recompui::Display::Flex : recompui::Display::None);
                queue_update();
            });
            tabs->set_active_tab(0, false);
            join_section->set_display(recompui::Display::None);

            // What this window shows of the split screen.
            recompui::Element* display = column(this, 12.0f);
            text(display, "Your screen", Typography::LabelLG);
            recompui::Radio* display_radio = context.create_element<recompui::Radio>(display);
            display_radio->add_option("Split screen");
            display_radio->add_option("My view only");
            display_radio->set_index(conker::netplay_view::is_mine_only() ? 1 : 0);
            display_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_view::set_mine_only(index == 1); });
            text(display, "\"My view only\" fills the window with your view (two players here: one above the other). "
                "The host's Screens setting can choose it for everyone.",
                Typography::Body, color::TextDim);

            set_state(State::idle, "");
            show_code();
            queue_update();
        }

        bool connecting() const { return state == State::connecting; }
        void cancel_if_connecting() {
            if (state == State::connecting) {
                on_cancel();
            }
        }
    };

    NetplayPage* page = nullptr;
}

void conker::netplay_menu::init(const std::u8string& id, const std::filesystem::path& rom) {
    game_id = id;
    stored_rom = rom;
}

void conker::netplay_menu::open() {
    if (modal == nullptr) {
        // The launcher's option calls this with the launcher's context open, and only one can be: it's
        // closed while the menu's is made.
        recompui::ContextId launcher = recompui::get_current_context();
        const bool launcher_open = launcher.slot_id != recompui::ContextId::null().slot_id;
        if (launcher_open) {
            launcher.close();
        }
        modal = recompui::Modal::create_modal(recompui::ModalType::Fullscreen);
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        // In a scroll container: the page is taller than a small window, and squeezed into it its rows and
        // texts would be drawn over each other.
        modal->get_body()->set_overflow_y(recompui::Overflow::Hidden);
        recompui::ScrollContainer* scroll = context.create_element<recompui::ScrollContainer>(modal->get_body(), recompui::ScrollDirection::Vertical);
        scroll->set_background_color(recompui::theme::color::Background1);    // the page's, below it too
        page = context.create_element<NetplayPage>(scroll);
        context.close();
        if (launcher_open) {
            launcher.open();
        }
        // Closing it while connecting gives up.
        modal->set_on_close_callback([]() {
            if (page != nullptr) {
                page->cancel_if_connecting();
            }
        });
    }
    modal->open();
}

void conker::netplay_menu::update() {
    std::string why;
    {
        std::lock_guard lock(over_mutex);
        if (!over_pending) {
            return;
        }
        over_pending = false;
        why = over_text;
    }
    if (!why.empty()) {
        why[0] = (char)std::toupper((unsigned char)why[0]);
    }
    recompui::open_info_prompt("The match is over",
        why + ". The game goes on without anyone's input from here: quit when you like.", "Quit",
        []() { ultramodern::quit(); });
}
