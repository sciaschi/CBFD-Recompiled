// RetroAchievements (a prototype): the game's achievements from retroachievements.org, earned as you play.
//
// rcheevos (tools/rcheevos, MIT) does the work: rc_client logs in, identifies the game by its ROM's hash,
// fetches its achievements and, once a game frame, checks their conditions against the game's memory. It
// asks us for three things:
// - Memory: RetroAchievements' N64 addresses are RDRAM's (0 to 0x7FFFFF, the expansion pak's included, as
//   0x80000000 on), laid out as the N64 emulators its sets are made with expose it: 32-bit words in the host's
//   byte order (mupen64plus's RDRAM, through libretro), not the N64's big-endian bytes. The recompiled game
//   keeps RDRAM so too, so it's read as it is. (Read as the N64's bytes, with ^ 3 as MEM_B does, nothing
//   triggered: a set's 24-bit little-endian read of a pointer, I:0xW0d326c, takes the low 24 bits of the
//   word, the address, only in the emulators' layout.)
// - HTTP: each request rc_client makes (a URL, with POST data or not) is sent on a thread of its own, and
//   the reply handed back. On Windows with WinHTTP (the system's, nothing to ship); elsewhere not yet: those
//   builds report the request failed, so they run as without achievements.
// - The UI: an unlock shows as a line in the top-left corner for a few seconds (a recompui context of its
//   own that takes no input, as the FPS counter's), and everything rc_client says goes to the console.
//   The line waits for the game to start: recompui shows the launcher only while no context is shown, so
//   shown at the launcher ("logged in as ...", a second after it opens) it kept the screen black until it
//   went.
//
// The RetroAchievements settings tab (add_tab, made with the other tabs in conker_config.cpp) logs in and
// out and lists the game's achievements, unlocked and not, once the game has started and been recognized
// (the ROM is hashed as the game starts: mods may patch it then, and it isn't the final ROM before). It's
// a tab of its own rather than a config: the login isn't an option to keep in a json, and its parts change
// as the login and the unlocks come back from the server, so it rebuilds itself when they do (a version
// number each, bumped from whichever thread a reply came on, read on the UI's own update).
//
// The login is kept in retroachievements.txt in the config folder (next to the other settings):
//   username=<name>
//   token=<token>
// The password typed in the tab is only sent: once logged in, the token the server gives is kept instead.
// (A password=<password> line, written by hand, logs in the same way and is replaced by the token too.)
// Logging out deletes the file. Without it RetroAchievements does nothing until you log in.
// Casual (softcore) only: hardcore mode needs RetroAchievements' approval of the program, and its rules (no
// cheats: the mods would have to be off) aren't in place yet.
//
// Threads: rc_client locks itself. The game thread runs the frames (conker_frame_dl_begin, widescreen.cpp)
// and starts loading the game once logged in; the main thread logs in and draws; replies come on their own
// threads.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include "rc_client.h"
#include "rc_consoles.h"

#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_text_input.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;

    constexpr const char* program_name = "ConkerRecompiled";
    constexpr const char* program_version = "0.1.5";
    // RDRAM with the expansion pak: RetroAchievements' N64 addresses.
    constexpr uint32_t rdram_size = 0x800000;

    rc_client_t* client = nullptr;
    // The game's RDRAM, from the game thread's first frame.
    std::atomic<uint8_t*> game_rdram = nullptr;
    std::atomic<bool> logged_in = false;
    std::atomic<bool> game_load_started = false;
    std::string user_agent;

    // What the settings tab shows, set from the replies' threads. The versions say what to rebuild: the
    // account (login state) and the achievement list (the game loading, an unlock).
    enum class LoginState { Off, LoggingIn, LoggedIn, Failed };
    enum class GameState { None, Loading, Loaded, Unknown };
    std::mutex state_mutex;
    LoginState login_state = LoginState::Off;
    GameState game_state = GameState::None;
    std::string login_error;
    std::string game_error;
    std::atomic<uint32_t> account_version = 1;
    std::atomic<uint32_t> list_version = 1;

    void set_login_state(LoginState state, const std::string& error = {}) {
        {
            std::lock_guard lock{state_mutex};
            login_state = state;
            login_error = error;
        }
        account_version++;
        list_version++;
    }

    void set_game_state(GameState state, const std::string& error = {}) {
        {
            std::lock_guard lock{state_mutex};
            game_state = state;
            game_error = error;
        }
        list_version++;
    }

    std::filesystem::path login_path() {
        return recomp::get_config_path() / "retroachievements.txt";
    }

    std::map<std::string, std::string> read_login() {
        std::map<std::string, std::string> values;
        std::ifstream file(login_path());
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t equals = line.find('=');
            if (equals != std::string::npos) {
                values[line.substr(0, equals)] = line.substr(equals + 1);
            }
        }
        return values;
    }

    // TEMP-DEBUG (the prototype): retroachievements_log.txt beside the login, with everything rcheevos says, the
    // events, each achievement's state once the game loads, and a check of the memory reads.
    std::mutex log_mutex;
    void log_line(const char* format, ...) {
        static FILE* file = std::fopen((recomp::get_config_path() / "retroachievements_log.txt").string().c_str(), "w");
        if (file == nullptr) {
            return;
        }
        std::lock_guard lock{log_mutex};
        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);
        std::fputc('\n', file);
        std::fflush(file);
    }

    // The unlocks to show, from the game thread, for the main thread.
    std::mutex messages_mutex;
    std::deque<std::string> messages;
    void show_message(const std::string& text) {
        std::printf("[achievements] %s\n", text.c_str());
        std::fflush(stdout);
        log_line("[achievements] %s", text.c_str());
        std::lock_guard lock{messages_mutex};
        messages.push_back(text);
    }

    // --- Memory ---

    uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*) {
        uint8_t* rdram = game_rdram.load();
        if (rdram == nullptr || address >= rdram_size) {
            return 0;
        }
        const uint32_t count = std::min<uint32_t>(num_bytes, rdram_size - address);
        for (uint32_t i = 0; i < count; i++) {
            buffer[i] = rdram[address + i];
        }
        return count;
    }

    // --- HTTP ---

#if defined(_WIN32)
    std::wstring widen(const std::string& text) {
        if (text.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wide.data(), length);
        return wide;
    }

    // One request, as rc_client gives it: GET, or POST with its data. Returns the HTTP status, or
    // RC_API_SERVER_RESPONSE_CLIENT_ERROR if it couldn't be sent.
    int send_request(const std::string& url, const std::string& post_data, const std::string& content_type, std::string& body) {
        const std::wstring wide_url = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        wchar_t extra[2048] = {};
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
            return RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        }
        int status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        HINTERNET session = WinHttpOpen(widen(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
        const bool secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
        const std::wstring object = std::wstring(path) + extra;
        const bool post = !post_data.empty();
        HINTERNET request = connection ? WinHttpOpenRequest(connection, post ? L"POST" : L"GET", object.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        if (request != nullptr) {
            const std::wstring headers = post ? (L"Content-Type: " + widen(content_type.empty() ? "application/x-www-form-urlencoded" : content_type)) : L"";
            const BOOL sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1,
                post ? (LPVOID)post_data.data() : WINHTTP_NO_REQUEST_DATA, (DWORD)post_data.size(), (DWORD)post_data.size(), 0);
            if (sent && WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0;
                DWORD size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                status = (int)code;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    std::vector<char> chunk(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                        break;
                    }
                    body.append(chunk.data(), read);
                }
            }
            WinHttpCloseHandle(request);
        }
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        return status;
    }
#else
    int send_request(const std::string&, const std::string&, const std::string&, std::string&) {
        return RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    }
#endif

    void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t*) {
        std::string url = request->url;
        std::string post_data = request->post_data ? request->post_data : "";
        std::string content_type = request->content_type ? request->content_type : "";
        std::thread([url, post_data, content_type, callback, callback_data] {
            std::string body;
            rc_api_server_response_t response{};
            response.http_status_code = send_request(url, post_data, content_type, body);
            // TEMP-DEBUG (the prototype): the game's achievement definitions (the "achievementsets" reply, "patch" in older versions: each achievement's
            // conditions, the memory it reads) in retroachievements_patch.json beside the log. Only the reply: what's
            // sent has the login token.
            auto is_request = [&](const char* name) {
                const std::string param = std::string("r=") + name;
                return url.find(param) != std::string::npos || post_data.find(param) != std::string::npos;
            };
            if ((is_request("patch") || is_request("achievementsets")) && !body.empty()) {
                std::ofstream patch(recomp::get_config_path() / "retroachievements_patch.json", std::ios::trunc | std::ios::binary);
                patch << body;
            }
            response.body = body.c_str();
            response.body_length = body.size();
            callback(&response, callback_data);
        }).detach();
    }

    // --- Events ---

    void RC_CCONV log_message(const char* message, const rc_client_t*) {
        std::printf("[rcheevos] %s\n", message);
        std::fflush(stdout);
        log_line("[rcheevos] %s", message);
    }

    void RC_CCONV handle_event(const rc_client_event_t* event, rc_client_t*) {
        log_line("[event] type %u%s%s", event->type, event->achievement ? " achievement: " : "", event->achievement ? event->achievement->title : "");
        switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            show_message(std::string("Achievement unlocked: ") + event->achievement->title + " (" +
                std::to_string(event->achievement->points) + ")");
            list_version++;
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            show_message("All achievements unlocked!");
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            if (event->server_error != nullptr && event->server_error->error_message != nullptr) {
                std::printf("[achievements] server error: %s\n", event->server_error->error_message);
            }
            break;
        default:
            break;
        }
    }

    void RC_CCONV game_loaded(int result, const char* error_message, rc_client_t* client, void*) {
        if (result != RC_OK) {
            show_message(std::string("RetroAchievements: this ROM isn't recognized (") + (error_message ? error_message : "unknown error") + ")");
            set_game_state(GameState::Unknown, error_message ? error_message : "unknown error");
            return;
        }
        set_game_state(GameState::Loaded);
        const rc_client_game_t* game = rc_client_get_game_info(client);
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);
        show_message(std::string("RetroAchievements: ") + (game ? game->title : "the game") + ", " +
            std::to_string(summary.num_unlocked_achievements) + " of " + std::to_string(summary.num_core_achievements) + " unlocked");
        // Every achievement and its state (0 inactive, 1 active, 2 unlocked, 3 disabled).
        rc_client_achievement_list_t* list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
            RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        if (list != nullptr) {
            for (uint32_t b = 0; b < list->num_buckets; b++) {
                const rc_client_achievement_bucket_t& bucket = list->buckets[b];
                log_line("[list] %s", bucket.label ? bucket.label : "");
                for (uint32_t a = 0; a < bucket.num_achievements; a++) {
                    const rc_client_achievement_t* achievement = bucket.achievements[a];
                    log_line("  %u state %u unlocked %u: %s (%s)", achievement->id, achievement->state, achievement->unlocked,
                        achievement->title, achievement->description);
                }
            }
            rc_client_destroy_achievement_list(list);
        }
    }

    void RC_CCONV logged_in_callback(int result, const char* error_message, rc_client_t* client, void*) {
        if (result != RC_OK) {
            show_message(std::string("RetroAchievements: login failed (") + (error_message ? error_message : "unknown error") + ")");
            set_login_state(LoginState::Failed, error_message ? error_message : "unknown error");
            return;
        }
        // Keep the token, not the password.
        const rc_client_user_t* user = rc_client_get_user_info(client);
        if (user != nullptr && user->token != nullptr) {
            std::ofstream file(login_path(), std::ios::trunc);
            file << "username=" << user->username << "\n" << "token=" << user->token << "\n";
        }
        show_message(std::string("RetroAchievements: logged in as ") + (user ? user->display_name : "?"));
        logged_in = true;
        set_login_state(LoginState::LoggedIn);
    }

    // The client, made once whether there's a login yet or not, so the settings tab can log in.
    void create_client() {
        if (client != nullptr) {
            return;
        }
        client = rc_client_create(read_memory, server_call);
        rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
        rc_client_set_event_handler(client, handle_event);
        rc_client_set_hardcore_enabled(client, 0);
        char clause[128] = {};
        rc_client_get_user_agent_clause(client, clause, sizeof(clause));
        user_agent = std::string(program_name) + "/" + program_version + " " + clause;
    }

    void log_in_with_password(const std::string& username, const std::string& password) {
        create_client();
        set_login_state(LoginState::LoggingIn);
        rc_client_begin_login_with_password(client, username.c_str(), password.c_str(), logged_in_callback, nullptr);
    }

    // Logging out unloads the game too (rc_client's own), so a login after loads it again on the next frame.
    void log_out() {
        if (client == nullptr) {
            return;
        }
        rc_client_logout(client);
        logged_in = false;
        game_load_started = false;
        std::error_code error;
        std::filesystem::remove(login_path(), error);
        set_game_state(GameState::None);
        set_login_state(LoginState::Off);
    }

    // --- The overlay ---

    std::atomic<bool> ui_ready = false;
    bool overlay_created = false;
    recompui::ContextId overlay = recompui::ContextId::null();
    recompui::Label* overlay_label = nullptr;
    std::string shown_text;
    clock::time_point shown_until;
    constexpr auto message_time = std::chrono::seconds(5);

    void create_overlay() {
        overlay = recompui::create_context();
        overlay.open();
        overlay.set_captures_input(false);
        overlay.set_captures_mouse(false);
        recompui::Element* box = overlay.create_element<recompui::Element>(overlay.get_root_element());
        box->set_position(recompui::Position::Absolute);
        box->set_top(8.0f);
        box->set_left(8.0f);
        box->set_padding_left(10.0f);
        box->set_padding_right(10.0f);
        box->set_padding_top(4.0f);
        box->set_padding_bottom(4.0f);
        box->set_border_radius(6.0f);
        box->set_background_color(recompui::theme::color::BGOverlay);
        overlay_label = overlay.create_element<recompui::Label>(box, "", recompui::LabelStyle::Normal);
        overlay.close();
        overlay_created = true;
    }

    // --- The settings tab ---

    // The page: the account on the left (logging in, or who's logged in and logging out), the game's
    // achievements on the right. Made each time the tab is opened (the modal makes a tab's contents
    // anew), it rebuilds a side when its version moves on.
    class AchievementsPage : public recompui::ConfigPage {
    private:
        uint32_t shown_account = 0;
        uint32_t shown_list = 0;
        recompui::TextInput* username_input = nullptr;
        recompui::TextInput* password_input = nullptr;
    protected:
        std::string_view get_type_name() override { return "AchievementsPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::Update) {
                refresh();
                queue_update();
            }
        }

        void refresh() {
            const uint32_t account = account_version.load();
            if (account != shown_account) {
                shown_account = account;
                build_account();
            }
            const uint32_t list = list_version.load();
            if (list != shown_list) {
                shown_list = list;
                build_list();
            }
        }

        void build_account() {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* side = body->get_left();
            side->clear_children();
            username_input = nullptr;
            password_input = nullptr;
            side->set_display(recompui::Display::Flex);
            side->set_flex_direction(recompui::FlexDirection::Column);
            side->set_gap(16.0f);
            side->set_as_navigation_container(recompui::NavigationType::Vertical);

            LoginState state;
            std::string error;
            {
                std::lock_guard lock{state_mutex};
                state = login_state;
                error = login_error;
            }

            context.create_element<recompui::Label>(side, "RetroAchievements", recompui::theme::Typography::Header3);
            auto add_text = [&](const std::string& text, recompui::theme::color color) {
                recompui::Label* label = context.create_element<recompui::Label>(side, text, recompui::theme::Typography::Body);
                label->set_line_height(28.0f);
                label->set_color(color);
                return label;
            };

            if (state == LoginState::LoggedIn) {
                const rc_client_user_t* user = client ? rc_client_get_user_info(client) : nullptr;
                add_text(std::string("Logged in as ") + (user ? user->display_name : "?") + " (" +
                    std::to_string(user ? user->score_softcore : 0) + " casual points).", recompui::theme::color::Text);
                add_text("Achievements are earned in casual mode. Hardcore mode isn't available yet.", recompui::theme::color::TextDim);
                recompui::Button* button = context.create_element<recompui::Button>(side, "Log Out", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Medium);
                button->add_pressed_callback([] { log_out(); });
                return;
            }
            if (state == LoginState::LoggingIn) {
                add_text("Logging in...", recompui::theme::color::Text);
                return;
            }

            add_text("Log in with your retroachievements.org account to earn the game's achievements as you play.",
                recompui::theme::color::Text);
#if !defined(_WIN32)
            add_text("Not available on this platform yet: logging in will fail.", recompui::theme::color::Warning);
#endif
            if (state == LoginState::Failed) {
                add_text("Login failed: " + error, recompui::theme::color::Danger);
            }
            context.create_element<recompui::Label>(side, "Username", recompui::theme::Typography::LabelMD);
            username_input = context.create_element<recompui::TextInput>(side);
            {
                // The name logged in with last, if any: retyping only the password after logging out.
                const auto login = read_login();
                const auto username = login.find("username");
                if (username != login.end()) {
                    username_input->set_text(username->second);
                }
            }
            context.create_element<recompui::Label>(side, "Password", recompui::theme::Typography::LabelMD);
            password_input = context.create_element<recompui::TextInput>(side, false);
            add_text("Only sent to RetroAchievements to log in. The token it gives back is kept instead.",
                recompui::theme::color::TextDim);
            recompui::Button* button = context.create_element<recompui::Button>(side, "Log In", recompui::ButtonStyle::Primary, recompui::ButtonSize::Medium);
            button->add_pressed_callback([this] {
                if (username_input == nullptr || password_input == nullptr) {
                    return;
                }
                const std::string username = username_input->get_text();
                const std::string password = password_input->get_text();
                if (username.empty() || password.empty()) {
                    return;
                }
                log_in_with_password(username, password);
            });
        }

        void build_list() {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* side = body->get_right();
            side->clear_children();
            side->set_display(recompui::Display::Flex);
            side->set_flex_direction(recompui::FlexDirection::Column);
            side->set_gap(8.0f);

            LoginState login;
            GameState game;
            std::string error;
            {
                std::lock_guard lock{state_mutex};
                login = login_state;
                game = game_state;
                error = game_error;
            }
            auto add_text = [&](recompui::Element* parent, const std::string& text, recompui::theme::Typography typography, recompui::theme::color color) {
                recompui::Label* label = context.create_element<recompui::Label>(parent, text, typography);
                if (typography == recompui::theme::Typography::Body) {
                    label->set_line_height(28.0f);
                }
                label->set_color(color);
                return label;
            };

            if (login != LoginState::LoggedIn) {
                add_text(side, "Log in to see the game's achievements.", recompui::theme::Typography::Body, recompui::theme::color::TextDim);
                return;
            }
            if (game == GameState::None || game == GameState::Loading) {
                add_text(side, game == GameState::None ? "Start the game to load its achievements." : "Loading the game's achievements...",
                    recompui::theme::Typography::Body, recompui::theme::color::TextDim);
                return;
            }
            if (game == GameState::Unknown) {
                add_text(side, "RetroAchievements doesn't recognize this ROM (" + error + "). Its achievements are for the US release.",
                    recompui::theme::Typography::Body, recompui::theme::color::Warning);
                return;
            }

            const rc_client_game_t* info = rc_client_get_game_info(client);
            rc_client_user_game_summary_t summary{};
            rc_client_get_user_game_summary(client, &summary);
            add_text(side, info ? info->title : "The game", recompui::theme::Typography::Header3, recompui::theme::color::Text);
            add_text(side, std::to_string(summary.num_unlocked_achievements) + " of " + std::to_string(summary.num_core_achievements) +
                " unlocked, " + std::to_string(summary.points_unlocked) + " of " + std::to_string(summary.points_core) + " points",
                recompui::theme::Typography::Body, recompui::theme::color::TextDim);

            // The list, scrolled: grouped as rcheevos groups them (recently unlocked, almost there, locked,
            // unlocked...). Each one is focusable, so a controller can scroll through them too.
            recompui::ScrollContainer* scroll = context.create_element<recompui::ScrollContainer>(side, recompui::ScrollDirection::Vertical);
            scroll->set_as_navigation_container(recompui::NavigationType::Vertical);
            recompui::Element* list = context.create_element<recompui::Element>(scroll);
            list->set_display(recompui::Display::Flex);
            list->set_flex_direction(recompui::FlexDirection::Column);
            list->set_gap(8.0f);
            list->set_padding_right(8.0f);
            rc_client_achievement_list_t* achievements = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
            if (achievements == nullptr) {
                return;
            }
            for (uint32_t b = 0; b < achievements->num_buckets; b++) {
                const rc_client_achievement_bucket_t& bucket = achievements->buckets[b];
                recompui::Label* heading = add_text(list, bucket.label ? bucket.label : "", recompui::theme::Typography::LabelMD,
                    recompui::theme::color::TextDim);
                heading->set_margin_top(b == 0 ? 0.0f : 12.0f);
                for (uint32_t a = 0; a < bucket.num_achievements; a++) {
                    const rc_client_achievement_t* achievement = bucket.achievements[a];
                    recompui::Element* row = context.create_element<recompui::Element>(list);
                    row->set_display(recompui::Display::Flex);
                    row->set_flex_direction(recompui::FlexDirection::Column);
                    row->set_gap(4.0f);
                    row->set_padding(12.0f);
                    row->set_border_radius(recompui::theme::border::radius_sm);
                    row->set_border_width(recompui::theme::border::width);
                    row->set_border_color(achievement->unlocked ? recompui::theme::color::SuccessA50 : recompui::theme::color::BorderSoft);
                    row->set_background_color(achievement->unlocked ? recompui::theme::color::SuccessA5 : recompui::theme::color::BGShadow);
                    row->set_focusable(true);
                    row->set_tab_index_auto();
                    std::string title = std::string(achievement->title) + " (" + std::to_string(achievement->points) + ")";
                    if (achievement->measured_progress[0] != '\0' && !achievement->unlocked) {
                        title += "  " + std::string(achievement->measured_progress);
                    }
                    add_text(row, title, recompui::theme::Typography::LabelMD,
                        achievement->unlocked ? recompui::theme::color::Text : recompui::theme::color::TextDim);
                    add_text(row, achievement->description ? achievement->description : "", recompui::theme::Typography::Body,
                        recompui::theme::color::TextDim);
                }
            }
            rc_client_destroy_achievement_list(achievements);
        }
    public:
        AchievementsPage(recompui::ResourceId rid, recompui::Element* parent)
            : ConfigPage(rid, parent, recompui::Events(recompui::EventType::Update)) {
            set_as_navigation_container(recompui::NavigationType::Horizontal);
            body->get_right()->set_overflow_y(recompui::Overflow::Hidden);
            refresh();
            queue_update();
        }
    };
}

void conker::achievements::init() {
    if (client != nullptr) {
        return;
    }
    create_client();
    const auto login = read_login();
    const auto username = login.find("username");
    if (username == login.end() || username->second.empty()) {
        return;  // No login yet: nothing happens until one is made in the settings tab.
    }
    const auto token = login.find("token");
    const auto password = login.find("password");
    if (token != login.end() && !token->second.empty()) {
        set_login_state(LoginState::LoggingIn);
        rc_client_begin_login_with_token(client, username->second.c_str(), token->second.c_str(), logged_in_callback, nullptr);
    }
    else if (password != login.end() && !password->second.empty()) {
        set_login_state(LoginState::LoggingIn);
        rc_client_begin_login_with_password(client, username->second.c_str(), password->second.c_str(), logged_in_callback, nullptr);
    }
}

void conker::achievements::add_tab() {
    recompui::config::create_tab("RetroAchievements", "retroachievements",
        [](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<AchievementsPage>(parent);
        });
}

void conker::achievements::game_frame(uint8_t* rdram) {
    if (client == nullptr) {
        return;
    }
    game_rdram = rdram;
    if (!logged_in) {
        return;
    }
    if (!game_load_started.exchange(true)) {
        const std::span<const uint8_t> rom = recomp::get_rom();
        // The exception vectors, as the N64's osInitialize leaves them: libultra's preamble (lui k0, %hi(handler);
        // addiu k0, k0, %lo(handler); jr k0; nop) copied to 0x80000000, 0x80000080, 0x80000100 and 0x80000180. The
        // recompiled game takes no exceptions there and never copies it, so those words were 0; Conker's achievements
        // tell the game's versions apart by the handler's address in the second word (0x80000004: 0x71E0 in the US
        // ROM, a 16-bit read of 29152 there) and none of them could trigger. Found in the ROM by its instructions
        // (big-endian), so each version gets its own; written as the game's words are held.
        for (size_t at = 0; at + 16 <= std::min<size_t>(rom.size(), 0x20000); at += 4) {
            const uint8_t* w = rom.data() + at;
            const bool preamble = (w[0] == 0x3C && w[1] == 0x1A) && (w[4] == 0x27 && w[5] == 0x5A) &&
                (w[8] == 0x03 && w[9] == 0x40 && w[10] == 0x00 && w[11] == 0x08) && (w[12] == 0 && w[13] == 0 && w[14] == 0 && w[15] == 0);
            if (!preamble) {
                continue;
            }
            for (uint32_t vector : { 0x000u, 0x080u, 0x100u, 0x180u }) {
                for (uint32_t k = 0; k < 4; k++) {
                    const uint8_t* b = w + k * 4;
                    const uint32_t word = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
                    std::memcpy(rdram + vector + k * 4, &word, sizeof(word));
                }
            }
            log_line("[memory] exception vectors from ROM offset 0x%zX: second word %02X%02X%02X%02X", at, w[4], w[5], w[6], w[7]);
            break;
        }
        set_game_state(GameState::Loading);
        rc_client_begin_identify_and_load_game(client, RC_CONSOLE_NINTENDO_64, nullptr, rom.data(), rom.size(), game_loaded, nullptr);
    }
    rc_client_do_frame(client);

    // TEMP-DEBUG: the memory reads checked every 150 game frames: Conker's x (gObjects[0] + 0x14, a float) through
    // read_memory (a little-endian word, as the emulators' layout reads) against the word as the game holds it, and
    // the values Birdy's achievement ("Alcohol Is the Best Medicine") goes by.
    static uint32_t frames = 0;
    if (++frames % 150 == 0) {
        constexpr uint32_t conker_x = 0x800CC2D0 + 0x14;
        uint8_t bytes[4] = {};
        read_memory(conker_x - 0x80000000, bytes, 4, client);
        const uint32_t through_reader = ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[1] << 8) | bytes[0];
        uint32_t direct = 0;
        std::memcpy(&direct, rdram + (conker_x - 0x80000000), sizeof(direct));
        float x = 0.0f;
        std::memcpy(&x, &direct, sizeof(x));
        log_line("[memory] frame %u Conker's x 0x%08X through the reader, 0x%08X direct (%.1f)", frames, through_reader, direct, x);
        // Birdy's: the version (16-bit at 0x4: 29904, or 29152 for the other alternative), the level byte (0x0BEE14,
        // or 0x0BE9F4: 41 in Hangover), and bit 3 of the byte 3 past the 24-bit pointer at 0x0D326C (or 0x0D2E4C).
        auto read_le = [&](uint32_t address, uint32_t size) {
            uint8_t b[4] = {};
            read_memory(address, b, size, client);
            return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        };
        const uint32_t pointer_a = read_le(0x0D326C, 3), pointer_b = read_le(0x0D2E4C, 3);
        log_line("[birdy] version %u level %u/%u pointer 0x%06X flag %u / pointer 0x%06X flag %u", read_le(0x4, 2),
            read_le(0x0BEE14, 1), read_le(0x0BE9F4, 1), pointer_a, (read_le(pointer_a + 3, 1) >> 3) & 1, pointer_b, (read_le(pointer_b + 3, 1) >> 3) & 1);
    }
}

std::vector<uint8_t> conker::achievements::save_progress() {
    std::vector<uint8_t> progress;
    if (client == nullptr) {
        return progress;
    }
    progress.resize(rc_client_progress_size(client));
    if (progress.empty() || rc_client_serialize_progress_sized(client, progress.data(), progress.size()) != RC_OK) {
        progress.clear();
    }
    return progress;
}

void conker::achievements::load_progress(std::span<const uint8_t> progress) {
    if (client == nullptr) {
        return;
    }
    // Without progress (a state taken before the game's achievements loaded), rcheevos starts them over.
    rc_client_deserialize_progress_sized(client, progress.empty() ? nullptr : progress.data(), progress.size());
}

bool conker::achievements::hardcore() {
    return client != nullptr && rc_client_get_hardcore_enabled(client);
}

void conker::achievements::show_notice(const std::string& text) {
    std::printf("[notice] %s\n", text.c_str());
    std::fflush(stdout);
    std::lock_guard lock{messages_mutex};
    messages.push_back(text);
}

void conker::achievements::on_ui_ready() {
    ui_ready = true;
}

void conker::achievements::update() {
    if (!ui_ready || client == nullptr) {
        return;
    }
    // Not before the game has started: shown at the launcher, the line kept it from being shown (recompui
    // brings the launcher back only while no context is shown) and the screen stayed black. The lines wait.
    if (!ultramodern::is_game_started()) {
        return;
    }
    const clock::time_point now = clock::now();
    if (shown_text.empty() || now >= shown_until) {
        std::string next;
        {
            std::lock_guard lock{messages_mutex};
            if (!messages.empty()) {
                next = messages.front();
                messages.pop_front();
            }
        }
        if (!overlay_created && !next.empty()) {
            create_overlay();
        }
        if (overlay_created) {
            if (next.empty()) {
                if (!shown_text.empty()) {
                    recompui::hide_context(overlay);
                    shown_text.clear();
                }
                return;
            }
            shown_text = next;
            shown_until = now + message_time;
            overlay.open();
            overlay_label->set_text(shown_text);
            overlay.close();
            if (!recompui::is_context_shown(overlay)) {
                recompui::show_context(overlay, "");
            }
        }
    }
}
