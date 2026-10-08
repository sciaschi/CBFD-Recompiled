#ifndef __CONKER_HPP__
#define __CONKER_HPP__

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"

namespace recomp::config {
    class Config;
}

struct _SDL_GameController;

namespace conker {
    // overlays.cpp
    void register_overlays();
    void register_tlb_mapped_code();
    void map_tlb_code_pages(uint8_t* rdram);
    std::vector<uint8_t> decompress_rom(std::span<const uint8_t> rom);

    // mod_api.cpp: functions the game exports to mods.
    void register_mod_exports();

    // cutscene_aspect.cpp: the Cutscene Aspect Ratio setting, full cutscenes in 4:3.
    namespace cutscene_aspect {
        // Every frame, as the game starts its display list: sets the renderer's aspect ratio for a
        // full cutscene, and back after it.
        void update(uint8_t* rdram);
        // conker_config.cpp: whether the setting is 4:3.
        bool in_4x3();
    }

    // rom_versions.cpp: the ROMs the game accepts, and the versions of them kept for the
    // launcher to switch between.
    namespace roms {
        inline constexpr uint64_t us_rom_hash = 0x23FBBA2DBCF2FD8EULL; // XXH3-64 of the US ROM (big-endian .z64)
        // The US ROM, or a ROM hack that only changes the game's assets.
        bool accept(std::span<const uint8_t> rom);
        // Keeps versions next to the runtime's stored ROM (the one in play).
        void init(const std::filesystem::path& stored_rom);
        // Keeps a newly stored ROM (from Load ROM) as a version. True if the one in play changed.
        bool update();
        size_t version_count();
        // "US Original", "US Uncensored" or "US ROM hack (<hash>)"; empty with no ROM yet.
        std::string current_name();
        // Puts the next kept version in play. False if there's no other.
        bool switch_to_next();
        // The region a ROM file's header names ("US", "European"...); empty if it can't tell.
        std::string region_of(const std::filesystem::path& rom_path);
    }

    // main.cpp: why a ROM was refused.
    const char* rom_error_text(recomp::RomValidationError error);

    // main.cpp: the device info for whichever input backend is active.
    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);

    // null_renderer.cpp
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

#if defined(CONKER_RT64)
    inline constexpr const char* program_name = "Conker's Bad Fur Day: Recompiled";
    // The data folder's name (%LOCALAPPDATA%\<id>). CONKER_TEST_PROFILE=1 gives test
    // runs their own ("ConkerRecompiledTest"), so they never touch the player's saves.
    std::u8string program_id();

    // frontend.cpp: RecompFrontend's launcher and menus (recompui) and input (recompinput).
    namespace frontend {
        // The window's size when it opens (main.cpp's --window); 1600 x 900 when 0.
        inline int window_width = 0;
        inline int window_height = 0;
        // Registers the game with the launcher and sets up the menus.
        void init(recomp::GameEntry& game);
        // The window, renderer, input and error callbacks.
        void set_callbacks(recomp::Configuration& cfg);
        void on_vi();
        ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
        // The controllers holding ports 1-4, in port order; returns how many hold one.
        int port_controllers(std::array<_SDL_GameController*, 4>& out);
    }

    // pad_mappings.cpp: C-buttons SDL maps as face buttons made the right stick (issue #28).
    namespace pad_mappings {
        // Rewrites the mapping of each such controller connected.
        void fix_all();
        // From SDL's event watch: a controller was connected (fixed at the next update).
        void on_device_added();
        // Every VI: fixes the controllers connected since the last one.
        void update();
    }

    // texture_packs.cpp: RT64 texture packs (issue #63).
    namespace texture_packs {
        // Registers the texture pack content type and .rtz files with the mod loader, and has the mod
        // installer take .htc files.
        void register_type();
        // gliden64_packs.cpp: unpacks each GLideN64 texture cache (.htc) in the mods folder not unpacked
        // yet into a pack folder, on a thread of its own, and has the runtime open it.
        void unpack_gliden64_packs();
        // gliden64_packs.cpp: on the main thread (update_gfx), shows the unpacking's progress, and once
        // it's done has the runtime open the packs.
        void update_unpacking();
        // gliden64_packs.cpp: the mod id of the pack a .htc unpacks into.
        std::string gliden64_pack_id(const std::filesystem::path& htc);
        // The Texture Packs settings tab, listing the packs in the mods folder (a .htc as the pack it
        // unpacks into).
        void add_tab();
        // Turns on the pack chosen in the settings and the others off (unless it's left to the
        // Mods menu). Needs the runtime to have opened the mods.
        void apply();
    }

    // rumble.cpp: the Rumble Pak, sent to each port's controller.
    namespace rumble {
        // Its settings (motor, style), on the General tab.
        void add_options(recomp::config::Config& config);
        // The runtime's rumble callback: the game turning a port's Rumble Pak on or off.
        void set(int port, bool on);
        // Every VI: sends each port's rumble to its controller.
        void update();
    }

    // conker_config.cpp: the settings tabs.
    void init_config();

    // look_aim.cpp: gyro and mouse in the look mode (hold R), and how each input moves the view.
    namespace look_aim {
        // Its settings, on the General tab.
        void add_options(recomp::config::Config& config);
        // Called on every input poll: queues its mouse and gyro movement for the look mode.
        void on_input_poll();
        // The Mouse: Turn the Camera setting: whether the mouse turns the third-person camera
        // (mouse_camera.cpp).
        bool mouse_turns_camera();
        // The Right Stick: Free Camera setting (issue #65), in single player: whether the right stick
        // turns the third-person camera (mouse_camera.cpp) instead of pressing the C-buttons.
        bool stick_free_camera();
        // Camera: Invert Turning, for the free camera's stick: turning (x) and tilting (y).
        void free_camera_invert(bool& x, bool& y);
        // Camera: Turning Speed, 1 at 100%.
        float camera_turn_speed();
        // Camera: Field of View: the normal camera's vertical field of view, degrees.
        float camera_field_of_view();
    }

    // conker_config.cpp: whether the Graphics tab's settings exist (not with --headless, which makes
    // no menus). Their readers fall back to the defaults without them.
    bool graphics_config_ready();

    // Graphics: Overscan Borders (conker_config.cpp; the crop is set in widescreen.cpp).
    namespace overscan_borders {
        // Hidden: the picture is zoomed so the game's black border at its sides is off the screen.
        bool hidden();
    }

    // Graphics: Conker's Detail (conker_config.cpp; the model is picked in model_detail.cpp).
    namespace model_detail {
        // Always Highest: Conker stays on his most detailed model in single player.
        bool always_highest();
    }

    // interpolation.cpp: shadows aren't drawn on their first game frame back after a gap (issue #75).
    namespace shadows {
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

    // reticle.cpp: Aiming: Reticle, a ring in the middle of the screen while aiming (issue #74).
    namespace reticle {
        // look_aim.cpp: whether the setting is on.
        bool enabled();
        // From the game thread, each frame a camera aims in the second aiming mode (conker_aim_stick,
        // $s0 the camera state): player 1's shows the reticle, unless zoomed in or in split screen.
        void aim_frame(uint8_t* rdram, gpr camera);
        // The same each frame of the look mode (conker_look_targets), with its state: only the
        // slingshot's and the throwing knives' show the reticle, not plain R-Look's.
        void look_frame(uint8_t* rdram, gpr camera, uint32_t state);
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the reticle can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides the reticle.
        void update();
    }

    // fps_counter.cpp: Show FPS, the frame rate counter.
    namespace fps_counter {
        // conker_config.cpp: whether the setting is on.
        bool enabled();
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the counter can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides the counter, and updates it.
        void update();
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

    // field_of_view.cpp: Camera: Field of View.
    namespace field_of_view {
        // From updateCullScales_1510B958's return (widescreen.cpp): with the setting widening the
        // camera's view, works the cull scales out for the view it draws.
        void adjust_cull_scales(uint8_t* rdram, uint64_t camera);
    }

    // mouse_camera.cpp: the free orbit camera, turned by the mouse and the right stick.
    namespace mouse_camera {
        // Whether last frame's camera update was the normal camera (where the C-buttons turn it: not
        // R-Look, aiming, cutscenes or other special cameras).
        bool normal_camera();
        // Whether the right stick turned the orbit last frame's camera update could run (the setting
        // on, the normal camera, not R-Look or aiming): the controller's input then leaves the right
        // stick off the C-buttons (frontend.cpp). Safe from any thread.
        bool stick_turns_camera();
    }

    // SDL sound output (audio_output.cpp).
    namespace audio {
        void queue_samples(int16_t* samples, size_t sample_count);
        size_t get_frames_remaining();
        void set_frequency(uint32_t frequency);
    }
#endif
}

#endif
