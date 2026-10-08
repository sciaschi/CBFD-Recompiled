// N64-style controllers whose SDL mapping makes the C-buttons face buttons (issue #28).
//
// SDL3 (and so sdl2-compat, what Linux distributions ship as SDL2) maps these controllers as
// Switch pads: the C-buttons become Back, X, Y and a trigger or misc button, which the controls
// screen shows as those and one of which (C-Right) SDL2's names can't hold at all. RecompFrontend
// binds the C-buttons to the right stick, so each such mapping is rewritten with its C-buttons as
// the right stick's four directions, the rest of it kept as SDL has it:
// - The Nintendo Switch Online N64 controller, and pads like the 8BitDo 64 in its Switch mode,
//   through the kernel's driver (057e:2019): the C-buttons are buttons 10 (up), 3 (down), 4 (left)
//   and 2 (right). Only a mapping with the kernel driver's layout (A b0, B b1, Start b11) is taken.
// - The 8BitDo 64 in its D-Input mode (2dc8:3019): the C-buttons are two axes, SDL mapping their
//   halves to Back (up), X (down), Y (left) and Misc2 (right). Those two axes become the right stick.
// A mapping that already has a right stick is left alone (SDL2's own on Windows, for instance).
//
// CONKER_FAKE_PAD=nso64 or 8bitdo64 (TEMP-DEBUG) attaches a virtual copy of either, with SDL3's
// mapping for it, and presses its C-buttons in turn (up, down, left, right) once the game runs,
// so the rewrite can be checked without the controller: frontend.cpp prints what arrives.
// CONKER_FAKE_PAD=raphnet64 does the same for raphnet-tech's N64 adapter, with the mapping loaded
// for the real one (assets/controllerdb.txt), then presses A, B, Z and Start.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>

#include "conker.hpp"

namespace {
    constexpr Uint16 vendor_nintendo = 0x057E, product_n64 = 0x2019;
    constexpr Uint16 vendor_8bitdo = 0x2DC8, product_8bitdo_64 = 0x3019;

    std::vector<std::string> split_mapping(const std::string& mapping) {
        std::vector<std::string> fields;
        size_t start = 0;
        while (start < mapping.size()) {
            size_t end = mapping.find(',', start);
            if (end == std::string::npos) {
                end = mapping.size();
            }
            if (end > start) {
                fields.push_back(mapping.substr(start, end - start));
            }
            start = end + 1;
        }
        return fields;
    }

    // An element's name without its half ("+rightx" is rightx) and the input it's bound to
    // without its half or inversion ("-a3~" is a3).
    std::string element_name(const std::string& field) {
        std::string name = field.substr(0, field.find(':'));
        if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
            name.erase(0, 1);
        }
        return name;
    }

    std::string bound_input(const std::string& field) {
        size_t colon = field.find(':');
        std::string input = colon == std::string::npos ? std::string() : field.substr(colon + 1);
        if (!input.empty() && (input[0] == '+' || input[0] == '-')) {
            input.erase(0, 1);
        }
        if (!input.empty() && input.back() == '~') {
            input.pop_back();
        }
        return input;
    }

    bool has_field(const std::vector<std::string>& fields, const char* field) {
        for (size_t i = 2; i < fields.size(); i++) {
            if (fields[i] == field) {
                return true;
            }
        }
        return false;
    }

    // The input bound to element (as "name:<half>aN"), or empty.
    std::string axis_half_for(const std::vector<std::string>& fields, const char* element, char half) {
        const std::string prefix = std::string(element) + ":" + half + "a";
        for (size_t i = 2; i < fields.size(); i++) {
            if (fields[i].compare(0, prefix.size(), prefix) == 0) {
                return bound_input(fields[i]);
            }
        }
        return {};
    }

    // The mapping with the C-buttons as the right stick, or empty if it isn't one to rewrite.
    std::string rewrite_mapping(Uint16 vendor, Uint16 product, const std::string& mapping) {
        std::vector<std::string> fields = split_mapping(mapping);
        if (fields.size() < 3) {
            return {};
        }
        for (size_t i = 2; i < fields.size(); i++) {
            const std::string name = element_name(fields[i]);
            if (name == "rightx" || name == "righty") {
                return {};
            }
        }

        std::vector<std::string> c_inputs;
        std::string c_stick;
        if (vendor == vendor_nintendo && product == product_n64) {
            if (!has_field(fields, "a:b0") || !has_field(fields, "b:b1") || !has_field(fields, "start:b11")) {
                return {};
            }
            c_inputs = { "b10", "b3", "b4", "b2" };
            c_stick = "-righty:b10,+righty:b3,-rightx:b4,+rightx:b2";
        } else if (vendor == vendor_8bitdo && product == product_8bitdo_64) {
            const std::string vertical = axis_half_for(fields, "back", '-');
            const std::string horizontal = axis_half_for(fields, "y", '-');
            if (vertical.empty() || horizontal.empty() || vertical == horizontal) {
                return {};
            }
            c_inputs = { vertical, horizontal };
            c_stick = "rightx:" + horizontal + ",righty:" + vertical;
        } else {
            return {};
        }

        std::string result = fields[0] + "," + fields[1] + ",";
        for (size_t i = 2; i < fields.size(); i++) {
            const std::string input = bound_input(fields[i]);
            bool on_c = false;
            for (const std::string& c : c_inputs) {
                on_c = on_c || input == c;
            }
            if (!on_c) {
                result += fields[i] + ",";
            }
        }
        return result + c_stick + ",";
    }
}

void conker::pad_mappings::fix_all() {
    for (int index = 0; index < SDL_NumJoysticks(); index++) {
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        if (mapping == nullptr) {
            continue;
        }
        const std::string fixed = rewrite_mapping(SDL_JoystickGetDeviceVendor(index), SDL_JoystickGetDeviceProduct(index), mapping);
        SDL_free(mapping);
        if (fixed.empty()) {
            continue;
        }
        if (SDL_GameControllerAddMapping(fixed.c_str()) < 0) {
            std::printf("[controller] couldn't remap the C-buttons: %s\n", SDL_GetError());
        } else {
            std::printf("[controller] C-buttons remapped to the right stick: %s\n", fixed.c_str());
        }
    }
    std::fflush(stdout);
}

// TEMP-DEBUG: CONKER_FAKE_PAD.
namespace {
    struct FakePad {
        const char* id;
        const char* name;
        Uint16 vendor, product;
        int axes, buttons, hats;
        // Its mapping, the GUID left for the device's: SDL3's (SDL_gamepad_db.h, Linux), or else
        // the one loaded for the real device's GUID (assets/controllerdb.txt).
        const char* sdl3_mapping;
        const char* db_guid;
        // The C-buttons in turn (up, down, left, right), then any others: a button, or an axis and
        // its value.
        struct Press { int button; int axis; Sint16 value; } presses[8];
        int press_count;
    };

#if defined(_WIN32)
    constexpr const char* raphnet_n64_guid = "030000009b2800006100000000000000";
#else
    constexpr const char* raphnet_n64_guid = "030000009b2800006100000001010000";
#endif

    const FakePad fake_pads[] = {
        { "nso64", "Nintendo.Co.Ltd. N64 Controller", vendor_nintendo, product_n64, 2, 13, 1,
            "Nintendo N64 Controller,a:b0,b:b1,back:b2,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b9,lefttrigger:b8,leftx:a0,lefty:a1,misc1:b5,rightshoulder:b7,righttrigger:b3,start:b11,x:b4,y:b10,",
            nullptr, { { 10, -1, 0 }, { 3, -1, 0 }, { 4, -1, 0 }, { 2, -1, 0 } }, 4 },
        { "8bitdo64", "8BitDo 64 Bluetooth Controller", vendor_8bitdo, product_8bitdo_64, 6, 20, 1,
            "8BitDo 64 Bluetooth Controller,a:b0,b:b1,back:-a3,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,misc1:b17,misc2:+a2,misc3:b10,rightshoulder:b7,righttrigger:a4,start:b11,x:+a3,y:-a2,",
            nullptr, { { -1, 3, -32768 }, { -1, 3, 32767 }, { -1, 2, -32768 }, { -1, 2, 32767 } }, 4 },
        // raphnet-tech's N64 to USB adapter: then A, B, Z and Start.
        { "raphnet64", "raphnet.net N64 to USB", 0x289B, 0x0061, 2, 14, 0, nullptr, raphnet_n64_guid,
            { { 6, -1, 0 }, { 7, -1, 0 }, { 8, -1, 0 }, { 9, -1, 0 }, { 0, -1, 0 }, { 1, -1, 0 }, { 2, -1, 0 }, { 3, -1, 0 } }, 8 },
    };

    const FakePad* fake_pad = nullptr;
    SDL_Joystick* fake_joystick = nullptr;
    int fake_frames = 0;
}

void conker::pad_mappings::attach_fake_pad() {
    const char* id = SDL_getenv("CONKER_FAKE_PAD");
    if (id == nullptr) {
        return;
    }
    for (const FakePad& pad : fake_pads) {
        if (std::strcmp(pad.id, id) == 0) {
            fake_pad = &pad;
        }
    }
    if (fake_pad == nullptr) {
        std::printf("[controller] CONKER_FAKE_PAD: no pad '%s' (nso64, 8bitdo64, raphnet64)\n", id);
        return;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = (Uint16)fake_pad->axes;
    desc.nbuttons = (Uint16)fake_pad->buttons;
    desc.nhats = (Uint16)fake_pad->hats;
    desc.vendor_id = fake_pad->vendor;
    desc.product_id = fake_pad->product;
    desc.name = fake_pad->name;
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0) {
        std::printf("[controller] CONKER_FAKE_PAD: couldn't attach: %s\n", SDL_GetError());
        fake_pad = nullptr;
        return;
    }
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    std::string mapping;
    if (fake_pad->sdl3_mapping != nullptr) {
        mapping = fake_pad->sdl3_mapping;
    } else if (char* loaded = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUIDFromString(fake_pad->db_guid))) {
        mapping = loaded;
        mapping.erase(0, mapping.find(',') + 1);
        SDL_free(loaded);
    } else {
        std::printf("[controller] CONKER_FAKE_PAD: no mapping loaded for %s\n", fake_pad->db_guid);
    }
    if (!mapping.empty()) {
        SDL_GameControllerAddMapping((std::string(guid) + "," + mapping).c_str());
    }
    fake_joystick = SDL_JoystickOpen(index);
    std::printf("[controller] CONKER_FAKE_PAD: attached %s\n", fake_pad->name);
}

void conker::pad_mappings::update() {
    if (fake_joystick == nullptr) {
        return;
    }
    // Two seconds in, each button held half a second, then half a second apart.
    constexpr int start = 120, step = 60, hold = 30;
    const int frame = fake_frames++ - start;
    if (frame < 0 || frame >= step * fake_pad->press_count || frame % step > hold) {
        return;
    }
    const FakePad::Press& press = fake_pad->presses[frame / step];
    const bool down = frame % step < hold;
    if (press.button >= 0) {
        SDL_JoystickSetVirtualButton(fake_joystick, press.button, down ? SDL_PRESSED : SDL_RELEASED);
    } else {
        SDL_JoystickSetVirtualAxis(fake_joystick, press.axis, down ? press.value : 0);
    }
}
