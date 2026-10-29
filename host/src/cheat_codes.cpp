// Remember Cheat Codes (General tab, issue #166): the codes entered in the game's Cheats menu, kept from one time
// the game is played to the next. Called from a hook in conker.toml and from the frame's start (widescreen.cpp).
//
// The game keeps the cheats on as bits of D_800E9D00, one for each code: its cheat menu (in func_151EE184) sets
// the bit of the code typed in once it matches one of its list, clearing the other of the two at 0x400 and 0x800
// (they can't both be on), and the game reads the bits where each cheat counts (the lives' at 0x200, for
// BOVRILBULLETHOLE: found by saving a state before and after typing it in). Nothing saves them: func_151EEFF0
// clears them as the game boots (entrypoint), and that's the only other write. So with the setting on, the bits
// the game has are kept in cheat_codes.txt beside the settings, and put back as it boots (just after it clears
// them), and the codes are on again without typing them.
//
// Off (the default, as the original game) leaves the bits alone and forgets the ones kept. Never in a netplay
// match: the game's memory has to be the same on every machine.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "recomp.h"
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "recompui/config.h"

#include "conker.hpp"

namespace {
    namespace options {
        const std::string remember = "remember_cheat_codes";
    }
    enum class Remember : uint32_t { Off, On };

    constexpr gpr game_cheats = (gpr)(int32_t)0x800E9D00; // D_800E9D00
    // The two cheats that can't both be on: the menu clears either as it sets the other.
    constexpr uint32_t exclusive_first = 0x400, exclusive_second = 0x800;

    bool options_added = false;
    // The bits kept (from cheat_codes.txt).
    std::atomic<uint32_t> kept = 0;
    bool loaded = false;

    std::filesystem::path kept_path() {
        return recomp::get_config_path() / "cheat_codes.txt";
    }

    void load() {
        if (loaded) {
            return;
        }
        loaded = true;
        std::ifstream file(kept_path());
        uint32_t bits = 0;
        if (file >> std::hex >> bits) {
            kept = bits;
        }
    }

    void write(uint32_t bits) {
        kept = bits;
        std::ofstream file(kept_path(), std::ios::trunc);
        file << std::hex << bits << '\n';
    }

    // The setting, read from whichever config has it (each time: the tabs' configs are kept in a vector, which moves
    // them as tabs are added), or nothing without it (headless, or in a netplay match).
    std::optional<Remember> setting() {
        if (!options_added || conker::netplay_profile::active()) {
            return std::nullopt;
        }
        recomp::config::Config* config = recompui::config::find_option_config(options::remember);
        if (config == nullptr) {
            return std::nullopt;
        }
        return static_cast<Remember>(std::get<uint32_t>(config->get_option_value(options::remember)));
    }
}

void conker::cheat_codes::add_options(recomp::config::Config& config) {
    static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
        {Remember::Off, "Off", "Off"},
        {Remember::On, "On", "On"},
    };
    config.add_enum_option(options::remember, "Remember Cheat Codes",
        "Keeps the codes you enter in the game's <recomp-color primary>Cheats</recomp-color> menu on from then on, "
        "every time you play, instead of typing them in again after the game is turned off. "
        "<recomp-color primary>Off</recomp-color> matches the original game, and forgets the codes kept (they stay on "
        "until the game is closed). Not in netplay matches.",
        choices, Remember::Off);
    options_added = true;
}

// func_151EEFF0 at 0x151EEFF8, just after it cleared the cheats as the game boots: the kept ones put back.
extern "C" void conker_cheat_codes_boot(uint8_t* rdram, recomp_context* ctx) {
    if (setting() != Remember::On) {
        return;
    }
    load();
    MEM_W(0, game_cheats) = (int32_t)kept.load();
}

void conker::cheat_codes::game_frame(uint8_t* rdram) {
    const std::optional<Remember> remember = setting();
    if (!remember) {
        return;
    }
    load();
    if (*remember == Remember::Off) {
        // Turned off: the codes kept are forgotten.
        if (kept != 0) {
            write(0);
        }
        return;
    }
    const uint32_t game = (uint32_t)MEM_W(0, game_cheats);
    // A code entered (or the ones on as the setting is turned on): kept with the others. A state loaded from before
    // some were entered doesn't forget them; only the two that can't both be on follow the game.
    uint32_t bits = kept | game;
    if ((game & exclusive_first) && !(game & exclusive_second)) {
        bits &= ~exclusive_second;
    }
    else if ((game & exclusive_second) && !(game & exclusive_first)) {
        bits &= ~exclusive_first;
    }
    if (bits != kept) {
        write(bits);
    }
}
