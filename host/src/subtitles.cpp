// Generic subtitle overlay (a host primitive for mods).
//
// This draws a line of text over the game, like a film's subtitle, with styling a mod chooses. It
// holds no game logic of its own: it only renders what a mod last asked for, through the functions
// the host exports to mods (mod_api.cpp registers them). A mod (mods/hollywood_subtitles) decides
// when to show a line, what it says and how it looks; the host just paints it. That keeps the
// feature in a toggleable mod and the base app free of it, and the game's own code untouched.
//
// Exports (called from a mod; see mods/include/recompsubtitles.h):
//   - recomp_subtitle_show(text, style): show text, with a packed style (position, size, colors).
//   - recomp_subtitle_hide(): hide it.
// The host renders on the main thread (update()), from whatever the exports last set. The exports
// may be called from the game thread, so the state they set is guarded.
//
// The style word (so one register carries it, o32-simply) packs:
//   bits 0-1   position: 0 bottom, 1 top, 2 middle
//   bits 2-3   text size: 0 small, 1 normal, 2 large
//   bit  4     text has a shadow/outline look (uses the Large/Normal style's weight)  [reserved]
//   bits 8-15  background opacity 0-255 (0 = no box)
//   (text color is passed separately as a 0xRRGGBB word, 0xFFFFFF default)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

#if defined(CONKER_RT64)
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#endif

#include "conker.hpp"

namespace {
    // What a mod last asked for. Set by the exports (any thread), read by update() (main thread).
    std::mutex state_mutex;
    bool want_shown = false;
    std::string want_text;
    uint32_t want_style = 0;
    uint32_t want_color = 0xFFFFFF;
    uint64_t want_generation = 0; // bumped on any change, so update() only touches recompui on change
}

// --- Voice playback detection (event-driven subtitles) ------------------------------------------
//
// The runtime calls conker_on_rom_read (weak hook in librecomp/src/pi.cpp, added by
// recomp/n64modernruntime.patch) for every ROM->RAM DMA. Conker's MP3 voices are streamed from
// ROM; each time a new voice clip starts decoding, the game loads the MP3 decoder's work buffers
// with two large reads (seen while testing: ~17408 and ~42000 bytes, from ROM ~0x13F7xxxx). Those
// large reads are rare outside this, so a read of at least this size marks "a new voice line just
// started". We count them; a mod reads the counter and advances its subtitle on each new event.
// This keeps the subtitles synced to the actual voice, not a timer. The host holds no transcript or
// per-line logic: it only counts voice-start events and draws what a mod asks for.
namespace {
    // Voice-clip tracking. Written only from conker_on_rom_read (which may run on the audio thread)
    // with relaxed atomics; nothing else on that path (no timing calls, no locks).
    //
    // When a voice clip starts, the MP3 decoder loads its work buffers with two big reads (~17408 and
    // ~42000 bytes). The next 2048-byte read is the clip's first MP3 frame block: its ROM address
    // identifies the clip. While the clip plays, its MP3 is streamed in 2048-byte reads from just
    // after that address; when they stop, the voice has ended.
    std::atomic<bool> awaiting_clip = false;       // a decoder load was seen, clip address not yet
    std::atomic<uint32_t> clip_addr = 0;           // ROM address of the clip playing (its MP3 start)
    std::atomic<uint64_t> clip_count = 0;          // clips started so far
    std::atomic<uint64_t> clip_activity = 0;       // streaming reads of the current clip so far

    constexpr size_t decoder_load_min_size = 32768; // the ~42000-byte load, once per clip
    constexpr size_t stream_read_size = 2048;
    constexpr uint32_t clip_window = 0x40000;       // a clip's MP3 is well under 256 KB
}

// Weak hook defined in the runtime (pi.cpp): called on every ROM->RAM DMA. MUST stay minimal: it
// runs on the game's audio/DMA thread in a hot path.
extern "C" void conker_on_rom_read(uint32_t physical_addr, size_t num_bytes) {
    if (num_bytes >= decoder_load_min_size) {
        awaiting_clip.store(true, std::memory_order_relaxed);
        return;
    }
    if (num_bytes != stream_read_size) {
        return;
    }
    if (awaiting_clip.load(std::memory_order_relaxed)) {
        awaiting_clip.store(false, std::memory_order_relaxed);
        clip_addr.store(physical_addr, std::memory_order_relaxed);
        clip_activity.fetch_add(1, std::memory_order_relaxed);
        clip_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t start = clip_addr.load(std::memory_order_relaxed);
    if (start != 0 && physical_addr >= start && physical_addr - start < clip_window) {
        clip_activity.fetch_add(1, std::memory_order_relaxed);
    }
}

// Exports (mod_api.cpp registers these). Declared here, defined below, so they can touch the state.
extern "C" void recomp_subtitle_show(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_subtitle_hide(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_subtitle_voice_event(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_subtitle_voice_clip(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_subtitle_voice_activity(uint8_t* rdram, recomp_context* ctx);

namespace {
    std::string read_mod_string(uint8_t* rdram, gpr address) {
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

// void recomp_subtitle_show(const char* text, uint32_t style, uint32_t color_rgb):
// $a0 text pointer, $a1 the packed style word, $a2 a 0xRRGGBB color.
extern "C" void recomp_subtitle_show(uint8_t* rdram, recomp_context* ctx) {
    std::string text = read_mod_string(rdram, ctx->r4);
    const uint32_t style = (uint32_t)ctx->r5;
    const uint32_t color = (uint32_t)ctx->r6 & 0xFFFFFF;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!want_shown || want_text != text || want_style != style || want_color != color) {
        want_shown = true;
        want_text = std::move(text);
        want_style = style;
        want_color = color;
        want_generation++;
    }
}

// void recomp_subtitle_hide(void).
extern "C" void recomp_subtitle_hide(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (want_shown) {
        want_shown = false;
        want_text.clear();
        want_generation++;
    }
}

// unsigned long recomp_subtitle_voice_event(void): returns how many voice-clip starts have been
// detected so far. A mod reads this each frame; when it increases, a new line is being spoken.
extern "C" void recomp_subtitle_voice_event(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = (gpr)(int64_t)(int32_t)(uint32_t)clip_count.load(std::memory_order_relaxed);
}

// unsigned long recomp_subtitle_voice_clip(void): ROM address of the voice clip playing now (its
// first MP3 frame). Stable per recorded line, so a mod can key subtitles to it.
extern "C" void recomp_subtitle_voice_clip(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = (gpr)(int64_t)(int32_t)clip_addr.load(std::memory_order_relaxed);
}

// unsigned long recomp_subtitle_voice_activity(void): counts the current clip's streaming reads.
// While it keeps rising the voice is playing; when it stops, the voice has ended.
extern "C" void recomp_subtitle_voice_activity(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = (gpr)(int64_t)(int32_t)(uint32_t)clip_activity.load(std::memory_order_relaxed);
}

#if defined(CONKER_RT64)
namespace {
    std::atomic<bool> ui_ready = false;

    // Style field helpers (used only by the renderer).
    enum class Pos : uint32_t { Bottom = 0, Top = 1, Middle = 2 };
    enum class Size : uint32_t { Small = 0, Normal = 1, Large = 2 };
    Pos style_pos(uint32_t s) { return static_cast<Pos>(s & 0x3); }
    Size style_size(uint32_t s) { return static_cast<Size>((s >> 2) & 0x3); }
    uint8_t style_bg_opacity(uint32_t s) { return (uint8_t)((s >> 8) & 0xFF); }

    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* row = nullptr;
    recompui::Element* box = nullptr;
    recompui::Label* label = nullptr;
    recompui::LabelStyle label_style = recompui::LabelStyle::Normal;
    bool shown = false;
    uint64_t applied_generation = 0;

    // Rebuilds the Label if the style asks for a different LabelStyle (its size is fixed at creation).
    // Also does the first creation (when label is null).
    void ensure_label_style(recompui::LabelStyle style) {
        if (label_style == style && label != nullptr) {
            return;
        }
        label_style = style;
        // recompui has no "remove element"; make a fresh context for the new size. Hidden first so the
        // old one goes away cleanly.
        if (created) {
            if (shown) {
                recompui::hide_context(context);
                shown = false;
            }
        }
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        row = context.create_element<recompui::Element>(context.get_root_element());
        row->set_position(recompui::Position::Absolute);
        row->set_left(0.0f);
        row->set_right(0.0f);
        row->set_display(recompui::Display::Flex);
        row->set_justify_content(recompui::JustifyContent::Center);
        box = context.create_element<recompui::Element>(row);
        box->set_width(80.0f, recompui::Unit::Percent);
        box->set_padding_left(16.0f);
        box->set_padding_right(16.0f);
        box->set_padding_top(6.0f);
        box->set_padding_bottom(6.0f);
        box->set_border_radius(8.0f);
        label = context.create_element<recompui::Label>(box, "", style);
        label->set_text_align(recompui::TextAlign::Center);
        context.close();
        created = true;
    }

    recompui::LabelStyle label_style_for(Size size) {
        switch (size) {
            case Size::Small:  return recompui::LabelStyle::Small;
            case Size::Large:  return recompui::LabelStyle::Large;
            case Size::Normal:
            default:           return recompui::LabelStyle::Normal;
        }
    }
}
#endif

void conker::subtitles::on_ui_ready() {
#if defined(CONKER_RT64)
    ui_ready = true;
#endif
}

void conker::subtitles::update() {
#if defined(CONKER_RT64)
    bool local_shown;
    std::string text;
    uint32_t style;
    uint32_t color;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_shown = want_shown;
        text = want_text;
        style = want_style;
        color = want_color;
        generation = want_generation;
    }

    const bool wanted = ui_ready && ultramodern::is_game_started() && local_shown && !text.empty();
    if (!wanted) {
        if (created && shown) {
            recompui::hide_context(context);
            shown = false;
        }
        return;
    }

    // Pick the Label size (may rebuild the context) before showing.
    ensure_label_style(label_style_for(style_size(style)));

    if (generation != applied_generation) {
        applied_generation = generation;
        context.open();

        // Vertical placement.
        switch (style_pos(style)) {
            case Pos::Top:
                row->set_top(6.0f, recompui::Unit::Percent);
                break;
            case Pos::Middle:
                row->set_top(45.0f, recompui::Unit::Percent);
                break;
            case Pos::Bottom:
            default:
                row->set_bottom(6.0f, recompui::Unit::Percent);
                break;
        }

        // Background box opacity (0 = no box).
        const uint8_t bg = style_bg_opacity(style);
        box->set_background_color(recompui::Color{ 0, 0, 0, bg });

        // Text color.
        label->set_color(recompui::Color{
            (uint8_t)((color >> 16) & 0xFF),
            (uint8_t)((color >> 8) & 0xFF),
            (uint8_t)(color & 0xFF),
            255 });

        label->set_text(text);
        context.close();
    }

    if (!shown) {
        recompui::show_context(context, "");
        shown = true;
    }
#endif
}
