#ifndef __CONKER_HPP__
#define __CONKER_HPP__

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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
        // Whether this frame is a cutscene drawn with the 4:3 bars (set by update).
        bool pillarboxed();
        // For save states (host_state.cpp): where the ring of its display lists is.
        uint32_t dl_ring();
        void set_dl_ring(uint32_t slot);
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

    // netplay/profile.cpp: the netplay settings profile. While it's active (the harness is
    // used), the settings that change the game or its memory are fixed: each reader checks it.
    namespace netplay_profile {
#if defined(CONKER_NETPLAY)
        bool active();
        void set_active(bool active);
        // XXH3-64 of the profile's description, for recordings.
        uint64_t hash();
        // Whether no mods but texture packs are on, for recordings and replays (false: each was printed).
        bool check_mods();
        // A hash of the mods on but texture packs, in load order: each one's id, version, file and saved
        // settings (0 for none), for netplay's handshake: everyone in a match needs the same.
        uint64_t mods_hash();
#else
        inline bool active() { return false; }
#endif
        // How much wider than 4:3 the culls and sprites are widened in it, whatever the window: 16:9's.
        inline constexpr float widescreen_ratio = (16.0f / 9.0f) / (4.0f / 3.0f);
    }

    // Mouse and gyro while the harness is used (netplay/motion.cpp): each player's movement is part of their
    // input for each controller read, taken by the hooks of their camera (look_aim.cpp, mouse_camera.cpp),
    // with their own Controls (netplay/net/packets.hpp): the settings that change what it does.
    namespace netplay {
        struct Motion;
    }
    namespace motion {
        // A read's movement for one player: the mouse in pixels (sensitivity applied), gyro in recompinput's
        // unit (sensitivity applied), the scroll wheel in notches.
        struct Turn {
            float mouse_x = 0, mouse_y = 0, gyro_x = 0, gyro_y = 0;
            int wheel = 0;
        };
        // Controls' bits.
        inline bool mouse_direct(uint32_t c) { return (c & 0x1) != 0; }
        inline bool gyro_direct(uint32_t c) { return (c & 0x2) != 0; }
        inline bool stick_direct(uint32_t c) { return (c & 0x4) != 0; }
        inline bool mouse_turns_camera(uint32_t c) { return (c & 0x8) == 0; }
        // 0 none, 1 X, 2 Y, 3 both.
        inline int mouse_invert(uint32_t c) { return (c >> 4) & 3; }
        inline int gyro_invert(uint32_t c) { return (c >> 6) & 3; }
        inline int stick_invert(uint32_t c) { return (c >> 8) & 3; }
        // Aiming: Swap Sticks.
        inline bool swap_sticks(uint32_t c) { return (c & 0x10000) != 0; }
        // Their camera's field of view in degrees (vertical), or 0 for the game's own (packets.hpp's codes).
        inline float field_of_view(uint32_t c) {
            const int code = (c >> 10) & 0x3F;
            return code == 0 ? 0.0f : (float)(34 + (code < 46 ? code : 46));
        }
        // The code for a setting in degrees (0 for none).
        inline uint32_t field_of_view_code(double degrees) {
            if (!(degrees > 0.0)) return 0;
            const double clamped = degrees < 35.0 ? 35.0 : degrees > 80.0 ? 80.0 : degrees;
            return (uint32_t)((int)(clamped + 0.5) - 34);
        }
        // Every setting's default (the stick's Invert Y), for a port with no player and recordings from before.
        constexpr uint32_t default_controls = 0x0200;
        // This machine's player's (look_aim.cpp, from their settings).
        uint32_t local_controls();
#if defined(CONKER_NETPLAY)
        // From the input thread, after each poll: adds its movement for the next read.
        void on_input_poll();
        // On the game thread, at a read: this machine's movement since the last read, with its player's
        // Controls as they are now; and dropping it.
        void capture(netplay::Motion& out);
        void clear_local();
        // At each read, every port's movement and Controls (4).
        void set_read(const netplay::Motion* motions);
        // The current read's movement for `port`, once each for the aiming modes and the orbit (false: taken).
        bool take_aim(int port, Turn& out);
        bool take_orbit(int port, Turn& out);
        // A port's Controls at the current read (the defaults for a port with no player).
        uint32_t controls(int port);
        // Aiming: Swap Sticks (look_aim.cpp): a port's camera ran an aiming mode (game thread); whether it did at
        // this read or the two before (any thread).
        void note_aim(int port);
        bool aimed_lately(int port);
        // The port this machine's controller `controller` (0 to 3) plays: its player's in a netplay match (-1 for
        // none), otherwise its own.
        int local_port(int controller);
        // For save states (host_state.cpp): the current read's movement and what took it.
        std::vector<uint32_t> state();
        void set_state(const std::vector<uint32_t>& words);
#else
        inline bool take_aim(int, Turn&) { return false; }
        inline bool take_orbit(int, Turn&) { return false; }
        inline uint32_t controls(int) { return default_controls; }
        inline void note_aim(int) {}
        inline bool aimed_lately(int) { return false; }
        inline int local_port(int controller) { return controller; }
#endif
    }

#if defined(CONKER_NETPLAY)
    // The determinism harness (netplay/harness.cpp): records and replays the game's inputs, and logs a
    // hash of its memory at each controller read.
    namespace harness {
        // A command line option of the harness's, taking its value too (true if it was one).
        bool parse_option(int argc, char** argv, int& i);
        // Checks the options and opens the files, before the game starts (false: a message was printed).
        bool prepare();
        // Recording, replaying or hashing.
        bool active();
        // The game's entrypoint: while active, it starts from an empty save.
        void entrypoint(uint8_t* rdram, recomp_context* ctx);
        // A netplay match connected from the menu (lockstep.hpp) is about to start: the harness is used
        // for it, and says the match is over with `on_match_over` (why, for the player; on the game
        // thread) instead of quitting.
        void begin_menu_match(std::function<void(const std::string&)> on_match_over);
        void start(uint8_t* rdram);
        void finish();
        bool replaying();
        // Whether every player sees only their own view of the split screen: a match's rule (the host's), a
        // replay's (its recording's), or --own-view's. It changes the game's culls (netplay/view.cpp).
        bool own_view_rule();
        // The ports whose views are shown two to a window under that rule (bit n: port n; each machine with two
        // players'): a match's (Start), a replay's, or --view-pairs'. It changes their culls too.
        uint8_t view_pairs();
        // While recording, replaying or hashing, the game sees fixed controllers (--ports, no paks).
        bool fixes_ports();
        ultramodern::input::connected_device_info_t connected_device_info(int controller_num);
    }
#endif

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
        // Rewrites the mapping of each such controller connected. From the main thread only: at start,
        // and from SDL's event watch as a controller connects (it lists SDL's devices, issue #88).
        void fix_all();
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
        // The Hotkeys (frontend.cpp): Texture Pack On/Off, switching between the texture packs and the game's own
        // textures (F8 by default).
        void toggle_originals();
        // On the General tab (conker_config.cpp): Texture Pack Notifications, the F8 line.
        void add_notification_options(recomp::config::Config& config);
        // The Texture Pack setting, at the end of the Graphics tab, listing the packs in the mods
        // folder (a .htc as the pack it unpacks into).
        void add_options(recomp::config::Config& config);
        // Turns on the pack chosen in the settings and the others off (unless it's left to the
        // Mods menu). Needs the runtime to have opened the mods.
        void apply();
        // Whether a mod (by its id) is a texture pack in the mods folder.
        bool is_pack(const std::string& mod_id);
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
        // Its settings: the camera's and the stick's aiming on the Camera tab, the mouse's and
        // gyro's on the Mouse & Gyro tab.
        void add_camera_options(recomp::config::Config& config);
        void add_mouse_gyro_options(recomp::config::Config& config);
        // Called on every input poll: queues its mouse and gyro movement for the look mode.
        void on_input_poll();
        // The Mouse: Turn the Camera setting: whether the mouse turns the third-person camera
        // (mouse_camera.cpp).
        bool mouse_turns_camera();
        // The Right Stick: Free Camera setting (issue #65), in single player: whether the right stick
        // turns the third-person camera (mouse_camera.cpp) instead of pressing the C-buttons.
        bool stick_free_camera();
        // Aiming: Swap Sticks (issue #84): whether a player (0 to 3) is aiming now (either aiming mode
        // ran for their camera in the last few frames) with the setting on, so their controller's sticks
        // are swapped (frontend.cpp): the right stick aims and the left moves. Not with recompinput's own
        // multiplayer mode (players assigned in its menus), which maps each controller itself. While the harness
        // is used, by each player's own setting (their Controls) and the reads since they aimed (motion.cpp).
        bool swap_sticks_now(int player = 0);
        // Whether the player's character is aiming the shotgun (state 0x3B), where Z (the laser sight) makes
        // C-Left and C-Right turn the aim: the swapped left stick leaves them out while Z is held (frontend.cpp).
        bool z_turns_aim(int player);
        // The bank's slow motion leaps (issue #85): from the game thread as a frame starts, whether
        // player 1's Conker is leaping; whether he was, from any thread; and, from the input thread,
        // the mouse's tilt added to player 1's stick while he is (the mouse moves the crosshair).
        void leap_frame(uint8_t* rdram);
        bool leaping_now();
        void leap_stick(float& x, float& y);
        // Aiming: Swap Sticks in a leap (issue #138): player 1's sticks swapped, the right stick moving the
        // crosshair (the game's stick) and the left the camera (the C-buttons), as when aiming elsewhere.
        bool swap_sticks_in_leap();
        // Camera: Invert Turning, for the free camera's stick: turning (x) and tilting (y).
        void free_camera_invert(bool& x, bool& y);
        // Camera: Turning Speed, 1 at 100%.
        float camera_turn_speed();
        // For save states (host_state.cpp): what each player's mouse turned of the aiming angle that was too
        // small to take yet.
        std::vector<uint32_t> state();
        void set_state(const std::vector<uint32_t>& words);
        // Camera: Field of View: the normal camera's vertical field of view, degrees.
        float camera_field_of_view();
    }

    // conker_config.cpp: whether the Graphics tab's settings exist (not with --headless, which makes
    // no menus). Their readers fall back to the defaults without them.
    bool graphics_config_ready();

    // conker_config.cpp: General's Start Game Directly (issue #133), read from the General tab's saved file
    // (main asks before the tabs exist).
    bool start_game_directly();
    // The Quit prompt's Return to Launcher (main.cpp): quits the game, then starts the program again at the
    // launcher.
    void return_to_launcher();

    // sound_volumes.cpp: the Sound tab's Music, Sound Effects and Speech volumes, and Sound Mode.
    namespace sound_volumes {
        // conker_config.cpp: the options, on the Sound tab.
        void add_options(recomp::config::Config& config);
        // From the game thread, as the game starts a frame's display list: the volumes are read from the settings.
        void game_frame(uint8_t* rdram);
        // The same, with that hook's context (for calling the game): Sound Mode kept the same as the game's own.
        void sound_mode_frame(uint8_t* rdram, recomp_context* ctx);
    }

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
        // For save states (host_state.cpp): the game frames so far and when each shadow was last drawn.
        std::vector<uint32_t> drawn();
        void set_drawn(const std::vector<uint32_t>& words);
    }

    // reticle.cpp: Aiming: Reticle, a ring, crosshair or dot in the middle of the screen while aiming (issues #74, #124).
    namespace reticle {
        // Aiming: Reticle (Ring has the key and value the setting's On had) and Aiming: Reticle Size.
        enum class Style : uint32_t { Ring, Off, Crosshair, Dot };
        enum class Size : uint32_t { Small, Medium, Large };
        // look_aim.cpp: the settings.
        Style style();
        Size size();
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

    // local_achievements.cpp: the program's own achievements, for reaching the game's scenes, kept on this computer.
    namespace local_achievements {
        // At start (frontend.cpp, once the config folder is known): reads the unlocks so far.
        void init();
        // With the other settings tabs (conker_config.cpp): the Achievements tab, and on the General tab,
        // Achievement Notifications (issue #128).
        void add_tab();
        void add_options(recomp::config::Config& config);
        // From the game thread, as the game starts a frame's display list: unlocks what the save has reached.
        void game_frame(uint8_t* rdram);
        // Save states (save_states.cpp), as one is loaded: the scenes it has are put back with the game's memory,
        // so they count for nothing, as a save's do.
        void state_loaded();
    }

#if defined(CONKER_RT64) && defined(CONKER_NETPLAY)
    // netplay/menu.cpp: the launcher's Netplay menu: host or join a match.
    namespace netplay_menu {
        // The game to start, and where its ROM is stored (to hash it before the game loads it).
        void init(const std::u8string& game_id, const std::filesystem::path& stored_rom);
        // Opens the menu (the launcher's Netplay option).
        void open();
        // On the main thread (update_gfx): says when a match is over.
        void update();
    }

    // netplay/status.cpp: the match's status on screen: waiting for a player, a player leaving, and with
    // General: Netplay Details, the input delay and each player's round trip in a corner.
    namespace netplay_status {
        // On the General tab (conker_config.cpp): Netplay Details.
        void add_options(recomp::config::Config& config);
        // From the launcher's init (frontend.cpp): recompui's UI exists now.
        void on_ui_ready();
        // On the main thread (update_gfx).
        void update();
    }

    // netplay/view.cpp: "my view only", the window showing only this player's part of the split screen.
    namespace netplay_view {
        void set_mine_only(bool on);
        bool is_mine_only();
        // Replaying a recording (to try "my view only"): the players whose views are shown (bit n: port n), as on
        // a machine with them (player 1 by default).
        void set_replay_ports(uint8_t ports);
        // At the start of each game frame's display list (widescreen.cpp's conker_frame_dl_begin).
        void game_frame(uint8_t* rdram);
    }
#endif

    // notices.cpp: a line in the top-left corner for a few seconds, one at a time (an unlock, a save state's
    // "State saved"...).
    namespace notices {
        // From any thread. With replace, it's shown at once in place of what's shown (and of an earlier one to
        // replace), as a save state's slot is as F6 is pressed again and again.
        void show(const std::string& text, bool replace = false);
        // A line saying something couldn't be done: shown whatever the notification settings are.
        void show_failure(const std::string& text);
        // On the General tab (conker_config.cpp): Notifications, all of them on or off.
        void add_options(recomp::config::Config& config);
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the line's context can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows the waiting lines, one at a time, once the game has started.
        void update();
    }

    // host_state.cpp: what the host keeps outside game memory that decides what it writes into it, so a
    // save state holds it too (save_states.cpp, the harness) and a load goes on as the state did: the
    // song marks (ultra_extras.cpp), where our display lists' rings are (widescreen.cpp,
    // cutscene_aspect.cpp), when each shadow was last drawn (interpolation.cpp), and the sound buffers
    // still playing on the timeline (audio_clock.cpp).
    namespace host_state {
        std::vector<uint8_t> save();
        // Empty, or cut short (a state from before it was held): what's missing starts afresh.
        void load(const std::vector<uint8_t>& bytes);
    }
    // Parts of it (the others are with their modules' functions above), as 32-bit words.
    // audio_clock.cpp: in deterministic mode the game's sound buffers play on the timeline, not on the
    // sound device's pace. wrap gives the callbacks to register in place of the device's.
    namespace audio_clock {
        ultramodern::audio_callbacks_t wrap(const ultramodern::audio_callbacks_t& device);
        std::vector<uint32_t> state();
        void set_state(const std::vector<uint32_t>& words);
    }
    namespace songs {
        std::vector<uint32_t> marks();
        void set_marks(const std::vector<uint32_t>& words);
    }
    namespace widescreen {
        uint32_t dl_ring();
        void set_dl_ring(uint32_t offset);
    }

    // save_states.cpp: save states (issue #94), F5 to save, F7 to load, F6 for the next slot.
    namespace save_states {
        // At start (frontend.cpp): the runtime's idle moment that states are taken at.
        void init();
        // The Hotkeys (frontend.cpp): Save State, Load State and Next Save State Slot.
        void save();
        void load();
        void next_slot();
        // On the General tab (conker_config.cpp): Save State Notifications (issue #133).
        void add_options(recomp::config::Config& config);
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
        // For save states (host_state.cpp): how much each camera's view is widened now.
        std::vector<uint32_t> state();
        void set_state(const std::vector<uint32_t>& words);
    }

    // mouse_camera.cpp: the free orbit camera, turned by the mouse and the right stick.
    namespace mouse_camera {
        // Whether last frame's camera update was the normal camera (where the C-buttons turn it: not
        // R-Look, aiming, cutscenes or other special cameras).
        bool normal_camera();
        // The same for a player's camera while the harness is used (each player's orbit).
        bool normal_camera_of(int player);
        // Whether the right stick turned the orbit last frame's camera update could run (the setting
        // on, the normal camera, not R-Look or aiming): the controller's input then leaves the right
        // stick off the C-buttons (frontend.cpp). Safe from any thread.
        bool stick_turns_camera();
        // The scroll wheel's notches since this was last called (netplay/motion.cpp, from the input thread).
        int take_wheel_notches();
        // For save states (host_state.cpp): every orbit, which writes the camera's eye.
        std::vector<uint32_t> state();
        void set_state(const std::vector<uint32_t>& words);
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
