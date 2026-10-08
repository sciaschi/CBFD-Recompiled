// Gyro and mouse aiming in the look mode (hold R, look around with the stick), and a choice of
// smooth or direct response for each of the stick, the mouse and gyro.
//
// func_15120158 runs the look mode once a frame. The stick moves a target yaw and pitch, in
// degrees ($t0 + 0x34 and + 0x38; yaw grows to the left and isn't wrapped), then the pitch target
// is clamped to limits that depend on what Conker is doing (0x15120B44 to 0x15120D28), and then a
// spring (func_15049688) eases the current angles ($s0 + 0x37C yaw, $t0 + 0x3C pitch) toward the
// targets, keeping their velocities in $t0 + 0x18 and + 0x1C. Everything the camera is built from
// is copied from the current angles after that.
//
// Hooks (conker.toml):
//   conker_look_stick_yaw_a/_b and conker_look_stick_pitch, where the stick's turn is stored into
//     the targets (0x15120A14, 0x15120A44, 0x15120A98): each can turn its axis around. The game's
//     stick has X normal and Y inverted (up looks down).
//   conker_look_targets, before the clamp: the mouse and gyro turn the targets, so the game's own
//     limits apply to them just as they do to the stick.
//   conker_look_currents, after the springs (L_15120E8C, where both of their branches meet): for
//     an input set to Direct, the current angles take its movement in the same frame instead of
//     being eased toward it.
//   conker_look_yaw_from_facing(_scaled), where some states work the yaw target out each frame from
//     Conker's facing less an aiming angle the stick turns: the mouse and gyro turn that angle too.
//   conker_aim_stick, in the second aiming mode (func_15126378: the sniper scope, the magnum,
//     throwables): the stick's invert, and the mouse and gyro, scaled by the zoom.
// The mouse and gyro are player 1's only: in multiplayer both modes run for each player's camera.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <mutex>
#include <stdexcept>
#include <type_traits>

#include "recomp.h"

#include "conker.hpp"

#if defined(CONKER_RT64)
#include <SDL.h>

#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "util/steam_deck.h"
#endif

namespace {
    // Field offsets (see above).
    constexpr int32_t target_yaw = 0x34, target_pitch = 0x38;      // $t0
    constexpr int32_t yaw_velocity = 0x18, pitch_velocity = 0x1C;  // $t0
    constexpr int32_t current_yaw = 0x37C;                         // $s0
    constexpr int32_t current_pitch = 0x3C;                        // $t0

    // Degrees per pixel of mouse movement at 100% mouse sensitivity.
    constexpr float mouse_degrees_per_pixel = 0.1f;
    // recompinput's gyro delta isn't an angle: it adds up the controller's angular velocity
    // (degrees per second) once per sensor event, without the time between events, so a frame's
    // worth depends on the controller's report rate. Tuned by feel, not derived.
    constexpr float gyro_scale = 1.0f / 60.0f;

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }

    // Each input poll's mouse and gyro movement, queued so the look mode takes one poll's worth a
    // frame. Measured in the look mode: 30 polls and 30 look updates a second, but on different
    // threads (the game's controller thread polls), so two polls sometimes land between two
    // updates and none before the next. recompinput keeps only the latest poll's movement, and
    // taking that once per update left some frames still and lost others: the view hitched.
    // Queued, nothing is lost, and a second poll waits a frame instead of doubling one. Outside
    // the look mode nothing takes from the queue, so the oldest is dropped past two, rather than
    // saved up and turned into a jump when R is pressed.
    struct Movement {
        float mouse_x, mouse_y, gyro_x, gyro_y;
    };
    std::mutex queue_mutex;
    std::deque<Movement> queue;

    // What conker_look_targets did this frame, for conker_look_currents.
    struct Frame {
        bool targets_moved = false;
        float pitch_target_set = 0.0f;  // the pitch target as conker_look_targets left it
        float direct_yaw = 0.0f;        // the part of the turn from inputs set to Direct
        float direct_pitch = 0.0f;
    } frame;

    // Set by conker_look_yaw_from_facing(_scaled) when this frame's look works its yaw out from the
    // way Conker faces less the aiming angle, for conker_look_targets: how many of the aiming angle's
    // 16-bit units turn the view a degree (0 when it doesn't). And the part of a unit not yet taken.
    float aim_units_per_degree = 0.0f;
    float aim_remainder = 0.0f;

    // Aiming: Swap Sticks (issue #84): when player 1's camera last ran either aiming mode, from the game
    // thread (steady clock ticks), for the input thread (swap_sticks_now). Like the reticle's, it counts
    // as aiming a little over two game frames after, so a frame the game takes longer over doesn't swap
    // the sticks back for a poll.
    std::atomic<int64_t> last_aim_ticks = 0;
    // The aim's yaw for the view this frame, from the look mode (conker_look_currents), while swapped.
    float swap_view_yaw = 0.0f;
    bool swap_view_yaw_set = false;
    constexpr auto aim_linger = std::chrono::milliseconds(80);
    void note_aiming() {
        last_aim_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    // TEMP-DEBUG (issue #84: aiming felt jumpy with the sticks swapped, as if it reset): with CONKER_SWAP_LOG
    // set, swap_log_<time>.txt next to the executable gets a line for every input poll of player 1
    // (frontend.cpp: swapped or not, the time since the last aiming frame, both sticks, what the N64 gets)
    // and every aiming frame of player 1's camera (which mode, its state and angles), each with the
    // milliseconds since the log started, from the input and the game thread.
    std::mutex swap_log_mutex;
    FILE* swap_log_file() {
        static FILE* file = [] {
            if (std::getenv("CONKER_SWAP_LOG") == nullptr) {
                return (FILE*)nullptr;
            }
            const std::time_t now = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof(name), "swap_log_%Y%m%d_%H%M%S.txt", std::localtime(&now));
            char* base = SDL_GetBasePath();
            const std::string path = std::string(base != nullptr ? base : "") + name;
            SDL_free(base);
            return std::fopen(path.c_str(), "w");
        }();
        return file;
    }
    const auto swap_log_start = std::chrono::steady_clock::now();
    // TEMP-DEBUG (issue #84): the address of player 1's aiming angle (+0x12 of the look state's +0x3D4),
    // from the look mode, for conker_watch_store_h (claude-scratch/scripts/watch_aim_stores.py).
    std::atomic<uint32_t> aim_angle_address = 0;

#if defined(CONKER_RT64)
    namespace options {
        const std::string stick_response = "look_stick_response";
        const std::string stick_invert = "look_stick_invert";
        const std::string mouse_response = "look_mouse_response";
        const std::string gyro_response = "look_gyro_response";
        const std::string mouse_invert = "look_mouse_invert";
        const std::string gyro_invert = "look_gyro_invert";
        const std::string camera_turn_invert = "camera_invert_turning";
        const std::string camera_turn_speed = "camera_turn_speed";
        const std::string mouse_camera = "mouse_turns_camera";
        const std::string stick_camera = "stick_free_camera";
        const std::string camera_fov = "camera_field_of_view_degrees";
        const std::string aim_reticle = "aim_reticle";
        const std::string aim_swap_sticks = "aim_swap_sticks";
        const std::string aim_lock_on = "aim_lock_on";
    }

    enum class Response : uint32_t { Smooth, Direct };
    enum class Toggle : uint32_t { On, Off };
    enum class Invert : uint32_t { None, X, Y, Both };

    // Whether the General tab's settings exist: not with --headless, which makes no menus, though
    // the game still asks for the camera's settings (its field of view every frame). Reading them
    // then threw, and the headless run aborted ("General config has not been created yet").
    bool general_ready() {
        static bool ready = false;
        if (!ready) {
            try {
                (void)recompui::config::get_general_config();
                ready = true;
            }
            catch (const std::exception&) {
            }
        }
        return ready;
    }

    // An enum setting, or its first value without the General tab (Off for a toggle).
    template <typename T>
    T option(const std::string& id) {
        if (!general_ready()) {
            if constexpr (std::is_same_v<T, Toggle>) {
                return Toggle::Off;
            }
            return T{};
        }
        return static_cast<T>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(id)));
    }

    void apply_invert(Invert invert, float& x, float& y) {
        if (invert == Invert::X || invert == Invert::Both) x = -x;
        if (invert == Invert::Y || invert == Invert::Both) y = -y;
    }
#endif
}

#if defined(CONKER_RT64)
// Smooth is every input's default. Known: with Direct (or the stick's Direct, which makes every
// input direct), gyro now and then hitches turning left and right, never up and down; with Smooth
// throughout it doesn't. Not yet traced: input noise the spring hides, or something the game does
// to the yaw alone.
void conker::look_aim::add_options(recomp::config::Config& config) {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptions response = {
        {Response::Smooth, "Smooth", "Smooth"},
        {Response::Direct, "Direct", "Direct"},
    };
    static EnumOptions invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    const std::string about =
        "<br /><recomp-color primary>Smooth</recomp-color>: the view eases toward where you aim, as in the original game."
        "<br /><recomp-color primary>Direct</recomp-color>: the view follows it exactly, with no easing.";
    static EnumOptions turn_invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    static EnumOptions toggle = {
        {Toggle::On, "On", "On"},
        {Toggle::Off, "Off", "Off"},
    };

    // Grouped by what they're about, each group's names starting alike: the camera, then aiming with
    // the stick, the mouse and gyro. The sensitivities are RecompFrontend's options (same ids, so
    // saved values carry over), added here instead of by its General tab to sit with their group.
    config.add_enum_option(options::camera_turn_invert, "Camera: Invert Turning",
        "Inverts the camera's left and right turning in single player, with the right stick or C-Left and C-Right. "
        "<recomp-color primary>None</recomp-color> matches the original game. Strafing in multiplayer isn't affected. "
        "Y inverts tilting up and down, with the right stick when Right Stick: Free Camera is on.",
        turn_invert, Invert::None);
    config.add_number_option(options::camera_turn_speed, "Camera: Turning Speed",
        "Sets how fast the camera turns left and right in single player, with the right stick or C-Left and C-Right. "
        "Strafing in multiplayer isn't affected.",
        50.0, 300.0, 5.0, 0, true, 100.0);
    config.add_enum_option(options::stick_camera, "Right Stick: Free Camera",
        "A modern third-person camera in single player: the right stick turns the camera around Conker and tilts it up "
        "and down, instead of pressing the C-buttons, and the left stick moves Conker. The C-buttons' other controls "
        "stay: by default C-Up (first person) on the right stick's click and C-Down on RB. <recomp-color primary>Off</recomp-color> matches the "
        "original game. Only the normal camera: in R-Look, aiming and cutscenes, the right stick is the C-buttons as usual.",
        toggle, Toggle::Off);
    config.add_number_option(options::camera_fov, "Camera: Field of View",
        "How wide the normal camera sees, as its vertical field of view (the same whatever the aspect ratio). "
        "<recomp-color primary>50\xC2\xB0</recomp-color> matches the original game (60.6\xC2\xB0 across at 4:3). More shows more "
        "around Conker, less brings the view in closer; the camera stays as far away. Only the normal camera: R-Look, "
        "aiming (so the scope's zoom), cutscenes and other special cameras stay as the game has them.",
        35.0, 80.0, 1.0, 0, false, 50.0);
    recompui::set_number_option_suffix(options::camera_fov, "\xC2\xB0");

    config.add_enum_option(options::stick_response, "Stick: Aiming Response",
        "How the view follows the stick in R-Look (hold R and look around)." + about +
        " This also applies to the mouse and gyro, as the view catches up with the stick at once.",
        response, Response::Smooth);
    config.add_enum_option(options::stick_invert, "Stick: Invert Aiming",
        "Inverts the stick in R-Look (hold R and look around) and in the second aiming mode (e.g. the sniper scope, the magnum, throwables), separately from the mouse and gyro. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game: pushing the stick up looks down.",
        invert, Invert::Y);
    config.add_enum_option(options::aim_reticle, "Aiming: Reticle",
        "Shows a green ring in the middle of the screen while aiming the slingshot, the throwing knives or the magnum, "
        "like Conker: Live & Reloaded's, where what you aim at is. Not in plain R-Look, zoomed in (the sniper scope has "
        "its own crosshair) or in split screen. <recomp-color primary>Off</recomp-color> matches the original game, "
        "where you aim by eye.",
        toggle, Toggle::Off);
    config.add_enum_option(options::aim_swap_sticks, "Aiming: Swap Sticks",
        "Swaps the controller's sticks while aiming in single player, like a third-person shooter: the right stick aims "
        "and the left stick moves, as the C-buttons do (e.g. the shotgun, the slingshot, the sniper scope, and R-Look). "
        "Holding Z (the shotgun's laser sight, where C-Left and C-Right turn the aim) the left stick only moves forward "
        "and back, so it doesn't turn the aim against the right stick. "
        "Back to normal once you stop aiming. <recomp-color primary>Off</recomp-color> matches the original game, where "
        "the stick aims and the C-buttons move.",
        toggle, Toggle::Off);
    config.add_enum_option(options::aim_lock_on, "Aiming: Lock-On",
        "Whether aiming locks on to enemies, as with the shotgun once the zombies come: the game turns your aim (and the "
        "camera with it) toward one, whatever the stick does. <recomp-color primary>On</recomp-color> matches the "
        "original game. Off leaves the aim to you.",
        toggle, Toggle::On);

    config.add_percent_number_option(recompui::config::general::options::mouse_sensitivity, "Mouse: Sensitivity",
        "How fast the mouse turns the camera and aims, in R-Look (hold R and look around) and the second aiming mode "
        "(e.g. the sniper scope). <b>Zero turns mouse control off</b> and leaves the cursor free. "
        "Mouse buttons can be bound to controls with the keyboard's controls.",
        recompui::is_steam_deck() ? 50.0 : 0.0);
    config.add_enum_option(options::mouse_camera, "Mouse: Turn the Camera",
        "Whether the mouse turns the camera around Conker, outside R-Look and aiming. Needs Mouse: Sensitivity above zero. "
        "<recomp-color primary>Off</recomp-color> leaves that camera to the stick and C-buttons; the mouse still aims in "
        "R-Look and the second aiming mode (e.g. the sniper scope). Gyro that Steam Input or a controller's own software "
        "sends as mouse movement counts as the mouse.",
        toggle, Toggle::On);
    config.add_enum_option(options::mouse_response, "Mouse: Aiming Response",
        "How the view follows the mouse in R-Look (hold R and look around). Needs Mouse: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::mouse_invert, "Mouse: Invert Aiming",
        "Inverts the mouse in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and gyro. With <recomp-color primary>None</recomp-color>, moving the mouse up looks up; <recomp-color primary>Invert Y</recomp-color> matches the game's stick, where up looks down.",
        invert, Invert::None);

    config.add_percent_number_option(recompui::config::general::options::gyro_sensitivity, "Gyro: Sensitivity",
        "How strongly gyro aims in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), "
        "on controllers that have it. <b>Zero turns gyro off.</b>"
        "<br /><br /><b>To recalibrate gyro, set the controller down on a still, flat surface for 5 seconds.</b>",
        25.0);
    config.add_enum_option(options::gyro_response, "Gyro: Aiming Response",
        "How the view follows gyro in R-Look (hold R and look around). Needs Gyro: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::gyro_invert, "Gyro: Invert Aiming",
        "Inverts gyro in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and the mouse. With <recomp-color primary>None</recomp-color>, the view turns the way the controller is turned.",
        invert, Invert::None);
}
#endif

void conker::look_aim::on_input_poll() {
#if defined(CONKER_RT64)
    Movement m;
    recompinput::get_mouse_deltas(&m.mouse_x, &m.mouse_y);
    recompinput::get_gyro_deltas(0, &m.gyro_x, &m.gyro_y);
    std::lock_guard lock{queue_mutex};
    queue.push_back(m);
    while (queue.size() > 2) {
        queue.pop_front();
    }
#endif
}

#if defined(CONKER_RT64)
bool conker::reticle::enabled() {
    return option<Toggle>(options::aim_reticle) == Toggle::On;
}

bool conker::look_aim::mouse_turns_camera() {
    return option<Toggle>(options::mouse_camera) == Toggle::On;
}

double conker::look_aim::since_aiming_ms() {
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks.load();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(since)).count();
}

void conker::look_aim::swap_log(const char* format, ...) {
    FILE* file = swap_log_file();
    if (file == nullptr) {
        return;
    }
    std::lock_guard lock{swap_log_mutex};
    std::fprintf(file, "%9.1f ", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swap_log_start).count());
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

// TEMP-DEBUG (issue #84): called after every halfword store to offset 0x12 in an instrumented build
// (watch_aim_stores.py): a store to player 1's aiming angle is logged with the code that made it.
extern "C" void conker_watch_store_h(uint32_t address, uint32_t value, uint32_t pc, uint32_t ra) {
    if (address == aim_angle_address.load(std::memory_order_relaxed) && address != 0) {
        conker::look_aim::swap_log("store aim 0x%04X pc 0x%08X ra 0x%08X", value & 0xFFFF, pc, ra);
    }
}

// Aiming: Lock-On (issue #84). func_15063A38 turns the aim (+0x12 of $a0's +0x31C, an s16) by the stick's X
// each frame, and past +-9000 Conker himself. While +0x84 of that struct is set (an enemy to lock on to: the
// shotgun's zombies), it calls func_150639BC instead, which turns the aim straight toward the target found by
// func_15063390, whatever the stick does; the look mode works the camera's yaw out from the aim (state 0x3B),
// so the view swung with it. At 0x15063A80, $t1 holds that flag, about to be tested: with the setting Off it
// reads as clear, and the stick turns the aim as when there's no target.
extern "C" void conker_aim_lock_on(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (general_ready() && option<Toggle>(options::aim_lock_on) == Toggle::Off) {
        ctx->r9 = 0;
    }
#endif
}

// TEMP-DEBUG (issue #84: the shotgun's laser sight while moving). In Conker's state 0x3B (aiming the shotgun,
// func_15065A5C's case at 0x15068BF0), 0x15068D80 tests Z held (D_800CC290 & 0x2000) and, if so, sets bits
// in +0x1B4 of $s0's +0x31C (|4 or |1) and D_800CC2B0: perhaps the laser. Holding Z also makes C-Left and
// C-Right turn the aim, so Conker can't strafe with it; R aims while he moves and strafes, without the
// laser. With CONKER_LASER_ON_R set, at 0x15068D84 ($t8 the held buttons, used by this test only) R counts
// as Z, so that test's bits are set while R is held and the rest of the code sees the buttons as they are.
extern "C" void conker_laser_on_r(uint8_t* rdram, recomp_context* ctx) {
    static const bool on = std::getenv("CONKER_LASER_ON_R") != nullptr;
    if (on && (ctx->r24 & 0x10) != 0) {
        conker::look_aim::swap_log("laser: R counted as Z (held 0x%04X)", (uint32_t)ctx->r24 & 0xFFFF);
        ctx->r24 |= 0x2000;
    }
}

// TEMP-DEBUG (issue #84): called after every Z test in func_15065A5C in an instrumented build
// (claude-scratch/scripts/watch_z_tests.py): logged with CONKER_SWAP_LOG, to find the one that shows the laser.
extern "C" void conker_watch_z(uint32_t pc, uint32_t result) {
    conker::look_aim::swap_log("z 0x%08X %s", pc, result != 0 ? "Z" : "-");
}

// TEMP-DEBUG (issue #84: the shotgun's laser sight with R alone). In Conker's state 0x3B, R and Z together
// (the laser) set +0x83 and +0x8A of Conker ($s0) to 8 (0x15068F70), where R alone's movement leaves them
// 0 and 0x14 (logged), and nothing outside func_15065A5C tests Z: one of them is probably what the laser
// goes by. After R alone's movement (func_15063E84, returning at 0x15069028), with CONKER_LASER_TRY set
// to 83, 8A or both, those are set as R and Z set them, while R is held without Z (D_800CC290).
// A memory dump of both stances standing still (CONKER_SWAP_DUMP) found these differ too, in the struct at
// Conker's +0x31C: +0x3A, +0x88 and +0x8A (0x20 with R and Z: copies of the buttons, Z's high byte) and
// +0x1B4 (4). CONKER_LASER_TRY=31C_3A, 31C_88, 31C_8A or 31C_1B4 sets that one as R and Z have it, here
// and in the look mode (conker_look_targets), whichever runs before what draws the laser.
// Tried one at a time (R held without Z): 31C_3A nothing; 31C_8A the laser's sound and the light on the gun,
// but no beam; 31C_88 fires; 31C_1B4 takes away the shot's mark. CONKER_LASER_TRY takes a list (commas), to
// try them together.
namespace {
    int laser_try() {
        static const int which = [] {
            const char* value = std::getenv("CONKER_LASER_TRY");
            if (value == nullptr) return 0;
            const std::string list = std::string(",") + value + ",";
            int bits = 0;
            const char* names[] = { "83", "8A", "31C_3A", "31C_88", "31C_8A", "31C_1B4", "2B0", "1B4or", "aimwalk" };
            for (int i = 0; i < 9; i++) {
                if (list.find(std::string(",") + names[i] + ",") != std::string::npos) bits |= 1 << i;
            }
            if (list.find(",both,") != std::string::npos) bits |= 3;
            return bits;
        }();
        return which;
    }

    void apply_laser_try(uint8_t* rdram, gpr conker) {
        const int which = laser_try();
        const uint32_t held = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800CC290);
        if (which == 0 || (held & 0x10) == 0 || (held & 0x2000) != 0) {
            return;
        }
        const gpr state = (gpr)(int32_t)MEM_W(0x31C, conker);
        if (which & 1) MEM_B(0x83, conker) = 8;
        if (which & 2) MEM_B(0x8A, conker) = 8;
        if (which & 4) MEM_B(0x3A, state) = (int8_t)(MEM_BU(0x3A, state) | 0x20);
        if (which & 8) MEM_B(0x88, state) = (int8_t)(MEM_BU(0x88, state) | 0x20);
        if (which & 16) MEM_B(0x8A, state) = (int8_t)(MEM_BU(0x8A, state) | 0x20);
        if (which & 32) MEM_B(0x1B4, state) = 4;
        // D_800CC2B0: func_15019130 runs the laser's update (func_150636F0, which works out where the beam
        // ends) only while it's set (0x15019340); 0x15068D80 sets it with Z held (a call tally of both
        // stances found func_150636F0's chain running only with R and Z).
        if (which & 64) MEM_B(0, (gpr)(int32_t)0x800CC2B0) = 1;
        // func_150636F0 goes by +0x1B4's bits for each player (then clears them): 4 runs the beam's update
        // (func_151D57F8 with 0), 2 the shot's (and keeps the rest), 1 func_15063628. 0x15068D80 ORs in 4 for
        // the shotgun (+0x198 == 2). Setting it to 4 (31C_1B4) wiped the shot's 2: its mark went.
        if (which & 128) MEM_B(0x1B4, state) = (int8_t)(MEM_BU(0x1B4, state) | 4);
    }
}

// TEMP-DEBUG (issue #84): which functions run in the aiming frames with R alone and with R and Z, to find
// what works out where the laser's beam ends (redrawn every frame only with R and Z; with R alone the beam
// stayed where the last shot left it). In an instrumented build (claude-scratch/scripts/count_calls.py)
// every recompiled function marks itself in call_ran as it starts; each aiming frame of player 1 adds the
// marks to the stance's tally and clears them, and every 150 such frames CONKER_SWAP_LOG gets the functions
// that ran in most of one stance's frames and few of the other's (indices into calls_index.txt).
namespace {
    constexpr int max_counted = 8192;
    uint8_t call_ran[max_counted];
    int call_tally[2][max_counted];
    int stance_frames[2];

    void tally_calls(uint8_t* rdram) {
        const uint32_t held = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800CC290);
        int stance = -1;
        if ((held & 0x2010) == 0x2010) stance = 1;
        else if ((held & 0x10) != 0) stance = 0;
        if (stance >= 0) {
            stance_frames[stance]++;
            for (int i = 0; i < max_counted; i++) {
                call_tally[stance][i] += call_ran[i];
            }
        }
        std::memset(call_ran, 0, sizeof(call_ran));
        static int frames = 0;
        if (stance >= 0 && ++frames % 150 == 0 && stance_frames[0] > 20 && stance_frames[1] > 20) {
            std::string only_rz, only_r;
            for (int i = 0; i < max_counted; i++) {
                const float rz = (float)call_tally[1][i] / stance_frames[1], r = (float)call_tally[0][i] / stance_frames[0];
                char item[32];
                std::snprintf(item, sizeof(item), " %d:%.2f/%.2f", i, r, rz);
                if (rz > 0.5f && r < 0.1f) only_rz += item;
                if (r > 0.5f && rz < 0.1f) only_r += item;
            }
            conker::look_aim::swap_log("calls frames R %d RZ %d | mostly R+Z (index:R/RZ)%s | mostly R%s",
                stance_frames[0], stance_frames[1], only_rz.c_str(), only_r.c_str());
        }
    }
}

extern "C" void conker_count_call(int index) {
    if (index >= 0 && index < max_counted) {
        call_ran[index] = 1;
    }
}

extern "C" void conker_laser_try(uint8_t* rdram, recomp_context* ctx) {
    apply_laser_try(rdram, ctx->r16);
}

// aimwalk: R alone's walk is chosen in func_15064B94 (its case for state 0x3B, 0x15065184): 0x223 below speed 20,
// 0x224 from there (0x150651F8, in $v1 for the animation call at L_15065A10, its playback speed in $f14).
// R and Z's walk is 0x31B (func_15065A5C, 0x15068FF8), which keeps the gun up where the laser comes from.
// Here, at 0x150651FC, 0x224 becomes 0x31B. (Returned from the state's case instead, R's movement set 0x224
// again each frame and the two restarted each other: Conker looked stuck in his walk.)
extern "C" void conker_laser_walk(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t held = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800CC290);
    if ((laser_try() & 256) && (held & 0x10) && !(held & 0x2000) && (uint32_t)ctx->r3 == 0x224) {
        ctx->r3 = 0x31B;
    }
}

// Aiming: Swap Sticks (issue #84): the view looks the way the aim does. func_1512C490 (building the view) has
// just copied the look-at point and the eye into the view's (+0x2E0, and +0x2EC from +0x2F8; $s0 the camera,
// before 0x1512C640). The aiming camera's eye follows Conker a little behind, while the look-at point moves with
// him at once: walking with the left stick, the line between them swung, and the view turned with the stick
// (logged: up to 2.4 degrees a frame walking back and to the side, the aim still). With the sticks swapped, the
// look-at point is turned about the eye to the aim's direction across the ground: the view's yaw (from the eye
// toward it, atan2 of z over x) is 270 degrees less the look mode's (+0x37C; measured, within 1.4 degrees
// standing still). Its distance and height stay, and so does the eye.
extern "C" void conker_look_view(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r16;
    if (MEM_BU(0x23D, camera) != 0 || !conker::look_aim::swap_sticks_now()) {
        return;
    }
    // The eye from where it's copied (+0x2F8): the copy's z is stored after this, in the branch's delay slot.
    const float ex = read_float(rdram, camera, 0x2F8), ez = read_float(rdram, camera, 0x300);
    const float dx = read_float(rdram, camera, 0x2E0) - ex, dz = read_float(rdram, camera, 0x2E8) - ez;
    const float across = std::sqrt(dx * dx + dz * dz);
    if (across < 1.0f) {
        return;
    }
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    const float aim_yaw = swap_view_yaw_set ? swap_view_yaw : read_float(rdram, camera, current_yaw);
    swap_view_yaw_set = false;
    const float yaw = (270.0f - aim_yaw) * degrees_to_radians;
    write_float(rdram, camera, 0x2E0, ex + std::cos(yaw) * across);
    write_float(rdram, camera, 0x2E8, ez + std::sin(yaw) * across);
#endif
}

bool conker::look_aim::swap_sticks_now() {
    if (!general_ready() || option<Toggle>(options::aim_swap_sticks) != Toggle::On || !recompinput::players::is_single_player_mode()) {
        return false;
    }
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks.load();
    return since >= 0 && std::chrono::steady_clock::duration(since) < aim_linger;
}

bool conker::look_aim::stick_free_camera() {
    return option<Toggle>(options::stick_camera) == Toggle::On && recompinput::players::is_single_player_mode();
}

// The bank robbery's slow motion leaps (issue #85): standing on a B pad, B leaps (func_15065A5C, 0x15066FC4: the bank,
// 0x36 in D_800BE9F0, B pressed and +0x2EC of Conker's object 1), and Conker flies across the room in slow motion
// while the stick aims a crosshair. The leap has no aiming mode or camera of its own: Conker stays in the bank's state
// (0x96, +0x4 of his object) with the leap's animation (+0x84: 0x20F, or 0x20E one way round), the follow camera stays
// as it is when walking (its +0x84 0xC0001006, logged through two leaps), and the stick turns into his movement as
// ever (a direction and a speed: +0x4C of his +0x31C, +0x44), which turns him in the air and his guns with him: the
// crosshair is where they point. It moves with the stick, comes back to the middle once it's let go, and the camera
// barely moves (logged: the camera's yaw 5 degrees in a second of the stick, as it eased back behind him anyway).
// The C-buttons turn the camera, as they do when walking: the original game's (logged: 22 degrees in a second of
// C-Right), so with Right Stick: Free Camera off the right stick turns it too.
//
// The mouse turned the camera instead (the orbit, mouse_camera.cpp, runs where the C-buttons turn it) and never moved
// the crosshair. Now, in a leap, the mouse is the stick: its movement moves a tilt, which stays where the mouse leaves
// it (so the crosshair does), added to the stick's own for player 1 (frontend.cpp, leap_stick), and the orbit doesn't
// run (mouse_camera.cpp). The tilt goes back to the middle when the leap ends.
namespace {
    constexpr uint32_t conker_object = 0x800CC2D0;  // gObjects[0]
    constexpr uint8_t bank_state = 0x96;
    constexpr uint16_t leap_animation = 0x20F, leap_animation_other_way = 0x20E;
    // Mouse movement (with Mouse: Sensitivity) for the stick's full tilt: at 100%, 300 pixels of mouse.
    constexpr float leap_full_tilt = 300.0f;

    // From the game thread (leap_frame), for the input thread.
    std::atomic<bool> leaping = false;
    // The mouse's tilt, x and y (-1 to 1, up positive), from the input thread only.
    float leap_x = 0.0f, leap_y = 0.0f;
}

void conker::look_aim::leap_frame(uint8_t* rdram) {
    const gpr conker = (gpr)(int32_t)conker_object;
    const uint16_t animation = (uint16_t)MEM_HU(0x84, conker);
    leaping = MEM_BU(0x4, conker) == bank_state && (animation == leap_animation || animation == leap_animation_other_way)
        && recompinput::players::is_single_player_mode();
}

bool conker::look_aim::leaping_now() {
    return leaping;
}

void conker::look_aim::leap_stick(float& x, float& y) {
    if (!leaping) {
        leap_x = leap_y = 0.0f;
        return;
    }
    float mouse_x = 0.0f, mouse_y = 0.0f;
    recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
    // The mouse's y grows downward and the stick's upward: without inverting, the mouse up is the stick up.
    if (general_ready()) {
        apply_invert(option<Invert>(options::mouse_invert), mouse_x, mouse_y);
    }
    leap_x += mouse_x / leap_full_tilt;
    leap_y -= mouse_y / leap_full_tilt;
    // Kept within the stick's circle, so moving the mouse back brings the crosshair back at once.
    const float length = std::sqrt(leap_x * leap_x + leap_y * leap_y);
    if (length > 1.0f) {
        leap_x /= length;
        leap_y /= length;
    }
    x += leap_x;
    y += leap_y;
}

float conker::look_aim::camera_field_of_view() {
    // None without the General tab: the game's own field of view (field_of_view.cpp).
    if (!general_ready()) {
        return 0.0f;
    }
    return (float)std::get<double>(recompui::config::get_general_config().get_option_value(options::camera_fov));
}

void conker::look_aim::free_camera_invert(bool& x, bool& y) {
    const Invert invert = option<Invert>(options::camera_turn_invert);
    x = invert == Invert::X || invert == Invert::Both;
    y = invert == Invert::Y || invert == Invert::Both;
}
#endif

#if defined(CONKER_RT64)
namespace {
    // The game's own stick is Invert Y, so X turns around when the setting has X, and Y when the
    // setting lacks it.
    bool turn_stick_x() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::X || invert == Invert::Both;
    }
    bool turn_stick_y() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::None || invert == Invert::X;
    }
}
#endif

// The stick's yaw: target - stick x * scale, stored at 0x15120A14 (one state, which then goes on
// through the next one too) and at 0x15120A44 (every state). Turned around, it's a +.
extern "C" void conker_look_stick_yaw_a(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x()) {
        ctx->f6.fl = ctx->f8.fl + ctx->f18.fl;  // 0x15120A10: sub.s $f6, $f8, $f18
    }
#endif
}

extern "C" void conker_look_stick_yaw_b(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x()) {
        ctx->f18.fl = ctx->f4.fl + ctx->f10.fl;  // 0x15120A40: sub.s $f18, $f4, $f10
    }
#endif
}

// The stick's pitch: target + stick y * scale, stored at 0x15120A98. Turned around, it's a -.
extern "C" void conker_look_stick_pitch(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_y()) {
        ctx->f10.fl = ctx->f6.fl - ctx->f8.fl;  // 0x15120A94: add.s $f10, $f6, $f8
    }
#endif
}

// Camera: Invert Turning. At 0x1512D3F0 func_1512D390 ($s0 the camera) has just stored the
// C-buttons' turn direction, 1 (C-Right) or -1 (C-Left), at + 0x6B0.
extern "C" void conker_camera_turn_invert(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const Invert invert = option<Invert>(options::camera_turn_invert);
    if (invert == Invert::X || invert == Invert::Both) {
        MEM_W(0x6B0, ctx->r16) = -MEM_W(0x6B0, ctx->r16);
    }
#endif
}

#if defined(CONKER_RT64)
float conker::look_aim::camera_turn_speed() {
    if (!general_ready()) {
        return 1.0f;
    }
    return (float)(std::get<double>(recompui::config::get_general_config().get_option_value(options::camera_turn_speed)) / 100.0);
}
#endif

// Camera: Turning Speed. The frame's turn, about to be passed to func_1508EF80: in $f18 while
// C-Left or C-Right is held (0x1512D4C4), in $f4 while the turn glides to a stop (0x1512D53C).
// Only the turn applied is scaled, not the speed the game keeps, so it eases in and out as before.
extern "C" void conker_camera_turn_speed_held(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    ctx->f18.fl *= conker::look_aim::camera_turn_speed();
#endif
}

extern "C" void conker_camera_turn_speed_released(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    ctx->f4.fl *= conker::look_aim::camera_turn_speed();
#endif
}

#if defined(CONKER_RT64)
namespace {
    // The mouse and keyboard are player 1's. The camera (struct108, the look mode's and the second
    // aiming mode's $s0) keeps its index into the cameras (D_800BE628) at + 0x23D, 0 for player 1;
    // in multiplayer the aiming code runs for each player's camera in turn.
    bool is_player_one(uint8_t* rdram, gpr camera) {
        return MEM_BU(0x23D, camera) == 0;
    }

    // One poll's mouse and gyro movement as a turn of the view, in degrees: the yaw grows to the left
    // and the pitch downward (see conker_look_targets). False if there was none.
    bool take_movement(float& mouse_yaw, float& mouse_pitch, float& gyro_yaw, float& gyro_pitch) {
        Movement m;
        {
            std::lock_guard lock{queue_mutex};
            if (queue.empty()) {
                return false;
            }
            m = queue.front();
            queue.pop_front();
        }
        mouse_yaw = -m.mouse_x * mouse_degrees_per_pixel;
        mouse_pitch = m.mouse_y * mouse_degrees_per_pixel;
        gyro_yaw = m.gyro_y * gyro_scale;
        gyro_pitch = -m.gyro_x * gyro_scale;
        apply_invert(option<Invert>(options::mouse_invert), mouse_yaw, mouse_pitch);
        apply_invert(option<Invert>(options::gyro_invert), gyro_yaw, gyro_pitch);
        return true;
    }
}
#endif

// The second aiming mode's stick (func_15126378, at 0x15126EA0; $s0 is the camera): the yaw's turn in
// $f14 is about to be subtracted and the pitch's in $f12 added, in degrees, the same directions as
// the look mode's stick (X normal, Y inverted), so the same setting turns them around. Some states
// have scaled or clamped them (symmetrically) already.
//
// The mouse and gyro turn it here too (issue #61: the sniper scope didn't follow the mouse at all).
// This mode turns the aim at once, with no spring to ease it. Zoomed in, the stick turns slower;
// the mouse and gyro turn by the same part of the view: their turn is scaled by the zoom, the
// tangent of the field of view in use (the camera's + 0x74) against the unzoomed one's (+ 0x6C),
// as func_1510B128 sets it.
extern "C" void conker_aim_stick(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // Aiming: Reticle (reticle.cpp): this camera aims this frame.
    conker::reticle::aim_frame(rdram, ctx->r16);
    // Aiming: Swap Sticks: player 1 is aiming.
    if (is_player_one(rdram, ctx->r16)) {
        note_aiming();
        conker::look_aim::swap_log("game aim2 stick yaw %.3f pitch %.3f camera yaw %.2f", ctx->f14.fl, ctx->f12.fl,
            read_float(rdram, ctx->r16, current_yaw));
    }

    if (turn_stick_x()) {
        ctx->f14.fl = -ctx->f14.fl;
    }
    if (turn_stick_y()) {
        ctx->f12.fl = -ctx->f12.fl;
    }

    const gpr camera = ctx->r16;
    if (!is_player_one(rdram, camera)) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }
    const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + MEM_BU(0x23D, camera) * 0x180);
    constexpr float half_degrees_to_radians = 3.14159265358979f / 360.0f;
    const float fov = read_float(rdram, view, 0x74), unzoomed = read_float(rdram, view, 0x6C);
    float zoom = 1.0f;
    if (fov > 0.0f && unzoomed > 0.0f && fov < 180.0f && unzoomed < 180.0f) {
        zoom = std::clamp(std::tan(fov * half_degrees_to_radians) / std::tan(unzoomed * half_degrees_to_radians), 0.02f, 2.0f);
    }
    // The yaw's turn is subtracted, the pitch's added.
    ctx->f14.fl -= (mouse_yaw + gyro_yaw) * zoom;
    ctx->f12.fl += (mouse_pitch + gyro_pitch) * zoom;
#endif
}

// At the start of the two paths where the look mode works its yaw target out each frame from the way
// Conker faces ($s0 + 0x3D0 is his object, its + 0x7A his facing) less an aiming angle (+ 0x12 of
// $s0 + 0x3D4, an s16 the stick turns: 65536 to a turn, scaled by 0.35, D_800A33B4, on the second
// path). A yaw added to the target alone was thrown away the next frame: the view sprang back.
extern "C" void conker_look_yaw_from_facing(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f;
}

extern "C" void conker_look_yaw_from_facing_scaled(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f / 0.35f;
}

// Before the pitch clamp: $s0 is the look state, $t0 the targets (reloaded at 0x15120B40).
extern "C" void conker_look_targets(uint8_t* rdram, recomp_context* ctx) {
    frame = Frame{};
#if defined(CONKER_RT64)
    // Aiming: Reticle (reticle.cpp): the look mode's state ($v0 saved at 0xA0($sp) as it starts,
    // what picks its paths) says whether it's aiming something.
    conker::reticle::look_frame(rdram, ctx->r16, (uint32_t)MEM_W(0xA0, ctx->r29));
    // Aiming: Swap Sticks: player 1 is aiming (the look mode, R-Look too: the stick aims, not moves).
    if (is_player_one(rdram, ctx->r16)) {
        note_aiming();
        aim_angle_address = (uint32_t)MEM_W(0x3D4, ctx->r16) + 0x12;
        // The view as last drawn (the camera's +0x2EC eye, +0x2E0 look-at point): its yaw and pitch, degrees, and the eye.
        {
            const float ex = read_float(rdram, ctx->r16, 0x2EC), ey = read_float(rdram, ctx->r16, 0x2F0), ez = read_float(rdram, ctx->r16, 0x2F4);
            const float dx = read_float(rdram, ctx->r16, 0x2E0) - ex, dy = read_float(rdram, ctx->r16, 0x2E4) - ey, dz = read_float(rdram, ctx->r16, 0x2E8) - ez;
            constexpr float to_degrees = 180.0f / 3.14159265358979f;
            conker::look_aim::swap_log("game view yaw %.2f pitch %.2f eye %.1f %.1f %.1f",
                std::atan2(dz, dx) * to_degrees, std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * to_degrees, ex, ey, ez);
        }
        conker::look_aim::swap_log("game look state 0x%X target yaw %.2f pitch %.2f current yaw %.2f pitch %.2f aim 0x%04X",
            (uint32_t)MEM_W(0xA0, ctx->r29), read_float(rdram, ctx->r8, target_yaw), read_float(rdram, ctx->r8, target_pitch),
            read_float(rdram, ctx->r16, current_yaw), read_float(rdram, ctx->r8, current_pitch),
            (uint32_t)(uint16_t)MEM_H(0x12, (gpr)(int32_t)MEM_W(0x3D4, ctx->r16)));
        // Conker's object (+0x3D0): his animation (+0x84), speed (+0x44) and the bytes the shotgun's state sets
        // differently with R alone and with R and Z (+0x83, +0x89, +0x8A; func_15065A5C at 0x15068F70).
        const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
        apply_laser_try(rdram, conker);
        tally_calls(rdram);
        const gpr laser_state = (gpr)(int32_t)MEM_W(0x31C, conker);
        conker::look_aim::swap_log("game conker anim 0x%X speed %.2f 83 %02X 89 %02X 8A %02X facing 0x%04X | laser 31C_8A %02X 1B4 %02X 2B0 %02X beam %.0f,%.0f,%.0f",
            (uint32_t)MEM_HU(0x84, conker), read_float(rdram, conker, 0x44), (uint32_t)MEM_BU(0x83, conker),
            (uint32_t)MEM_BU(0x89, conker), (uint32_t)MEM_BU(0x8A, conker), (uint32_t)MEM_HU(0x7A, conker),
            (uint32_t)MEM_BU(0x8A, laser_state), (uint32_t)MEM_BU(0x1B4, laser_state), (uint32_t)MEM_BU(0, (gpr)(int32_t)0x800CC2B0),
            read_float(rdram, laser_state, 0xB8), read_float(rdram, laser_state, 0xBC), read_float(rdram, laser_state, 0xC0));
        // TEMP-DEBUG (issue #84): with CONKER_SWAP_DUMP set too, Conker's object and the struct at its +0x31C,
        // 0x200 bytes each, every aiming frame: compared between R alone and R and Z to find what shows the laser.
        static const bool dump = std::getenv("CONKER_SWAP_DUMP") != nullptr;
        if (dump) {
            auto hex = [&](gpr base) {
                std::string out;
                char byte[3];
                for (int i = 0; i < 0x200; i++) {
                    std::snprintf(byte, sizeof(byte), "%02X", (uint32_t)MEM_BU(i, base));
                    out += byte;
                }
                return out;
            };
            conker::look_aim::swap_log("dump obj %s", hex(conker).c_str());
            conker::look_aim::swap_log("dump 31C %s", hex((gpr)(int32_t)MEM_W(0x31C, conker)).c_str());
        }
    }
#endif
    const float units_per_degree = aim_units_per_degree;
    aim_units_per_degree = 0.0f;
#if defined(CONKER_RT64)
    // Directions, all measured by playing: the yaw grows to the left and the pitch downward (the
    // game's stick is inverted: up looks down). The mouse's x grows to the right and its y
    // downward. recompinput's gyro gives the controller's turn on y (positive turning left) and
    // its tilt on x (positive tilting it back, toward you). Without inverting, the mouse and gyro
    // look the way they move: mouse or controller up looks up (take_movement).
    if (!is_player_one(rdram, ctx->r16)) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }

    const float yaw = mouse_yaw + gyro_yaw, pitch = mouse_pitch + gyro_pitch;
    if (yaw == 0.0f && pitch == 0.0f) {
        return;
    }
    const gpr targets = ctx->r8;
    write_float(rdram, targets, target_yaw, read_float(rdram, targets, target_yaw) + yaw);
    if (units_per_degree != 0.0f) {
        // Turn the aiming angle as far, for the next frame's target, which grows as it shrinks.
        const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, ctx->r16);
        const float units = yaw * units_per_degree + aim_remainder;
        const int32_t whole = (int32_t)units;
        aim_remainder = units - (float)whole;
        MEM_H(0x12, aim) = (int16_t)(MEM_H(0x12, aim) - whole);
    }
    frame.pitch_target_set = read_float(rdram, targets, target_pitch) + pitch;
    write_float(rdram, targets, target_pitch, frame.pitch_target_set);
    frame.targets_moved = true;
    if (option<Response>(options::mouse_response) == Response::Direct) {
        frame.direct_yaw += mouse_yaw;
        frame.direct_pitch += mouse_pitch;
    }
    if (option<Response>(options::gyro_response) == Response::Direct) {
        frame.direct_yaw += gyro_yaw;
        frame.direct_pitch += gyro_pitch;
    }
#endif
}

// After the springs: $s0 is the look state, $t0 the targets (reloaded at 0x15120E28 or 0x15120E88).
extern "C" void conker_look_currents(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr state = ctx->r16, targets = ctx->r8;
    // Aiming: Swap Sticks (issue #84): the view's yaw is the aim's. Walking back and to the side (C-Down with
    // C-Left or C-Right, which the left stick presses) swung the look mode's current yaw up to 27 degrees off its
    // target, the aim still (logged), and the spring then pulled it back: the camera turned with the left stick.
    // The current yaw is set to the target with no speed left, and the target is kept for the view
    // (conker_look_view), which is turned to it whatever moves the current yaw after this.
    if (is_player_one(rdram, state) && conker::look_aim::swap_sticks_now()) {
        const float yaw = read_float(rdram, targets, target_yaw);
        write_float(rdram, state, current_yaw, yaw);
        write_float(rdram, targets, yaw_velocity, 0.0f);
        swap_view_yaw = yaw;
        swap_view_yaw_set = true;
    }
    if (option<Response>(options::stick_response) == Response::Direct) {
        // The view is where the targets are, and the spring keeps no speed to overshoot with.
        write_float(rdram, state, current_yaw, read_float(rdram, targets, target_yaw));
        write_float(rdram, targets, current_pitch, read_float(rdram, targets, target_pitch));
        write_float(rdram, targets, yaw_velocity, 0.0f);
        write_float(rdram, targets, pitch_velocity, 0.0f);
        return;
    }
    if (!frame.targets_moved) {
        return;
    }
    write_float(rdram, state, current_yaw, read_float(rdram, state, current_yaw) + frame.direct_yaw);
    // The clamp may have held the pitch target back; the current pitch mustn't pass it either.
    const float target = read_float(rdram, targets, target_pitch);
    const float clamped = target - frame.pitch_target_set;
    float pitch = read_float(rdram, targets, current_pitch) + frame.direct_pitch;
    if ((clamped < 0.0f && pitch > target) || (clamped > 0.0f && pitch < target)) {
        pitch = target;
    }
    write_float(rdram, targets, current_pitch, pitch);
#endif
}
