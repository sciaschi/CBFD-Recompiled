// Texture packs (issue #63): RT64's texture replacements, an rt64.json and its textures, as a .rtz
// file (a zip) or a folder with a mod.json in the mods folder. RecompFrontend's renderer hands the
// enabled ones to RT64; the mod menu installs them and turns them on and off, as in the other recomps.
//
// The Texture Pack setting (at the end of the Graphics tab; it had a tab of its own before, its choice
// carried over by conker_config.cpp) picks one of them. Its list is the packs in the mods folder when the
// game starts (the settings are made before the runtime opens the mods), so a pack installed while it
// runs is listed from the next start. A GLideN64 pack (.htc, gliden64_packs.cpp) is listed as the pack
// it unpacks into, which is there once the launcher has unpacked it. Choosing one turns it on and every other pack off, through the
// runtime, which saves it as if done in the Mods menu. "Set in the Mods Menu" (the default) leaves them
// as the Mods menu has them, where several can be on at once, the later ones taking precedence. The
// choice is applied again when the launcher opens, once the runtime has opened the mods.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"

#include "conker.hpp"

namespace {
    const std::string option_id = "texture_pack";
    constexpr uint32_t mods_menu = 0;
    constexpr uint32_t none = 1;
    constexpr uint32_t first_pack = 2;

    struct Pack {
        std::string id;
        std::string name;
    };
    std::vector<Pack> packs;

    // A .rtz without a mod.json takes its file name as its id. A folder needs a mod.json, and is a
    // texture pack if it has an rt64.json. A .htc is listed as the folder it unpacks into, if that isn't
    // there yet.
    std::vector<Pack> find_packs(const std::filesystem::path& folder) {
        std::vector<Pack> found;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{ folder, std::filesystem::directory_options::skip_permission_denied, ec }) {
            const std::filesystem::path& path = entry.path();
            if (entry.is_regular_file(ec) && path.extension() == ".rtz") {
                found.push_back({ path.stem().string(), path.stem().string() });
            }
            else if (entry.is_directory(ec) && std::filesystem::exists(path / "rt64.json", ec)) {
                std::ifstream file(path / "mod.json");
                const nlohmann::json manifest = nlohmann::json::parse(file, nullptr, false);
                if (manifest.is_object() && manifest.contains("id") && manifest["id"].is_string()) {
                    const std::string id = manifest["id"];
                    found.push_back({ id, manifest.value("display_name", id) });
                }
            }
        }
        for (const auto& entry : std::filesystem::directory_iterator{ folder, std::filesystem::directory_options::skip_permission_denied, ec }) {
            const std::filesystem::path& path = entry.path();
            if (entry.is_regular_file(ec) && path.extension() == ".htc") {
                const std::string id = conker::texture_packs::gliden64_pack_id(path);
                if (std::none_of(found.begin(), found.end(), [&](const Pack& pack) { return pack.id == id; })) {
                    found.push_back({ id, path.stem().string() });
                }
            }
        }
        std::sort(found.begin(), found.end(), [](const Pack& a, const Pack& b) { return a.name < b.name; });
        return found;
    }

    uint32_t selected() {
        recomp::config::Config* config = recompui::config::find_option_config(option_id);
        return config ? std::get<uint32_t>(config->get_option_value(option_id)) : mods_menu;
    }
}

void conker::texture_packs::register_type() {
    recomp::mods::ModContentType texture_pack{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = [](recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
            recompui::renderer::enable_texture_pack(context, mod);
        },
        .on_disabled = [](recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
            recompui::renderer::disable_texture_pack(mod);
        },
        .on_reordered = [](recomp::mods::ModContext&) {
            recompui::renderer::trigger_texture_pack_update();
        },
    };
    const recomp::mods::ModContentTypeId texture_pack_id = recomp::mods::register_mod_content_type(texture_pack);
    recomp::mods::register_mod_container_type("rtz", { texture_pack_id }, false);

    // GLideN64 packs (gliden64_packs.cpp): the Mods menu's installer copies a .htc into the mods folder,
    // and its refresh (which follows an install) unpacks it.
    recompui::register_mod_file_extension(".htc");
    recompui::register_mod_scan_callback(unpack_gliden64_packs);
}

void conker::texture_packs::add_options(recomp::config::Config& config) {
    packs = find_packs(recomp::mods::get_mods_directory());
    static std::vector<recomp::config::ConfigOptionEnumOption> options;
    options.clear();
    options.emplace_back(mods_menu, "ModsMenu", "Set in the Mods Menu");
    options.emplace_back(none, "None", "None");
    for (size_t i = 0; i < packs.size(); i++) {
        options.emplace_back(first_pack + (uint32_t)i, "pack:" + packs[i].id, packs[i].name);
    }

    config.add_enum_option(option_id, "Texture Pack",
        "Replaces the game's textures with a texture pack's, such as HD textures. Install a pack (a <b>.rtz</b> file, "
        "or a GLideN64 pack's <b>.htc</b> file) by putting it in the <b>mods</b> folder or dropping it onto the Mods menu; "
        "it's listed here from the next start. "
        "<recomp-color primary>Set in the Mods Menu</recomp-color> leaves the packs as the Mods menu has them, where "
        "several can be on at once. <recomp-color primary>None</recomp-color> turns them all off. Choosing a pack turns "
        "it on and the others off.",
        options, mods_menu);
    config.add_option_change_callback(option_id,
        [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            // Once applied: the Graphics tab keeps a choice as temporary until its Apply button
            // (Temporary, and Temporary again if it's discarded), and apply() reads the applied value.
            if (context == recomp::config::OptionChangeContext::Permanent) {
                apply();
            }
        });
}

void conker::texture_packs::apply() {
    const uint32_t choice = selected();
    if (choice == mods_menu) {
        return;
    }
    for (size_t i = 0; i < packs.size(); i++) {
        recomp::mods::enable_mod(packs[i].id, choice == first_pack + i);
    }
}
