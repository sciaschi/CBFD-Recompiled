// Mouse and gyro while the harness is used (netplay matches, recordings and replays; conker::motion in
// conker.hpp). Outside it, the mouse and gyro go straight from recompinput to player 1's camera (look_aim.cpp,
// mouse_camera.cpp), on whichever thread polls the controllers, when it does: that can't be the same on
// two machines, or in a replay. Here instead each player's movement is part of their input for a controller
// read, like their pad:
//
// - This machine's: every input poll's movement (already scaled by the player's sensitivity) is added up,
//   and taken at the next controller read on the game thread, in fixed point (netplay::Motion: 1/8 pixel,
//   half recompinput's gyro unit), the part too small for it kept for the next, with the player's settings
//   as they are then. Lockstep sends it with the pad, the harness records it.
// - Every player's, at each read (the harness: the match's, the recording's, or this machine's): kept for
//   that read, and taken once by the camera hooks of that player's camera, for the aiming modes and once
//   for the orbit. The game runs one aiming update and one camera update per read for each player's
//   camera (measured in 2- and 4-player matches), so nothing is lost or doubled, and what isn't taken (not
//   aiming) is dropped at the next read.
// - Each player's Controls (packets.hpp): their settings that change what their input does, part of their
//   movement for each read, so a setting changed in a match takes effect at the same read everywhere.
// - Aiming: Swap Sticks counts a player as aiming for a little while after their camera last ran an aiming
//   mode (look_aim.cpp): here, by reads, the game's own time, rather than by the host's clock as in single player.
//
// What's kept for the read is in save states (host_state.cpp), as the hooks write what they take into game
// memory.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

#include "../conker.hpp"
#include "lockstep.hpp"
#include "net/packets.hpp"

#if defined(CONKER_RT64)
#include "recompinput/input_state.h"
#endif

namespace {
    using conker::netplay::Motion;
    constexpr int ports = conker::netplay::max_players;
    constexpr float mouse_unit = 8.0f;  // a Motion's mouse units in a pixel
    constexpr float gyro_unit = 2.0f;   // ...and gyro units in recompinput's

    // This machine's movement since the last read, from the input thread, and what was too small to send.
    std::mutex local_mutex;
    float mouse_x = 0, mouse_y = 0, gyro_x = 0, gyro_y = 0;
    int wheel = 0;

    // Every player's for the current read, and whether the hooks have taken it.
    std::array<Motion, ports> pending = [] {
        std::array<Motion, ports> none{};
        for (Motion& m : none) m.controls = conker::motion::default_controls;
        return none;
    }();
    std::array<bool, ports> aim_taken{};
    std::array<bool, ports> orbit_taken{};
    // The reads since each player's camera last ran an aiming mode (none_yet: not lately), from the game thread;
    // the input thread reads this machine's player's (look_aim.cpp's swap_sticks_now).
    constexpr uint8_t none_yet = 0xFF;
    constexpr uint8_t aim_linger_reads = 2;
    std::array<std::atomic<uint8_t>, ports> reads_since_aim = { none_yet, none_yet, none_yet, none_yet };
    // The port each of this machine's controllers plays (-1: none), kept at each read for the input thread
    // (lockstep's are the game thread's), and every port's Controls (pending's) for it too.
    std::array<std::atomic<int>, ports> local = { 0, 1, 2, 3 };
    std::array<std::atomic<uint32_t>, ports> controls_now = { conker::motion::default_controls, conker::motion::default_controls,
                                                               conker::motion::default_controls, conker::motion::default_controls };

    // `total` in fixed point, as far as an int16 goes; what's left stays in `total`.
    int16_t take_fixed(float& total, float unit) {
        if (!std::isfinite(total)) {
            total = 0;
        }
        const float units = std::clamp(std::trunc(total * unit), -32767.0f, 32767.0f);
        total -= units / unit;
        return (int16_t)units;
    }

    conker::motion::Turn turn_of(const Motion& m) {
        return { m.mouse_x / mouse_unit, m.mouse_y / mouse_unit, m.gyro_x / gyro_unit, m.gyro_y / gyro_unit, m.wheel };
    }
}

void conker::motion::on_input_poll() {
#if defined(CONKER_RT64)
    float mx = 0, my = 0, gx = 0, gy = 0;
    recompinput::get_mouse_deltas(&mx, &my);
    recompinput::get_gyro_deltas(0, &gx, &gy);
    const int notches = conker::mouse_camera::take_wheel_notches();
    std::lock_guard lock(local_mutex);
    mouse_x += mx;
    mouse_y += my;
    gyro_x += gx;
    gyro_y += gy;
    wheel += notches;
#endif
}

void conker::motion::capture(netplay::Motion& out) {
    std::lock_guard lock(local_mutex);
    out.mouse_x = take_fixed(mouse_x, mouse_unit);
    out.mouse_y = take_fixed(mouse_y, mouse_unit);
    out.gyro_x = take_fixed(gyro_x, gyro_unit);
    out.gyro_y = take_fixed(gyro_y, gyro_unit);
    out.wheel = (int8_t)std::clamp(wheel, -127, 127);
    wheel -= out.wheel;
    out.controls = local_controls();
}

void conker::motion::clear_local() {
    std::lock_guard lock(local_mutex);
    mouse_x = mouse_y = gyro_x = gyro_y = 0;
    wheel = 0;
}

void conker::motion::set_read(const netplay::Motion* motions) {
    // Alone (recording, hashing), controller n plays port n; in a match, this machine's player n's port.
    const bool match = conker::lockstep::connected();
    for (int i = 0; i < ports; i++) {
        const int port = match ? conker::lockstep::port_of_local(i) : i;
        local[i] = (port >= 0 && port < ports) ? port : -1;
    }
    for (int i = 0; i < ports; i++) {
        pending[i] = motions[i];
        controls_now[i] = motions[i].controls;
        aim_taken[i] = false;
        orbit_taken[i] = false;
        const uint8_t since = reads_since_aim[i].load();
        if (since != none_yet) {
            reads_since_aim[i] = since + 1;
        }
    }
}

void conker::motion::note_aim(int port) {
    if (port >= 0 && port < ports) {
        reads_since_aim[port] = 0;
    }
}

bool conker::motion::aimed_lately(int port) {
    return port >= 0 && port < ports && reads_since_aim[port].load() <= aim_linger_reads;
}

int conker::motion::local_port(int controller) {
    return (controller >= 0 && controller < ports) ? local[controller].load() : -1;
}

bool conker::motion::take_aim(int port, Turn& out) {
    if (port < 0 || port >= ports || aim_taken[port]) {
        return false;
    }
    aim_taken[port] = true;
    out = turn_of(pending[port]);
    return true;
}

bool conker::motion::take_orbit(int port, Turn& out) {
    if (port < 0 || port >= ports || orbit_taken[port]) {
        return false;
    }
    orbit_taken[port] = true;
    out = turn_of(pending[port]);
    return true;
}

uint32_t conker::motion::controls(int port) {
    return (port >= 0 && port < ports) ? controls_now[port].load() : default_controls;
}

// Per port: mouse x and y (low and high 16 bits), gyro x and y, then the wheel (low 8 bits), whether the
// aiming and the orbit took it (bits 8 and 9) and the reads since its camera last aimed (bits 16 to 23), then
// the Controls.
std::vector<uint32_t> conker::motion::state() {
    std::vector<uint32_t> words;
    for (int i = 0; i < ports; i++) {
        const Motion& m = pending[i];
        words.push_back((uint16_t)m.mouse_x | ((uint32_t)(uint16_t)m.mouse_y << 16));
        words.push_back((uint16_t)m.gyro_x | ((uint32_t)(uint16_t)m.gyro_y << 16));
        words.push_back((uint8_t)m.wheel | (aim_taken[i] ? 0x100u : 0) | (orbit_taken[i] ? 0x200u : 0) |
                        ((uint32_t)reads_since_aim[i].load() << 16));
        words.push_back(m.controls);
    }
    return words;
}

void conker::motion::set_state(const std::vector<uint32_t>& words) {
    for (int i = 0; i < ports; i++) {
        // States from before Swap Sticks in matches have 3 words a port, the Controls in the high 16 bits of the third.
        const size_t per_port = (words.size() == ports * 3) ? 3 : 4;
        if (words.size() < (size_t)(i + 1) * per_port) {
            pending[i] = {};
            pending[i].controls = default_controls;
            aim_taken[i] = orbit_taken[i] = false;
            reads_since_aim[i] = none_yet;
            continue;
        }
        const uint32_t* w = words.data() + i * per_port;
        const uint32_t controls = (per_port == 3) ? (w[2] >> 16) : w[3];
        // A state's Controls are checked as a packet's are (a field of view past the codes, an unknown bit).
        pending[i] = { (int16_t)(w[0] & 0xFFFF), (int16_t)(w[0] >> 16), (int16_t)(w[1] & 0xFFFF), (int16_t)(w[1] >> 16),
                       (int8_t)(w[2] & 0xFF), netplay::valid_controls(controls) ? controls : default_controls };
        aim_taken[i] = (w[2] & 0x100) != 0;
        orbit_taken[i] = (w[2] & 0x200) != 0;
        reads_since_aim[i] = (per_port == 3) ? none_yet : (uint8_t)(w[2] >> 16);
    }
    for (int i = 0; i < ports; i++) {
        controls_now[i] = pending[i].controls;
    }
}
