// The launcher's Netplay menu: host a match or join one by its code, wait for
// every player there, then the game starts on every machine at once. It's a modal of its own over the
// launcher; the connecting is lockstep's (lockstep.hpp), a step at each of the menu's updates, so a
// refusal (another build, ROM or settings, a full match) is said here and the player can try again.
//
// A match started from here doesn't quit when it ends (a desync, the host leaving): the game stops taking
// input and a prompt says why, with Quit.
//
// By default a match goes through a room on a netplay server (tools/netplay/server/): the server gives the
// host a room code, with no address in it, and the players reach each other through its relay, so nobody
// sees another's address (unless direct connections are allowed) and no port needs forwarding. The server's
// address and that choice are settings of their own, kept in netplay.json in the config folder.
//
// A room can be public: the server lists it, under a name made from its number, for players
// whose build, ROM, settings and mods are the same, on platforms that play together. The Play tab shows
// that list while it's open, asked for again every few seconds, and joins a room from it. Quick Match joins
// the first open one that fits, or hosts one in public with settings of its own (a place for one more player,
// lockstep's defaults).
//
// The tabs: Play (Quick Match, a code, the lists), Host (the match's players and who can join, the rest
// under More options) and Settings (the server, peer-to-peer, the connection test, this window's screen).
//
// Once hosting or in, the setup gives way to the lobby: the room or join code to share (hosting), and the
// waiting room, which shows who's there (by names made from their player ids), whether they're ready, their
// platform and round trip; the match starts once it's full and everyone's ready (the host's Ready is Start).
// The title and the status line stay above the page's scrolling part. The host can kick a player; anyone can
// block one (kept in netplay.json: a host refuses them, a joiner leaves a room they're in).
//
// A match hosted directly can be visible on this network: games there list it on their Play tab (heard
// from its beacon: lan.hpp) and join it with no code and no server.
//
// Or directly: the host's join code is made, once hosting, from the address the host gives and the match's
// random key: the game can't know its public address without a server to ask, so the host looks it up (or
// gives its local one, for a match at home). The code is the host's address, so the menu says to share it
// only with people the host trusts. The input delay is lockstep's default, the same on every machine.
//
// Rollback (--rollback) is the host's choice too, the same for everyone in the match.
//
// Spectators: Watch (by a code, or on a listed match) joins with nobody playing here: the match is watched, from its
// start or as it's played, never waited for. The host chooses whether the match takes them (More options), and sees
// them in the waiting room and the Esc menu's Netplay tab, where it can kick them as it can a player. There, during a match,
// and in the waiting room before it, a spectator can take an open place (Play as), a joiner's players can step out to
// watch (Watch instead), and the host can seat a spectator (Seat as: a free place, or one someone left; Swap with: a
// joiner's, whose players watch) or make a joiner's players spectators (all of that machine's).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <SDL.h>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_config_page.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "elements/ui_radio.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_tab_set.h"
#include "elements/ui_text_input.h"
#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../conker.hpp"
#include "hello.hpp"
#include "lockstep.hpp"
#include "net/join_code.hpp"
#include "net/session.hpp"

namespace {
    constexpr int default_port = 27015;
    // The netplay server players use unless they set another.
    // None by default: matches connect directly (a join code, or this network's) until a player sets one.
    constexpr const char* default_server = "";

    // What the Host tab and Players on this computer were last set to, to be so again next time.
    struct HostChoices {
        int players = 2;
        bool public_room = false;
        int input_delay = -1;       // -1: auto
        int rollback = 12;          // rollback by default (the playtest felt it smoother than lockstep)
        bool own_view = true;
        bool directly = true;       // without a server (there's none by default)
        int port = default_port;
        std::string address;
        bool lan_visible = true;
        int local_players = 1;
        bool spectators = true;
        int characters = 0;         // Multiplayer Characters' (more_bots.cpp): 0, the game's own, 12 or 16
    };

    // The match this player joined last (not one they hosted), to come back to it after the game quit:
    // its code, the players on this computer then (they get their places back only with as many), and when it
    // started. Forgotten once it ends in a way that can't be come back from.
    struct LastMatch {
        std::string code;
        int local_players = 1;
        int64_t started = 0;        // seconds since 1970
    };
    // Rejoin last match is offered this long after the match started.
    constexpr int64_t last_match_seconds = 2 * 60 * 60;

    // The server settings, kept in netplay.json in the config folder, with this player's id (random, made the
    // first time: others see a name made from it), their block list (player ids), the Host tab's choices and
    // the match joined last.
    struct ServerSettings {
        std::string server = default_server;
        bool allow_direct = false;
        uint64_t player = 0;
        std::vector<uint64_t> blocked;
        HostChoices host;
        LastMatch last;
    };

    std::string hex_id(uint64_t id) {
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)id);
        return text;
    }

    std::filesystem::path settings_path() {
        return recomp::get_config_path() / "netplay.json";
    }

    int64_t now_seconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // A seat's platform, or empty if it's not known.
    std::string platform_name(int platform) {
        static const char* const names[] = { "", "Linux", "Windows", "macOS" };
        return platform >= 1 && platform <= 3 ? names[platform] : "";
    }

    // The settings in netplay.json, the defaults for what isn't there. `readable` says whether the file was
    // (or there's none): one that's there but isn't JSON is better kept than written over with the defaults.
    ServerSettings load_settings(bool* readable = nullptr) {
        ServerSettings settings;
        std::ifstream file(settings_path());
        const nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (readable != nullptr) {
            *readable = !file || json.is_object();
        }
        if (json.is_object()) {
            if (json.contains("server") && json["server"].is_string()) settings.server = json["server"].get<std::string>();
            if (json.contains("allow_direct") && json["allow_direct"].is_boolean()) settings.allow_direct = json["allow_direct"].get<bool>();
            if (json.contains("player") && json["player"].is_string()) settings.player = std::strtoull(json["player"].get<std::string>().c_str(), nullptr, 16);
            if (json.contains("blocked") && json["blocked"].is_array()) {
                for (const auto& id : json["blocked"]) {
                    const uint64_t player = id.is_string() ? std::strtoull(id.get<std::string>().c_str(), nullptr, 16) : 0;
                    if (player != 0) settings.blocked.push_back(player);
                }
            }
            // Each value only if it's one the menu offers.
            const auto number = [](const nlohmann::json& object, const char* key, int low, int high, int& value) {
                // Read wide: a value too big for an int mustn't wrap into the range.
                if (object.contains(key) && object[key].is_number_integer() && object[key].get<int64_t>() >= low &&
                    object[key].get<int64_t>() <= high) {
                    value = (int)object[key].get<int64_t>();
                }
            };
            const auto flag = [](const nlohmann::json& object, const char* key, bool& value) {
                if (object.contains(key) && object[key].is_boolean()) value = object[key].get<bool>();
            };
            if (json.contains("host") && json["host"].is_object()) {
                const nlohmann::json& host = json["host"];
                HostChoices& choices = settings.host;
                number(host, "players", 2, conker::netplay::max_players, choices.players);
                flag(host, "public", choices.public_room);
                number(host, "input_delay", -1, 4, choices.input_delay);
                number(host, "rollback", 0, 12, choices.rollback);
                choices.rollback -= choices.rollback % 4;
                flag(host, "own_view", choices.own_view);
                flag(host, "directly", choices.directly);
                number(host, "port", 1, 65535, choices.port);
                if (host.contains("address") && host["address"].is_string()) choices.address = host["address"].get<std::string>().substr(0, 64);
                flag(host, "lan_visible", choices.lan_visible);
                number(host, "local_players", 1, conker::netplay::max_players - 1, choices.local_players);
                flag(host, "spectators", choices.spectators);
                number(host, "characters", 0, 16, choices.characters);
                if (choices.characters != 12 && choices.characters != 16) choices.characters = 0;
            }
            if (json.contains("last_match") && json["last_match"].is_object()) {
                const nlohmann::json& last = json["last_match"];
                if (last.contains("code") && last["code"].is_string() && last.contains("started") && last["started"].is_number_integer() &&
                    last["started"].get<int64_t>() > 0) {
                    settings.last.code = last["code"].get<std::string>().substr(0, 64);
                    settings.last.started = last["started"].get<int64_t>();
                    number(last, "local_players", 1, conker::netplay::max_players - 1, settings.last.local_players);
                }
            }
        }
        return settings;
    }

    void save_settings(const ServerSettings& settings) {
        std::ofstream file(settings_path(), std::ios::trunc);
        if (file) {
            nlohmann::json blocked = nlohmann::json::array();
            for (const uint64_t player : settings.blocked) blocked.push_back(hex_id(player));
            const HostChoices& choices = settings.host;
            nlohmann::json json{ { "server", settings.server }, { "allow_direct", settings.allow_direct },
                { "player", hex_id(settings.player) }, { "blocked", blocked },
                { "host", { { "players", choices.players }, { "public", choices.public_room }, { "input_delay", choices.input_delay },
                    { "rollback", choices.rollback }, { "own_view", choices.own_view }, { "directly", choices.directly },
                    { "port", choices.port }, { "address", choices.address }, { "lan_visible", choices.lan_visible },
                    { "local_players", choices.local_players }, { "spectators", choices.spectators },
                    { "characters", choices.characters } } } };
            if (!settings.last.code.empty()) {
                json["last_match"] = { { "code", settings.last.code }, { "local_players", settings.last.local_players },
                    { "started", settings.last.started } };
            }
            file << json.dump(4) << "\n";
        }
    }

    // Adds `player` to the block list (kept), and tells lockstep (a host kicks them).
    void block_player(uint64_t player) {
        bool readable = false;
        ServerSettings settings = load_settings(&readable);
        // Never this player's own id (two windows sharing a config folder share one).
        if (player == settings.player) {
            return;
        }
        if (readable && player != 0 && std::find(settings.blocked.begin(), settings.blocked.end(), player) == settings.blocked.end()) {
            settings.blocked.push_back(player);
            save_settings(settings);
        }
        conker::lockstep::block(player);
    }

    // The match joined last (`code` empty: forgets it), kept at once: the game may never come back to the menu.
    void remember_match(const std::string& code, int local_players) {
        bool readable = false;
        ServerSettings settings = load_settings(&readable);
        if (!readable) {
            return;
        }
        settings.last = LastMatch{ code, local_players, code.empty() ? 0 : now_seconds() };
        save_settings(settings);
    }

    // Whether Rejoin last match is offered for `last`.
    bool may_rejoin(const LastMatch& last) {
        const int64_t now = now_seconds();
        return !last.code.empty() && now >= last.started && now - last.started < last_match_seconds;
    }

    // The settings, with a player id made the first time.
    ServerSettings load_or_make_settings() {
        ServerSettings settings = load_settings();
        // (The system's random source: who may come back to a place, and who's kicked or blocked, go by it. Without
        // one, none is kept and each match makes its own, lockstep::begin.)
        if (settings.player == 0 && conker::netplay::random_key(settings.player)) {
            save_settings(settings);
        }
        return settings;
    }

    std::string trimmed(std::string text) {
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        return text;
    }

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
        recompui::Element* setup_box = nullptr;    // the menu while idle: the settings, Host and Join
        recompui::Element* lobby_box = nullptr;    // in its place while connecting: the code and the waiting room
        recompui::Element* code_box = nullptr;     // hosting: the code to share
        recompui::Label* code_title = nullptr;
        recompui::Label* code_label = nullptr;
        recompui::Label* code_hint = nullptr;
        recompui::Button* copy_button = nullptr;
        bool shown_in = false;                     // the waiting room was on show (its Ready is focused when it comes)
        bool refocus = false;                      // a button of the waiting room was pressed: Ready is focused once it's made again
        std::vector<recompui::Label*> seat_about;  // each seat's "ready, platform, round trip", or null for a free one
        std::vector<std::string> shown_about;      // what they show
        std::string shown_code;             // the join code on show, for Copy
        recompui::Element* status_row = nullptr;   // hidden while there's nothing to say
        recompui::Element* waiting_box = nullptr;  // the waiting room, while connecting
        std::string shown_waiting;                 // what it shows, to make it again only when that changes
        recompui::Label* status_label = nullptr;
        recompui::Element* direct_rows = nullptr;  // hosting directly: the port and address
        recompui::Element* room_row = nullptr;     // hosting through a room: private or public
        recompui::Radio* here_radio = nullptr;     // Players on this computer
        recompui::Element* rejoin_row = nullptr;   // Rejoin last match, while there's one to come back to
        recompui::Label* rejoin_label = nullptr;
        recompui::Label* clipboard_hint = nullptr; // the code field was filled from the clipboard
        std::string clipboard_code;                // what it was filled with (each code on the clipboard is offered once)
        std::string copied_code;                   // a code this menu put on the clipboard (Copy): not offered back
        recompui::Element* advanced_box = nullptr; // the Host tab's More options
        recompui::Button* advanced_button = nullptr;
        recompui::Label* advanced_summary = nullptr;   // what they're set to, while they're hidden
        bool advanced_open = false;
        recompui::Element* rooms_box = nullptr;    // the public rooms, a row each
        recompui::Label* rooms_label = nullptr;    // how the list goes, or what it shows
        std::string shown_rooms;                   // what rooms_box shows, to make it again only when that changes
        bool browsing = false;                     // the Play tab is open: the public list is polled
        recompui::Element* lan_box = nullptr;      // the matches on this network, a row each
        recompui::Label* lan_label = nullptr;
        std::string shown_lan;
        bool lan_listening = false;                // the Play tab is open: this network's matches are polled
        bool testing = false;                      // Test connection runs (the room list waits for it)
        recompui::Element* test_box = nullptr;     // its findings, a line each
        recompui::Button* test_button = nullptr;
        std::chrono::steady_clock::time_point lan_since;   // listening since
        bool lan_shown = false;                    // the list (or that it's empty) is on show
        bool lan_visible = false;                  // hosting directly: visible on this network
        bool join_tab = false;                     // the Play tab is the one open (from the menu's first on_open)
        bool closing = false;                      // the menu is closing: no room list
        std::optional<conker::netplay::Hello> browse_hello;   // the room list's, made once each time the menu opens
        recompui::Element* join_section = nullptr; // one at a time, by the Play, Host and Settings tabs
        recompui::Element* host_section = nullptr;
        recompui::Element* settings_section = nullptr;
        recompui::Button* host_button = nullptr;
        recompui::Button* join_button = nullptr;
        recompui::Button* watch_button = nullptr;
        recompui::Button* cancel_button = nullptr;
        int players = 2;
        int local_players = 1;              // the players on this computer, hosting or joining
        bool hosting = false;               // a match is hosted (connecting, as the host)
        bool quick = false;                 // Quick Match is connecting (it may end up hosting a room)
        bool host_directly = false;         // hosting: by a join code (a port), not a room on the server
        bool public_room = false;           // hosting through a room: in the server's public list
        ServerSettings settings = load_or_make_settings();
        bool own_view_for_everyone = true;  // hosting: the match's screens rule
        int input_delay = -1;               // hosting: reads, or -1 for auto
        int rollback = 12;                  // hosting: reads predicted, 0 (off) to 12
        bool spectators = true;             // hosting: the match takes spectators
        int characters = 0;                 // hosting: the most characters in a multiplayer game (0: the game's own)
        bool watching = false;              // joining (or in) as a spectator
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

        // The room code, or the join code (it holds the match's key, new each time), at the top of the lobby
        // while hosting.
        void show_code() {
            const bool blank = address_text.find_first_not_of(' ') == std::string::npos;
            // Quick Match hosts through the server, once it finds no room to join.
            const bool quick_hosting = quick && state == State::connecting && conker::lockstep::waiting_room().hosting;
            const bool host_directly = this->host_directly && !quick_hosting;
            const bool public_room = this->public_room || quick_hosting;
            const bool hosting = this->hosting || quick_hosting;
            const std::string code = !hosting ? std::string() :
                host_directly ? conker::lockstep::join_code(address_text) : conker::lockstep::room_code();
            if (code != shown_code) {
                copy_button->set_text("Copy");
            }
            shown_code = code;
            copy_button->set_display(code.empty() ? recompui::Display::None : recompui::Display::Flex);
            code_box->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
            const std::string lan = lan_visible ? " Players on this network also find the match on their Play tab." : "";
            if (!host_directly) {
                code_title->set_text("Room code");
                code_label->set_text(code.empty() ? "Making a room..." : code);
                code_hint->set_text(public_room ? "Public: the room is in the server's list too. Send the code to anyone "
                    "else you want to play with." : "Private: send this code to the people you want to play with.");
            }
            else if (blank) {
                // Only with the match visible on this network (on_host asks for an address otherwise).
                code_title->set_text("On this network");
                code_label->set_text("Visible");
                code_hint->set_text("Players on this network find the match on their Play tab. Give your address (Host "
                    "tab, More options) for a join code too.");
            }
            else if (code.empty()) {
                code_title->set_text("Join code");
                code_label->set_text("None");
                code_hint->set_text("That isn't an address: four numbers such as 203.0.113.7, or an IPv6 one. Close the "
                    "room and host again." + lan);
            }
            else {
                code_title->set_text("Join code");
                code_label->set_text(code);
                code_hint->set_text("It contains your IP address: share it only with people you trust." + lan);
            }
        }

        void set_state(State next, const std::string& status) {
            state = next;
            if (state == State::idle) {
                hosting = false;
                quick = false;
                watching = false;
                shown_in = false;
            }
            show_code();
            status_label->set_text(status);
            status_row->set_display(status.empty() && state != State::connecting ? recompui::Display::None : recompui::Display::Flex);
            host_button->set_enabled(state == State::idle);
            join_button->set_enabled(state == State::idle);
            watch_button->set_enabled(state == State::idle);
            // Nothing in the setup can change a match under way: it's hidden until it's idle again.
            setup_box->set_display(state == State::idle ? recompui::Display::Flex : recompui::Display::None);
            lobby_box->set_display(state == State::idle ? recompui::Display::None : recompui::Display::Flex);
            // In the room, its Leave takes Cancel's place.
            cancel_button->set_display(state == State::connecting && !shown_in ? recompui::Display::Flex : recompui::Display::None);
            // The room list only while nothing is being connected (it's asked for again once it's idle).
            rooms_box->set_display(state == State::idle ? recompui::Display::Flex : recompui::Display::None);
            lan_box->set_display(state == State::idle ? recompui::Display::Flex : recompui::Display::None);
            if (state == State::idle && join_tab && !browsing && !closing && !testing) {
                start_browsing();
            }
            // An update applies the text (and, while connecting, polls).
            queue_update();
        }

        // Saves the settings, with the block list and the match joined last as they're kept (the Esc menu's
        // Netplay tab may have added to the one, the end of a match changed the other).
        void save() {
            const ServerSettings kept = load_settings();
            settings.blocked = kept.blocked;
            settings.last = kept.last;
            save_settings(settings);
        }

        // One line of `text`, without the spaces around it, at most 64 characters (the codes read dashes and
        // spaces anyway).
        static std::string one_line(std::string text) {
            return trimmed(text.substr(0, text.find_first_of("\r\n"))).substr(0, 64);
        }

        void set_join_text(const std::string& code) {
            join_text = code;
            join_input->set_text(code);
            clipboard_hint->set_display(recompui::Display::None);
            queue_update();     // which applies the field's new value
        }

        // A room code or a join code on the clipboard (one this menu didn't copy) fills the empty code field.
        void offer_clipboard() {
            if (state != State::idle || join_text.find_first_not_of(' ') != std::string::npos || !SDL_HasClipboardText()) {
                return;
            }
            char* pasted = SDL_GetClipboardText();
            if (pasted == nullptr) {
                return;
            }
            const std::string code = one_line(pasted);
            SDL_free(pasted);
            if (code.empty() || code == copied_code || code == clipboard_code ||
                (!conker::netplay::decode_room_code(code) && !conker::netplay::decode_join_code(code))) {
                return;
            }
            clipboard_code = code;
            set_join_text(code);
            clipboard_hint->set_display(recompui::Display::Block);
        }

        // Rejoin last match, while the match joined last may still be played.
        void show_rejoin() {
            const LastMatch last = load_settings().last;
            const bool offered = may_rejoin(last);
            rejoin_row->set_display(offered ? recompui::Display::Flex : recompui::Display::None);
            if (!offered) {
                return;
            }
            const int64_t minutes = (now_seconds() - last.started) / 60;
            const std::optional<uint64_t> room = conker::netplay::decode_room_code(last.code);
            std::string about = room ? conker::netplay::room_name(*room) + " (" + last.code + ")" : last.code;
            about += minutes < 1 ? ", started just now" : minutes == 1 ? ", started a minute ago" :
                ", started " + std::to_string(minutes) + " minutes ago";
            rejoin_label->set_text(about);
        }

        void on_rejoin() {
            const LastMatch last = load_settings().last;
            if (state != State::idle) {
                return;
            }
            // The menu may have stayed open past the time it's offered for.
            if (!may_rejoin(last)) {
                show_rejoin();
                set_state(State::idle, "That match started too long ago to come back to.");
                return;
            }
            // As many players on this computer as then: the host gives the places back only so.
            local_players = last.local_players;
            here_radio->set_index((uint32_t)(local_players - 1));
            set_join_text(last.code);
            on_join();
        }

        // This game's Hello, from the stored ROM (false: there's none yet).
        bool my_hello(conker::netplay::Hello& hello) {
            std::ifstream file(stored_rom, std::ios::binary);
            const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (rom.empty()) {
                return false;
            }
            hello = conker::netplay::hello_for_rom(rom);
            return true;
        }

        // Hosting or joining: the Hello from the stored ROM, then lockstep's begin.
        void begin() {
            stop_browsing();
            stop_test();
            conker::netplay::Hello hello;
            if (!my_hello(hello)) {
                set_state(State::idle, "Load the ROM first (Start Game on the launcher).");
                return;
            }
            std::string error;
            conker::lockstep::set_identity(settings.player, settings.blocked, false);
            if (!conker::lockstep::begin(hello, error)) {
                set_state(State::idle, "Couldn't: " + error);
                return;
            }
            set_state(State::connecting, "Connecting...");
            // The button pressed is hidden with the setup: a controller can still give up.
            cancel_button->focus();
        }

        static std::string seat_text(const conker::lockstep::Seat& seat) {
            std::string about = seat.ready ? "ready" : "not ready";
            about += !platform_name(seat.platform).empty() ? ", " + platform_name(seat.platform) : "";
            about += seat.round_trip_ms >= 0 ? ", " + std::to_string(seat.round_trip_ms) + " ms" : "";
            return about;
        }

        // The waiting room as it is now (while connecting), made again when who's there or who's ready changes.
        // A round trip only changes its seat's text: made again, the room would lose a controller's focus.
        void show_waiting() {
            const conker::lockstep::WaitingRoom room = state == State::connecting ? conker::lockstep::waiting_room()
                                                                                    : conker::lockstep::WaitingRoom{};
            std::string summary = room.in ? (room.ready ? "R" : "r") : "";
            for (const conker::lockstep::Seat& seat : room.seats) {
                summary += "|" + std::to_string(seat.player) + (seat.ready ? "R" : "");
            }
            for (const conker::lockstep::Spectator& spectator : room.spectators) {
                summary += "|w" + std::to_string(spectator.number) + ":" + std::to_string(spectator.player);
            }
            if (summary == shown_waiting) {
                for (size_t port = 0; port < seat_about.size() && port < room.seats.size(); port++) {
                    const std::string about = seat_text(room.seats[port]);
                    if (seat_about[port] != nullptr && about != shown_about[port]) {
                        seat_about[port]->set_text(about);
                        shown_about[port] = about;
                    }
                }
                return;
            }
            seat_about.clear();
            shown_waiting = summary;
            waiting_box->clear_children();
            waiting_box->set_display(room.in ? recompui::Display::Flex : recompui::Display::None);
            // In the room, its Leave takes Cancel's place.
            cancel_button->set_display(state == State::connecting && !room.in ? recompui::Display::Flex : recompui::Display::None);
            const bool arrived = room.in && !shown_in;
            shown_in = room.in;
            if (!room.in) {
                return;
            }
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            std::string rules = "Waiting room: " + std::to_string(room.players) + " players, input delay " +
                (room.delay < 0 ? std::string("auto") : std::to_string(room.delay)) +
                (room.rollback > 0 ? ", rollback " + std::to_string(room.rollback) : ", no rollback") +
                (room.own_view ? ", everyone sees only their own view" : "") +
                (room.characters > 0 ? ", up to " + std::to_string(room.characters) + " characters" : "") +
                (room.takes_spectators ? "" : ", no spectators");
            text(waiting_box, rules, Typography::LabelMD);
            seat_about.assign(room.players, nullptr);
            shown_about.assign(room.players, "");
            for (int port = 0; port < room.players; port++) {
                const conker::lockstep::Seat& seat = room.seats[port];
                recompui::Element* line = row(waiting_box);
                text(line, "Player " + std::to_string(port + 1), Typography::LabelMD);
                // (Seat changes before the start: done by the host at once.)
                const auto room_button = [&](const std::string& label, std::function<void()> act) {
                    recompui::Button* button = context.create_element<recompui::Button>(line, label, recompui::ButtonStyle::Tertiary);
                    button->set_white_space(recompui::WhiteSpace::Nowrap);
                    button->add_pressed_callback([this, act]() {
                        act();
                        shown_waiting.clear();
                        refocus = true;
                        queue_update();
                    });
                };
                if (seat.player == 0) {
                    text(line, "free", Typography::Body, color::TextDim);
                    if (room.watching) {
                        room_button("Play as Player " + std::to_string(port + 1), [port]() { conker::lockstep::claim(port); });
                    }
                    continue;
                }
                text(line, seat.name + (seat.mine ? " (you)" : ""), Typography::Body);
                shown_about[port] = seat_text(seat);
                seat_about[port] = text(line, shown_about[port], Typography::Body, seat.ready ? color::Text : color::TextDim);
                if (seat.mine) {
                    if (seat.movable && !room.hosting && room.takes_spectators && room.spectators.size() < conker::netplay::max_spectators) {
                        room_button("Watch instead", []() { conker::lockstep::step_out(); });
                    }
                    continue;
                }
                const uint64_t player = seat.player;
                if (room.hosting && seat.movable && room.takes_spectators) {
                    room_button("Make spectator", [port, player]() { conker::lockstep::make_spectator(port, player); });
                }
                if (room.hosting) {
                    recompui::Button* kick = context.create_element<recompui::Button>(line, "Kick", recompui::ButtonStyle::Tertiary);
                    kick->add_pressed_callback([this, port, player]() {
                        conker::lockstep::kick(port, player);
                        shown_waiting.clear();
                        refocus = true;
                        queue_update();
                    });
                }
                recompui::Button* block = context.create_element<recompui::Button>(line, "Block", recompui::ButtonStyle::Tertiary);
                block->add_pressed_callback([this, player]() {
                    block_player(player);
                    settings.blocked = load_settings().blocked;
                    shown_waiting.clear();
                    refocus = true;
                    queue_update();
                });
            }
            add_spectator_rows(room);
            recompui::Element* buttons = row(waiting_box);
            if (room.watching) {
                // A spectator has nothing to be ready for: it watches once the players start.
                recompui::Button* leave = context.create_element<recompui::Button>(buttons, "Leave", recompui::ButtonStyle::Tertiary);
                leave->set_as_primary_focus();
                leave->add_pressed_callback([this]() { on_cancel("You left the room."); });
                if (arrived || refocus) {
                    leave->focus();
                }
                refocus = false;
                text(waiting_box, "You're watching: the match starts once the players are ready. Nobody waits for you, and "
                    "your controllers play nothing.", Typography::Body, color::TextDim);
                return;
            }
            const char* label = room.hosting ? (room.ready ? "Don't start yet" : "Start when everyone's ready") : (room.ready ? "Not ready" : "Ready");
            recompui::Button* ready = context.create_element<recompui::Button>(buttons, label,
                room.ready ? recompui::ButtonStyle::Secondary : recompui::ButtonStyle::Primary);
            // Where a controller goes when someone coming or going made the room again under its focus.
            ready->set_as_primary_focus();
            const bool now_ready = room.ready;
            ready->add_pressed_callback([this, now_ready]() {
                conker::lockstep::set_ready(!now_ready);
                shown_waiting.clear();
                refocus = true;
                queue_update();
            });
            recompui::Button* leave = context.create_element<recompui::Button>(buttons, room.hosting ? "Close room" : "Leave",
                recompui::ButtonStyle::Tertiary);
            leave->set_white_space(recompui::WhiteSpace::Nowrap);
            leave->add_pressed_callback([this, hosting = room.hosting]() {
                on_cancel(hosting ? "You closed the room." : "You left the room.");
            });
            // The host can start before every place is taken: the free ones stay open, for players joining later.
            int taken = 0;
            for (int port = 0; port < room.players; port++) {
                taken += room.seats[port].player != 0 ? 1 : 0;
            }
            if (room.hosting && taken < room.players) {
                const int free = room.players - taken;
                recompui::Button* start_now = context.create_element<recompui::Button>(buttons,
                    "Start now (" + std::to_string(free) + (free == 1 ? " open place)" : " open places)"), recompui::ButtonStyle::Secondary);
                start_now->set_white_space(recompui::WhiteSpace::Nowrap);
                start_now->add_pressed_callback([this]() {
                    conker::lockstep::start_now();
                    shown_waiting.clear();
                    refocus = true;
                    queue_update();
                });
            }
            // The Host or Join pressed is hidden now, or the button pressed was made again: a controller goes on
            // from Ready.
            if (arrived || refocus) {
                ready->focus();
            }
            refocus = false;
            text(waiting_box, room.hosting ? "The match starts once every place is taken and every player is ready, or with Start "
                "now once those in are ready: the free places stay open, and players can join the match while it's played. Kick "
                "removes a player; Block keeps them out of your matches from now on." :
                "The match starts once every place is taken and every player is ready (the host too). Block keeps a player "
                "out of your matches from now on (you leave a room they're in).", Typography::Body, color::TextDim);
        }

        // The waiting room's spectators, a row each (the host can kick them; anyone can block them).
        void add_spectator_rows(const conker::lockstep::WaitingRoom& room) {
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            for (const conker::lockstep::Spectator& spectator : room.spectators) {
                recompui::Element* line = row(waiting_box);
                text(line, "Watching", Typography::LabelMD);
                text(line, spectator.name + (spectator.mine ? " (you)" : ""), Typography::Body);
                if (!platform_name(spectator.platform).empty()) {
                    text(line, platform_name(spectator.platform), Typography::Body, color::TextDim);
                }
                if (spectator.mine) {
                    continue;
                }
                const uint64_t player = spectator.player;
                if (room.hosting) {
                    recompui::Button* kick = context.create_element<recompui::Button>(line, "Kick", recompui::ButtonStyle::Tertiary);
                    kick->add_pressed_callback([this, number = spectator.number, player]() {
                        conker::lockstep::kick_spectator(number, player);
                        shown_waiting.clear();
                        refocus = true;
                        queue_update();
                    });
                    // Into a place: a free one, or a joiner's (whose players watch instead).
                    for (int port = 1; port < room.players; port++) {
                        const conker::lockstep::Seat& seat = room.seats[port];
                        if (seat.player != 0 && !seat.movable) {
                            continue;
                        }
                        recompui::Button* seat_button = context.create_element<recompui::Button>(line,
                            (seat.player != 0 ? "Swap with Player " : "Seat as Player ") + std::to_string(port + 1), recompui::ButtonStyle::Tertiary);
                        seat_button->set_white_space(recompui::WhiteSpace::Nowrap);
                        seat_button->add_pressed_callback([this, port, number = spectator.number, player]() {
                            conker::lockstep::seat(port, number, player);
                            shown_waiting.clear();
                            refocus = true;
                            queue_update();
                        });
                    }
                }
                recompui::Button* block = context.create_element<recompui::Button>(line, "Block", recompui::ButtonStyle::Tertiary);
                block->add_pressed_callback([this, player]() {
                    block_player(player);
                    settings.blocked = load_settings().blocked;
                    shown_waiting.clear();
                    refocus = true;
                    queue_update();
                });
            }
        }

        // Through the server (hosting a room public if `listed`): false (and says why) if none is set.
        bool use_server(bool listed = false) {
            settings.server = trimmed(settings.server);
            if (settings.server.empty()) {
                set_state(State::idle, "Set a netplay server first (Settings tab).");
                return false;
            }
            save();
            conker::lockstep::set_server(settings.server, settings.allow_direct, listed);
            return true;
        }

        // The public room list: asked for while the Play tab is open and nothing is being connected.
        void start_browsing() {
            // Not while the connection test has the server.
            if (testing) {
                return;
            }
            stop_browsing();
            shown_rooms.clear();
            rooms_box->clear_children();
            settings.server = trimmed(settings.server);
            // Hashing the ROM and the mods takes a moment: once each time the menu opens (the mods can only
            // change while it's closed).
            conker::netplay::Hello hello;
            if (!browse_hello && my_hello(hello)) {
                browse_hello = hello;
            }
            std::string error;
            // This network's matches need no server.
            stop_lan();
            shown_lan.clear();
            lan_box->clear_children();
            if (!browse_hello) {
                lan_label->set_text("");
            }
            else if (!conker::lockstep::lan_browse(*browse_hello, error)) {
                lan_label->set_text("Can't look for matches on this network: " + error + ".");
            }
            else {
                lan_label->set_text("Listening for matches on this network...");
                lan_listening = true;
                lan_since = std::chrono::steady_clock::now();
                lan_shown = false;
            }
            if (settings.server.empty()) {
                rooms_label->set_text("Set a netplay server (Settings tab) to see its public rooms.");
            }
            else if (!browse_hello) {
                rooms_label->set_text("Load the ROM first (Start Game on the launcher) to see the public rooms.");
            }
            else {
                conker::lockstep::set_server(settings.server, settings.allow_direct);
                // The block list hides its players' rooms.
                conker::lockstep::set_identity(settings.player, settings.blocked, false);
                if (!conker::lockstep::browse(*browse_hello, error)) {
                    rooms_label->set_text("Couldn't get the public rooms: " + error + ".");
                }
                else {
                    rooms_label->set_text("Getting the public rooms...");
                    browsing = true;
                }
            }
            queue_update();
        }

        // Test connection: the server, its relay, the NAT and the LAN port (lockstep::start_connection_test). The
        // room list stops while it runs (one connection to the server at a time).
        void on_test() {
            if (state != State::idle || testing) {
                return;
            }
            stop_browsing();
            settings.server = trimmed(settings.server);
            conker::lockstep::set_server(settings.server, settings.allow_direct);
            test_box->clear_children();
            std::string error;
            if (!conker::lockstep::start_connection_test(error)) {
                text(test_box, "Couldn't test the connection: " + error + ".", recompui::theme::Typography::Body);
                return;
            }
            text(test_box, "Testing the connection (up to 30 seconds)...", recompui::theme::Typography::Body, recompui::theme::color::TextDim);
            testing = true;
            test_button->set_enabled(false);
            queue_update();
        }

        // Stopped before it's done (a match begins, the menu closes): nothing to show.
        void stop_test() {
            if (testing) {
                conker::lockstep::stop_connection_test();
                testing = false;
                test_button->set_enabled(true);
                test_box->clear_children();
            }
        }

        void show_test(const conker::netplay::ConnectionReport& report) {
            using Verdict = conker::netplay::ConnectionReport::Verdict;
            test_box->clear_children();
            text(test_box, report.summary, recompui::theme::Typography::LabelMD);
            const std::pair<const std::string*, Verdict> lines[] = { { &report.server_text, report.server }, { &report.relay_text, report.relay },
                { &report.direct_text, report.direct }, { &report.lan_text, report.lan } };
            for (const auto& [line, verdict] : lines) {
                if (!line->empty()) {
                    text(test_box, (verdict == Verdict::good ? "Yes: " : verdict == Verdict::bad ? "No: " : "") + *line,
                        recompui::theme::Typography::Body, recompui::theme::color::TextDim);
                }
            }
        }

        void stop_browsing() {
            if (browsing) {
                conker::lockstep::stop_browsing();
                browsing = false;
            }
            stop_lan();
        }

        void stop_lan() {
            if (lan_listening) {
                conker::lockstep::stop_lan();
                lan_listening = false;
            }
        }

        // A row for a match to join by `code`: its name, how it's going, and Join if it's open. A room of
        // another build says which release it is instead, with a button to get it (a release's).
        void add_room_row(recompui::Element* box, const conker::lockstep::ListedRoom& room, const std::string& code) {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* line = row(box);
            // Playing, with free places: anyone can join it as it's played. Full: only a player who left it can get back
            // in (the host refuses anyone else).
            const bool open = room.joined < room.players && !room.other_build;
            const bool under_way = room.playing && !room.other_build;
            text(line, room.name, recompui::theme::Typography::LabelMD);
            std::string about = std::to_string(room.joined) + " of " + std::to_string(room.players) + " players";
            about += room.playing ? ", playing" : "";
            about += room.watching > 0 ? ", " + std::to_string(room.watching) + " watching" : "";
            about += room.rollback > 0 ? ", rollback " + std::to_string(room.rollback) : "";
            about += box == lan_box ? "" : room.direct ? ", direct allowed" : ", relay only";
            if (room.other_build) {
                about += ", on " + room.release + " (yours: " + conker::netplay::release_name() + "): everyone needs the same release";
            }
            text(line, about, recompui::theme::Typography::Body, recompui::theme::color::TextDim);
            const std::string url = conker::netplay::release_url(room.release);
            if (room.other_build && !url.empty()) {
                recompui::Button* get = context.create_element<recompui::Button>(line, "Get " + room.release, recompui::ButtonStyle::Secondary);
                get->add_pressed_callback([url]() { SDL_OpenURL(url.c_str()); });
            }
            if (open || under_way) {
                recompui::Button* join_room = context.create_element<recompui::Button>(line, open ? "Join" : "Rejoin",
                    recompui::ButtonStyle::Secondary);
                join_room->add_pressed_callback([this, code]() {
                    if (state != State::idle) {
                        return;
                    }
                    join_text = code;
                    join_input->set_text(code);
                    on_join();
                });
            }
            if (!room.other_build && room.spectators) {
                recompui::Button* watch_room = context.create_element<recompui::Button>(line, "Watch", recompui::ButtonStyle::Tertiary);
                watch_room->add_pressed_callback([this, code]() {
                    if (state != State::idle) {
                        return;
                    }
                    join_text = code;
                    join_input->set_text(code);
                    on_join(true);
                });
            }
        }

        // This network's matches as they are now, made again when that changes.
        void show_lan(const std::vector<conker::lockstep::ListedRoom>& matches) {
            std::string summary;
            for (const conker::lockstep::ListedRoom& room : matches) {
                summary += "|" + room.code + " " + std::to_string(room.joined) + (room.playing ? "p" : "") + "w" +
                    std::to_string(room.watching) + (room.spectators ? "s" : "");
            }
            // Nothing yet for the first 2 s: a host's beacon comes once a second.
            const bool listening = matches.empty() && std::chrono::steady_clock::now() - lan_since < std::chrono::seconds(2);
            if (listening || (summary == shown_lan && lan_shown)) {
                return;
            }
            shown_lan = summary;
            lan_shown = true;
            lan_label->set_text(matches.empty() ? "No matches on this network. (One there should show here if its host chose "
                "\"Visible\"; if it doesn't, a firewall may be blocking UDP port 27016 on this computer.)" : "Matches on this network:");
            lan_box->clear_children();
            for (const conker::lockstep::ListedRoom& room : matches) {
                add_room_row(lan_box, room, room.code);
            }
        }

        // The list as it is now: a row each, made again when it changes.
        void show_rooms(const conker::lockstep::Browse& list) {
            if (list.state == conker::lockstep::Browse::State::failed) {
                stop_browsing();
                shown_rooms.clear();
                rooms_box->clear_children();
                rooms_label->set_text("Couldn't get the public rooms: " + list.error + ". Press Refresh to try again.");
                return;
            }
            if (list.state != conker::lockstep::Browse::State::listed) {
                return;
            }
            std::string summary = std::to_string(list.others) + " " + std::to_string(list.older);
            for (const conker::lockstep::ListedRoom& room : list.rooms) {
                summary += "|" + room.code + " " + std::to_string(room.joined) + (room.playing ? "p" : "") + (room.other_build ? "o" : "") +
                    "w" + std::to_string(room.watching) + (room.spectators ? "s" : "");
            }
            if (summary == shown_rooms) {
                return;
            }
            shown_rooms = summary;
            const std::string mods = conker::netplay_profile::mods_text();
            const bool none = std::none_of(list.rooms.begin(), list.rooms.end(), [](const auto& room) { return !room.other_build; });
            std::string words = none ? "No public rooms for this game yet: Quick Match opens one, or host one (Public, on the Host tab)." :
                "Public rooms for this game";
            // (Mods are never on in a match: lockstep::begin refuses them before the list matters.)
            words += mods.empty() ? (none ? "" : ":") : (none ? " Netplay needs your mods off: " + mods + "." :
                " (netplay needs your mods off: " + mods + "):");
            if (list.others > 0) {
                words += " " + std::to_string(list.others) + (list.others == 1 ? " more room has" : " more rooms have") +
                    " other settings than yours, so it can't be joined from here.";
            }
            if (list.older > 0) {
                words += " " + std::to_string(list.older) + (list.older == 1 ? " more room is" : " more rooms are") +
                    " on an older release than yours (" + conker::netplay::release_name() + ") and can't say which game, so it can't be joined from here.";
            }
            rooms_label->set_text(words);
            rooms_box->clear_children();
            for (const conker::lockstep::ListedRoom& room : list.rooms) {
                add_room_row(rooms_box, room, room.code);
            }
        }

        void on_host() {
            const int port = host_directly ? std::atoi(port_text.c_str()) : 0;
            if (host_directly && (port < 1 || port > 65535)) {
                set_state(State::idle, "The port is a number from 1 to 65535.");
                return;
            }
            if (host_directly && !lan_visible && address_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Give your address first (More options): the join code is made from it, or make the "
                    "match visible on this network.");
                return;
            }
            if (!host_directly && !use_server(public_room)) {
                return;
            }
            if (local_players >= players) {
                set_state(State::idle, "With " + std::to_string(local_players) + " players on this computer, the match needs " +
                    std::to_string(local_players + 1) + " or more players: someone has to join.");
                return;
            }
            settings.host = HostChoices{ players, public_room, input_delay, rollback, own_view_for_everyone, host_directly,
                host_directly ? port : settings.host.port, trimmed(address_text), lan_visible, local_players, spectators, characters };
            save();
            conker::lockstep::set_host(port, players, own_view_for_everyone, input_delay, local_players, rollback, spectators);
            conker::lockstep::set_characters(characters);
            conker::lockstep::set_lan(host_directly && lan_visible);
            hosting = true;
            begin();
            show_code();
        }

        // `watch`: as a spectator, with nobody playing here.
        void on_join(bool watch = false) {
            if (join_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Type the host's room code or join code first.");
                return;
            }
            const bool room = conker::netplay::decode_room_code(join_text).has_value();
            if (room && !use_server()) {
                return;
            }
            if (!room && !conker::netplay::decode_join_code(join_text)) {
                set_state(State::idle, "That isn't a room code (8 characters) or a join code (24 or 44): check it.");
                return;
            }
            settings.host.local_players = local_players;
            save();
            conker::lockstep::set_join(join_text, watch ? 0 : local_players);
            watching = watch;
            begin();
        }

        // Not the Host tab's settings: the room it may host has a place for one more player, with lockstep's
        // defaults (its own view for everyone, input delay auto), and the Host tab's default rollback.
        void on_quick() {
            if (!use_server()) {
                return;
            }
            settings.host.local_players = local_players;
            save();
            conker::lockstep::set_quick(local_players + 1, true, -1, local_players, HostChoices{}.rollback, settings.host.spectators);
            conker::lockstep::set_characters(0);   // Quick Match: the game's own
            quick = true;
            begin();
        }

        // What More options are set to, said under its button while they're hidden.
        void show_advanced() {
            advanced_box->set_display(advanced_open ? recompui::Display::Flex : recompui::Display::None);
            advanced_summary->set_display(advanced_open ? recompui::Display::None : recompui::Display::Block);
            advanced_button->set_text(advanced_open ? "Fewer options" : "More options");
            std::string about = input_delay < 0 ? "Input delay auto" : "Input delay " + std::to_string(input_delay);
            about += rollback > 0 ? ", rollback " + std::to_string(rollback) : ", no rollback";
            about += own_view_for_everyone ? ", everyone sees only their own view" : ", players choose their screen";
            about += characters > 0 ? ", up to " + std::to_string(characters) + " characters" : "";
            about += spectators ? ", spectators welcome" : ", no spectators";
            about += !host_directly ? ", through the server" : lan_visible ? ", without a server, visible on this network" :
                ", without a server";
            advanced_summary->set_text(about + ".");
            room_row->set_display(host_directly ? recompui::Display::None : recompui::Display::Flex);
            direct_rows->set_display(host_directly ? recompui::Display::Flex : recompui::Display::None);
        }

        void on_cancel(const std::string& status = "Cancelled.") {
            conker::lockstep::cancel();
            conker::lockstep::set_host(-1, players);
            set_state(State::idle, status);
        }

    protected:
        std::string_view get_type_name() override { return "NetplayPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) {
                return;
            }
            if (testing) {
                conker::netplay::ConnectionReport report;
                if (conker::lockstep::poll_connection_test(report)) {
                    conker::lockstep::stop_connection_test();
                    testing = false;
                    test_button->set_enabled(true);
                    show_test(report);
                    if (state == State::idle && join_tab && !closing) {
                        start_browsing();
                    }
                }
                else {
                    queue_update();
                }
            }
            if (browsing && state == State::idle) {
                show_rooms(conker::lockstep::poll_browse());
            }
            if (lan_listening && state == State::idle) {
                show_lan(conker::lockstep::poll_lan());
            }
            if ((browsing || lan_listening) && state == State::idle) {
                queue_update();
            }
            show_waiting();
            if (state == State::connecting) {
                std::string status;
                switch (conker::lockstep::poll_connect(status)) {
                    case conker::lockstep::Progress::waiting:
                        status_label->set_text(status);
                        if ((hosting || quick) && shown_code.empty()) {
                            show_code();    // the room code, once the server has made it
                        }
                        break;
                    case conker::lockstep::Progress::failed:
                        conker::lockstep::set_host(-1, players);
                        set_state(State::idle, status);
                        break;
                    case conker::lockstep::Progress::started:
                        // Joined (not hosted) to play: Rejoin last match, should the game quit or lose the host.
                        if (!watching) {
                            remember_match(conker::lockstep::joined_code(), local_players);
                        }
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
            set_width(100.0f, recompui::Unit::Percent);
            set_height(100.0f, recompui::Unit::Percent);
            set_background_color(recompui::theme::color::Background1);
            set_as_navigation_container(recompui::NavigationType::Vertical);

            // The title and how it goes, above the scrolling part: seen whatever part of the page is scrolled to.
            recompui::Element* bar = column(this, 12.0f);
            bar->set_padding(32.0f);
            bar->set_padding_bottom(16.0f);
            text(bar, "Netplay", Typography::Header3);
            status_row = row(bar);
            status_label = text(status_row, "", Typography::LabelLG);
            cancel_button = context.create_element<recompui::Button>(status_row, "Cancel", recompui::ButtonStyle::Tertiary);
            cancel_button->add_pressed_callback([this]() { on_cancel(); });

            // The rest scrolls: the page is taller than a small window, and squeezed into it its rows and texts
            // would be drawn over each other.
            recompui::ScrollContainer* scroll = context.create_element<recompui::ScrollContainer>(this, recompui::ScrollDirection::Vertical);
            scroll->set_height_auto();
            scroll->set_flex(1.0f, 1.0f, 0.0f);
            scroll->set_min_height(0.0f);
            recompui::Element* content = column(scroll, 28.0f);
            content->set_padding(32.0f);
            content->set_padding_top(0.0f);

            // While connecting: the code to share (hosting), then the waiting room.
            lobby_box = column(content, 20.0f);
            code_box = column(lobby_box, 8.0f);
            code_title = text(code_box, "", Typography::LabelMD, color::TextDim);
            recompui::Element* code_row = row(code_box);
            code_label = text(code_row, "", Typography::Header2);
            copy_button = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Secondary);
            copy_button->add_pressed_callback([this]() {
                // Said on the button: the status line is busy while waiting for players.
                if (!shown_code.empty() && SDL_SetClipboardText(shown_code.c_str()) == 0) {
                    copied_code = shown_code;
                    copy_button->set_text("Copied");
                    queue_update();
                }
            });
            code_hint = text(code_box, "", Typography::Body, color::TextDim);
            waiting_box = column(lobby_box, 10.0f);
            waiting_box->set_display(recompui::Display::None);

            // The Host tab and Players on this computer as they were last.
            const HostChoices& choices = settings.host;
            players = choices.players;
            public_room = choices.public_room;
            input_delay = choices.input_delay;
            rollback = choices.rollback;
            own_view_for_everyone = choices.own_view;
            spectators = choices.spectators;
            characters = choices.characters;
            host_directly = choices.directly;
            port_text = std::to_string(choices.port);
            address_text = choices.address;
            lan_visible = choices.lan_visible;
            local_players = choices.local_players;

            // While idle: what the match is to be, and Host or Join.
            setup_box = column(content, 28.0f);
            rejoin_row = row(setup_box);
            recompui::Button* rejoin_button = context.create_element<recompui::Button>(rejoin_row, "Rejoin last match",
                recompui::ButtonStyle::Primary);
            rejoin_button->set_white_space(recompui::WhiteSpace::Nowrap);
            rejoin_button->add_pressed_callback([this]() { on_rejoin(); });
            rejoin_label = text(rejoin_row, "", Typography::Body, color::TextDim);
            recompui::Button* forget_button = context.create_element<recompui::Button>(rejoin_row, "Forget", recompui::ButtonStyle::Tertiary);
            forget_button->add_pressed_callback([this]() {
                remember_match("", 1);
                show_rejoin();
                queue_update();
            });
            rejoin_row->set_display(recompui::Display::None);
            text(setup_box, "Play multiplayer online. Everyone needs this same build and the same ROM, with no mods on "
                "(texture packs are fine). The game starts from the beginning.", Typography::Body, color::TextDim);

            // Who plays on this computer, whichever way the match is found.
            recompui::Element* here = column(setup_box, 8.0f);
            recompui::Element* here_row = row(here);
            text(here_row, "Players on this computer", Typography::LabelMD);
            here_radio = context.create_element<recompui::Radio>(here_row);
            for (int n = 1; n < conker::netplay::max_players; n++) {
                here_radio->add_option(std::to_string(n));
            }
            here_radio->set_index((uint32_t)(local_players - 1));
            here_radio->add_index_changed_callback([this](uint32_t index) { local_players = 1 + (int)index; });
            text(here, "One controller each; the first player also has the keyboard, mouse and gyro.", Typography::Body,
                color::TextDim);

            recompui::TabSet* tabs = context.create_element<recompui::TabSet>(setup_box);
            tabs->set_flex_shrink(0.0f);
            tabs->add_tab("Play");
            tabs->add_tab("Host");
            tabs->add_tab("Settings");

            // Playing: Quick Match, a code, or a match from the lists.
            recompui::Element* join = column(setup_box, 16.0f);
            join_section = join;
            recompui::Element* quick_row = row(join);
            recompui::Button* quick_button = context.create_element<recompui::Button>(quick_row, "Quick Match", recompui::ButtonStyle::Primary);
            quick_button->set_white_space(recompui::WhiteSpace::Nowrap);
            // Where a controller starts.
            quick_button->set_as_primary_focus();
            quick_button->add_pressed_callback([this]() {
                if (state == State::idle) {
                    on_quick();
                }
            });
            text(quick_row, "Joins an open public room, or opens one and waits for a player.", Typography::Body, color::TextDim);
            recompui::Element* join_row = row(join);
            text(join_row, "Have a code?", Typography::LabelMD);
            join_input = input(join_row, "", 360.0f);
            join_input->add_text_changed_callback([this](const std::string& value) {
                join_text = value;
                if (value != clipboard_code) {
                    clipboard_hint->set_display(recompui::Display::None);
                }
            });
            recompui::Button* paste_button = context.create_element<recompui::Button>(join_row, "Paste", recompui::ButtonStyle::Tertiary);
            paste_button->add_pressed_callback([this]() {
                char* pasted = SDL_GetClipboardText();
                if (pasted != nullptr) {
                    const std::string code = one_line(pasted);
                    SDL_free(pasted);
                    set_join_text(code);
                }
            });
            join_button = context.create_element<recompui::Button>(join_row, "Join", recompui::ButtonStyle::Secondary);
            join_button->add_pressed_callback([this]() { on_join(); });
            watch_button = context.create_element<recompui::Button>(join_row, "Watch", recompui::ButtonStyle::Tertiary);
            watch_button->add_pressed_callback([this]() { on_join(true); });
            clipboard_hint = text(join, "The code on your clipboard: press Join to join it.", Typography::Body, color::TextDim);
            clipboard_hint->set_display(recompui::Display::None);
            recompui::Element* public_row = row(join);
            text(public_row, "Open matches", Typography::LabelMD);
            recompui::Button* refresh_button = context.create_element<recompui::Button>(public_row, "Refresh", recompui::ButtonStyle::Tertiary);
            refresh_button->add_pressed_callback([this]() {
                if (state == State::idle && browsing) {
                    conker::lockstep::browse_again();
                }
                else if (state == State::idle) {
                    start_browsing();
                }
            });
            rooms_label = text(join, "", Typography::Body, color::TextDim);
            rooms_box = column(join, 8.0f);
            text(join, "On this network", Typography::LabelMD);
            lan_label = text(join, "", Typography::Body, color::TextDim);
            lan_box = column(join, 8.0f);

            // Hosting: how many play and who can join; the rest under More options.
            recompui::Element* host = column(setup_box, 12.0f);
            host_section = host;
            recompui::Element* host_row = row(host);
            text(host_row, "Players in the match", Typography::LabelMD);
            recompui::Radio* count_radio = context.create_element<recompui::Radio>(host_row);
            for (int n = 2; n <= conker::netplay::max_players; n++) {
                count_radio->add_option(std::to_string(n));
            }
            count_radio->set_index((uint32_t)(players - 2));
            count_radio->add_index_changed_callback([this](uint32_t index) { players = 2 + (int)index; });
            room_row = row(host);
            text(room_row, "Who can join", Typography::LabelMD);
            recompui::Radio* listed_radio = context.create_element<recompui::Radio>(room_row);
            listed_radio->add_option("Private: people with the code");
            listed_radio->add_option("Public: in the room list");
            listed_radio->set_index(public_room ? 1 : 0);
            listed_radio->add_index_changed_callback([this](uint32_t index) { public_room = index == 1; });
            advanced_button = context.create_element<recompui::Button>(host, "More options", recompui::ButtonStyle::Tertiary);
            advanced_button->set_white_space(recompui::WhiteSpace::Nowrap);
            advanced_button->add_pressed_callback([this]() {
                advanced_open = !advanced_open;
                show_advanced();
                queue_update();
            });
            advanced_summary = text(host, "", Typography::Body, color::TextDim);
            advanced_box = column(host, 12.0f);
            advanced_box->set_padding_left(24.0f);
            recompui::Element* delay_row = row(advanced_box);
            text(delay_row, "Input delay", Typography::LabelMD);
            recompui::Radio* delay_radio = context.create_element<recompui::Radio>(delay_row);
            delay_radio->add_option("Auto");
            for (int reads = 0; reads <= 4; reads++) {
                delay_radio->add_option(std::to_string(reads));
            }
            delay_radio->set_index((uint32_t)(input_delay + 1));
            delay_radio->add_index_changed_callback([this](uint32_t index) {
                input_delay = (int)index - 1;
                show_advanced();
            });
            text(advanced_box, "Auto suits most matches. Raise it if the game stutters waiting for a player.",
                Typography::Body, color::TextDim);
            recompui::Element* rollback_row = row(advanced_box);
            text(rollback_row, "Rollback", Typography::LabelMD);
            recompui::Radio* rollback_radio = context.create_element<recompui::Radio>(rollback_row);
            // Off, 4, 8 or 12 reads.
            rollback_radio->add_option("Off");
            for (int reads = 4; reads <= 12; reads += 4) {
                rollback_radio->add_option(std::to_string(reads));
            }
            rollback_radio->set_index((uint32_t)(rollback / 4));
            rollback_radio->add_index_changed_callback([this](uint32_t index) {
                rollback = 4 * (int)index;
                show_advanced();
            });
            text(advanced_box, "Guesses the others' input up to that many reads ahead (about 30 a second) and corrects a wrong "
                "guess, so lag is felt less. 12 suits most matches; Off waits for every input instead.", Typography::Body, color::TextDim);
            recompui::Element* screens_row = row(advanced_box);
            text(screens_row, "Screens", Typography::LabelMD);
            recompui::Radio* screens_radio = context.create_element<recompui::Radio>(screens_row);
            screens_radio->add_option("Everyone sees only their own view");
            screens_radio->add_option("Players choose");
            screens_radio->set_index(own_view_for_everyone ? 0 : 1);
            screens_radio->add_index_changed_callback([this](uint32_t index) {
                own_view_for_everyone = index == 0;
                show_advanced();
            });
            recompui::Element* characters_row = row(advanced_box);
            text(characters_row, "Characters", Typography::LabelMD);
            recompui::Radio* characters_radio = context.create_element<recompui::Radio>(characters_row);
            characters_radio->add_option("Original");
            characters_radio->add_option("Up to 12");
            characters_radio->add_option("Up to 16");
            characters_radio->set_index(characters == 16 ? 2 : characters == 12 ? 1 : 0);
            characters_radio->add_index_changed_callback([this](uint32_t index) {
                characters = index == 2 ? 16 : index == 1 ? 12 : 0;
                show_advanced();
            });
            text(advanced_box, "The most characters, players and bots, in the match's multiplayer games, as the General tab's "
                "Multiplayer Characters (Tank stops at 12, Race is the game's own). Original: the game's own, which allows no "
                "bots with 4 players.", Typography::Body, color::TextDim);
            recompui::Element* spectators_row = row(advanced_box);
            text(spectators_row, "Spectators", Typography::LabelMD);
            recompui::Radio* spectators_radio = context.create_element<recompui::Radio>(spectators_row);
            spectators_radio->add_option("Welcome");
            spectators_radio->add_option("None");
            spectators_radio->set_index(spectators ? 0 : 1);
            spectators_radio->add_index_changed_callback([this](uint32_t index) {
                spectators = index == 0;
                show_advanced();
            });
            text(advanced_box, "Up to 8 people can watch the match with Watch, from its start or as it's played. They see "
                "every player's view, and nobody waits for them.", Typography::Body, color::TextDim);
            recompui::Element* through_row = row(advanced_box);
            text(through_row, "Host", Typography::LabelMD);
            recompui::Radio* through_radio = context.create_element<recompui::Radio>(through_row);
            through_radio->add_option("Through the server");
            through_radio->add_option("Without a server");
            through_radio->set_index(host_directly ? 1 : 0);
            through_radio->add_index_changed_callback([this](uint32_t index) {
                host_directly = index == 1;
                show_advanced();
                show_code();
                queue_update();
            });
            direct_rows = column(advanced_box, 12.0f);
            recompui::Element* direct_row = row(direct_rows);
            text(direct_row, "UDP port", Typography::LabelMD);
            port_input = input(direct_row, port_text, 120.0f);
            port_input->add_text_changed_callback([this](const std::string& value) { port_text = value; });
            text(direct_row, "Your address", Typography::LabelMD);
            address_input = input(direct_row, address_text, 320.0f);
            address_input->add_text_changed_callback([this](const std::string& value) {
                address_text = value;
                show_code();
            });
            recompui::Element* lan_row = row(direct_rows);
            text(lan_row, "On this network", Typography::LabelMD);
            recompui::Radio* lan_radio = context.create_element<recompui::Radio>(lan_row);
            lan_radio->add_option("Hidden");
            lan_radio->add_option("Visible");
            lan_radio->set_index(lan_visible ? 1 : 0);
            lan_radio->add_index_changed_callback([this](uint32_t index) {
                lan_visible = index == 1;
                show_advanced();
                show_code();
                queue_update();
            });
            text(direct_rows, "Visible: anyone on this network (at home, at a LAN party) sees the match on their Play tab and "
                "can join it with no code. Leave the address blank for that alone.", Typography::Body, color::TextDim);
            text(direct_rows, "For players outside your network, forward the UDP port on your router and give your public "
                "address (any \"what is my IP\" site). The join code contains it: share it only with people you trust.",
                Typography::Body, color::TextDim);
            host_button = context.create_element<recompui::Button>(host, "Host", recompui::ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { on_host(); });

            // Settings: the netplay server, how players connect through it, and this window's screen.
            recompui::Element* setup = column(setup_box, 16.0f);
            settings_section = setup;
            recompui::Element* server = column(setup, 8.0f);
            recompui::Element* server_row = row(server);
            text(server_row, "Server", Typography::LabelMD);
            recompui::TextInput* server_input = input(server_row, settings.server, 320.0f);
            server_input->add_text_changed_callback([this](const std::string& value) {
                settings.server = value;
                save();
            });
            test_button = context.create_element<recompui::Button>(server_row, "Test connection", recompui::ButtonStyle::Tertiary);
            test_button->set_white_space(recompui::WhiteSpace::Nowrap);
            test_button->add_pressed_callback([this]() { on_test(); });
            text(server, "The netplay server (address and port) makes the rooms and relays the matches.", Typography::Body,
                color::TextDim);
            test_box = column(server, 4.0f);
            recompui::Element* direct = column(setup, 8.0f);
            recompui::Element* direct_choice = row(direct);
            text(direct_choice, "Connections", Typography::LabelMD);
            recompui::Radio* direct_radio = context.create_element<recompui::Radio>(direct_choice);
            direct_radio->add_option("Relay only");
            direct_radio->add_option("Peer-to-peer when possible");
            direct_radio->set_index(settings.allow_direct ? 1 : 0);
            direct_radio->add_index_changed_callback([this](uint32_t index) {
                settings.allow_direct = index == 1;
                save();
            });
            text(direct, "Relay only: nobody sees your address. Peer-to-peer can be quicker, but the others see your address.",
                Typography::Body, color::TextDim);
            // What this window shows of the split screen.
            recompui::Element* display = column(setup, 8.0f);
            recompui::Element* display_row = row(display);
            text(display_row, "Your screen", Typography::LabelMD);
            recompui::Radio* display_radio = context.create_element<recompui::Radio>(display_row);
            display_radio->add_option("Split screen");
            display_radio->add_option("My view only");
            display_radio->set_index(conker::netplay_view::is_mine_only() ? 1 : 0);
            display_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_view::set_mine_only(index == 1); });
            text(display, "My view only fills the window with your view, unless the host's Screens choose it for everyone.",
                Typography::Body, color::TextDim);

            tabs->set_change_tab_callback([this](int index) {
                join_section->set_display(index == 0 ? recompui::Display::Flex : recompui::Display::None);
                host_section->set_display(index == 1 ? recompui::Display::Flex : recompui::Display::None);
                settings_section->set_display(index == 2 ? recompui::Display::Flex : recompui::Display::None);
                join_tab = index == 0;
                if (join_tab && state == State::idle) {
                    start_browsing();
                    offer_clipboard();
                }
                else if (!join_tab) {
                    stop_browsing();
                }
                queue_update();
            });
            tabs->set_active_tab(0, false);
            host_section->set_display(recompui::Display::None);
            settings_section->set_display(recompui::Display::None);
            show_advanced();

            set_state(State::idle, "");
            show_code();
            // The Play tab is open, its lists asked for once the menu is shown (on_open).
            join_tab = true;
            queue_update();
        }

        bool connecting() const { return state == State::connecting; }
        // The menu closes: whatever it was connecting is given up.
        void on_close() {
            closing = true;
            stop_browsing();
            stop_test();
            if (state == State::connecting) {
                conker::lockstep::cancel();
                conker::lockstep::set_host(-1, players);
                set_state(State::idle, "Cancelled.");
            }
            closing = false;
        }
        // The menu opens again: the room list, if that's the tab open.
        void on_open() {
            browse_hello.reset();
            show_rejoin();
            if (state == State::idle && join_tab && !testing) {
                start_browsing();
                offer_clipboard();
            }
        }
    };

    NetplayPage* page = nullptr;

    // The Esc menu's Netplay tab: during a match, who's in it, with Kick (the host's) and Block, and Leave
    // match. The game doesn't pause for it (no machine can pause a match alone): this player stands idle while
    // it's open.
    class MatchPage : public recompui::ConfigPage {
        std::string shown;      // what it shows, to make it again only when that changes

        recompui::Label* add_text(recompui::Element* parent, const std::string& words, recompui::theme::Typography typography,
            recompui::theme::color color = recompui::theme::color::Text) {
            recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
            label->set_color(color);
            return label;
        }

        // One person in the match (the right column is narrow): a line saying who, kept on one line, and under it
        // their buttons, which go on to another line if they must.
        struct Entry {
            recompui::Element* who;
            recompui::Element* buttons;
        };

        Entry add_entry(recompui::Element* parent, const std::string& title) {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* entry = context.create_element<recompui::Element>(parent);
            entry->set_display(recompui::Display::Flex);
            entry->set_flex_direction(recompui::FlexDirection::Column);
            entry->set_gap(6.0f);
            recompui::Element* who = context.create_element<recompui::Element>(entry);
            who->set_display(recompui::Display::Flex);
            who->set_flex_direction(recompui::FlexDirection::Row);
            who->set_align_items(recompui::AlignItems::Center);
            who->set_gap(12.0f);
            recompui::Element* buttons = context.create_element<recompui::Element>(entry);
            buttons->set_display(recompui::Display::Flex);
            buttons->set_flex_direction(recompui::FlexDirection::Row);
            buttons->set_flex_wrap(recompui::FlexWrap::Wrap);
            buttons->set_gap(8.0f);
            add_words(who, title, recompui::theme::Typography::LabelMD);
            return { who, buttons };
        }

        // Words on an entry's line: never wrapped or squeezed (a long line runs past the column rather than
        // over its neighbours).
        recompui::Label* add_words(recompui::Element* line, const std::string& words, recompui::theme::Typography typography,
            recompui::theme::color color = recompui::theme::color::Text) {
            recompui::Label* label = add_text(line, words, typography, color);
            label->set_white_space(recompui::WhiteSpace::Nowrap);
            label->set_flex_shrink(0.0f);
            return label;
        }

        recompui::Button* add_button(const Entry& entry, const std::string& words, recompui::ButtonStyle style = recompui::ButtonStyle::Tertiary) {
            recompui::Button* button = recompui::get_current_context().create_element<recompui::Button>(entry.buttons, words, style);
            button->set_white_space(recompui::WhiteSpace::Nowrap);
            return button;
        }

        void build(const conker::lockstep::WaitingRoom& room) {
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* left = body->get_left();
            left->clear_children();
            left->set_display(recompui::Display::Flex);
            left->set_flex_direction(recompui::FlexDirection::Column);
            left->set_gap(16.0f);
            add_text(left, "Netplay", Typography::Header3);
            // Netplay Details (status.cpp's corner), in or out of a match.
            recompui::Element* details_row = context.create_element<recompui::Element>(left);
            details_row->set_display(recompui::Display::Flex);
            details_row->set_flex_direction(recompui::FlexDirection::Row);
            details_row->set_align_items(recompui::AlignItems::Center);
            details_row->set_gap(24.0f);
            add_text(details_row, "Netplay Details", Typography::LabelMD);
            recompui::Radio* details_radio = context.create_element<recompui::Radio>(details_row);
            details_radio->add_option("Off");
            details_radio->add_option("On");
            details_radio->set_index(conker::netplay_status::details_wanted() ? 1 : 0);
            details_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_status::set_details_wanted(index == 1); });
            add_text(left, conker::netplay_status::details_description, Typography::Body, color::TextDim);
            recompui::Element* right = body->get_right();
            right->clear_children();
            right->set_display(recompui::Display::Flex);
            right->set_flex_direction(recompui::FlexDirection::Column);
            right->set_gap(18.0f);
            if (!room.in) {
                add_text(left, "No netplay match is being played. Start one from the launcher's Netplay menu.", Typography::Body, color::TextDim);
                return;
            }
            add_text(left, std::to_string(room.players) + " players, input delay " + std::to_string(room.delay) +
                (room.rollback > 0 ? ", rollback " + std::to_string(room.rollback) : "") +
                (room.own_view ? ", everyone sees only their own view" : "") +
                (room.characters > 0 ? ", up to " + std::to_string(room.characters) + " characters" : ""), Typography::Body);
            add_text(left, room.watching ? "You're watching: nobody waits for you, and your controllers play nothing." :
                "The match goes on while this menu is open: your player stands still until you close it.", Typography::Body, color::TextDim);
            add_text(left, room.hosting ? "Kick removes a player: their place plays on, idle. Block also keeps them out of your "
                "matches from now on. A spectator can take a free place, or one someone left (they can't come back to it), or swap "
                "with a player, who watches from then on." : room.watching ? "Play as takes a free place: you play it from a moment later." :
                "Block keeps a player out of your matches from now on.", Typography::Body, color::TextDim);
            recompui::Button* leave = context.create_element<recompui::Button>(left, room.watching ? "Stop watching" : "Leave match",
                recompui::ButtonStyle::Danger);
            leave->add_pressed_callback([]() { conker::lockstep::leave_match(); });
            for (int port = 0; port < room.players; port++) {
                const conker::lockstep::Seat& seat = room.seats[port];
                const Entry entry = add_entry(right, "Player " + std::to_string(port + 1));
                if (seat.player == 0) {
                    add_words(entry.who, seat.open ? "open" : seat.left ? "left" : "not known", Typography::Body, color::TextDim);
                    if (room.watching && seat.open) {
                        recompui::Button* play = add_button(entry, "Play as Player " + std::to_string(port + 1), recompui::ButtonStyle::Secondary);
                        play->add_pressed_callback([this, port]() {
                            conker::lockstep::claim(port);
                            shown.clear();
                        });
                    }
                    continue;
                }
                add_words(entry.who, seat.name + (seat.mine ? " (you)" : ""), Typography::Body, seat.left ? color::TextDim : color::Text);
                std::string about = seat.left ? "left" : platform_name(seat.platform);
                about += !seat.left && seat.round_trip_ms >= 0 ? ", " + std::to_string(seat.round_trip_ms) + " ms" : "";
                add_words(entry.who, about, Typography::Body, color::TextDim);
                if (seat.mine) {
                    // (A joiner's players can stop playing and watch.)
                    if (seat.movable && !room.hosting && room.takes_spectators && room.spectators.size() < conker::netplay::max_spectators) {
                        recompui::Button* watch = add_button(entry, "Watch instead");
                        watch->add_pressed_callback([this]() {
                            conker::lockstep::step_out();
                            shown.clear();
                        });
                    }
                    continue;
                }
                const uint64_t player = seat.player;
                if (room.hosting && !seat.left && seat.movable && room.takes_spectators) {
                    recompui::Button* unseat = add_button(entry, "Make spectator");
                    unseat->add_pressed_callback([this, port, player]() {
                        conker::lockstep::make_spectator(port, player);
                        shown.clear();
                    });
                }
                if (room.hosting && !seat.left) {
                    recompui::Button* kick = add_button(entry, "Kick");
                    kick->add_pressed_callback([this, port, player]() {
                        conker::lockstep::kick(port, player);
                        shown.clear();
                    });
                }
                recompui::Button* block = add_button(entry, "Block");
                block->add_pressed_callback([this, player]() {
                    block_player(player);
                    shown.clear();
                });
            }
            for (const conker::lockstep::Spectator& spectator : room.spectators) {
                const Entry entry = add_entry(right, "Watching");
                add_words(entry.who, spectator.name + (spectator.mine ? " (you)" : ""), Typography::Body);
                add_words(entry.who, platform_name(spectator.platform), Typography::Body, color::TextDim);
                if (spectator.mine) {
                    continue;
                }
                const uint64_t player = spectator.player;
                if (room.hosting) {
                    recompui::Button* kick = add_button(entry, "Kick");
                    kick->add_pressed_callback([this, number = spectator.number, player]() {
                        conker::lockstep::kick_spectator(number, player);
                        shown.clear();
                    });
                    // Into a place: a free one, one someone left, or a joiner's (a swap).
                    for (int port = 1; port < room.players; port++) {
                        const conker::lockstep::Seat& seat = room.seats[port];
                        if (seat.player != 0 && (!seat.movable || seat.left)) {
                            continue;
                        }
                        const std::string label = (seat.player != 0 && !seat.left ? "Swap with " : "Seat as ") + std::string("Player ") +
                            std::to_string(port + 1);
                        recompui::Button* seat_button = add_button(entry, label);
                        seat_button->add_pressed_callback([this, port, number = spectator.number, player]() {
                            conker::lockstep::seat(port, number, player);
                            shown.clear();
                        });
                    }
                }
                recompui::Button* block = add_button(entry, "Block");
                block->add_pressed_callback([this, player]() {
                    block_player(player);
                    shown.clear();
                });
            }
        }

    protected:
        std::string_view get_type_name() override { return "NetplayMatchPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) {
                return;
            }
            const conker::lockstep::WaitingRoom room = conker::lockstep::match_room();
            std::string summary = room.in ? "in" : "";
            summary += room.watching ? "w" : "";
            for (const conker::lockstep::Seat& seat : room.seats) {
                summary += "|" + std::to_string(seat.player) + (seat.left ? "L" : "") + (seat.open ? "O" : "") + std::to_string(seat.round_trip_ms / 10);
            }
            for (const conker::lockstep::Spectator& spectator : room.spectators) {
                summary += "|w" + std::to_string(spectator.number) + ":" + std::to_string(spectator.player);
            }
            if (summary != shown) {
                shown = summary;
                build(room);
            }
            queue_update();
        }

    public:
        MatchPage(recompui::ResourceId rid, recompui::Element* parent)
            : ConfigPage(rid, parent, recompui::Events(recompui::EventType::Update)) {
            set_as_navigation_container(recompui::NavigationType::Horizontal);
            queue_update();
        }
    };
}

void conker::netplay_menu::add_tab() {
    recompui::config::create_tab("Netplay", "netplay", [](recompui::ContextId context, recompui::Element* parent) {
        context.create_element<MatchPage>(parent);
    });
}

void conker::netplay_menu::init(const std::u8string& id, const std::filesystem::path& rom) {
    game_id = id;
    stored_rom = rom;
}

void conker::netplay_menu::open() {
    // The launcher's option calls this with the launcher's context open, and only one can be: it's closed
    // while the menu's is used.
    recompui::ContextId launcher = recompui::get_current_context();
    const bool launcher_open = launcher.slot_id != recompui::ContextId::null().slot_id;
    if (launcher_open) {
        launcher.close();
    }
    if (modal == nullptr) {
        modal = recompui::Modal::create_modal(recompui::ModalType::Fullscreen);
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        // The page scrolls within itself, below its title and status line.
        modal->get_body()->set_overflow_y(recompui::Overflow::Hidden);
        page = context.create_element<NetplayPage>(modal->get_body());
        context.close();
        // Closing it while connecting gives up.
        modal->set_on_close_callback([]() {
            if (page != nullptr) {
                page->on_close();
            }
        });
    }
    modal->open();
    // The room list, if the Play tab is open: only once the modal is shown (started before, while the page is
    // made or opened again, it kept the modal from showing).
    recompui::ContextId context = modal->modal_root_context;
    context.open();
    page->on_open();
    context.close();
    if (launcher_open) {
        launcher.open();
    }
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
    // A joiner that left, or lost the host, may come back while the others play on: Rejoin last match
    // is offered next time. Otherwise there's nothing to come back to.
    const bool can_come_back = conker::lockstep::can_come_back();
    if (!can_come_back) {
        remember_match("", 1);
    }
    const std::string back = can_come_back
        ? " If the others are still playing, you can come back: start the game again, and press Rejoin last match on "
          "the Netplay menu."
        : "";
    recompui::open_info_prompt("The match is over",
        why + ". The game goes on without anyone's input from here: quit when you like." + back, "Quit",
        []() { ultramodern::quit(); });
}
