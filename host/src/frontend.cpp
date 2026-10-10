// The window build's front end: RecompFrontend's launcher, settings and mod menus
// (recompui) and remappable keyboard/controller input (recompinput), on an SDL
// window rendered by RT64 through recompui's renderer.

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(_WIN32) || defined(__APPLE__)
// Only Windows and macOS need the native window handle. On Linux this header brings
// in X11's, whose None macro breaks ultramodern's Device::None.
#include <SDL_syswm.h>
#endif

#include "nfd.h"

#include "librecomp/game.hpp"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "base/ui_launcher.h"
#include "util/file.h"

#include "conker.hpp"

// recompui's launcher shows the first entry (ui_launcher.cpp declares it extern).
std::vector<recomp::GameEntry> supported_games;
// The game window, which recompui also uses (ui_state.cpp declares it extern).
SDL_Window* window = nullptr;

void conker_mouse_camera_init();

namespace {
    std::vector<char> thumbnail;

    // Each poll also tells the look mode that the next mouse and gyro movement is in.
    void poll_inputs() {
        recompinput::poll_inputs();
        conker::look_aim::on_input_poll();
    }

    // The launcher's Version and Add ROM options, and the window title that names the
    // version in play. The title is set on the main thread (update_gfx), as macOS requires.
    recompui::GameOption* version_option = nullptr;
    recompui::GameOption* add_rom_option = nullptr;
    std::mutex title_mutex;
    std::string pending_title;

    // Prints what SDL says about a controller (name, GUID, mapping, device), for controller reports.
    void print_controller(int index) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
        const char* name = SDL_GameControllerNameForIndex(index);
        const char* path = SDL_GameControllerPathForIndex(index);
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        std::printf("[controller] connected: %s (GUID %s, device %s)\n  mapping: %s\n", name ? name : "?", guid,
            path ? path : "?", mapping ? mapping : "none");
        SDL_free(mapping);
    }

    // The controllers connected, each opened here too (SDL counts its opens, so recompinput's own
    // stays as it is), by instance ID. Only the main thread changes this, as SDL announces a
    // controller connecting or disconnecting (watch_controllers) and at start (create_gfx); the
    // other threads read it (get_port_controllers), so none of them asks SDL for its devices.
    //
    // Issue #88: macOS crashed when a controller connected while playing. The Mac build's SDL2 is
    // sdl2-compat, SDL2's functions over SDL3, and it keeps one list of devices for the index-based
    // calls: SDL_NumJoysticks frees it and fetches it again, without a lock around the two. As a
    // DualSense connected, sdl2-compat refreshed it on the main thread for the event while the VI
    // thread's rumble listed the controllers (SDL_NumJoysticks, for the port controllers), and the
    // list was freed twice. Listing them only on the main thread, as SDL announces each, they're
    // never listed at once. A controller's buttons, sticks and rumble are safe from any thread.
    std::mutex open_controllers_mutex;
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open_controllers;

    // From the main thread: the controller at device index, if it's one SDL has a mapping for and
    // it isn't in the list yet.
    void open_controller(int index) {
        if (!SDL_IsGameController(index)) {
            return;
        }
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        {
            std::lock_guard lock{ open_controllers_mutex };
            for (const auto& [open_id, controller] : open_controllers) {
                if (open_id == id) {
                    return;
                }
            }
        }
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller == nullptr) {
            std::printf("[controller] couldn't open device %d: %s\n", index, SDL_GetError());
            return;
        }
        std::lock_guard lock{ open_controllers_mutex };
        open_controllers.emplace_back(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)), controller);
    }

    // From the main thread: the controller with that instance ID leaves the list.
    void close_controller(SDL_JoystickID id) {
        SDL_GameController* closing = nullptr;
        {
            std::lock_guard lock{ open_controllers_mutex };
            for (auto it = open_controllers.begin(); it != open_controllers.end(); ++it) {
                if (it->first == id) {
                    closing = it->second;
                    open_controllers.erase(it);
                    break;
                }
            }
        }
        if (closing != nullptr) {
            SDL_GameControllerClose(closing);
        }
    }

    // Watches controllers connecting: each is printed, opened into the list above, and its C-buttons
    // remapped if they need it (pad_mappings.cpp). SDL calls this on the thread that announces the
    // device, the main one, as it pumps events, so the devices are listed there.
    int SDLCALL watch_controllers(void*, SDL_Event* event) {
        switch (event->type) {
        case SDL_JOYDEVICEADDED:
            conker::pad_mappings::fix_all();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            print_controller(event->cdevice.which);
            open_controller(event->cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event->cdevice.which);
            break;
        }
        std::fflush(stdout);
        return 1;
    }

    void* create_gfx() {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if defined(__linux__)
        // Nintendo's online classic controllers (the N64 one, and pads like the 8BitDo 64 in its
        // Switch mode) through the kernel's driver, not SDL's HIDAPI one, so there's one layout for
        // pad_mappings.cpp to make the C-buttons the right stick of (issue #28): SDL3, under Linux
        // distributions' sdl2-compat, maps both as a Switch pad, and HIDAPI's puts one C-button on
        // an axis. Windows keeps SDL's default: its SDL is ours (2.26), and without HIDAPI the pad
        // may have no mapping there. The SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC environment variable
        // still overrides this.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, "0");
#endif
        // Debugging aid: CONKER_NO_CONTROLLER=1 ignores game controllers, e.g. for test
        // runs while someone else is playing with the controller on the same machine.
        Uint32 subsystems = SDL_INIT_VIDEO;
        if (SDL_getenv("CONKER_NO_CONTROLLER") == nullptr) {
            subsystems |= SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
        }
        if (SDL_Init(subsystems) != 0) {
            std::fprintf(stderr, "[frontend] SDL_Init failed: %s\n", SDL_GetError());
        }
        SDL_version sdl_version;
        SDL_GetVersion(&sdl_version);
        std::printf("[frontend] SDL %d.%d.%d\n", sdl_version.major, sdl_version.minor, sdl_version.patch);
        // N64 pads and adapters SDL has no mapping for, or maps as other pads (assets/controllerdb.txt).
        // Only mappings, so the controllers connected at start are picked up when they're opened.
        const std::u8string controller_db = recompui::file::get_asset_path("controllerdb.txt").u8string();
        const int controller_mappings = SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db.c_str()));
        if (controller_mappings < 0) {
            std::fprintf(stderr, "[frontend] couldn't load the controller mappings: %s\n", SDL_GetError());
        } else {
            std::printf("[frontend] controller mappings: %d added\n", controller_mappings);
        }
        SDL_AddEventWatch(watch_controllers, nullptr);
        // The controllers connected at start were announced before the watch.
        for (int index = 0; index < SDL_NumJoysticks(); index++) {
            print_controller(index);
            open_controller(index);
        }
        conker::pad_mappings::fix_all();
        // The file dialogs (Load ROM, mods). Only after SDL: on macOS, NFD_Init creates the
        // application object if it doesn't exist yet and makes it an accessory app, and SDL
        // then leaves it that way (no Dock icon, and the window opens behind the terminal).
        NFD_Init();
        // The mouse camera's scroll wheel zoom (mouse_camera.cpp).
        conker_mouse_camera_init();
        return nullptr;
    }

    ultramodern::renderer::WindowHandle create_window(void*) {
        uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(RT64_SDL_WINDOW_VULKAN)
        flags |= SDL_WINDOW_VULKAN;
#elif defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#endif
        const int width = conker::frontend::window_width > 0 ? conker::frontend::window_width : 1600;
        const int height = conker::frontend::window_height > 0 ? conker::frontend::window_height : 900;
        window = SDL_CreateWindow(conker::program_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height, flags);
        if (window == nullptr) {
            std::fprintf(stderr, "[frontend] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return {};
        }
#if defined(_WIN32)
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        return ultramodern::renderer::WindowHandle{ info.info.win.window, GetCurrentThreadId() };
#elif defined(__APPLE__)
        // RT64 renders with Metal into the CAMetalLayer of a view added to the window.
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        SDL_MetalView view = SDL_Metal_CreateView(window);
        return ultramodern::renderer::WindowHandle{ info.info.cocoa.window, SDL_Metal_GetLayer(view) };
#else
        return ultramodern::renderer::WindowHandle{ window };
#endif
    }

    void update_gfx(void*) {
        recompinput::handle_events();
        conker::texture_packs::update_unpacking();
        conker::fps_counter::update();
        conker::notices::update();
        conker::reticle::update();
        std::string title;
        {
            std::lock_guard lock(title_mutex);
            title.swap(pending_title);
        }
        if (!title.empty() && window != nullptr) {
            SDL_SetWindowTitle(window, title.c_str());
        }
    }

    // Shows which version of the ROM is in play: on the launcher's Version option and in
    // the window title. Version and Add ROM are hidden until there's a ROM (Start Game's
    // Load ROM picks the first).
    void show_version() {
        std::string name = conker::roms::current_name();
        for (recompui::GameOption* option : { version_option, add_rom_option }) {
            if (option != nullptr) {
                if (name.empty()) {
                    option->display_hide();
                }
                else {
                    option->display_show();
                }
            }
        }
        if ((version_option != nullptr) && !name.empty()) {
            version_option->set_title("Version: " + name);
        }
        std::printf("[frontend] ROM version in play: %s (%zu kept)\n", name.empty() ? "none" : name.c_str(),
            conker::roms::version_count());
        std::lock_guard lock(title_mutex);
        pending_title = name.empty() ? std::string(conker::program_name)
                                     : std::string(conker::program_name) + " (" + name + ")";
    }

    // The Version option: switches to the next version kept.
    void on_version_selected() {
        if (conker::roms::version_count() < 2) {
            recompui::message_box("This is the only version of the ROM loaded. Add another with Add ROM "
                "(a ROM hack that only changes the game's assets, such as an uncensored one) to switch between them.");
            return;
        }
        if (conker::roms::switch_to_next()) {
            show_version();
        }
    }

    // The Add ROM option: loads another ROM, which is kept as a version and put in play.
    void on_add_rom_selected() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) {
                return;
            }
            recomp::RomValidationError result = recomp::select_rom(path, supported_games[0].game_id);
            if (result == recomp::RomValidationError::IncorrectVersion) {
                // Conker's Bad Fur Day, but not a US ROM the game plays: say which region it is.
                std::string region = conker::roms::region_of(path);
                if (!region.empty() && region != "US") {
                    std::string text = "This is the " + region + " version of Conker's Bad Fur Day. Only the US "
                        "version is supported (and ROM hacks of it that only change the game's assets).";
                    recompui::message_box(text.c_str());
                    return;
                }
            }
            if (result != recomp::RomValidationError::Good) {
                recompui::message_box(conker::rom_error_text(result));
            }
            // The launcher's update picks the new version up once it's written.
        });
    }

    // The launcher's options: RecompFrontend's usual ones, with Version and Add ROM after Start Game.
    void init_launcher(recompui::LauncherMenu* menu) {
        const recomp::GameEntry& game = supported_games[0];
        // Here rather than at startup, so a ROM given with --rom is already the one stored.
        conker::roms::init(recomp::get_config_path() / game.stored_filename());
        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            game.game_id, game.mod_game_id, game.display_name, game.thumbnail_bytes);
        // Lower than recompui's 25% from the bottom: with Version and Add ROM, seven options
        // would reach up into the title.
        options->set_bottom(10.0f, recompui::Unit::Percent);
        recompui::update_game_mod_id(game.mod_game_id);
        // The runtime has opened the mods by now: turn on the texture pack chosen in the settings.
        conker::texture_packs::apply();
        // And unpack the GLideN64 packs not unpacked yet, showing the progress over the launcher.
        conker::texture_packs::unpack_gliden64_packs();
        // recompui's UI exists now: the FPS counter and the aiming reticle can make their own.
        conker::fps_counter::on_ui_ready();
        conker::notices::on_ui_ready();
        conker::achievements::init();
        conker::reticle::on_ui_ready();
        options->add_start_game_or_load_rom_option();
        version_option = options->add_option("Version", on_version_selected);
        add_rom_option = options->add_option("Add ROM", on_add_rom_selected);
        options->add_setup_controls_option();
        options->add_settings_option();
        options->add_mods_option();
        options->add_exit_option();
        show_version();
    }

    void update_launcher(recompui::LauncherMenu*) {
        if (conker::roms::update()) {
            show_version();
        }
    }

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        return recompui::renderer::create_render_context(rdram, window_handle,
            ultramodern::renderer::PresentationMode::PresentEarly,
            // TEMP-DEBUG: CONKER_DEV_MODE turns on RT64's developer tools (F1: inspector).
            developer_mode || SDL_getenv("CONKER_DEV_MODE") != nullptr);
    }

}

void conker::frontend::on_vi() {
    conker::rumble::update();
}

// Controller ports. recompinput's single-player mode reports all four ports as plugged
// in and gives every port the merged input of every device, so on the multiplayer
// screen one Start press joined players 2-4 as well. Instead, each controller gets its
// own port, in the order the controllers first press a button (Windows lists them in
// its own order, not the order they were turned on): the first to press one is player
// 1, with the keyboard, the next player 2, and so on. A controller that disconnects
// gives up its port, and the ones after it move up. As many ports as there are
// controllers report as plugged in (at least one), so the game sees player 2's port
// from the start; it gets that controller's input once it presses a button. Every
// controller uses the single-player controller bindings.
namespace {
    constexpr int max_ports = 4;

    std::mutex port_mutex;
    // The controllers holding ports 1-4, in the order they first pressed a button.
    std::vector<SDL_JoystickID> port_order;

    bool controller_any_button(SDL_GameController* controller) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) {
                return true;
            }
        }
        // The triggers are axes; the sticks are left out, so a drifting stick doesn't claim a port.
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384 ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
    }

    // Updates the port order and fills `out` with the controllers holding ports, in port
    // order. Returns how many hold ports; `connected` gets how many controllers are open.
    int get_port_controllers(std::array<SDL_GameController*, max_ports>& out, int* connected = nullptr) {
        std::lock_guard lock{ port_mutex };
        // The controllers connected, from the main thread's list (open_controllers): this runs on
        // the game's threads (input, rumble), where SDL's own device list isn't safe to read (#88).
        std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open;
        {
            std::lock_guard open_lock{ open_controllers_mutex };
            open = open_controllers;
        }
        auto find_open = [&](SDL_JoystickID id) -> SDL_GameController* {
            for (const auto& [open_id, controller] : open) {
                if (open_id == id) {
                    return controller;
                }
            }
            return nullptr;
        };
        // Disconnected controllers give up their ports.
        std::erase_if(port_order, [&](SDL_JoystickID id) { return find_open(id) == nullptr; });
        // Controllers pressing a button for the first time take the next port.
        for (const auto& [id, controller] : open) {
            if ((int)port_order.size() < max_ports && std::find(port_order.begin(), port_order.end(), id) == port_order.end() &&
                controller_any_button(controller)) {
                port_order.push_back(id);
            }
        }
        int count = 0;
        for (SDL_JoystickID id : port_order) {
            out[count++] = find_open(id);
        }
        if (connected != nullptr) {
            *connected = std::min((int)open.size(), max_ports);
        }
        return count;
    }

    // The stick axis read for an axis bound: with swap (Aiming: Swap Sticks, issue #84), the left stick's
    // axes are read from the right stick and the right's from the left, so whatever the profile binds to
    // either stick (by default the N64 stick to the left, the C-buttons to the right) moves to the other.
    int stick_axis(int axis, bool swap) {
        if (!swap) {
            return axis;
        }
        switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: return SDL_CONTROLLER_AXIS_RIGHTX;
        case SDL_CONTROLLER_AXIS_LEFTY: return SDL_CONTROLLER_AXIS_RIGHTY;
        case SDL_CONTROLLER_AXIS_RIGHTX: return SDL_CONTROLLER_AXIS_LEFTX;
        case SDL_CONTROLLER_AXIS_RIGHTY: return SDL_CONTROLLER_AXIS_LEFTY;
        default: return axis;
        }
    }

    float controller_field_analog(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        switch (field.input_type) {
        case recompinput::InputType::ControllerDigital:
            if (field.input_id >= 0 && field.input_id < SDL_CONTROLLER_BUTTON_MAX) {
                return SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)field.input_id) ? 1.0f : 0.0f;
            }
            return 0.0f;
        case recompinput::InputType::ControllerAnalog: {
            int axis = std::abs(field.input_id) - 1;
            if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
                return 0.0f;
            }
            float value = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)stick_axis(axis, swap)) * (1.0f / 32768.0f);
            if (field.input_id < 0) {
                value = -value;
            }
            return std::clamp(value, 0.0f, 1.0f);
        }
        default:
            return 0.0f;
        }
    }

    bool controller_field_digital(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        if (field.input_type == recompinput::InputType::ControllerAnalog) {
            return controller_field_analog(controller, field, swap) >= recompinput::axis_digital_threshold;
        }
        return controller_field_analog(controller, field, swap) > 0.0f;
    }

    // The keyboard and the mouse are player 1's: their bindings count on port 1 only. Mouse buttons
    // can be bound with the keyboard's controls or, in single player, the controller's.
    bool is_keyboard_or_mouse(const recompinput::InputField& field) {
        return field.input_type == recompinput::InputType::Keyboard || field.input_type == recompinput::InputType::Mouse;
    }

    // A binding to the right stick (either axis, either way).
    bool is_right_stick(const recompinput::InputField& field) {
        if (field.input_type != recompinput::InputType::ControllerAnalog) {
            return false;
        }
        const int axis = std::abs(field.input_id) - 1;
        return axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY;
    }

    // One controller (or none) and/or the keyboard and mouse, through the single-player bindings.
    // With free_stick (Right Stick: Free Camera, while the stick turns the camera), the controller's
    // right stick presses no button: it turns the camera instead (mouse_camera.cpp). With swap_sticks
    // (Aiming: Swap Sticks, while aiming), the controller's two sticks are read the other way round.
    void read_port(SDL_GameController* controller, bool keyboard, bool free_stick, bool swap_sticks, bool z_turns_aim, uint16_t* buttons, float* x, float* y) {
        using recompinput::GameInput;
        static constexpr uint16_t button_values[] = {
            0x8000, 0x4000, 0x2000, 0x0020, 0x0010, 0x1000, 0x0008,
            0x0004, 0x0002, 0x0001, 0x0800, 0x0400, 0x0200, 0x0100,
        };
        // A..C Right, then the D-pad (N64_BUTTON_COUNT stops at C Right).
        static_assert(std::size(button_values) == (size_t)GameInput::DPAD_RIGHT - (size_t)GameInput::N64_BUTTON_START + 1);
        const int cont_profile = recompinput::profiles::get_sp_controller_profile_index();
        const int kb_profile = recompinput::profiles::get_sp_keyboard_profile_index();
        auto binding = [](int profile, GameInput input, size_t i) -> const recompinput::InputField& {
            return recompinput::profiles::get_input_binding(profile, input, i);
        };
        auto cont_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(cont_profile, input, i);
                if (field.input_type == recompinput::InputType::Mouse) {
                    v += keyboard ? recompinput::get_input_analog(0, field) : 0.0f;
                }
                else if (controller != nullptr) {
                    v += controller_field_analog(controller, field, swap_sticks);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };
        auto kb_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(kb_profile, input, i);
                if (is_keyboard_or_mouse(field)) {
                    v += recompinput::get_input_analog(0, field);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };

        uint16_t cur_buttons = 0;
        // With swap_sticks, the buttons pressed only by the left stick (bindings to the right stick, read
        // from the left): see below.
        uint16_t left_stick_buttons = 0;
        float cur_x = 0.0f;
        float cur_y = 0.0f;
        for (size_t b = 0; b < std::size(button_values); b++) {
            GameInput input = (GameInput)((size_t)GameInput::N64_BUTTON_START + b);
            bool pressed = false;
            bool by_left_stick = false;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                if (cont_profile >= 0) {
                    const recompinput::InputField& field = binding(cont_profile, input, i);
                    if (field.input_type == recompinput::InputType::Mouse) {
                        pressed |= keyboard && recompinput::get_input_digital(0, field);
                    }
                    else if (controller != nullptr && !(free_stick && is_right_stick(field))) {
                        const bool down = controller_field_digital(controller, field, swap_sticks);
                        if (swap_sticks && is_right_stick(field)) {
                            by_left_stick |= down;
                        }
                        else {
                            pressed |= down;
                        }
                    }
                }
                if (keyboard && kb_profile >= 0) {
                    const recompinput::InputField& field = binding(kb_profile, input, i);
                    if (is_keyboard_or_mouse(field)) {
                        pressed |= recompinput::get_input_digital(0, field);
                    }
                }
            }
            if (pressed) {
                cur_buttons |= button_values[b];
            }
            else if (by_left_stick) {
                left_stick_buttons |= button_values[b];
            }
        }
        // Aiming: Swap Sticks: the left stick presses the C-buttons, which move Conker while aiming, but not
        // C-Left and C-Right while Z is held aiming the shotgun (issue #84, z_turns_aim). With the shotgun, R aims
        // while he moves and strafes with the C-buttons; Z (the laser sight) makes C-Left and C-Right turn the aim
        // instead, so pushing the left stick sideways turned the aim against the right stick (logged: 40 degrees in
        // 0.4 seconds, the right stick still). Its forward and back (C-Up, C-Down) still count. Elsewhere Z is
        // left alone: in split screen it fires, and strafing while shooting was lost.
        constexpr uint16_t z_button = 0x2000, c_left = 0x0002, c_right = 0x0001;
        if ((cur_buttons & z_button) && z_turns_aim) {
            left_stick_buttons &= (uint16_t)~(c_left | c_right);
        }
        cur_buttons |= left_stick_buttons;
        if (controller != nullptr && cont_profile >= 0) {
            cur_x = cont_analog(GameInput::X_AXIS_POS) - cont_analog(GameInput::X_AXIS_NEG);
            cur_y = cont_analog(GameInput::Y_AXIS_POS) - cont_analog(GameInput::Y_AXIS_NEG);
            recompinput::apply_joystick_deadzone(cur_x, cur_y, &cur_x, &cur_y);
        }
        if (keyboard && kb_profile >= 0) {
            const float kb_x = kb_analog(GameInput::X_AXIS_POS) - kb_analog(GameInput::X_AXIS_NEG);
            const float kb_y = kb_analog(GameInput::Y_AXIS_POS) - kb_analog(GameInput::Y_AXIS_NEG);
            if (swap_sticks) {
                // Aiming: Swap Sticks with the keyboard: the mouse aims, so the keys bound to the stick (W, A, S, D
                // by default) press the C-buttons and move Conker, as the left stick does, instead of aiming
                // (issue #84: they only turned the aim). Not C-Left or C-Right while Z is held, the same as the stick.
                constexpr uint16_t c_up = 0x0008, c_down = 0x0004;
                uint16_t keys = 0;
                if (kb_x <= -recompinput::axis_digital_threshold) keys |= c_left;
                if (kb_x >= recompinput::axis_digital_threshold) keys |= c_right;
                if (kb_y >= recompinput::axis_digital_threshold) keys |= c_up;
                if (kb_y <= -recompinput::axis_digital_threshold) keys |= c_down;
                if ((cur_buttons & z_button) && z_turns_aim) {
                    keys &= (uint16_t)~(c_left | c_right);
                }
                cur_buttons |= keys;
            }
            else {
                cur_x += kb_x;
                cur_y += kb_y;
            }
        }
        // In the bank's slow motion leaps the mouse moves the crosshair, as the stick (issue #85): port 1's.
        if (keyboard) {
            conker::look_aim::leap_stick(cur_x, cur_y);
        }
        *buttons = cur_buttons;
        *x = std::clamp(cur_x, -1.0f, 1.0f);
        *y = std::clamp(cur_y, -1.0f, 1.0f);
    }

    bool get_port_input(int port, uint16_t* buttons, float* x, float* y) {
        // recompinput's own multiplayer mode (players assigned in its menus) gives each
        // player their controller and profiles already.
        if (!recompinput::players::is_single_player_mode()) {
            return recompinput::profiles::get_n64_input(port, buttons, x, y);
        }
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        if (port < 0 || port >= max_ports) {
            return false;
        }
        std::array<SDL_GameController*, max_ports> controllers{};
        int connected = 0;
        int count = get_port_controllers(controllers, &connected);
        if (port >= std::max(connected, 1)) {
            return false;
        }
        if (!recompinput::game_input_disabled()) {
            // Port 1 has the keyboard, and its controller once one has pressed a button.
            read_port(port < count ? controllers[port] : nullptr, port == 0,
                port == 0 && conker::mouse_camera::stick_turns_camera(), conker::look_aim::swap_sticks_now(port), conker::look_aim::z_turns_aim(port), buttons, x, y);
        }
        return true;
    }
}

int conker::frontend::port_controllers(std::array<SDL_GameController*, max_ports>& out) {
    return get_port_controllers(out);
}

ultramodern::input::connected_device_info_t conker::frontend::get_connected_device_info(int controller_num) {
    if (!recompinput::players::is_single_player_mode()) {
        if (recompinput::players::get_player_is_assigned(controller_num)) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    std::array<SDL_GameController*, max_ports> controllers{};
    int connected = 0;
    get_port_controllers(controllers, &connected);
    if (controller_num == 0 || controller_num < connected) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

std::u8string conker::program_id() {
    return SDL_getenv("CONKER_TEST_PROFILE") != nullptr ? u8"ConkerRecompiledTest" : u8"ConkerRecompiled";
}

void conker::frontend::init(recomp::GameEntry& game) {
    recompui::programconfig::set_program_name(program_name);
    recompui::programconfig::set_program_id(program_id());

    // The launcher's picture of the game.
    std::ifstream file(recompui::file::get_asset_path("thumbnail.png"), std::ios::binary);
    thumbnail.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    game.thumbnail_bytes = std::span<const char>(thumbnail);
    supported_games.push_back(game);

    // Versions of the ROM (the original, an uncensored ROM hack...) to switch between.
    recompui::register_launcher_init_callback(init_launcher);
    recompui::register_launcher_update_callback(update_launcher);

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompui::register_ui_exports();
    recompinput::players::set_single_player_mode(true);
    conker::init_config();

    // Texture packs (texture_packs.cpp).
    conker::texture_packs::register_type();
}

void conker::frontend::set_callbacks(recomp::Configuration& cfg) {
    cfg.renderer_callbacks.create_render_context = create_render_context;
    cfg.gfx_callbacks = { create_gfx, create_window, update_gfx };
    cfg.input_callbacks = { poll_inputs, get_port_input, conker::rumble::set,
                            conker::get_connected_device_info };
    cfg.error_handling_callbacks.message_box = recompui::message_box;
}
