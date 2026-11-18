// The netplay settings profile: while the harness records, replays or hashes (and,
// later, in netplay), the settings that change what the game does or what's in its memory are fixed,
// so that two runs on two machines, or in a window and headless, are given the same game. Each setting's
// reader checks conker::netplay_profile::active() and uses the profile's value:
//
//   Each player's own: the mouse and gyro in R-Look, the aiming modes and the orbit camera, and the
//     settings that change what they and the stick do there (responses, inverts, Mouse: Turn the Camera,
//     Camera: Field of View, Aiming: Swap Sticks): their movement is part of each read's input, with their
//     settings (their Controls) as they are at that read (motion.cpp). Swap Sticks counts a player as aiming
//     by reads, not by the time between frames. Not part of the profile.
//   Off: Right Stick: Free Camera and the mouse in the bank's leaps (look_aim.cpp, mouse_camera.cpp): single
//     player's, and the first goes by the time between frames.
//   At their defaults (as headless, which has no settings): the Camera tab's others (Field of View, Invert
//     Turning, Turning Speed, Lock-On), Conker's Detail (Always Highest), Cutscene Aspect Ratio (Expand),
//     and the sound volumes (100%, as they're set in the game's synthesizer).
//   Widescreen: the culls and sprites are widened for 16:9, whatever the window's size and the aspect
//     ratio setting (widescreen.cpp). A wider window can lose a little at its sides. While the match's rule
//     is that everyone sees only their own view, the split screen's culls are as tall and wide as it's drawn
//     then (by the match's view pairs too).
//   Save states (F5, F7): off.
//   Mods: none, in netplay matches, recordings and replays (check_mods; lockstep::begin): a mod is native
//     code the other players would have to trust, and a recording doesn't say which were on. The handshake
//     still carries mods_hash (each mod on, its version, its file and its settings), so a player with mods
//     on is refused by the host. Texture packs only change the picture, so they're left out of both and
//     can be on.
//   The renderer: RT64 renders nothing back into game memory (its Render to RAM), as headless has no
//     renderer to: see tools/netplay/README.md for what the game then reads instead.
//
// Left to the player: what only changes the picture or the sound outside the game's memory (the
// renderer's settings, Overscan Borders, Show FPS, the reticle, texture packs), and what only changes
// the pads the game is given (the controls, Z turning the shotgun's aim): the pads are what's recorded
// and what netplay sends.
//
// Its hash, of the text below, goes into recordings (and later the netplay handshake): a change to the
// profile changes the text, and a recording made with another profile is refused.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "librecomp/mods.hpp"

#include "../conker.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#if defined(CONKER_RT64)
namespace RT64 {
    // rt64.patch: renders nothing back into game memory while off.
    void setRenderToRAMOff(bool off);
}
#endif

namespace {
    bool on = false;

    constexpr const char* profile_text =
        "conker netplay profile 4\n"
        "mouse, gyro and their settings each player's own, swap sticks too, by reads; stick free camera off\n"
        "camera tab defaults; conker detail always highest; cutscene aspect expand; volumes 100\n"
        "widescreen culls 16:9; own view rule: split screen culls as tall and wide as drawn\n"
        "save states off; mods off, texture packs free\n"
        "renderer writes nothing to game memory\n";
}

bool conker::netplay_profile::active() {
    return on;
}

void conker::netplay_profile::set_active(bool active) {
    on = active;
#if defined(CONKER_RT64)
    RT64::setRenderToRAMOff(active);
#endif
}

uint64_t conker::netplay_profile::hash() {
    return XXH3_64bits(profile_text, std::char_traits<char>::length(profile_text));
}

namespace {
    // The mods on that change the game: every enabled mod but texture packs, in load order.
    std::vector<recomp::mods::ModDetails> game_mods() {
        std::vector<std::pair<size_t, recomp::mods::ModDetails>> on;
        for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("conker")) {
#if defined(CONKER_RT64)
            if (conker::texture_packs::is_pack(mod.mod_id)) {
                continue;
            }
#endif
            if (recomp::mods::is_mod_enabled(mod.mod_id)) {
                on.emplace_back(recomp::mods::get_mod_order_index(mod.mod_id), mod);
            }
        }
        std::sort(on.begin(), on.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<recomp::mods::ModDetails> mods;
        for (auto& [order, mod] : on) {
            mods.push_back(std::move(mod));
        }
        return mods;
    }

    void hash_file(XXH3_state_t& state, const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const uint64_t size = bytes.size();
        XXH3_64bits_update(&state, &size, sizeof(size));
        XXH3_64bits_update(&state, bytes.data(), bytes.size());
    }

    void hash_text(XXH3_state_t& state, const std::string& text) {
        XXH3_64bits_update(&state, text.data(), text.size() + 1);
    }
}

uint64_t conker::netplay_profile::mods_hash() {
    const std::vector<recomp::mods::ModDetails> mods = game_mods();
    if (mods.empty()) {
        return 0;
    }
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (const recomp::mods::ModDetails& mod : mods) {
        hash_text(state, mod.mod_id);
        hash_text(state, mod.version.to_string());
        // The mod itself (a file, or a folder of files), and its settings as saved.
        std::error_code error;
        const std::filesystem::path file = recomp::mods::get_mod_filename(mod.mod_id);
        if (std::filesystem::is_regular_file(file, error)) {
            hash_file(state, file);
        }
        else if (std::filesystem::is_directory(file, error)) {
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(file, error)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const std::filesystem::path& part : files) {
                hash_text(state, std::filesystem::relative(part, file, error).generic_string());
                hash_file(state, part);
            }
        }
        const std::filesystem::path settings = recomp::get_config_path() / recomp::mods::mod_config_directory / (mod.mod_id + ".json");
        if (std::filesystem::is_regular_file(settings, error)) {
            hash_file(state, settings);
        }
    }
    std::printf("[netplay] %zu mods on (texture packs aside):", mods.size());
    for (const recomp::mods::ModDetails& mod : mods) {
        std::printf(" %s %s;", mod.mod_id.c_str(), mod.version.to_string().c_str());
    }
    std::printf("\n");
    const uint64_t hash = XXH3_64bits_digest(&state);
    return hash == 0 ? 1 : hash;
}

std::string conker::netplay_profile::mods_text() {
    std::string text;
    for (const recomp::mods::ModDetails& mod : game_mods()) {
        text += (text.empty() ? "" : ", ") + mod.display_name + " " + mod.version.to_string();
    }
    return text;
}

bool conker::netplay_profile::check_mods() {
    bool ok = true;
    for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("conker")) {
#if defined(CONKER_RT64)
        if (conker::texture_packs::is_pack(mod.mod_id)) {
            continue;
        }
#endif
        if (recomp::mods::is_mod_enabled(mod.mod_id)) {
            std::fprintf(stderr, "[harness] the mod %s is on: turn it off in the Mods menu (netplay, recordings and replays allow no mods but texture packs)\n",
                mod.display_name.c_str());
            ok = false;
        }
    }
    return ok;
}
