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
// The mouse and gyro are player 1's only: in multiplayer both modes run for each player's camera. While the
// harness is used (netplay, recordings, replays: netplay/motion.cpp), each player's camera takes their own
// movement instead, one read's worth per aiming update, with their own settings (their Controls).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include "recomp.h"

#include "conker.hpp"
#include "game_trig.hpp"

#if defined(CONKER_RT64)
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
    float aim_remainder[4] = {};    // each player's (only player 1's outside the harness)

    // Aiming: Swap Sticks (issue #84): when each player's camera (by its player, +0x23D) last ran either aiming
    // mode, from the game thread (steady clock ticks), for the input thread (swap_sticks_now). Like the reticle's,
    // it counts as aiming a little over two game frames after, so a frame the game takes longer over doesn't swap
    // the sticks back for a poll. Each player's, so in split screen each controller swaps as its player aims.
    // While the harness is used, by reads instead (motion.cpp), and with each player's own setting (their
    // Controls): what the swap writes into game memory (conker_look_targets) is then the same on every machine.
    constexpr int aim_players = 4;
    std::atomic<int64_t> last_aim_ticks[aim_players] = {};
    // The aim's yaw for each player's view this frame, from the look mode (conker_look_currents), while swapped.
    float swap_view_yaw[aim_players] = {};
    bool swap_view_yaw_set[aim_players] = {};
    constexpr auto aim_linger = std::chrono::milliseconds(80);
    int camera_player(uint8_t* rdram, gpr camera) {
        const int player = (int)MEM_BU(0x23D, camera);
        return (player < aim_players) ? player : -1;
    }
    // Whether each player's character is in the shotgun's aiming state (0x3B, +0x4 of the camera's +0x3D0), where Z
    // (the laser sight) makes C-Left and C-Right turn the aim, for the input thread (z_turns_aim).
    constexpr uint8_t shotgun_aim_state = 0x3B;
    std::atomic<bool> shotgun_aiming[aim_players] = {};
    void note_aiming(uint8_t* rdram, gpr camera, int player) {
        if (player >= 0 && player < aim_players) {
            last_aim_ticks[player] = std::chrono::steady_clock::now().time_since_epoch().count();
            conker::motion::note_aim(player);
            const gpr character = (gpr)(int32_t)MEM_W(0x3D0, camera);
            shotgun_aiming[player] = (((uint32_t)character & 0xFF000000u) == 0x80000000u) && (MEM_BU(0x4, character) == shotgun_aim_state);
        }
    }

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
        const std::string aim_reticle_size = "aim_reticle_size";
        const std::string aim_swap_sticks = "aim_swap_sticks";
        const std::string aim_lock_on = "aim_lock_on";
    }

    enum class Response : uint32_t { Smooth, Direct };
    enum class Toggle : uint32_t { On, Off };
    enum class Invert : uint32_t { None, X, Y, Both };

    // The settings are on the Camera and Mouse & Gyro tabs (conker_config.cpp), read from whichever
    // config has them. There are none with --headless, which makes no menus, though the game still
    // asks for the camera's settings (its field of view every frame): reading the General tab's then
    // threw, and the headless run aborted ("General config has not been created yet").
    //
    // The netplay settings profile (netplay/profile.cpp) reads them all as headless does, but the reticle
    // (drawn over the picture, not in the game): their fallbacks are each setting's default, but the free
    // camera's, which is off. The mouse's and gyro's, and the stick's aiming, are each player's own then
    // (their Controls: stick_response and the others below; own_option reads this machine's for them).
    recomp::config::Config* option_config(const std::string& id) {
        if (conker::netplay_profile::active() && id != options::aim_reticle) {
            return nullptr;
        }
        return recompui::config::find_option_config(id);
    }

    // An enum setting, or the fallback without the settings (by default its first value, or Off for a
    // toggle).
    template <typename T>
    T option(const std::string& id, std::optional<T> fallback = std::nullopt) {
        recomp::config::Config* config = option_config(id);
        if (config == nullptr) {
            if (fallback) {
                return *fallback;
            }
            if constexpr (std::is_same_v<T, Toggle>) {
                return Toggle::Off;
            }
            return T{};
        }
        return static_cast<T>(std::get<uint32_t>(config->get_option_value(id)));
    }

    // A number setting, or the fallback without the settings.
    double number_option(const std::string& id, double fallback) {
        recomp::config::Config* config = option_config(id);
        return config ? std::get<double>(config->get_option_value(id)) : fallback;
    }

    void apply_invert(Invert invert, float& x, float& y) {
        if (invert == Invert::X || invert == Invert::Both) x = -x;
        if (invert == Invert::Y || invert == Invert::Both) y = -y;
    }

    // While the harness is used, each player's camera goes by their own input and settings (motion.cpp).
    bool per_player() {
        return conker::netplay_profile::active();
    }

    // A player's settings: their Controls while the harness is used, otherwise this machine's (player 1's).
    Response stick_response(int player) {
        return per_player() ? (conker::motion::stick_direct(conker::motion::controls(player)) ? Response::Direct : Response::Smooth)
            : option<Response>(options::stick_response);
    }
    Response mouse_response(int player) {
        return per_player() ? (conker::motion::mouse_direct(conker::motion::controls(player)) ? Response::Direct : Response::Smooth)
            : option<Response>(options::mouse_response);
    }
    Response gyro_response(int player) {
        return per_player() ? (conker::motion::gyro_direct(conker::motion::controls(player)) ? Response::Direct : Response::Smooth)
            : option<Response>(options::gyro_response);
    }
    Invert stick_invert(int player) {
        return per_player() ? (Invert)conker::motion::stick_invert(conker::motion::controls(player))
            : option<Invert>(options::stick_invert, Invert::Y);
    }
    Invert mouse_invert(int player) {
        return per_player() ? (Invert)conker::motion::mouse_invert(conker::motion::controls(player))
            : option<Invert>(options::mouse_invert);
    }
    Invert gyro_invert(int player) {
        return per_player() ? (Invert)conker::motion::gyro_invert(conker::motion::controls(player))
            : option<Invert>(options::gyro_invert);
    }

    // This machine's own setting, whatever the netplay profile reads (for its Controls).
    template <typename T>
    T own_option(const std::string& id, T fallback) {
        recomp::config::Config* config = recompui::config::find_option_config(id);
        return config ? static_cast<T>(std::get<uint32_t>(config->get_option_value(id))) : fallback;
    }
    double own_number(const std::string& id, double fallback) {
        recomp::config::Config* config = recompui::config::find_option_config(id);
        return config ? std::get<double>(config->get_option_value(id)) : fallback;
    }
#endif
}

#if defined(CONKER_RT64)
// Smooth is every input's default. Known: with Direct (or the stick's Direct, which makes every
// input direct), gyro now and then hitches turning left and right, never up and down; with Smooth
// throughout it doesn't. Not yet traced: input noise the spring hides, or something the game does
// to the yaw alone.
namespace {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    EnumOptions response = {
        {Response::Smooth, "Smooth", "Smooth"},
        {Response::Direct, "Direct", "Direct"},
    };
    EnumOptions invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    EnumOptions toggle = {
        {Toggle::On, "On", "On"},
        {Toggle::Off, "Off", "Off"},
    };
    // Aiming: Reticle was On or Off: the ring keeps On's key (and value), so a setting saved On is the ring.
    using ReticleStyle = conker::reticle::Style;
    using ReticleSize = conker::reticle::Size;
    EnumOptions reticle_styles = {
        {ReticleStyle::Off, "Off", "Off"},
        {ReticleStyle::Ring, "On", "Ring"},
        {ReticleStyle::Crosshair, "Crosshair", "Crosshair"},
        {ReticleStyle::Dot, "Dot", "Dot"},
    };
    EnumOptions reticle_sizes = {
        {ReticleSize::Small, "Small", "Small"},
        {ReticleSize::Medium, "Medium", "Medium"},
        {ReticleSize::Large, "Large", "Large"},
    };
    const std::string about =
        "<br /><recomp-color primary>Smooth</recomp-color>: the view eases toward where you aim, as in the original game."
        "<br /><recomp-color primary>Direct</recomp-color>: the view follows it exactly, with no easing.";
}

// The Camera tab: the normal camera, then aiming with the stick (R-Look and the second aiming mode) and
// the reticle. Grouped by what they're about, each group's names starting alike.
void conker::look_aim::add_camera_options(recomp::config::Config& config) {
    config.add_enum_option(options::camera_turn_invert, "Camera: Invert Turning",
        "Inverts the camera's left and right turning in single player, with the right stick or C-Left and C-Right. "
        "<recomp-color primary>None</recomp-color> matches the original game. Strafing in multiplayer isn't affected. "
        "Y inverts tilting up and down, with the right stick when Right Stick: Free Camera is on.",
        invert, Invert::None);
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
        "aiming (so the scope's zoom), cutscenes and other special cameras stay as the game has them. In a netplay match "
        "each player's own applies to their camera.",
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
        "Shows a reticle in the middle of the screen while aiming the slingshot, the throwing knives or the magnum, "
        "where what you aim at is: <recomp-color primary>Ring</recomp-color>, a green ring like Conker: Live & Reloaded's, "
        "a <recomp-color primary>Crosshair</recomp-color>, or a <recomp-color primary>Dot</recomp-color>. Not in plain "
        "R-Look, zoomed in (the sniper scope has its own crosshair) or in split screen. "
        "<recomp-color primary>Off</recomp-color> matches the original game, where you aim by eye.",
        reticle_styles, ReticleStyle::Off);
    config.add_enum_option(options::aim_reticle_size, "Aiming: Reticle Size",
        "How big the reticle is. <recomp-color primary>Medium</recomp-color> is a tenth of the screen's height.",
        reticle_sizes, ReticleSize::Medium);
    config.add_enum_option(options::aim_swap_sticks, "Aiming: Swap Sticks",
        "Controls aiming like a third-person shooter: while you aim, the right stick aims and the left stick moves, as "
        "the C-buttons do (e.g. the shotgun, the slingshot, the sniper scope, R-Look), in single player and netplay matches. "
        "Conker keeps facing where you aim and walks the way you push, sideways and back too. With the mouse, the keys "
        "bound to the stick (W, A, S, D) move. With the "
        "shotgun, holding R alone shows the laser sight, so you can move and strafe with it; holding Z as well, the left "
        "stick only moves forward and back, as Z makes C-Left and C-Right turn the aim. "
        "Back to normal once you stop aiming. <recomp-color primary>Off</recomp-color> matches the original game, where "
        "the stick aims and the C-buttons move.",
        toggle, Toggle::Off);
    config.add_enum_option(options::aim_lock_on, "Aiming: Lock-On",
        "Whether aiming locks on to enemies, as with the shotgun once the zombies come: the game turns your aim (and the "
        "camera with it) toward one, whatever the stick does. <recomp-color primary>On</recomp-color> matches the "
        "original game. Off leaves the aim to you.",
        toggle, Toggle::On);
}

// The Mouse & Gyro tab. The sensitivities are RecompFrontend's options (same ids, so saved values carry
// over), added here instead of by its General tab to sit with their group; RecompFrontend reads them from
// here (find_option_config). Each input's other settings are hidden while its sensitivity is zero, which
// turns that input off.
void conker::look_aim::add_mouse_gyro_options(recomp::config::Config& config) {
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

    static std::vector<recomp::config::ConfigValueVariant> off = { 0.0 };
    for (const std::string& id : { options::mouse_camera, options::mouse_response, options::mouse_invert }) {
        config.add_option_hidden_dependency(id, recompui::config::general::options::mouse_sensitivity, off);
    }
    for (const std::string& id : { options::gyro_response, options::gyro_invert }) {
        config.add_option_hidden_dependency(id, recompui::config::general::options::gyro_sensitivity, off);
    }
}
#endif

uint32_t conker::motion::local_controls() {
    uint32_t c = 0;
#if defined(CONKER_RT64)
    if (own_option(options::mouse_response, Response::Smooth) == Response::Direct) c |= 0x1;
    if (own_option(options::gyro_response, Response::Smooth) == Response::Direct) c |= 0x2;
    if (own_option(options::stick_response, Response::Smooth) == Response::Direct) c |= 0x4;
    if (own_option(options::mouse_camera, Toggle::On) == Toggle::Off) c |= 0x8;
    c |= (uint32_t)own_option(options::mouse_invert, Invert::None) << 4;
    c |= (uint32_t)own_option(options::gyro_invert, Invert::None) << 6;
    c |= (uint32_t)own_option(options::stick_invert, Invert::Y) << 8;
    c &= 0x03FF;    // only Controls' bits (a value out of range in the config would make the others refuse the packet)
    c |= conker::motion::field_of_view_code(own_number(options::camera_fov, 0.0)) << 10;
    if (own_option(options::aim_swap_sticks, Toggle::Off) == Toggle::On) c |= 0x10000;
#endif
    return c;
}

void conker::look_aim::on_input_poll() {
#if defined(CONKER_RT64)
    // While the harness is used, the movement is part of each read's input instead (motion.cpp): its timing
    // here depends on the host's.
    if (conker::netplay_profile::active()) {
#if defined(CONKER_NETPLAY)
        conker::motion::on_input_poll();
#endif
        return;
    }
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
conker::reticle::Style conker::reticle::style() {
    return option<Style>(options::aim_reticle);
}

conker::reticle::Size conker::reticle::size() {
    return option<Size>(options::aim_reticle_size);
}

bool conker::look_aim::mouse_turns_camera() {
    return option<Toggle>(options::mouse_camera) == Toggle::On;
}

// Aiming: Lock-On (issue #84). func_15063A38 turns the aim (+0x12 of $a0's +0x31C, an s16) by the stick's X
// each frame, and past +-9000 Conker himself. While +0x84 of that struct is set (an enemy to lock on to: the
// shotgun's zombies), it calls func_150639BC instead, which turns the aim straight toward the target found by
// func_15063390, whatever the stick does; the look mode works the camera's yaw out from the aim (state 0x3B),
// so the view swung with it. At 0x15063A80, $t1 holds that flag, about to be tested: with the setting Off it
// reads as clear, and the stick turns the aim as when there's no target.
extern "C" void conker_aim_lock_on(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (option_config(options::aim_lock_on) != nullptr && option<Toggle>(options::aim_lock_on) == Toggle::Off) {
        ctx->r9 = 0;
    }
#endif
}

// Aiming: Swap Sticks with the shotgun (issue #84): holding R alone shows the laser sight, and Conker walks with
// the gun up, as R and Z do. In his state 0x3B (aiming the shotgun, func_15065A5C's case at 0x15068BF0), Z held
// (0x15068D80) shows the laser, but also makes C-Left and C-Right turn the aim, so he can't strafe with it; R
// alone aims while he walks and strafes, without it. Found comparing the two stances (memory dumps, a tally of
// the functions run, then one at a time), with R and Z:
// - +0x8A of his +0x31C has 0x20 (a copy of the buttons' high byte): the laser's sound and the light on the gun;
// - D_800CC2B0 is set: func_15019130 runs the laser's update (func_150636F0, which works out where the beam ends)
//   only while it is (0x15019340);
// - +0x1B4 of +0x31C has 4: func_150636F0 runs the beam's update (func_151D57F8 with 0) for each player whose 4 is
//   set, then clears it (2 is the shot's, kept: set to 4 alone, the shot's mark went);
// - he walks with 0x31B (func_15065A5C, 0x15068FF8), which keeps the gun up where the laser comes from, where R
//   alone's walk is 0x224 (func_15064B94, its case for state 0x3B, 0x15065184; 0x223 below speed 20).
namespace {
    // Player 1 aiming with R and not Z, the sticks swapped (single player: D_800CC290 is theirs). Only the shotgun's
    // state (0x3B) runs these hooks and reads what's set. Not while the harness is used (netplay's multiplayer): which
    // player's character runs them isn't known here, and player 1's setting and buttons would decide for every one.
    bool shotgun_laser_on_r(uint8_t* rdram) {
        if (conker::netplay_profile::active() || !conker::look_aim::swap_sticks_now(0)) {
            return false;
        }
        const uint32_t held = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800CC290);
        return (held & 0x10) && !(held & 0x2000);
    }

    // As R and Z set them (above), for the laser on R alone.
    void show_shotgun_laser(uint8_t* rdram, gpr conker) {
        if (!shotgun_laser_on_r(rdram)) {
            return;
        }
        const gpr state = (gpr)(int32_t)MEM_W(0x31C, conker);
        MEM_B(0x8A, state) = (int8_t)(MEM_BU(0x8A, state) | 0x20);
        MEM_B(0, (gpr)(int32_t)0x800CC2B0) = 1;
        MEM_B(0x1B4, state) = (int8_t)(MEM_BU(0x1B4, state) | 4);
    }
}

// After R alone's movement in the shotgun's state (func_15063E84 returned, at 0x15069028; $s0 Conker). Also from
// the look mode (conker_look_targets): whichever runs before what draws the laser.
extern "C" void conker_shotgun_laser(uint8_t* rdram, recomp_context* ctx) {
    show_shotgun_laser(rdram, ctx->r16);
}

// R alone's walk, picked in func_15064B94 ($v1 at 0x150651FC, for the animation call at L_15065A10, its speed in
// $f14): 0x224 becomes R and Z's 0x31B. (Returned from the state's case instead, R's movement set 0x224 again each
// frame and the two restarted each other: Conker looked stuck in his walk.)
extern "C" void conker_shotgun_laser_walk(uint8_t* rdram, recomp_context* ctx) {
    if (shotgun_laser_on_r(rdram) && (uint32_t)ctx->r3 == 0x224) {
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
    const int player = camera_player(rdram, camera);
    if (!conker::look_aim::swap_sticks_now(player)) {
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
    const float aim_yaw = swap_view_yaw_set[player] ? swap_view_yaw[player] : read_float(rdram, camera, current_yaw);
    swap_view_yaw_set[player] = false;
    const float yaw = (270.0f - aim_yaw) * degrees_to_radians;
    // game_trig's: in netplay the same on every platform, as the C library's rounds some last bits differently.
    write_float(rdram, camera, 0x2E0, ex + conker::game_trig::cos(yaw) * across);
    write_float(rdram, camera, 0x2E8, ez + conker::game_trig::sin(yaw) * across);
#endif
}

bool conker::look_aim::swap_sticks_now(int player) {
    if (player < 0 || player >= aim_players) {
        return false;
    }
    if (per_player()) {
        return conker::motion::swap_sticks(conker::motion::controls(player)) && conker::motion::aimed_lately(player);
    }
    if (option<Toggle>(options::aim_swap_sticks) != Toggle::On || !recompinput::players::is_single_player_mode()) {
        return false;
    }
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks[player].load();
    return since >= 0 && std::chrono::steady_clock::duration(since) < aim_linger;
}

// Aiming: Swap Sticks with the sniper scope (issue #84): walking in any direction while scoped, zooming with the
// D-pad. Scoped, Conker's object is in state 0x80, and the scope's camera (func_15126378) zooms with C-Up and C-Down
// (+0x1A4: C-Up in, C-Down out, while held). Conker's movement (func_1504CB98) walks him by the C-buttons through a
// table of 16 directions (D_8009917C, by which of them are held, at 34 a frame and with the game's own collision),
// but only the ones a mask lets through (0x1504DD18): all four in some states, C-Left and C-Right alone while scoped,
// so he could strafe but not walk forward or back (logged: the store at 0x1504DE5C setting his moving angle to his
// facing plus 0x4000 or 0xC000). The left stick (or W, A, S, D) is the C-buttons with Swap Sticks, so with it on,
// scoped, all four go through, and the zoom takes D-pad Up and Down instead (D-pad Up in), which Controls can bind to
// anything (the mouse wheel by default). Both read only the game's own buttons and, in a netplay match, each player's
// own Swap Sticks from their Controls, so every machine does the same.
namespace {
    constexpr uint8_t scoped_state = 0x80;
    constexpr uint32_t c_buttons = 0x000F;
    constexpr uint32_t c_up = 0x0008, c_down = 0x0004, d_up = 0x0800, d_down = 0x0400;
    constexpr uint32_t game_objects = 0x800CC2D0, object_size = 0x32C;

    // A player's character: one of the first four objects, the player's port its index (-1 for any other).
    int character_player(gpr character) {
        const uint32_t object = (uint32_t)character;
        if (object < game_objects || object >= game_objects + 4 * object_size || (object - game_objects) % object_size != 0) {
            return -1;
        }
        return (int)((object - game_objects) / object_size);
    }

    // In a netplay match each player's own Swap Sticks, from their Controls (the same on every machine); otherwise
    // the setting, in single player.
    bool scope_walks(uint8_t* rdram, gpr character) {
        const int player = character_player(character);
        if (player < 0 || MEM_BU(0x4, character) != scoped_state) {
            return false;
        }
        if (per_player()) {
            return conker::motion::swap_sticks(conker::motion::controls(player));
        }
        return option<Toggle>(options::aim_swap_sticks) == Toggle::On && recompinput::players::is_single_player_mode();
    }
}

// func_1504CB98 at 0x1504DD1C: $a0 is the held buttons ($a1) through the mask ($v0), about to pick the way he walks;
// $s0 is his object. A player's character (the first four objects) that's scoped takes all four C-buttons.
extern "C" void conker_scope_walk(uint8_t* rdram, recomp_context* ctx) {
    if (scope_walks(rdram, ctx->r16)) {
        ctx->r4 = ctx->r5 & c_buttons;
    }
}

// func_15126378 at 0x15127054: $t3 is the buttons its zoom goes by (0x98($sp)), just loaded; $s0 is the camera. With
// Swap Sticks, scoped, C-Up and C-Down are walking: the zoom goes by D-pad Up and Down instead.
extern "C" void conker_scope_zoom(uint8_t* rdram, recomp_context* ctx) {
    const gpr character = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
    if (!scope_walks(rdram, character)) {
        return;
    }
    const uint32_t held = (uint32_t)ctx->r11;
    ctx->r11 = (held & ~(c_up | c_down)) | ((held & d_up) ? c_up : 0) | ((held & d_down) ? c_down : 0);
}

bool conker::look_aim::z_turns_aim(int player) {
    return (player >= 0) && (player < aim_players) && shotgun_aiming[player].load();
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
        && recompinput::players::is_single_player_mode() && !conker::netplay_profile::active();
}

bool conker::look_aim::leaping_now() {
    return leaping;
}

// Aiming: Swap Sticks in a leap (issue #138: the Matrix lobby's leaps didn't swap). The leap isn't an aiming
// mode (swap_sticks_now goes by those): in it the game's stick moves the crosshair and the C-buttons turn the
// camera, as the sticks swapped give them, so only the sticks are swapped (frontend.cpp), not what
// swap_sticks_now also does while aiming (Conker facing the view as he walks, the view following the aim).
bool conker::look_aim::swap_sticks_in_leap() {
    return leaping && option<Toggle>(options::aim_swap_sticks) == Toggle::On && recompinput::players::is_single_player_mode();
}

void conker::look_aim::leap_stick(float& x, float& y) {
    if (!leaping || conker::netplay_profile::active()) {
        leap_x = leap_y = 0.0f;
        return;
    }
    float mouse_x = 0.0f, mouse_y = 0.0f;
    recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
    // The mouse's y grows downward and the stick's upward: without inverting, the mouse up is the stick up.
    apply_invert(option<Invert>(options::mouse_invert), mouse_x, mouse_y);
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
    // None without the settings: the game's own field of view (field_of_view.cpp).
    return (float)number_option(options::camera_fov, 0.0);
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
    // setting lacks it. For the player whose camera it is ($s0 in both aiming modes).
    bool turn_stick_x(uint8_t* rdram, gpr camera) {
        const Invert invert = stick_invert(camera_player(rdram, camera));
        return invert == Invert::X || invert == Invert::Both;
    }
    bool turn_stick_y(uint8_t* rdram, gpr camera) {
        const Invert invert = stick_invert(camera_player(rdram, camera));
        return invert == Invert::None || invert == Invert::X;
    }
}
#endif

// The stick's yaw: target - stick x * scale, stored at 0x15120A14 (one state, which then goes on
// through the next one too) and at 0x15120A44 (every state). Turned around, it's a +.
extern "C" void conker_look_stick_yaw_a(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x(rdram, ctx->r16)) {
        ctx->f6.fl = ctx->f8.fl + ctx->f18.fl;  // 0x15120A10: sub.s $f6, $f8, $f18
    }
#endif
}

extern "C" void conker_look_stick_yaw_b(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x(rdram, ctx->r16)) {
        ctx->f18.fl = ctx->f4.fl + ctx->f10.fl;  // 0x15120A40: sub.s $f18, $f4, $f10
    }
#endif
}

// The stick's pitch: target + stick y * scale, stored at 0x15120A98. Turned around, it's a -.
extern "C" void conker_look_stick_pitch(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_y(rdram, ctx->r16)) {
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
// The mouse aiming's remainders, then Swap Sticks' view yaw for each player and whether it's set (bit per player):
// the look mode sets it and the view takes it, and a state can fall between them.
std::vector<uint32_t> conker::look_aim::state() {
    std::vector<uint32_t> words(9);
    std::memcpy(words.data(), aim_remainder, sizeof(aim_remainder));
    std::memcpy(words.data() + 4, swap_view_yaw, sizeof(swap_view_yaw));
    for (int i = 0; i < aim_players; i++) {
        words[8] |= swap_view_yaw_set[i] ? (1u << i) : 0u;
    }
    return words;
}

void conker::look_aim::set_state(const std::vector<uint32_t>& words) {
    std::fill(std::begin(swap_view_yaw), std::end(swap_view_yaw), 0.0f);
    std::fill(std::begin(swap_view_yaw_set), std::end(swap_view_yaw_set), false);
    if (words.size() < 4) {
        std::fill(std::begin(aim_remainder), std::end(aim_remainder), 0.0f);
        return;
    }
    std::memcpy(aim_remainder, words.data(), sizeof(aim_remainder));
    // States from before have the remainders alone.
    if (words.size() >= 9) {
        std::memcpy(swap_view_yaw, words.data() + 4, sizeof(swap_view_yaw));
        for (int i = 0; i < aim_players; i++) {
            swap_view_yaw_set[i] = (words[8] & (1u << i)) != 0;
        }
    }
}

float conker::look_aim::camera_turn_speed() {
    return (float)(number_option(options::camera_turn_speed, 100.0) / 100.0);
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
    // in multiplayer the aiming code runs for each player's camera in turn. While the harness is used,
    // each player's camera takes their own movement (motion.cpp). The player whose movement turns this
    // camera, or -1.
    int mover(uint8_t* rdram, gpr camera) {
        if (per_player()) {
            return camera_player(rdram, camera);
        }
        return MEM_BU(0x23D, camera) == 0 ? 0 : -1;
    }

    // One poll's mouse and gyro movement as a turn of the view, in degrees: the yaw grows to the left
    // and the pitch downward (see conker_look_targets). False if there was none. While the harness is
    // used, `player`'s movement for this read.
    bool take_movement(int player, float& mouse_yaw, float& mouse_pitch, float& gyro_yaw, float& gyro_pitch) {
        Movement m;
        if (per_player()) {
            conker::motion::Turn turn;
            if (!conker::motion::take_aim(player, turn)) {
                return false;
            }
            m = { turn.mouse_x, turn.mouse_y, turn.gyro_x, turn.gyro_y };
        }
        else {
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
        apply_invert(mouse_invert(player), mouse_yaw, mouse_pitch);
        apply_invert(gyro_invert(player), gyro_yaw, gyro_pitch);
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
    // Aiming: Swap Sticks: this camera's player is aiming.
    note_aiming(rdram, ctx->r16, camera_player(rdram, ctx->r16));
    if (turn_stick_x(rdram, ctx->r16)) {
        ctx->f14.fl = -ctx->f14.fl;
    }
    if (turn_stick_y(rdram, ctx->r16)) {
        ctx->f12.fl = -ctx->f12.fl;
    }

    const gpr camera = ctx->r16;
    const int player = mover(rdram, camera);
    if (player < 0) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(player, mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }
    const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + MEM_BU(0x23D, camera) * 0x180);
    constexpr float half_degrees_to_radians = 3.14159265358979f / 360.0f;
    const float fov = read_float(rdram, view, 0x74), unzoomed = read_float(rdram, view, 0x6C);
    float zoom = 1.0f;
    if (fov > 0.0f && unzoomed > 0.0f && fov < 180.0f && unzoomed < 180.0f) {
        zoom = std::clamp(conker::game_trig::tan(fov * half_degrees_to_radians) / conker::game_trig::tan(unzoomed * half_degrees_to_radians), 0.02f, 2.0f);
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
    // Aiming: Swap Sticks: this camera's player is aiming (the look mode, R-Look too: the stick aims, not moves).
    note_aiming(rdram, ctx->r16, camera_player(rdram, ctx->r16));
    if (MEM_BU(0x23D, ctx->r16) == 0) {
        show_shotgun_laser(rdram, (gpr)(int32_t)MEM_W(0x3D0, ctx->r16));
    }
#endif
    const float units_per_degree = aim_units_per_degree;
    aim_units_per_degree = 0.0f;
#if defined(CONKER_RT64)
    // Aiming: Swap Sticks moves Conker like a shooter's character (issue #84): he keeps facing where the view looks,
    // so the aim stays on the crosshair, and walks the way the left stick (or W, A, S, D) points from the view's
    // forward: forward, back, sideways or between. Here the view's yaw is his facing (+0x7A of +0x3D0) less the
    // aiming angle (+0x12 of +0x3D4, times 0.35 on the scaled path). Each frame the whole angle goes into his
    // facing (the view stays where it is), and his moving angle (+0x76), which the game walks him along, is set to
    // the way pressed. His legs play the walk forward whichever way he goes: the game has no strafe.
    // Left to the game, he turned his body toward the way he walked, the aim making up the difference, but only
    // to +-9000 units (about 49 degrees): strafing came out as a diagonal, walking back as forward, and with the
    // mouse turning the aim held at its limit on the turn's side, so he went the wrong way or ran on the spot.
    if ((units_per_degree != 0.0f) && conker::look_aim::swap_sticks_now(camera_player(rdram, ctx->r16))) {
        constexpr uint32_t c_up = 0x0008, c_down = 0x0004, c_left = 0x0002, c_right = 0x0001;
        // The buttons this camera's player holds (+0x36C points at them).
        const uint32_t held = (uint32_t)MEM_HU(0, (gpr)(int32_t)MEM_W(0x36C, ctx->r16));
        const int dx = ((held & c_right) ? 1 : 0) - ((held & c_left) ? 1 : 0);
        const int dy = ((held & c_up) ? 1 : 0) - ((held & c_down) ? 1 : 0);
        if (dx != 0 || dy != 0) {
            // The way pressed from the view's forward, in facing units: 65536 to a turn, growing to the left, as the
            // view's yaw does (45 degrees right of the view is less 8192).
            const int32_t offset = (int32_t)std::lround(std::atan2(-(double)dx, (double)dy) * 32768.0 / 3.14159265358979);
            const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, ctx->r16);
            const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
            // His facing's units to the aiming angle's: the view's yaw is his facing less the angle times this.
            const double facing_per_angle = (65536.0 / 360.0) / units_per_degree;
            const int32_t angle = (int32_t)MEM_H(0x12, aim);
            const uint16_t view = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)std::lround(angle * facing_per_angle));
            const uint16_t moving = (uint16_t)((uint32_t)view + (uint32_t)offset);
            MEM_H(0x7A, conker) = (int16_t)view;
            MEM_H(0x12, aim) = 0;
            MEM_H(0x76, conker) = (int16_t)moving;
            aim_remainder[camera_player(rdram, ctx->r16)] = 0.0f;
        }
    }
#endif
#if defined(CONKER_RT64)
    // Directions, all measured by playing: the yaw grows to the left and the pitch downward (the
    // game's stick is inverted: up looks down). The mouse's x grows to the right and its y
    // downward. recompinput's gyro gives the controller's turn on y (positive turning left) and
    // its tilt on x (positive tilting it back, toward you). Without inverting, the mouse and gyro
    // look the way they move: mouse or controller up looks up (take_movement).
    const int player = mover(rdram, ctx->r16);
    if (player < 0) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(player, mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
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
        const float units = yaw * units_per_degree + aim_remainder[player];
        const int32_t whole = (int32_t)units;
        aim_remainder[player] = units - (float)whole;
        // As the stick's turn does (func_15063A38, 0x15063AEC): past +-9000 units the aim stops there and Conker
        // turns by the rest (his facing, +0x7A, and +0x76 with it), so the view (facing less the aim) turns the
        // same and his body follows. The mouse turned the aim alone: swung fast, it went past 9000 and his body
        // stayed behind, and walking forward (the C-buttons, by his body) came out as a strafe (issue #84).
        constexpr int32_t aim_limit = 9000;
        int32_t angle = (int32_t)MEM_H(0x12, aim) - whole;
        if (angle < -aim_limit || angle > aim_limit) {
            const int32_t limit = (angle < 0) ? -aim_limit : aim_limit;
            const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
            const uint16_t facing = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)(angle - limit));
            MEM_H(0x7A, conker) = (int16_t)facing;
            MEM_H(0x76, conker) = (int16_t)facing;
            angle = limit;
        }
        MEM_H(0x12, aim) = (int16_t)angle;
    }
    frame.pitch_target_set = read_float(rdram, targets, target_pitch) + pitch;
    write_float(rdram, targets, target_pitch, frame.pitch_target_set);
    frame.targets_moved = true;
    if (mouse_response(player) == Response::Direct) {
        frame.direct_yaw += mouse_yaw;
        frame.direct_pitch += mouse_pitch;
    }
    if (gyro_response(player) == Response::Direct) {
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
    const int player = camera_player(rdram, state);
    if (conker::look_aim::swap_sticks_now(player)) {
        const float yaw = read_float(rdram, targets, target_yaw);
        write_float(rdram, state, current_yaw, yaw);
        write_float(rdram, targets, yaw_velocity, 0.0f);
        swap_view_yaw[player] = yaw;
        swap_view_yaw_set[player] = true;
    }
    if (stick_response(player) == Response::Direct) {
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
