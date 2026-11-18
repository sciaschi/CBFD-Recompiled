// Achievements earned by how you play, not by a scene alone (issue #171): the multiplayer ones, from the result of a
// match, and the story's survival and collecting ones. local_achievements.cpp lists them with the rest and unlocks
// them (earn); this tells when.
//
// Multiplayer. Read as a match ends: func_151E53E8 (a hook at its start, conker.toml), with 0 for a match played out
// (D_8008FD80 isn't the end: a match that runs out of time goes back to the bar without it changing).
// - D_800E0AC0: a byte for each of the 4 sides (the teams, or the players in a game without teams), 1 if it won;
//   more than one is a draw, which nobody wins. D_800E0C00: each character's side. D_8008FD88: the characters, and
//   D_8008FE44: each one's player, negative for a bot (the players come first).
// - D_800E0AD0: each character's deaths (16 bits; went up by 1 at each death made in a test). The scoreboard's
//   accuracy (func_151EA15C) is hits * 100 / shots, from the game state (gGameState) + 0xC a player: hits at +0x66,
//   shots at +0x6C (16 bits each; func_151E53E8 counts the shots into it as the match ends).
// - The mode: the game state's +0x42 (Heist 0, Tank 1, Raptor 2, Deathmatch's maps 3-7, Beach 8, War 9-10,
//   Race 11-12; more_bots.cpp).
// Any player on this machine counts: every player, or in a netplay match the ones playing here.
//
// The story. Player 1's stats (0x800D2138, 0x1C bytes a player; func_15085710 changes them): the squirrel tails
// (lives) at +0x0C (its codes 2-4: set, add up to 100, take), the health at +0x0D (5-7; 6 is full: seen draining to 0
// and back to 6 as a life was lost, in a test). A life is lost when the tails go down in play. Each boss and chapter is a stretch between two scenes (their bits, as local_achievements reads
// them), and is earned by reaching its end, having reached its start in play, without losing a life between. Iron
// Squirrel and Wad of Cash are over a whole game, from a new game, so they're kept in achievement_run.txt: a save
// loaded counts as that game's if it hasn't reached anything that game didn't (an earlier save of it).
// Cheats (BOVRILBULLETHOLE's lives, D_800E9D00 & 0x200, or the cheats mod), and save states loaded, end the
// stretches and the game's run: they could undo or skip a death.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

#include "conker.hpp"
#if defined(CONKER_NETPLAY)
#include "netplay/lockstep.hpp"
#endif

namespace {
    uint8_t byte_at(const uint8_t* rdram, uint32_t address) {
        return rdram[(address - 0x80000000) ^ 3];
    }
    int16_t half_at(const uint8_t* rdram, uint32_t address) {
        return (int16_t)((byte_at(rdram, address) << 8) | byte_at(rdram, address + 1));
    }
    uint32_t word_at(const uint8_t* rdram, uint32_t address) {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address - 0x80000000), sizeof(value));
        return value;
    }

    constexpr uint32_t arena_players = 0x800BE616;   // D_800BE616
    constexpr uint32_t character_count = 0x8008FD88; // D_8008FD88
    constexpr uint32_t character_players = 0x8008FE44; // D_8008FE44
    constexpr uint32_t character_sides = 0x800E0C00; // D_800E0C00
    constexpr uint32_t side_won = 0x800E0AC0;        // D_800E0AC0
    constexpr uint32_t character_deaths = 0x800E0AD0; // D_800E0AD0
    constexpr uint32_t game_state = 0x8008FDD4;      // gGameState
    constexpr uint32_t stats = 0x800D2138;           // player 1's
    constexpr uint32_t stats_tails = 0x0C;
    constexpr uint32_t game_cheats = 0x800E9D00;     // D_800E9D00
    constexpr uint32_t cheat_lives = 0x200;
    constexpr int sides = 4;
    constexpr int most_characters = 16;
    constexpr int sharpshooter_shots = 20;
    constexpr int sharpshooter_percent = 75;
    constexpr int army_characters = 12;
    constexpr int nine_lives = 9;
    // Wad of Cash, collected over one game (provisional: to set from a measured run).
    constexpr int64_t wad_of_cash = 2000;

    bool earn(const char* id) {
        return conker::local_achievements::earn(id);
    }

    // The multiplayer modes' wins, by the game state's +0x42.
    const char* win_id(int mode) {
        if (mode == 0) return "mp_win_heist";
        if (mode == 1) return "mp_win_tank";
        if (mode == 2) return "mp_win_raptor";
        if (mode >= 3 && mode <= 7) return "mp_win_deathmatch";
        if (mode == 8) return "mp_win_beach";
        if (mode == 9 || mode == 10) return "mp_win_war";
        if (mode == 11 || mode == 12) return "mp_win_race";
        return nullptr;
    }
    bool is_team_mode(int mode) {
        return mode == 0 || mode == 2 || mode == 8 || mode == 9 || mode == 10;
    }
    bool is_deathmatch(int mode) {
        return mode >= 3 && mode <= 7;
    }

    // In a netplay match with others (not a replay or recording).
    bool online() {
#if defined(CONKER_NETPLAY)
        return conker::lockstep::connected();
#else
        return false;
#endif
    }

    // Whether player (0-3) plays on this machine: all of them, but in a netplay match, the ones its controllers play.
    bool local_player(int player) {
        if (!online()) {
            return true;
        }
        for (int controller = 0; controller < 4; controller++) {
            if (conker::motion::local_port(controller) == player) {
                return true;
            }
        }
        return false;
    }

    // How long the arena's been watched (frames): a match must be watched this long before its end counts (not a
    // save state's, loaded at its end).
    int arena_frames = 0;
    constexpr int arena_frames_needed = 300;
    // A match played out has ended (conker_match_decided), to read at the next frame.
    bool match_ended = false;

    void match_over(const uint8_t* rdram) {
        const uint32_t state = word_at(rdram, game_state);
        if (state < 0x80000000 || state >= 0x80800000) {
            return;
        }
        const int mode = (int8_t)byte_at(rdram, state + 0x42);
        const int characters = std::min<int>((int8_t)byte_at(rdram, character_count), most_characters);
        int winners = 0;
        for (int side = 0; side < sides; side++) {
            winners += byte_at(rdram, side_won + side) == 1 ? 1 : 0;
        }
        int humans = 0;
        std::printf("[achievements] match over: mode %d, %d characters, %d winning side(s)%s:", mode, characters, winners,
            online() ? ", online" : "");
        for (int i = 0; i < characters; i++) {
            const int player = (int8_t)byte_at(rdram, character_players + i);
            const int side = (int8_t)byte_at(rdram, character_sides + i);
            humans += player >= 0 ? 1 : 0;
            std::printf(" [%d: player %d side %d%s deaths %d", i, player, side,
                side >= 0 && side < sides && byte_at(rdram, side_won + side) == 1 ? " (won)" : "", half_at(rdram, character_deaths + i * 2));
            if (player >= 0 && i < 4) {
                std::printf(" hits %d shots %d", half_at(rdram, state + i * 0xC + 0x66), half_at(rdram, state + i * 0xC + 0x6C));
            }
            std::printf("]");
        }
        std::printf("\n");
        std::fflush(stdout);

        if (humans == 4) {
            earn("mp_four");
        }
        if (winners != 1) {
            return;
        }
        for (int i = 0; i < characters && i < 4; i++) {
            const int player = (int8_t)byte_at(rdram, character_players + i);
            const int side = (int8_t)byte_at(rdram, character_sides + i);
            if (player < 0 || side < 0 || side >= sides || byte_at(rdram, side_won + side) != 1 || !local_player(i)) {
                continue;
            }
            if (const char* id = win_id(mode)) {
                earn(id);
            }
            if (is_deathmatch(mode) && half_at(rdram, character_deaths + i * 2) == 0) {
                earn("mp_flawless");
            }
            const int hits = half_at(rdram, state + i * 0xC + 0x66);
            const int shots = half_at(rdram, state + i * 0xC + 0x6C);
            if (shots >= sharpshooter_shots && hits * 100 >= sharpshooter_percent * shots) {
                earn("mp_sharpshooter");
            }
            if (characters >= army_characters) {
                earn("mp_army");
            }
            if (is_team_mode(mode)) {
                for (int j = 0; j < characters; j++) {
                    if (j != i && (int8_t)byte_at(rdram, character_players + j) >= 0 && (int8_t)byte_at(rdram, character_sides + j) == side) {
                        earn("mp_team");
                    }
                }
            }
            if (online()) {
                earn("mp_online");
            }
        }
    }

    void multiplayer_frame(const uint8_t* rdram) {
        // Read the frame after it ended: func_151E53E8 counts the shots as it goes.
        if (match_ended) {
            match_ended = false;
            match_over(rdram);
        }
        arena_frames = byte_at(rdram, arena_players) == 0 ? 0 : arena_frames + 1;
    }

    // The story's stretches: from a scene to another (or to the end of the game), without losing a life.
    constexpr int game_end = -1;
    struct Stretch {
        const char* id;
        int start;
        int end;
    };
    constexpr Stretch stretches[] = {
        { "sp_haystack", 0x43, 0x44 },   // Terminator Haystack to Barn Arena Escape
        { "sp_poo", 0x7B, 0x7C },        // Poovarotti Sings to Poovarotti Flush
        { "sp_boiler", 0x87, 0x4F },     // Boiler Big Nuts Boss to Inside Safe Cash
        { "sp_batula", 0xA9, 0xAB },     // Count Batula to Zombie Fest
        { "sp_alien", 0xB6, game_end },  // Fight The Alien to the end
        // The chapters, each from its first scene to the next one's.
        { "sp_chapter", 0x3C, 0x45 },    // Barn Boys: Frankie Wants to Poke You! to Meet the Toff Catfish
        { "sp_chapter", 0x57, 0xA6 },    // Uga Buga: Rock Rollin' Cavern to Horror Intro
        { "sp_chapter", 0xA6, 0x8A },    // Spooky: Horror Intro to Beach Assault Bloodbath
        { "sp_chapter", 0x8A, 0xAF },    // It's War: Beach Assault Bloodbath to Bank Entrance
        { "sp_chapter", 0xAF, game_end }, // The Heist: Bank Entrance to the end
    };
    constexpr size_t stretch_count = std::size(stretches);
    std::array<bool, stretch_count> under_way{};

    // The game's run, from a new game: whether it's lost a life (or been cheated or rewound), the scenes it's reached,
    // and the cash collected. Kept in achievement_run.txt.
    struct Run {
        bool active = false;
        bool clean = false;
        std::vector<bool> reached;
        int64_t cash = 0;
    };
    Run run;
    std::filesystem::path run_path;

    void write_run() {
        if (run_path.empty()) {
            return;
        }
        std::ofstream file(run_path, std::ios::trunc);
        file << (run.active ? 1 : 0) << ' ' << (run.clean ? 1 : 0) << ' ' << run.cash << ' ';
        for (bool bit : run.reached) {
            file << (bit ? '1' : '0');
        }
        file << '\n';
    }

    void read_run() {
        std::ifstream file(run_path);
        int active = 0, clean = 0;
        int64_t cash = 0;
        std::string bits;
        if (!(file >> active >> clean >> cash >> bits)) {
            return;
        }
        run.active = active != 0;
        run.clean = clean != 0;
        run.cash = cash;
        run.reached.assign(bits.size(), false);
        for (size_t i = 0; i < bits.size(); i++) {
            run.reached[i] = bits[i] == '1';
        }
    }

    // A life lost, a cheat or a rewind: every stretch under way ends, and the run isn't clean.
    void spoil(const char* why) {
        bool any = false;
        for (bool& on : under_way) {
            any = any || on;
            on = false;
        }
        if (run.active && run.clean) {
            run.clean = false;
            write_run();
            any = true;
        }
        if (any) {
            std::printf("[achievements] %s: no boss, chapter or Iron Squirrel this time\n", why);
            std::fflush(stdout);
        }
    }

    int last_tails = -1;

    bool cheating(const uint8_t* rdram) {
        return (word_at(rdram, game_cheats) & cheat_lives) != 0 || recomp::mods::is_mod_enabled("conker_cheats");
    }

    void story_frame(const uint8_t* rdram, bool in_play) {
        if (byte_at(rdram, arena_players) != 0 || !in_play) {
            last_tails = -1;
            return;
        }
        if (cheating(rdram)) {
            spoil("cheats on");
        }
        const int tails = byte_at(rdram, stats + stats_tails);
        if (last_tails >= 0 && tails != last_tails) {
            std::printf("[achievements] tails %d -> %d\n", last_tails, tails);
            std::fflush(stdout);
            if (tails < last_tails) {
                spoil("a life lost");
            }
            else if (tails >= nine_lives && last_tails < nine_lives && !cheating(rdram)) {
                earn("sp_nine_lives");
            }
        }
        last_tails = tails;
    }

    void reached_end(int end) {
        for (size_t i = 0; i < stretch_count; i++) {
            if (under_way[i] && stretches[i].end == end) {
                under_way[i] = false;
                earn(stretches[i].id);
            }
        }
    }
}

void conker::achievement_play::init() {
    run_path = recomp::get_config_path() / "achievement_run.txt";
    read_run();
}

void conker::achievement_play::game_frame(uint8_t* rdram, bool in_play) {
    multiplayer_frame(rdram);
    story_frame(rdram, in_play);
}

void conker::achievement_play::scene_reached(int scene) {
    reached_end(scene);
    for (size_t i = 0; i < stretch_count; i++) {
        if (stretches[i].start == scene) {
            under_way[i] = true;
        }
    }
    if (run.active && scene >= 0 && (size_t)scene < run.reached.size() && !run.reached[scene]) {
        run.reached[scene] = true;
        write_run();
    }
}

void conker::achievement_play::save_loaded(const std::vector<bool>& bits, bool new_game) {
    for (bool& on : under_way) {
        on = false;
    }
    if (new_game) {
        run.active = true;
        run.clean = true;
        run.cash = 0;
        run.reached = bits;
        write_run();
        return;
    }
    // Another save: the run's if it's reached nothing the run hasn't (the run's save, or an earlier one of it).
    bool runs = run.active && run.reached.size() == bits.size();
    for (size_t i = 0; runs && i < bits.size(); i++) {
        runs = !bits[i] || run.reached[i];
    }
    if (!runs && run.active) {
        run.active = false;
        write_run();
    }
}

void conker::achievement_play::state_loaded() {
    last_tails = -1;
    spoil("a save state loaded");
}

void conker::achievement_play::finished_game() {
    reached_end(game_end);
    if (run.active && run.clean) {
        earn("sp_iron_squirrel");
    }
}

void conker::achievement_play::cash_added(int64_t amount) {
    if (!run.active || amount <= 0 || amount >= 1000000) {
        return;
    }
    run.cash += amount;
    write_run();
    if (run.cash >= wad_of_cash) {
        earn("sp_wad_of_cash");
    }
}

// func_151E53E8, at its first instruction (conker.toml): a multiplayer match over, played out if its argument is 0.
#include "recomp.h"

extern "C" void conker_match_decided(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r4 == 0 && byte_at(rdram, arena_players) != 0 && arena_frames >= arena_frames_needed) {
        match_ended = true;
    }
}
