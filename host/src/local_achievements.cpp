// Achievements: the program's own, earned as you play and kept on this computer (no account, no server). They're
// not RetroAchievements' (achievements.cpp, held back from the releases): the titles, descriptions and the way
// each is detected are this project's own.
//
// What they go by: the game's scenes. The game has a table of 204 of them (D_80087430, 20 bytes each: a name or
// none, a number in the low bits of the second word, and the scene the game loads in the third word's top
// byte), the names Rare gave them, such as "Burn The Bats" and "Count Batula", for its Chapters menu. Each save
// keeps a bit per scene, set as you reach it: the menu (func_1503FB40) reads the bitfield from the pointer at
// D_800D2E4C, the scene's bit at its index in the table (func_1509CA98 gives the index of the menu's nth
// scene). So an achievement here is a scene whose bit is set, and the last one is every scene the game sets
// in a finished save.
//
// Each game frame (conker_frame_dl_begin, widescreen.cpp) the bits are read, and an achievement whose scene's
// bit is set is unlocked: a line in the corner (notices.cpp) and achievements.txt in the config folder
//   <id> <unix time>
// a line per unlock. A save that already has scenes reached (loading one from before, a slot from another
// copy) sets many bits at once: more than a few in one frame unlock quietly, with one line saying how many.
// The bits are of the save in play, so unlocks are kept whichever slot they came from.
//
// The Achievements settings tab (add_tab) lists them all, unlocked or not, with the date.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_scroll_container.h"

#include "conker.hpp"

namespace {
    // The pointer to the save's bitfield of scenes reached (D_800D2E4C), and how many scenes there are.
    constexpr uint32_t scenes_reached_pointer = 0x800D2E4C;
    constexpr uint32_t scene_count = 0xCC;
    // More new bits than this in one frame: a save loaded, not scenes reached as you play.
    constexpr int quiet_threshold = 3;

    // The scene for each achievement, by its index in the game's table, with the table's name for it.
    constexpr int every_scene = -1;
    struct Achievement {
        const char* id;
        const char* title;
        const char* description;
        int scene;
    };
    constexpr Achievement achievement_list[] = {
        { "first_death", "Bad Fur Day", "Lose a life for the first time.", 0xCA },               // First Death
        { "hangover", "Hair of the Squirrel", "Wake up with a hangover and start the story.", 0xBF }, // Introduction
        { "frying_pan", "Pan Handled", "Get your training with the frying pan.", 0x05 },        // Training - Frying Pan
        { "queen_bee", "Hive Got a Problem", "Hear about the Queen Bee's missing hive.", 0x0A },  // The Queen's lost her Home!
        { "dung_beetle", "Dung Deal", "Reach the big beetle on Poo Mountain.", 0x1C },          // Poo Mountain Big Beetle
        { "windmill", "Gone With the Wind", "Blow up the windmill.", 0x2A },                     // Windmill Exploded
        { "marvin", "Cheese Please", "Feed Marvin.", 0x2D },                                     // Feed Marvin
        { "sunflower", "Petal Pusher", "Meet the sunflower.", 0x37 },                            // Sunflower Babe
        { "leap", "Look Before You Leap", "Reach the Leap of Faith.", 0x3B },                    // Leap Of Faith
        { "franky", "Fork Off", "Meet Franky the pitchfork.", 0x3C },                            // Frankie Wants to Poke You!
        { "bats", "Bat Out of Hell", "Get to burning the bats in the barn.", 0x3D },             // Burn The Bats
        { "haystack", "Hay Fever", "Face the haystack in the barn.", 0x43 },                     // Terminator Haystack
        { "barn_escape", "Bale Out", "Escape the barn after the haystack.", 0x44 },              // Barn Arena Escape
        { "catfish", "Something Fishy", "Meet the catfish.", 0x45 },                             // Meet the Toff Catfish
        { "clangs", "Clang Gang", "Make it through the Clangs' domain.", 0x50 },                 // Through The Clang's Domain
        { "bats_tower", "Belfry Bound", "Reach the Vampire Bats' tower.", 0x55 },                // The Vampire Bats Tower
        { "boiler", "Boiling Point", "Face the boiler.", 0x87 },                                 // Boiler Big Nuts Boss
        { "poo_cabin", "Cabin Fever", "Enter the poo cabin.", 0x75 },                            // Enter Poo Cabin
        { "poovarotti", "Opera Pooed", "Hear the Great Mighty Poo sing.", 0x7B },                // Poovarotti Sings
        { "flush", "Flushed Away", "Flush the Great Mighty Poo.", 0x7C },                        // Poovarotti Flush
        { "cavemen", "Rock Bottom", "Roll into the cavemen's cavern.", 0x57 },                   // Rock Rollin' Cavern
        { "worship", "False Idol", "Get the cavemen to worship you.", 0x60 },                    // Worship Me
        { "rock_solid", "On the List", "Get into the Rock Solid club.", 0x6B },                  // Rock Solid
        { "weasel", "Meet the Boss", "Meet the weasel boss.", 0x70 },                            // MEET WEASEL BOSS
        { "lava_race", "Hot Laps", "Take on the lava race.", 0x83 },                             // Ugga Lava Race
        { "spooky", "Things That Go Bump", "Arrive at the haunted graveyard.", 0xA6 },           // Horror Intro
        { "batula", "Fangs a Lot", "Meet Count Batula.", 0xA9 },                                 // Count Batula
        { "zombies", "Undead Party", "Face the zombie horde.", 0xAB },                           // Zombie Fest
        { "beach", "Beach Party", "Storm the beach.", 0x8A },                                    // Beach Assault Bloodbath
        { "rodent", "Rodent Rescue", "Save Private Rodent.", 0x94 },                             // Saved Private Rodent
        { "tank", "Tank You Very Much", "Take the tank all the way.", 0x9B },                    // A Tank Too Far
        { "war_boss", "Last Line", "Face the Tediz's last weapon.", 0xA2 },                      // War Boss
        { "bank", "Heist Society", "Arrive at the bank.", 0xAF },                                // Bank Entrance
        { "vault", "Safe Cracker", "Reach the bank vault.", 0xB2 },                              // Bank Vault
        { "alien", "Chestburster", "Fight the alien.", 0xB6 },                                   // Fight The Alien
        { "credits", "What a Day", "Finish the game.", 0xC7 },                                   // End of Game Cutscene and Credits
        { "every_scene", "Every Chapter", "Reach every scene in the Chapters menu.", every_scene },
    };
    constexpr size_t achievement_count = std::size(achievement_list);

    // The scenes "Every Chapter" needs: the table's named story scenes (not the multiplayer ones, the logo, or
    // the deaths), as a finished save has them all.
    bool counts_for_every_scene(uint32_t scene) {
        static const std::vector<bool> scenes = [] {
            std::vector<bool> named(scene_count, false);
            // The named entries of the table, by index.
            for (uint32_t index : { 0x00, 0x04, 0x05, 0x06, 0x09, 0x0A, 0x0B, 0x14, 0x19, 0x1C, 0x22, 0x23, 0x27, 0x2A, 0x2B,
                     0x2C, 0x2D, 0x36, 0x37, 0x38, 0x3B, 0x3C, 0x3D, 0x3F, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48,
                     0x4B, 0x4C, 0x4D, 0x4F, 0x50, 0x51, 0x53, 0x55, 0x57, 0x59, 0x5E, 0x60, 0x62, 0x66, 0x67, 0x68,
                     0x69, 0x6B, 0x6C, 0x6E, 0x70, 0x71, 0x72, 0x73, 0x75, 0x78, 0x7A, 0x7B, 0x7C, 0x7E, 0x82, 0x83,
                     0x84, 0x86, 0x87, 0x88, 0x8A, 0x8D, 0x8E, 0x90, 0x91, 0x92, 0x93, 0x94, 0x96, 0x97, 0x9A, 0x9B,
                     0x9C, 0x9E, 0x9F, 0xA1, 0xA2, 0xA3, 0xA4, 0xA6, 0xA7, 0xA9, 0xAB, 0xAD, 0xAF, 0xB1, 0xB2, 0xB3,
                     0xB4, 0xB5, 0xB6, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC7 }) {
                named[index] = true;
            }
            return named;
        }();
        return scene < scene_count && scenes[scene];
    }

    // The unlocks: id to when (unix time). From the game thread, read by the tab.
    std::mutex unlocks_mutex;
    std::map<std::string, int64_t> unlocks;
    std::atomic<uint32_t> list_version = 1;
    std::filesystem::path unlocks_path;

    void write_unlocks() {
        if (unlocks_path.empty()) {
            return;
        }
        std::ofstream file(unlocks_path, std::ios::trunc);
        for (const auto& [id, when] : unlocks) {
            file << id << ' ' << when << '\n';
        }
    }

    // Unlocks achievement_list[i] if it isn't yet. False if it was.
    bool unlock(size_t i) {
        std::lock_guard lock{unlocks_mutex};
        if (unlocks.contains(achievement_list[i].id)) {
            return false;
        }
        unlocks[achievement_list[i].id] = (int64_t)std::time(nullptr);
        write_unlocks();
        list_version++;
        return true;
    }

    // The scenes' bits as last read, to tell the new ones.
    std::vector<bool> last_bits(scene_count, false);
    bool have_bits = false;

    std::string date_of(int64_t when) {
        const std::time_t time = (std::time_t)when;
        char text[32] = "";
        if (const std::tm* local = std::localtime(&time)) {
            std::strftime(text, sizeof(text), "%Y-%m-%d", local);
        }
        return text;
    }

    // The settings tab: how many are unlocked on the left, the list on the right.
    class AchievementsPage : public recompui::ConfigPage {
    private:
        uint32_t shown_list = 0;
    protected:
        std::string_view get_type_name() override { return "LocalAchievementsPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::Update) {
                const uint32_t list = list_version.load();
                if (list != shown_list) {
                    shown_list = list;
                    build();
                }
                queue_update();
            }
        }

        void build() {
            recompui::ContextId context = recompui::get_current_context();
            std::map<std::string, int64_t> unlocked;
            {
                std::lock_guard lock{unlocks_mutex};
                unlocked = unlocks;
            }
            auto add_text = [&](recompui::Element* parent, const std::string& text, recompui::theme::Typography typography,
                recompui::theme::color color) {
                recompui::Label* label = context.create_element<recompui::Label>(parent, text, typography);
                if (typography == recompui::theme::Typography::Body) {
                    label->set_line_height(28.0f);
                }
                label->set_color(color);
                return label;
            };

            recompui::Element* left = body->get_left();
            left->clear_children();
            left->set_display(recompui::Display::Flex);
            left->set_flex_direction(recompui::FlexDirection::Column);
            left->set_gap(16.0f);
            add_text(left, "Achievements", recompui::theme::Typography::Header3, recompui::theme::color::Text);
            add_text(left, std::to_string(unlocked.size()) + " of " + std::to_string(achievement_count) + " unlocked",
                recompui::theme::Typography::Body, recompui::theme::color::Text);
            add_text(left, "Earned as you reach the game's scenes, the ones its Chapters menu lists. They're kept on this "
                "computer, in achievements.txt with the other settings.", recompui::theme::Typography::Body, recompui::theme::color::TextDim);
            add_text(left, "A save that's already further along unlocks what it has reached when you play it.",
                recompui::theme::Typography::Body, recompui::theme::color::TextDim);

            recompui::Element* right = body->get_right();
            right->clear_children();
            right->set_display(recompui::Display::Flex);
            right->set_flex_direction(recompui::FlexDirection::Column);
            recompui::ScrollContainer* scroll = context.create_element<recompui::ScrollContainer>(right, recompui::ScrollDirection::Vertical);
            scroll->set_as_navigation_container(recompui::NavigationType::Vertical);
            recompui::Element* list = context.create_element<recompui::Element>(scroll);
            list->set_display(recompui::Display::Flex);
            list->set_flex_direction(recompui::FlexDirection::Column);
            list->set_gap(8.0f);
            list->set_padding_right(8.0f);
            for (const Achievement& achievement : achievement_list) {
                const auto found = unlocked.find(achievement.id);
                const bool done = found != unlocked.end();
                recompui::Element* row = context.create_element<recompui::Element>(list);
                row->set_display(recompui::Display::Flex);
                row->set_flex_direction(recompui::FlexDirection::Column);
                row->set_gap(4.0f);
                row->set_padding(12.0f);
                row->set_border_radius(recompui::theme::border::radius_sm);
                row->set_border_width(recompui::theme::border::width);
                row->set_border_color(done ? recompui::theme::color::SuccessA50 : recompui::theme::color::BorderSoft);
                row->set_background_color(done ? recompui::theme::color::SuccessA5 : recompui::theme::color::BGShadow);
                row->set_focusable(true);
                row->set_tab_index_auto();
                add_text(row, achievement.title, recompui::theme::Typography::LabelMD,
                    done ? recompui::theme::color::Text : recompui::theme::color::TextDim);
                add_text(row, achievement.description, recompui::theme::Typography::Body, recompui::theme::color::TextDim);
                if (done) {
                    add_text(row, "Unlocked " + date_of(found->second), recompui::theme::Typography::Body, recompui::theme::color::Success);
                }
            }
        }
    public:
        AchievementsPage(recompui::ResourceId rid, recompui::Element* parent)
            : ConfigPage(rid, parent, recompui::Events(recompui::EventType::Update)) {
            set_as_navigation_container(recompui::NavigationType::Horizontal);
            body->get_right()->set_overflow_y(recompui::Overflow::Hidden);
            shown_list = list_version.load();
            build();
            queue_update();
        }
    };
}

void conker::local_achievements::init() {
    unlocks_path = recomp::get_config_path() / "achievements.txt";
    std::ifstream file(unlocks_path);
    std::string line;
    std::lock_guard lock{unlocks_mutex};
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string id;
        int64_t when = 0;
        if (!(fields >> id >> when)) {
            continue;
        }
        for (const Achievement& achievement : achievement_list) {
            if (id == achievement.id) {
                unlocks[id] = when;
            }
        }
    }
    list_version++;
}

void conker::local_achievements::add_tab() {
    recompui::config::create_tab("Achievements", "achievements",
        [](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<AchievementsPage>(parent);
        });
}

void conker::local_achievements::game_frame(uint8_t* rdram) {
    // The pointer, as the game holds its words; the bits, as its bytes.
    uint32_t pointer = 0;
    std::memcpy(&pointer, rdram + (scenes_reached_pointer - 0x80000000), sizeof(pointer));
    if (pointer < 0x80000000 || pointer + scene_count / 8 + 1 > 0x80800000) {
        return;
    }
    std::vector<bool> bits(scene_count, false);
    for (uint32_t scene = 0; scene < scene_count; scene++) {
        const uint32_t address = pointer + scene / 8;
        bits[scene] = (rdram[(address - 0x80000000) ^ 3] >> (scene % 8)) & 1;
    }
    if (have_bits && bits == last_bits) {
        return;
    }
    int new_bits = 0;
    for (uint32_t scene = 0; scene < scene_count; scene++) {
        if (bits[scene] && !(have_bits && last_bits[scene])) {
            new_bits++;
        }
    }
    const bool quiet = new_bits > quiet_threshold;
    int reached_count = 0;
    for (uint32_t scene = 0; scene < scene_count; scene++) {
        reached_count += bits[scene] ? 1 : 0;
    }
    std::printf("[achievements] scenes reached: %d (%d new), from 0x%08X\n", reached_count, new_bits, pointer);
    std::fflush(stdout);
    last_bits = bits;
    have_bits = true;

    bool every = true;
    for (uint32_t scene = 0; scene < scene_count; scene++) {
        if (counts_for_every_scene(scene) && !bits[scene]) {
            every = false;
        }
    }
    int quiet_unlocks = 0;
    for (size_t i = 0; i < achievement_count; i++) {
        const int scene = achievement_list[i].scene;
        const bool reached = scene == every_scene ? every : bits[scene];
        if (!reached || !unlock(i)) {
            continue;
        }
        if (quiet) {
            quiet_unlocks++;
        }
        else {
            conker::notices::show(std::string("Achievement unlocked: ") + achievement_list[i].title);
        }
    }
    if (quiet_unlocks > 0) {
        conker::notices::show(std::to_string(quiet_unlocks) + (quiet_unlocks == 1 ? " achievement" : " achievements") +
            " unlocked from this save");
    }
}
