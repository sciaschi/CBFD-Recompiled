// RetroAchievements (a prototype): the game's achievements from retroachievements.org, earned as you play.
//
// rcheevos (tools/rcheevos, MIT) does the work: rc_client logs in, identifies the game by its ROM's hash,
// fetches its achievements and, once a game frame, checks their conditions against the game's memory. It
// asks us for three things:
// - Memory: RetroAchievements' N64 addresses are RDRAM's (0 to 0x7FFFFF, the expansion pak's included, as
//   0x80000000 on). The recompiled game keeps RDRAM in 32-bit words in the host's byte order, so the N64's
//   byte at an address is at that address ^ 3 (as MEM_B reads it).
// - HTTP: each request rc_client makes (a URL, with POST data or not) is sent on a thread of its own, and
//   the reply handed back. On Windows with WinHTTP (the system's, nothing to ship); elsewhere not yet: those
//   builds report the request failed, so they run as without achievements.
// - The UI: an unlock shows as a line in the top-left corner for a few seconds (a recompui context of its
//   own that takes no input, as the FPS counter's), and everything rc_client says goes to the console.
//
// Login, for now: retroachievements.txt in the config folder (next to the other settings) with
//   username=<name>
//   password=<password>
// Once logged in, the password is replaced by the token the server gives (token=...), so it isn't kept.
// Casual (softcore) only: hardcore mode needs RetroAchievements' approval of the program, and its rules (no
// cheats: the mods would have to be off) aren't in place yet.
//
// Threads: rc_client locks itself. The game thread runs the frames (conker_frame_dl_begin, widescreen.cpp)
// and starts loading the game once logged in; the main thread logs in and draws; replies come on their own
// threads.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

#include "rc_client.h"
#include "rc_consoles.h"

#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
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

    // The unlocks to show, from the game thread, for the main thread.
    std::mutex messages_mutex;
    std::deque<std::string> messages;
    void show_message(const std::string& text) {
        std::printf("[achievements] %s\n", text.c_str());
        std::fflush(stdout);
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
            buffer[i] = rdram[(address + i) ^ 3];
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
            response.body = body.c_str();
            response.body_length = body.size();
            callback(&response, callback_data);
        }).detach();
    }

    // --- Events ---

    void RC_CCONV log_message(const char* message, const rc_client_t*) {
        std::printf("[rcheevos] %s\n", message);
        std::fflush(stdout);
    }

    void RC_CCONV handle_event(const rc_client_event_t* event, rc_client_t*) {
        switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            show_message(std::string("Achievement unlocked: ") + event->achievement->title + " (" +
                std::to_string(event->achievement->points) + ")");
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
            return;
        }
        const rc_client_game_t* game = rc_client_get_game_info(client);
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);
        show_message(std::string("RetroAchievements: ") + (game ? game->title : "the game") + ", " +
            std::to_string(summary.num_unlocked_achievements) + " of " + std::to_string(summary.num_core_achievements) + " unlocked");
    }

    void RC_CCONV logged_in_callback(int result, const char* error_message, rc_client_t* client, void*) {
        if (result != RC_OK) {
            show_message(std::string("RetroAchievements: login failed (") + (error_message ? error_message : "unknown error") + ")");
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
}

void conker::achievements::init() {
    if (client != nullptr) {
        return;
    }
    const auto login = read_login();
    const auto username = login.find("username");
    if (username == login.end() || username->second.empty()) {
        return;  // No login file: RetroAchievements stays off.
    }
    client = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_event_handler(client, handle_event);
    rc_client_set_hardcore_enabled(client, 0);
    char clause[128] = {};
    rc_client_get_user_agent_clause(client, clause, sizeof(clause));
    user_agent = std::string(program_name) + "/" + program_version + " " + clause;

    const auto token = login.find("token");
    const auto password = login.find("password");
    if (token != login.end() && !token->second.empty()) {
        rc_client_begin_login_with_token(client, username->second.c_str(), token->second.c_str(), logged_in_callback, nullptr);
    }
    else if (password != login.end() && !password->second.empty()) {
        rc_client_begin_login_with_password(client, username->second.c_str(), password->second.c_str(), logged_in_callback, nullptr);
    }
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
        rc_client_begin_identify_and_load_game(client, RC_CONSOLE_NINTENDO_64, nullptr, rom.data(), rom.size(), game_loaded, nullptr);
    }
    rc_client_do_frame(client);
}

void conker::achievements::on_ui_ready() {
    ui_ready = true;
}

void conker::achievements::update() {
    if (!ui_ready || client == nullptr) {
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
