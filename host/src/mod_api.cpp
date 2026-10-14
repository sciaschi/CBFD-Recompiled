// Functions the game exports to mods (see mods/include/recomputils.h).

#include <cstdio>
#include <string>

#include "recomp.h"
#include "librecomp/overlays.hpp"

#include "conker.hpp"

namespace {
    std::string read_string(uint8_t* rdram, gpr address) {
        std::string text;
        for (int i = 0; i < 4096; i++) {
            char c = (char)MEM_B(i, address);
            if (c == '\0') {
                break;
            }
            text += c;
        }
        return text;
    }
}

// int recomp_printf(const char* fmt, ...): printf to the console. Arguments follow
// MIPS o32 varargs: $a1-$a3, then the stack from $sp + 0x10. Integer, string and
// character conversions are supported (no floats).
extern "C" void recomp_printf(uint8_t* rdram, recomp_context* ctx) {
    std::string fmt = read_string(rdram, ctx->r4);
    int next_arg = 1;
    auto arg = [&]() -> uint32_t {
        int n = next_arg++;
        switch (n) {
            case 1: return (uint32_t)ctx->r5;
            case 2: return (uint32_t)ctx->r6;
            case 3: return (uint32_t)ctx->r7;
            default: return (uint32_t)MEM_W(0x10 + 4 * (n - 4), ctx->r29);
        }
    };
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        // Copy the conversion spec (flags, width) up to its type character.
        std::string spec = "%";
        size_t j = i + 1;
        while (j < fmt.size() && std::string("-+ #0123456789.l").find(fmt[j]) != std::string::npos) {
            if (fmt[j] != 'l') {
                spec += fmt[j];
            }
            j++;
        }
        if (j >= fmt.size()) {
            break;
        }
        char type = fmt[j];
        char buffer[128];
        switch (type) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
                std::snprintf(buffer, sizeof(buffer), (spec + type).c_str(), arg());
                out += buffer;
                break;
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "0x%08X", arg());
                out += buffer;
                break;
            case 's':
                out += read_string(rdram, (int32_t)arg());
                break;
            case '%':
                out += '%';
                break;
            default:
                out += spec + type;
                break;
        }
        i = j;
    }
    std::fputs(out.c_str(), stdout);
    ctx->r2 = (int32_t)out.size();
}

// The subtitle overlay's exports (defined in host/src/subtitles.cpp, extern "C").
extern "C" void recomp_subtitle_show(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_subtitle_hide(uint8_t* rdram, recomp_context* ctx);

// The voice-clip probe's export (defined in host/src/voice_probe.cpp, extern "C"). Dev-only logging:
// the mod hands each finished voice clip over and the host appends it to voice_segments.txt.
extern "C" void recomp_voice_log_clip(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_voice_log_dialog(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_voice_dialog_done(uint8_t* rdram, recomp_context* ctx);
// Auto-play resume: the mod saves/loads/clears the next dialogue index so a run survives a crash.
extern "C" void recomp_voice_nav_save(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_voice_nav_load(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_voice_nav_clear(uint8_t* rdram, recomp_context* ctx);

void conker::register_mod_exports() {
    recomp::overlays::register_base_export("recomp_printf", recomp_printf);

    // The subtitle overlay (host/src/subtitles.cpp): a mod shows and hides a line of text, with a
    // style it chooses. The host holds no subtitle logic; it only renders what the mod asks for.
    recomp::overlays::register_base_export("recomp_subtitle_show", recomp_subtitle_show);
    recomp::overlays::register_base_export("recomp_subtitle_hide", recomp_subtitle_hide);

    // The voice-clip probe (host/src/voice_probe.cpp): dev-only, for building the transcript. A mod
    // logs each finished voice clip (scene, ROM address, order, duration) to voice_segments.txt, and
    // saves/loads its auto-navigate progress so a capture run resumes past a scene that froze the game.
    recomp::overlays::register_base_export("recomp_voice_log_clip", recomp_voice_log_clip);
    recomp::overlays::register_base_export("recomp_voice_log_dialog", recomp_voice_log_dialog);
    recomp::overlays::register_base_export("recomp_voice_dialog_done", recomp_voice_dialog_done);
    recomp::overlays::register_base_export("recomp_voice_nav_save", recomp_voice_nav_save);
    recomp::overlays::register_base_export("recomp_voice_nav_load", recomp_voice_nav_load);
    recomp::overlays::register_base_export("recomp_voice_nav_clear", recomp_voice_nav_clear);
}
