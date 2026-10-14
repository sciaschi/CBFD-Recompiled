// Voice-clip probe (a dev-only logging primitive for mods).
//
// The host half of the Hollywood Subtitles capture tooling. The mod watches the game's MP3 voice
// player (which clip plays, by ROM address, and for how long) and the cutscene system (which scene
// script is playing, and its clock). A mod can't write files from its MIPS code, so it hands each
// finished clip and each dialogue event to the host through the exports registered here, and the host
// appends a line to voice_segments.txt. An offline tool (mods/hollywood_subtitles/match_voice_to_text.py)
// then pairs those lines with the extracted speech-bubble text to build the mod's transcript.
//
// The file goes next to the game's other logs, in the config folder. All the file I/O happens on a
// dedicated thread drained from a queue, so the exports only format a short line, push it under a
// short lock and return: they're called from the mod's per-frame hook, never from the audio thread.
// Nothing here runs unless the mod calls the exports, so a normal build is unaffected.
//
// Exports (called from a mod; see mods/include/recompvoiceprobe.h):
//   recomp_voice_log_clip(scene, script, rom, order, duration_ms, serial, at_tick): one finished clip.
//   recomp_voice_log_dialog(event, scene, script, a, b): a dialogue started, ended, never started...
//   recomp_voice_dialog_done(scene, script): whether a dialogue has already been captured to its end.
//   recomp_voice_nav_save/load/clear: the auto-play run's progress, so it resumes after a crash.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "recomp.h"

#include "conker.hpp"

namespace {
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::string> queue;
    bool started = false;   // the logger thread is running

    // Drains the queue to voice_segments.txt, appending, with a banner per run so passes can be told
    // apart. Started lazily on the first line, so a build the mod never calls into does nothing.
    void logger_thread() {
        const std::filesystem::path path = recomp::get_config_path() / "voice_segments.txt";
        std::FILE* file = std::fopen(path.string().c_str(), "a");
        if (file == nullptr) {
            return;
        }
        std::fprintf(file,
            "# voice_segments.txt - Hollywood Subtitles capture\n"
            "# clip lines:   scene(hex) script(none = no cutscene) rom(hex) order duration_ms serial at_tick\n"
            "# dialog lines: dialog=<event> scene script ... (ticks are the cutscene clock, 2 a game frame)\n"
            "# a new run starts here\n");
        std::fflush(file);

        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [] { return !queue.empty(); });
                line = std::move(queue.front());
                queue.pop_front();
            }
            std::fputs(line.c_str(), file);
            std::fflush(file);
        }
    }

    void push_line(const char* text) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (!started) {
            started = true;
            std::thread(logger_thread).detach();
        }
        queue.emplace_back(text);
        queue_cv.notify_one();
    }

    // o32: the first four arguments in $a0-$a3, the rest on the stack from $sp + 0x10.
    uint32_t stack_arg(uint8_t* rdram, recomp_context* ctx, int n) {
        return (uint32_t)MEM_W(0x10 + 4 * (n - 4), ctx->r29);
    }

    void script_text(char* out, size_t size, uint32_t script) {
        if (script == 0xFF) {
            std::snprintf(out, size, "none");
        } else {
            std::snprintf(out, size, "%u", (unsigned)script);
        }
    }
}

// void recomp_voice_log_clip(u32 scene, u32 script, u32 rom, u32 order, u32 duration_ms, u32 serial,
//                            s32 at_tick): one finished voice clip. script is the cutscene script that
// was playing when it started (0xFF: none), order its place among that script's clips, at_tick the
// cutscene clock when it started.
extern "C" void recomp_voice_log_clip(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t scene = (uint32_t)ctx->r4;
    const uint32_t script = (uint32_t)ctx->r5;
    const uint32_t rom = (uint32_t)ctx->r6;
    const uint32_t order = (uint32_t)ctx->r7;
    const uint32_t duration_ms = stack_arg(rdram, ctx, 4);
    const uint32_t serial = stack_arg(rdram, ctx, 5);
    const int32_t at_tick = (int32_t)stack_arg(rdram, ctx, 6);
    char s[8];
    script_text(s, sizeof(s), script);
    char line[160];
    std::snprintf(line, sizeof(line),
        "scene=%02X script=%s rom=0x%08X order=%u duration_ms=%u serial=%u at_tick=%d\n",
        (unsigned)scene, s, (unsigned)rom, (unsigned)order, (unsigned)duration_ms, (unsigned)serial, (int)at_tick);
    push_line(line);
}

// void recomp_voice_log_dialog(u32 event, u32 scene, u32 script, u32 a, u32 b): an auto-play event.
//   0 start:   a = has bubble text, b = index in the dialogue list
//   1 end:     a = cutscene ticks it ran, b = clips it played
//   2 nostart: a = frames waited (the game never started it)
//   3 stalled: a = cutscene ticks, b = clips (still playing, but silent too long: waiting for input?)
//   4 cap:     a = cutscene ticks, b = clips (hit the per-dialogue time limit)
//   5 resume:  a = index the run resumed at (the one before it may have crashed the game)
//   6 done:    a = dialogues tried
//   7 skipped: a = cutscene ticks, b = clips (ended early: skipped by hand, or its clock jumped to the end)
namespace {
    void note_dialog_done(uint32_t scene, uint32_t script);
}
extern "C" void recomp_voice_log_dialog(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t event = (uint32_t)ctx->r4;
    const uint32_t scene = (uint32_t)ctx->r5;
    const uint32_t script = (uint32_t)ctx->r6;
    const uint32_t a = (uint32_t)ctx->r7;
    const uint32_t b = stack_arg(rdram, ctx, 4);
    char line[160];
    switch (event) {
        case 0:
            std::snprintf(line, sizeof(line), "dialog=start scene=%02X script=%u text=%u index=%u\n",
                (unsigned)scene, (unsigned)script, (unsigned)a, (unsigned)b);
            break;
        case 1:
        case 3:
        case 4:
        case 7:
            std::snprintf(line, sizeof(line), "dialog=%s scene=%02X script=%u ticks=%u clips=%u\n",
                event == 1 ? "end" : event == 3 ? "stalled" : event == 4 ? "cap" : "skipped",
                (unsigned)scene, (unsigned)script, (unsigned)a, (unsigned)b);
            if (event == 1) {
                note_dialog_done(scene, script);
            }
            break;
        case 2:
            std::snprintf(line, sizeof(line), "dialog=nostart scene=%02X script=%u waited_frames=%u\n",
                (unsigned)scene, (unsigned)script, (unsigned)a);
            break;
        case 5:
            std::snprintf(line, sizeof(line), "dialog=resume index=%u\n", (unsigned)a);
            break;
        case 6:
            std::snprintf(line, sizeof(line), "dialog=done tried=%u\n", (unsigned)a);
            break;
        default:
            std::snprintf(line, sizeof(line), "dialog=event%u scene=%02X script=%u a=%u b=%u\n",
                (unsigned)event, (unsigned)scene, (unsigned)script, (unsigned)a, (unsigned)b);
            break;
    }
    push_line(line);
}

// --- Auto-play resume -----------------------------------------------------------------------------
//
// Teleporting into some dialogues may crash the game (a scene loaded out of order, the heap...). The
// mod saves the index of the next dialogue to try before each one; after a crash the player relaunches,
// the mod reads it and carries on past the dialogue that crashed. A tiny file in the config folder,
// written straight away (one number now and then).

namespace {
    std::filesystem::path nav_progress_path() {
        return recomp::get_config_path() / "voice_nav_progress.txt";
    }
}

// void recomp_voice_nav_save(u32 next_index): remember the next dialogue index to try.
extern "C" void recomp_voice_nav_save(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    std::FILE* file = std::fopen(nav_progress_path().string().c_str(), "w");
    if (file != nullptr) {
        std::fprintf(file, "%u\n", (unsigned)(uint32_t)ctx->r4);
        std::fclose(file);
    }
}

// u32 recomp_voice_nav_load(void): the saved next index, or 0 if there is none. Returns in $v0.
extern "C" void recomp_voice_nav_load(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    unsigned index = 0;
    std::FILE* file = std::fopen(nav_progress_path().string().c_str(), "r");
    if (file != nullptr) {
        if (std::fscanf(file, "%u", &index) != 1) {
            index = 0;
        }
        std::fclose(file);
    }
    ctx->r2 = (int32_t)index;
}

// void recomp_voice_nav_clear(void): forget the saved progress (a run finished).
extern "C" void recomp_voice_nav_clear(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    std::remove(nav_progress_path().string().c_str());
}

// --- Dialogues already captured -------------------------------------------------------------------
//
// So a capture can be collected over several sittings without replaying (or skipping by hand) what's
// done: the dialogues voice_segments.txt already has a "dialog=end" for (played to their own end, in
// any earlier run) are read once, on the first query, and every end logged since is added. Auto-play
// asks before each dialogue and passes over the done ones. Called from the game thread only.

namespace {
    std::mutex done_mutex;
    bool done_loaded = false;
    std::set<uint32_t> done_dialogs; // (scene << 8) | script

    void load_done_dialogs() {
        done_loaded = true;
        std::FILE* file = std::fopen((recomp::get_config_path() / "voice_segments.txt").string().c_str(), "r");
        if (file == nullptr) {
            return;
        }
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            unsigned scene = 0;
            unsigned script = 0;
            if (std::sscanf(line, "dialog=end scene=%x script=%u", &scene, &script) == 2) {
                done_dialogs.insert((scene << 8) | (script & 0xFF));
            }
        }
        std::fclose(file);
    }

    void note_dialog_done(uint32_t scene, uint32_t script) {
        std::lock_guard<std::mutex> lock(done_mutex);
        if (!done_loaded) {
            load_done_dialogs();
        }
        done_dialogs.insert((scene << 8) | (script & 0xFF));
    }
}

// u32 recomp_voice_dialog_done(u32 scene, u32 script): 1 if that dialogue was already captured to its end.
extern "C" void recomp_voice_dialog_done(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t key = (((uint32_t)ctx->r4 & 0xFF) << 8) | ((uint32_t)ctx->r5 & 0xFF);
    std::lock_guard<std::mutex> lock(done_mutex);
    if (!done_loaded) {
        load_done_dialogs();
    }
    ctx->r2 = done_dialogs.count(key) ? 1 : 0;
}
