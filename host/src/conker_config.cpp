// The settings menu (recompui's config tabs) for the RT64 build.

#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/config.hpp"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "util/file.h"

#include "conker.hpp"

namespace {
    // Cutscene Aspect Ratio (cutscene_aspect.cpp), on the Graphics tab.
    const std::string cutscene_aspect_id = "cutscene_aspect";
    enum class CutsceneAspect : uint32_t { Expand, Original };
    // Show FPS (fps_counter.cpp), on the Graphics tab.
    const std::string show_fps_id = "show_fps";
    enum class ShowFps : uint32_t { Off, On };
    // Overscan Borders (widescreen.cpp, RT64's presentation crop), on the Graphics tab.
    const std::string overscan_borders_id = "overscan_borders";
    enum class OverscanBorders : uint32_t { Hidden, Shown };
    // Conker's Detail (model_detail.cpp, the model the game picks by the camera's distance), on the
    // Graphics tab.
    const std::string conker_detail_id = "conker_detail";
    enum class ConkerDetail : uint32_t { AlwaysHighest, Original };

    void add_graphics_options(recomp::config::Config& config) {
        static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
            {CutsceneAspect::Expand, "Expand", "Expand"},
            {CutsceneAspect::Original, "Original", "4:3"},
        };
        config.add_enum_option(cutscene_aspect_id, "Cutscene Aspect Ratio",
            "The aspect ratio of cutscenes: story scenes, conversations, B pads' hints and Conker's thoughts, "
            "whenever the game plays one and you can't move. "
            "<recomp-color primary>Expand</recomp-color> shows them as wide as the rest of the game, where characters "
            "waiting for their cue can sometimes be seen beside the original picture. "
            "<recomp-color primary>4:3</recomp-color> shows them as on the N64, with black bars at the sides, "
            "and the game goes on in widescreen after each one.",
            choices, CutsceneAspect::Expand);
        config.add_option_disable_dependency(cutscene_aspect_id,
            recompui::config::graphics::options::ar_option, ultramodern::renderer::AspectRatio::Original);

        static const std::vector<recomp::config::ConfigOptionEnumOption> fps_choices = {
            {ShowFps::Off, "Off", "Off"},
            {ShowFps::On, "On", "On"},
        };
        config.add_enum_option(show_fps_id, "Show FPS",
            "Shows the frame rate in the top-right corner while playing, to spot slowdowns. "
            "<recomp-color primary>FPS</recomp-color> is the frames drawn to the screen each second (with interpolation, "
            "more than the game makes): a drop there is the PC falling behind. "
            "<recomp-color primary>Game</recomp-color> is the frames the game itself makes, up to 30: a drop there with FPS "
            "steady is the game's own slowdown, as on the N64.",
            fps_choices, ShowFps::Off);

        static const std::vector<recomp::config::ConfigOptionEnumOption> border_choices = {
            {OverscanBorders::Hidden, "Hidden", "Hidden"},
            {OverscanBorders::Shown, "Shown", "Shown"},
        };
        config.add_enum_option(overscan_borders_id, "Overscan Borders",
            "The game leaves a thin black border at the left and right of its picture, which a TV's overscan hid. "
            "<recomp-color primary>Hidden</recomp-color> zooms the picture in just enough to push it off the screen, "
            "the same amount both ways so nothing is stretched (a sliver of the top and bottom goes with it). "
            "<recomp-color primary>Shown</recomp-color> shows the whole picture, border and all.",
            border_choices, OverscanBorders::Hidden);
        static const std::vector<recomp::config::ConfigOptionEnumOption> detail_choices = {
            {ConkerDetail::AlwaysHighest, "AlwaysHighest", "Always Highest"},
            {ConkerDetail::Original, "Original", "Original"},
        };
        config.add_enum_option(conker_detail_id, "Conker's Detail",
            "Characters have a few models, from the most detailed down, and the game picks one by how far the "
            "camera is, and whether they're moving. "
            "<recomp-color primary>Always Highest</recomp-color> keeps Conker on his most detailed model, so it no "
            "longer swaps as the camera pulls back or as he starts and stops walking. Multiplayer is left as it was. "
            "<recomp-color primary>Original</recomp-color> lets the game pick, as on the N64.",
            detail_choices, ConkerDetail::AlwaysHighest);
    }

    // Settings moved to another tab keep their ids, but each tab saves to its own json (<tab id>.json in
    // the config folder): the values saved in the old tab's file are copied into the new tab's, once,
    // before the tabs load them (any the new file has already are left as they are). The old file loses
    // them the next time its tab saves.
    void carry_over(const std::string& from_id, const std::string& to_id, const std::vector<std::string>& keys) {
        const std::filesystem::path folder = recomp::get_config_path();
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path);
            nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
            return json.is_object() ? json : nlohmann::json::object();
        };
        const nlohmann::json from = read(folder / (from_id + ".json"));
        nlohmann::json to = read(folder / (to_id + ".json"));
        bool changed = false;
        for (const std::string& key : keys) {
            if (from.contains(key) && !to.contains(key)) {
                to[key] = from[key];
                changed = true;
            }
        }
        if (changed) {
            std::ofstream file(folder / (to_id + ".json"), std::ios::trunc);
            file << to.dump(4);
        }
    }

    // The tabs the General tab's camera and aiming settings moved to (look_aim.cpp), and the Texture
    // Packs tab's one setting, moved to the end of the Graphics tab.
    const std::string camera_tab_id = "camera";
    const std::string mouse_gyro_tab_id = "mouse_gyro";
    void carry_over_moved_settings() {
        carry_over(recompui::config::general::id, camera_tab_id, {
            "camera_invert_turning", "camera_turn_speed", "stick_free_camera", "camera_field_of_view_degrees",
            "look_stick_response", "look_stick_invert", "aim_reticle" });
        carry_over(recompui::config::general::id, mouse_gyro_tab_id, {
            recompui::config::general::options::mouse_sensitivity, "mouse_turns_camera", "look_mouse_response",
            "look_mouse_invert", recompui::config::general::options::gyro_sensitivity, "look_gyro_response",
            "look_gyro_invert" });
        carry_over("texture_packs", recompui::config::graphics::id, { "texture_pack" });
    }

    void set_control_descriptions() {
        using recompinput::GameInput;
        using recompinput::set_game_input_description;
        const char* stick = "Moves Conker, and moves the cursor in menus.";
        set_game_input_description(GameInput::Y_AXIS_POS, stick);
        set_game_input_description(GameInput::Y_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_POS, stick);
        set_game_input_description(GameInput::A, "Jumps (press again in the air to hover with the tail), and confirms in menus.");
        set_game_input_description(GameInput::B, "Context-sensitive action: attack with the current weapon, or use a B pad.");
        set_game_input_description(GameInput::Z, "Crouches.");
        set_game_input_description(GameInput::L, "Unused in the main game. Mods may use it.");
        set_game_input_description(GameInput::R, "Centers the camera behind Conker.");
        set_game_input_description(GameInput::START, "Pauses the game and skips some cutscenes.");
        set_game_input_description(GameInput::C_UP, "Enters first-person view.");
        set_game_input_description(GameInput::C_DOWN, "Changes the camera distance.");
        set_game_input_description(GameInput::C_LEFT, "Rotates the camera.");
        set_game_input_description(GameInput::C_RIGHT, "Rotates the camera.");
        const char* dpad = "Unused in the main game. Mods may use it.";
        set_game_input_description(GameInput::DPAD_UP, dpad);
        set_game_input_description(GameInput::DPAD_DOWN, dpad);
        set_game_input_description(GameInput::DPAD_LEFT, dpad);
        set_game_input_description(GameInput::DPAD_RIGHT, dpad);
    }
}

void conker::init_config() {
    std::filesystem::path app_folder = recompui::file::get_app_folder_path();
    if (!app_folder.empty()) {
        std::filesystem::create_directories(app_folder);
    }

    carry_over_moved_settings();

    // The General tab's rumble strength and gyro and mouse sensitivities are added by rumble.cpp and
    // look_aim.cpp instead, with the same ids, so each sits with the settings it goes with: rumble on
    // General, the camera and the stick's aiming on Camera, the mouse and gyro on Mouse & Gyro (each its
    // own tab now that there are so many). Mouse sensitivity defaults to 0, which leaves the mouse, and
    // the cursor, alone. (Each Config is used as soon as it's made: create_config_tab keeps them in a
    // vector, so the next tab can move the last one.)
    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = false;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    conker::rumble::add_options(recompui::config::create_general_tab(general_options));
    conker::look_aim::add_camera_options(recompui::config::create_config_tab("Camera", camera_tab_id, false));
    conker::look_aim::add_mouse_gyro_options(recompui::config::create_config_tab("Mouse & Gyro", mouse_gyro_tab_id, false));

    recomp::config::Config& graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);
    conker::texture_packs::add_options(graphics_config);

    set_control_descriptions();
    recompui::config::create_controls_tab();

    recompui::config::create_sound_tab();

    recompui::config::create_mods_tab();

    recompui::config::finalize();
}

bool conker::graphics_config_ready() {
    // Not with --headless, which makes no menus: the game still asks for these every frame, and
    // reading them threw ("Graphics config has not been created yet").
    static bool ready = false;
    if (!ready) {
        try {
            (void)recompui::config::get_graphics_config();
            ready = true;
        }
        catch (const std::exception&) {
        }
    }
    return ready;
}

bool conker::cutscene_aspect::in_4x3() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(cutscene_aspect_id);
    return static_cast<CutsceneAspect>(std::get<uint32_t>(value)) == CutsceneAspect::Original;
}

bool conker::overscan_borders::hidden() {
    if (!conker::graphics_config_ready()) {
        return true;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(overscan_borders_id);
    return static_cast<OverscanBorders>(std::get<uint32_t>(value)) == OverscanBorders::Hidden;
}

bool conker::model_detail::always_highest() {
    if (!conker::graphics_config_ready()) {
        return true;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(conker_detail_id);
    return static_cast<ConkerDetail>(std::get<uint32_t>(value)) == ConkerDetail::AlwaysHighest;
}

bool conker::fps_counter::enabled() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(show_fps_id);
    return static_cast<ShowFps>(std::get<uint32_t>(value)) == ShowFps::On;
}
