// Hollywood Subtitles: movie-style subtitles for Conker's Bad Fur Day's voice acting.
//
// All the logic lives here, in the mod. The host only draws the text, through the subtitle overlay it
// exports (recomp_subtitle_show / recomp_subtitle_hide, see mods/include/recompsubtitles.h). The game's
// own code is untouched: the mod only watches the game's MP3 voice player.
//
// Timing comes from the game's own voice playback (the MP3 player the decomp names, from Rare's library):
//   - mp3_play_file(romaddr, filesize) (func_151F2960) starts each voice clip, with its ROM address. A clip
//     is one recording: one line, or a long speech (Conker's throne-room monologue is one 38.8 s clip).
//   - mp3main_continue_file (func_151F86B0) decodes the playing clip's next MPEG-2 Layer III frame: 576
//     samples at 22050 Hz, 26.1 ms. Counting the frames since the clip started gives its position exactly,
//     and it stops while the game is paused (the player only decodes while it's playing).
//   - The player's state (0x800E0E04): 1 playing, 2 paused, 5 loading, 7 unpausing; 0 or 3 once stopped.
// So the transcript gives each clip's lines as times from the clip's start, and the mod shows whichever
// line holds the clip's position now. Measured: the throne monologue decodes 1484 frames in every run,
// with two pauses or none, and its speech segments land where they were measured from the decoded audio.
// (The first version counted in the cutscene timer instead, 2 ticks a game frame: it falls behind the
// voice whenever the game draws fewer than 30 frames a second, which is why its lines drifted later on.)
//
// The mod hooks no function conker.toml hooks (a mod's hook replaces the toml's): so not func_15095D34, the
// speech bubbles' pieces, which the host hooks for widescreen and a crash. Hide Speech Bubbles waits on a way
// to tell speech bubbles from Conker's thought bubbles that doesn't hook it.
//
// The two player hooks run on the game's audio thread, which the runtime schedules with the game's other
// threads: they only store numbers (no printing, no options, no host calls). Printing there (in a test
// version on the host's side) hung the game. Everything else runs once a game frame, from the hook on
// func_1501BBB8.

#include "modding.h"
#include "PR/ultratypes.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompsubtitles.h"
#include "recompvoiceprobe.h"

// --- The voice player (read-only) -----------------------------------------------------------------

// The player's state (g_Mp3Vars.state).
#define g_mp3_state (*(volatile u32*)0x800E0E04)
#define MP3_IDLE 0
#define MP3_STOPPED 3

// The game's current scene number (D_800BE9F0), the same numbering the script bank and the Chapters
// menu use. Read each game frame (read-only), to tag logged voice clips with the scene they played in.
#define g_scene_number (*(volatile u32*)0x800BE9F0)

// --- Dialogue auto-play (dev-only) ----------------------------------------------------------------
//
// Teleports straight to each dialogue instead of playing to it. Every conversation in the game is a
// scene script (bank 6: scene N's script S, the [S.T] blocks extract_bubble_text.py writes), and the
// game starts one with func_1501D348(scene, script, slot, 0, 0) — what its own trigger zones and the
// Chapters menu call (game_20AE20.c: func_1501D348(*entry, script, 0, 0, 0)). When the scene isn't the
// one in play, func_1501D348 loads it first (func_1501C730(1, scene, ...)) and, once it's in, plays the
// script (func_150082CC). So no save progress or walking is needed: the run asks for every script in
// dialog_list.h (all 483 in the bank, generated from the ROM by make_dialog_list.py) one after another.
void func_1501D348(s32 scene, s32 script, s32 slot, s32 arg3, s32 arg4);
#include "dialog_list.h"

// The scene loader (Skip Intro jumps to the bar with func_1501C730(6, 0x1D, 0, 0, 1)): auto-play loads
// a dialogue's scene with it when that scene isn't in play, then starts the script in it.
void func_1501C730(s32 kind, s32 scene, u32 arg2, s32 arg3, s32 arg4);

// The buttons pressed this frame, player 1 (func_1501BBB8 sets it; the Skip Any Cutscene mod reads L
// there). Auto-play sets L after func_1501BBB8 returns to end a cutscene that has stalled.
#define g_buttons_pressed (*(volatile u16*)0x800BE710)
#define BUTTON_L 0x0020
#define BUTTON_B 0x4000 // advances a speech bubble ("press [B]")

// The cutscene system's state, slot 0 (game_49D30.c; the Skip Any Cutscene mod reads the same):
//   cutscene_state  0 none, 1 playing, 2 starting after a scene load (D_800C35EA)
//   cutscene_script the script id playing (D_800C35E8)
//   cutscene_timer  its clock, 2 ticks a game frame (D_800C35B0), toward cutscene_length (D_800C3640)
//   cutscene_pending a request waiting for its scene to load (D_800C35C4, script + 1)
#define cutscene_state   (*(volatile u8*)0x800C35EA)
#define cutscene_script  (*(volatile u8*)0x800C35E8)
#define cutscene_timer   (*(volatile s32*)0x800C35B0)
#define cutscene_length  (*(volatile s32*)0x800C3640)
#define cutscene_pending (*(volatile u8*)0x800C35C4)
// Nonzero while the game fades out a cutscene that's being skipped (func_1501E1B4 sets it when L is
// accepted): tells a dialogue skipped by hand from one that played to its end.
#define cutscene_skipping (*(volatile u8*)0x800C3C88)
#define CUTSCENE_PLAYING 1

// Dialogues known to crash or hang when teleported to: skipped without asking. Empty until a run finds
// one (the run's log says which dialogue it was on, and a relaunch resumes past it anyway).
static const u8 dialog_skip[][2] = {
    { 0xFF, 0xFF }, // placeholder (no scene 0xFF): add { scene, script } pairs here
};
#define DIALOG_SKIP_COUNT ((int)(sizeof(dialog_skip) / sizeof(dialog_skip[0])))

static int dialog_is_skipped(u32 scene, u32 script) {
    for (int i = 0; i < DIALOG_SKIP_COUNT; i++) {
        if (dialog_skip[i][0] == scene && dialog_skip[i][1] == script) {
            return 1;
        }
    }
    return 0;
}


// Set by the hooks below (audio thread), read each game frame.
static volatile u32 clip_rom = 0;     // the clip's ROM address, as the game passes it to mp3_play_file
static volatile u32 clip_frames = 0;  // MP3 frames decoded since it started
static volatile u32 clip_serial = 0;  // clips started so far (tells a clip played twice apart)

RECOMP_HOOK("func_151F2960") void hollywood_on_clip_start(u32 romaddr, u32 filesize) {
    clip_rom = romaddr;
    clip_frames = 0;
    clip_serial++;
}

RECOMP_HOOK("func_151F86B0") void hollywood_on_clip_frame(void) {
    clip_frames++;
}

// Milliseconds into the clip: 576 samples a frame at 22050 Hz, 576000 / 22050 = 1280 / 49 ms a frame (in 32 bits:
// a mod has no 64-bit division, and 1280 x frames fits for over 900 hours).
static u32 clip_ms(void) {
    return (clip_frames * 1280u) / 49u;
}

// --- The transcript -------------------------------------------------------------------------------
//
// Each clip the game plays, by ROM address, with its lines as times (ms) from the clip's start. A line
// ending at WHOLE_CLIP stays up as long as the clip plays (a clip that is one line). is_intro marks the
// clips the Intro Cutscene Subtitles toggle controls; the rest go with General Game Subtitles.

#define WHOLE_CLIP 0xFFFFFFFFu
// Keep a line up this long after its time ends (ms), so it doesn't flash off between words.
#define LINE_TAIL_MS 300
// And a whole-clip line this many game frames after its clip stops (about 0.3 s).
#define CLIP_TAIL_FRAMES 10

typedef struct {
    u32 start_ms;
    u32 end_ms;
    const char* text; // 0: nothing shows (a clip that isn't speech)
} SubLine;

typedef struct {
    u32 rom;
    int is_intro;
    const SubLine* lines;
    int count;
} SubClip;

// The chainsaw boot (scene 0x21): seven clips, in play order. The first three are the "N" and the chainsaw.
static const SubLine chainsaw_1[] = { { 0, WHOLE_CLIP, 0 } };
static const SubLine chainsaw_2[] = { { 0, WHOLE_CLIP, 0 } };
static const SubLine chainsaw_3[] = { { 0, WHOLE_CLIP, 0 } };
static const SubLine chainsaw_4[] = { { 0, WHOLE_CLIP, "Stupid logo!" } };
static const SubLine chainsaw_5[] = { { 0, WHOLE_CLIP, "There, much better." } };
static const SubLine chainsaw_6[] = { { 0, WHOLE_CLIP, "How can... get it... Ah!" } };
static const SubLine chainsaw_7[] = { { 0, WHOLE_CLIP, "Marvelous." } };

// Conker's throne-room monologue (scene 0x18): one 38.8 s clip, the whole speech, "Well." to "a bad fur day."
// Its 20 speech segments (ms) were measured from the clip's decoded audio (its loudness each MP3 frame, in the
// game), and each segment's words noted by ear from the game's own output, cut at them. (The first version
// decoded the clip offline past its end, to 55.75 s, and spread the speech over that: its lines drifted later
// and later.) The debug view numbers the segments ([seg N]).
static const u32 throne_segments[][2] = {
    {    50,   500 }, {  1520,  2320 }, {  3580,  5090 }, {  6030,  7990 }, {  8880,  9610 },
    { 10920, 11990 }, { 12150, 12770 }, { 13900, 17580 }, { 18130, 19070 }, { 20530, 20950 },
    { 21890, 23040 }, { 23850, 24500 }, { 24890, 25470 }, { 26830, 28060 }, { 28810, 29360 },
    { 29990, 31190 }, { 32080, 33310 }, { 34040, 34270 }, { 34530, 35110 }, { 35660, 36410 },
};

// Each line from its first segment's start; it stays up until just before the next line (or, with a long
// pause after it, a moment past its last segment), so the short ones can be read.
static const SubLine throne[] = {
    {     50,  1450, "Well." },                                                     // segment 1
    {   1520,  3450, "There I am." },                                               // 2
    {   3580,  5900, "Conker the King." },                                          // 3
    {   6030,  8750, "King of all the land." },                                     // 4
    {   8880, 10800, "Who'd have thought that?" },                                  // 5
    {  10920, 12100, "But how did I come to this," },                               // 6
    {  12150, 13750, "I hear you say." },                                           // 7
    {  13900, 18050, "And who are those strange fellows who surround my throne," },  // 8
    {  18130, 20000, "I hear you also say." },                                      // 9
    {  20530, 23750, "Well, it's a long story." },                                  // 10-11
    {  23850, 26400, "Come closer, and I'll tell you." },                           // 12-13
    {  26830, 29900, "It all started... yesterday," },                              // 14-15
    {  29990, 31950, "and what a day that was!" },                                  // 16
    {  32080, 33950, "It's what I call..." },                                       // 17
    {  34040, 37400, "a bad fur day." },                                            // 18-20
};

#define LINES(a) a, (int)(sizeof(a) / sizeof(a[0]))
static const SubClip clips[] = {
    { 0x017B3888, 1, LINES(chainsaw_1) },
    { 0x017AFB10, 1, LINES(chainsaw_2) },
    { 0x017B1CA0, 1, LINES(chainsaw_3) },
    { 0x017AA440, 1, LINES(chainsaw_4) },
    { 0x017A8EC8, 1, LINES(chainsaw_5) },
    { 0x017AE1D8, 1, LINES(chainsaw_6) },
    { 0x017A99E0, 1, LINES(chainsaw_7) },
    { 0x015E1FD8, 1, LINES(throne) },
};
#define CLIP_COUNT ((int)(sizeof(clips) / sizeof(clips[0])))

static const SubClip* find_clip(u32 rom) {
    for (int i = 0; i < CLIP_COUNT; i++) {
        if (clips[i].rom == rom) {
            return &clips[i];
        }
    }
    return 0;
}

// The text that holds this time in the clip (with its tail), or 0. Where a line's tail runs into the next line,
// the later one (the one that has started) wins, so a new line is never held back.
static const char* text_at(const SubClip* clip, u32 ms) {
    const char* text = 0;
    for (int i = 0; i < clip->count; i++) {
        const SubLine* line = &clip->lines[i];
        if (line->end_ms == WHOLE_CLIP || (ms >= line->start_ms && ms < line->end_ms + LINE_TAIL_MS)) {
            text = line->text;
        }
    }
    return text;
}

// --- Options --------------------------------------------------------------------------------------

// Enum options come back as the selected index (0-based), as the Skip Cutscene mod relies on.
static u32 opt(const char* key) { return (u32)recomp_get_config_u32(key); }

// Background opacity for the "Background Box" option (None/Light/Medium/Dark).
static u32 background_opacity(void) {
    switch (opt("background")) {
        case 1:  return 90;   // Light
        case 2:  return 160;  // Medium
        case 3:  return 220;  // Dark
        case 0:
        default: return 0;    // None
    }
}

// Text color for the "Text Color" option (White/Yellow/Cyan/Green).
static u32 text_color(void) {
    switch (opt("color")) {
        case 1:  return 0xFFE14A; // Yellow
        case 2:  return 0x4AE1FF; // Cyan
        case 3:  return 0x6CFF6C; // Green
        case 0:
        default: return 0xFFFFFF; // White
    }
}

static u32 build_style(void) {
    const u32 pos = opt("position"); // 0 bottom, 1 top, 2 middle (matches SUBTITLE_POS_*)
    const u32 size = opt("size");    // 0 small, 1 normal, 2 large (matches SUBTITLE_SIZE_*)
    return SUBTITLE_STYLE(pos, size, background_opacity());
}

// The debug view: "[0x015E1FD8 12.34s] " before the line, to write a clip's times by watching it.
static int debug_prefix(char* buf, int size, u32 rom, u32 ms) {
    static const char hex[] = "0123456789ABCDEF";
    int i = 0;
    const char* p = "[0x";
    while (*p && i < size - 1) buf[i++] = *p++;
    for (int shift = 28; shift >= 0 && i < size - 1; shift -= 4) buf[i++] = hex[(rom >> shift) & 0xF];
    if (i < size - 1) buf[i++] = ' ';
    char digits[12];
    int n = 0;
    u32 v = ms / 1000;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0 && n < 10);
    while (n > 0 && i < size - 1) buf[i++] = digits[--n];
    const u32 cs = (ms % 1000) / 10;
    if (i < size - 6) {
        buf[i++] = '.';
        buf[i++] = (char)('0' + cs / 10);
        buf[i++] = (char)('0' + cs % 10);
        buf[i++] = 's';
        buf[i++] = ']';
        buf[i++] = ' ';
    }
    buf[i] = '\0';
    return i;
}

// --- Hide Speech Bubbles -------------------------------------------------------------------------
//
// func_1516B6BC draws a speech bubble (its pieces through func_15095A48 -> func_15095D34, and its
// text): it takes the display list in $a0 and returns its new end in $v0, like the game's other
// drawers. func_15095D34 itself is hooked by the host (widescreen bubbles, the sprite size crash
// fix), and a mod's hook there would replace those; this hooks the bubble drawer above it instead,
// so the host's hooks still run. With the option on, everything the drawer wrote between entry and
// return is turned into no-ops (G_NOOP, 0), so the bubble isn't drawn and the subtitle takes its
// place. The game still runs the bubble (its timing, waiting for B) exactly as before.
// Found by reading the recompiled call chain (func_15095D34 <- func_15095A90 <- func_15095A48 <-
// func_1516B6BC, 15 pieces and 11 text calls); whether it also draws Conker's thought bubbles is
// still to be seen in game.

static int hide_bubbles = 0;            // the option, read once a game frame
static volatile u32 bubble_dl_start = 0; // where the bubble drawer started writing

RECOMP_HOOK("func_1516B6BC") void hollywood_bubble_begin(u32 dl) {
    bubble_dl_start = dl;
}

RECOMP_HOOK_RETURN("func_1516B6BC") void hollywood_bubble_end(void) {
    const u32 start = bubble_dl_start;
    bubble_dl_start = 0;
    if (!hide_bubbles || start == 0) {
        return;
    }
    const u32 end = recomphook_get_return_u32();
    // A sane range only (a bubble is a few dozen commands): never touch anything else.
    if (end <= start || end - start > 0x2000 || ((end - start) & 7) != 0) {
        return;
    }
    for (u32 at = start; at < end; at += 4) {
        *(volatile u32*)at = 0; // G_NOOP
    }
}

// --- Each game frame ------------------------------------------------------------------------------

// func_1501BBB8 runs once per game frame (it reads the controllers).
RECOMP_HOOK_RETURN("func_1501BBB8") void hollywood_on_frame(void) {
    hide_bubbles = opt("enabled") && opt("hide_bubbles");
    static const char* shown = 0;   // the line up now (0: none)
    static int debug_shown = 0;     // the debug view is up
    static u32 seen_serial = 0;     // the last clip logged
    static u32 tail = 0;            // game frames since the clip stopped

    // --- Capture (dev-only): log each finished voice clip to the host (voice_segments.txt) ---------
    // The audio-thread hooks reset clip_frames when a new clip starts, so the previous clip's length is
    // gone by then: snapshot the running clip's frame count and ROM here, every frame, and when a clip
    // ends (a new one starts, or playback has been stopped a while) hand the finished one to the host.
    // Each clip is tagged with the cutscene script playing when it started (0xFF: none) and the
    // cutscene clock then, so the matcher knows exactly which dialogue it belongs to and when in it.
    static u32 cap_serial = 0;      // the serial of the clip being measured now (0: none)
    static u32 cap_rom = 0;         // its ROM address
    static u32 cap_frames = 0;      // the most frames seen for it (its length)
    static u32 cap_scene = 0;       // the scene it played in (snapshotted at its start)
    static u32 cap_script = 0xFF;   // the cutscene script playing at its start (0xFF: none)
    static s32 cap_tick = -1;       // the cutscene clock at its start (-1: no cutscene)
    static u32 cap_order = 0;       // its position within that (scene, script), from 1
    static u32 cap_logged = 0;      // the serial last logged (so a clip is logged once)
    static u32 cap_key = 0xFFFFFFFFu; // the (scene, script) the order counter is counting within

    const u32 cap_on = opt("enabled") && (opt("capture") || opt("navigate"));

    // --- The subtitle logic (unchanged below) -----------------------------------------------------
    const u32 serial = clip_serial;
    const u32 rom = clip_rom;
    const u32 ms = clip_ms();
    const u32 state = g_mp3_state;
    const int stopped = state == MP3_IDLE || state == MP3_STOPPED;
    const SubClip* clip = find_clip(rom);

    if (cap_on) {
        // A clip just started (serial changed): finalize the one we were measuring, then begin the new.
        if (serial != cap_serial) {
            if (cap_serial != 0 && cap_serial != cap_logged && cap_frames != 0) {
                recomp_voice_log_clip(cap_scene, cap_script, cap_rom, cap_order,
                    (cap_frames * 1280u) / 49u, cap_serial, cap_tick);
                cap_logged = cap_serial;
            }
            // Begin measuring the new clip. Count its order within the (scene, script) it starts in.
            const u32 scene = g_scene_number;
            const int in_cutscene = cutscene_state == CUTSCENE_PLAYING;
            const u32 script = in_cutscene ? cutscene_script : 0xFF;
            const u32 key = (scene << 8) | script;
            if (key != cap_key) {
                cap_key = key;
                cap_order = 0;
            }
            cap_order++;
            cap_serial = serial;
            cap_rom = rom;
            cap_scene = scene;
            cap_script = script;
            cap_tick = in_cutscene ? cutscene_timer : -1;
            cap_frames = clip_frames;
        } else if (serial != 0) {
            // Still the same clip: keep its largest frame count (it only grows while playing).
            const u32 frames = clip_frames;
            if (frames > cap_frames) {
                cap_frames = frames;
            }
            // Once it has been stopped a while, finalize it (its last clip in a scene has no successor
            // to trigger the serial-change path above).
            if (stopped && tail >= CLIP_TAIL_FRAMES && cap_serial != cap_logged && cap_frames != 0) {
                recomp_voice_log_clip(cap_scene, cap_script, cap_rom, cap_order,
                    (cap_frames * 1280u) / 49u, cap_serial, cap_tick);
                cap_logged = cap_serial;
            }
        }
    }

    if (serial != seen_serial) {
        seen_serial = serial;
        tail = 0;
        recomp_printf("[hollywood_subtitles] clip 0x%08X starts%s\n", (unsigned)rom, clip ? "" : " (no lines for it)");
    }
    tail = stopped ? tail + 1 : 0;

    // --- Dialogue auto-play (dev-only): teleport to every dialogue in the game, one after another ----
    // With this on, the mod plays each script in dialog_list.h (all of the game's scene scripts) and
    // logs what happens: the script starting, every voice clip it plays (tagged with the script and its
    // clock, through the capture above), and how it ended. No walking and no save progress: for each
    // dialogue it loads the scene if it isn't in play (func_1501C730(6, scene, ...), the loader Skip
    // Intro uses), then starts the script with func_1501D348(scene, script, 0, 0, 0) — the game's own
    // "play this script", which in the scene in play starts it at once, whatever the trigger zones.
    //
    // It never presses B through speech bubbles (that would slow the run, and a bubble's text is in the
    // ROM anyway). A dialogue that falls silent for a while is logged as "stalled" and ended with L (as
    // the Skip Any Cutscene mod allows, which must be on), so the next one can start. Before each one the
    // next index is saved: if a dialogue crashes the game, relaunching carries on past it.
    {
        enum {
            DLG_LOAD_TIMEOUT  = 900,  // ~30 s for a scene to load
            DLG_LOAD_SETTLE   = 45,   // ~1.5 s in a freshly loaded scene before starting a script
            DLG_START_TIMEOUT = 150,  // ~5 s for a requested script to start
            DLG_FROZEN_FRAMES = 45,   // ~1.5 s with the cutscene clock stopped (and no voice): it waits for B
            DLG_MAX_TAPS      = 40,   // B taps without the dialogue ending: give up and end it with L
            DLG_MAX_FRAMES    = 5400, // ~3 min hard cap per dialogue
            DLG_CLEAR_TIMEOUT = 300,  // ~10 s pressing L to end a cutscene, else reload the scene
            DLG_SETTLE_FRAMES = 15,   // a breather between dialogues
        };
        enum { DLG_OFF, DLG_WAIT_BOOT, DLG_NEXT, DLG_LOAD, DLG_WAIT_START, DLG_PLAYING, DLG_CLEAR, DLG_SKIP, DLG_SETTLE };
        static int dlg_phase = DLG_OFF;
        static int dlg_index = 0;      // the dialog_list entry we're on
        static u32 dlg_frames = 0;     // frames in the current phase
        static u32 dlg_serial0 = 0;    // the clip serial when the dialogue started (clips = serial - this)
        static int dlg_loaded = 0;     // the scene being loaded is in play (settling)
        static int dlg_reload = 0;     // a cutscene couldn't be ended: reload the scene to clear it
        static int dlg_loads = 0;      // loads tried for the current dialogue (a scene that won't stay)
        static s32 dlg_last_tick = 0;  // the cutscene clock last frame
        static u32 dlg_frozen = 0;     // frames the clock hasn't moved (and no voice)
        static u32 dlg_taps = 0;       // B taps in the current dialogue
        static int dlg_skipped = 0;    // the dialogue was skipped (by hand, or its clock jumped to the end)
        static int dlg_loads_for = -1; // the dialogue index dlg_loads counts for

        const int dlg_on = opt("enabled") && opt("navigate");

        if (!dlg_on) {
            dlg_phase = DLG_OFF;
        } else if (dlg_phase == DLG_OFF) {
            dlg_index = (int)recomp_voice_nav_load();
            if (dlg_index < 0 || dlg_index >= DIALOG_COUNT) {
                dlg_index = 0;
            }
            if (dlg_index != 0) {
                recomp_voice_log_dialog(VOICE_DIALOG_RESUME, 0, 0, (unsigned long)dlg_index, 0);
            }
            recomp_printf("[hollywood_subtitles] auto-play: %d dialogues, starting at %d\n", DIALOG_COUNT, dlg_index + 1);
            dlg_phase = DLG_WAIT_BOOT;
        } else if (dlg_phase == DLG_WAIT_BOOT) {
            // Let the boot finish (the logos, scene 0x25, and the chainsaw opening, 0x21) before driving.
            if (g_scene_number != 0x25 && g_scene_number != 0x21) {
                dlg_phase = DLG_NEXT;
            }
        } else if (dlg_phase == DLG_NEXT) {
            if (dlg_index >= DIALOG_COUNT) {
                recomp_printf("[hollywood_subtitles] auto-play: done, %d dialogues\n", DIALOG_COUNT);
                recomp_voice_log_dialog(VOICE_DIALOG_DONE, 0, 0, (unsigned long)DIALOG_COUNT, 0);
                recomp_voice_nav_clear();
                dlg_index = DIALOG_COUNT;
                dlg_phase = DLG_SETTLE; // stays idle at the end (turn the option off to reset)
                dlg_frames = 0;
            } else {
                const u32 scene = dialog_list[dlg_index][0];
                const u32 script = dialog_list[dlg_index][1];
                if (dialog_is_skipped(scene, script) ||
                    (opt("navigate_skip_done") && recomp_voice_dialog_done(scene, script))) {
                    dlg_index++; // blacklisted, or already captured to its end in an earlier run
                } else if ((dlg_reload || g_scene_number != scene) &&
                           (dlg_loads_for == dlg_index ? dlg_loads : 0) >= 3) {
                    // Loaded three times and it still isn't in play (the game moves on from it): skip.
                    recomp_printf("[hollywood_subtitles] auto-play: scene 0x%02X won't stay loaded, skipping\n",
                        (unsigned)scene);
                    recomp_voice_log_dialog(VOICE_DIALOG_NOSTART, scene, script, 0, 3);
                    dlg_reload = 0;
                    dlg_index++;
                } else if (dlg_reload || g_scene_number != scene) {
                    // Load the dialogue's scene (or reload it, to clear a cutscene L couldn't end).
                    if (dlg_loads_for != dlg_index) {
                        dlg_loads_for = dlg_index;
                        dlg_loads = 0;
                    }
                    dlg_loads++;
                    recomp_voice_nav_save((unsigned long)(dlg_index + 1)); // a crash here resumes past it
                    recomp_printf("[hollywood_subtitles] auto-play: loading scene 0x%02X\n", (unsigned)scene);
                    func_1501C730(6, (s32)scene, 0, 0, 1);
                    dlg_reload = 0;
                    dlg_loaded = 0;
                    dlg_frames = 0;
                    dlg_phase = DLG_LOAD;
                } else if (cutscene_state == CUTSCENE_PLAYING) {
                    // Something is playing (the scene's own opening cutscene, say): it's captured as it
                    // plays; once it's silent a while, end it so ours can start.
                    dlg_frames = 0;
                    dlg_last_tick = cutscene_timer;
                    dlg_frozen = 0;
                    dlg_taps = 0;
                    dlg_skipped = 0;
                    dlg_serial0 = serial;
                    dlg_phase = DLG_CLEAR;
                } else if (cutscene_pending == 0) {
                    recomp_voice_nav_save((unsigned long)(dlg_index + 1));
                    recomp_voice_log_dialog(VOICE_DIALOG_START, scene, script,
                        dialog_list[dlg_index][2], (unsigned long)dlg_index);
                    recomp_printf("[hollywood_subtitles] auto-play %d/%d: scene 0x%02X script %u%s\n",
                        dlg_index + 1, DIALOG_COUNT, (unsigned)scene, (unsigned)script,
                        dialog_list[dlg_index][2] ? "" : " (no bubble text)");
                    func_1501D348((s32)scene, (s32)script, 0, 0, 0);
                    dlg_frames = 0;
                    dlg_phase = DLG_WAIT_START;
                }
            }
        } else if (dlg_phase == DLG_LOAD) {
            const u32 scene = dialog_list[dlg_index][0];
            dlg_frames++;
            if (!dlg_loaded && g_scene_number == scene) {
                dlg_loaded = 1;
                dlg_frames = 0;
            } else if (dlg_loaded && dlg_frames >= DLG_LOAD_SETTLE) {
                dlg_phase = DLG_NEXT;
            } else if (!dlg_loaded && dlg_frames >= DLG_LOAD_TIMEOUT) {
                recomp_printf("[hollywood_subtitles] auto-play: scene 0x%02X did not load\n", (unsigned)scene);
                recomp_voice_log_dialog(VOICE_DIALOG_NOSTART, scene, dialog_list[dlg_index][1], dlg_frames, 0);
                dlg_index++;
                dlg_phase = DLG_NEXT;
            }
        } else if (dlg_phase == DLG_WAIT_START) {
            const u32 scene = dialog_list[dlg_index][0];
            const u32 script = dialog_list[dlg_index][1];
            dlg_frames++;
            if (cutscene_state == CUTSCENE_PLAYING && cutscene_script == script && g_scene_number == scene) {
                dlg_phase = DLG_PLAYING;
                dlg_frames = 0;
                dlg_serial0 = serial;
                dlg_last_tick = cutscene_timer;
                dlg_frozen = 0;
                dlg_taps = 0;
                dlg_skipped = 0;
            } else if (dlg_frames >= DLG_START_TIMEOUT) {
                recomp_printf("[hollywood_subtitles] auto-play: scene 0x%02X script %u did not start\n",
                    (unsigned)scene, (unsigned)script);
                recomp_voice_log_dialog(VOICE_DIALOG_NOSTART, scene, script, dlg_frames, 0);
                dlg_index++;
                dlg_frames = 0;
                dlg_phase = DLG_SETTLE;
            }
        } else if (dlg_phase == DLG_PLAYING || dlg_phase == DLG_CLEAR) {
            // Let the cutscene run to its own end, however long, voiced or not: while its clock (2 ticks
            // a frame) moves, it is animating (a camera move, a cow, liquid draining...), so just wait.
            // Only when the clock has stopped and nothing is speaking is it waiting for input (a speech
            // bubble's "press B"): tap B once, which moves the dialogue on and lets its next lines play,
            // so they're captured too. If taps stop helping (or the cap is hit), end it with L.
            // DLG_CLEAR is the same for a cutscene that was already playing (the scene's own opening):
            // it's captured as it plays, then the next dialogue starts.
            const int voice_now = (serial != 0) && !stopped;
            const s32 tick = cutscene_timer;
            const int playing_ours = dlg_phase == DLG_PLAYING;
            const u32 scene = dialog_list[dlg_index < DIALOG_COUNT ? dlg_index : 0][0];
            const u32 script = dialog_list[dlg_index < DIALOG_COUNT ? dlg_index : 0][1];
            const u32 clips = serial - dlg_serial0;
            dlg_frames++;
            // A skip: the game's skip fade is running, or the clock leapt to the end (a skip sets it to the
            // cutscene's length at once; playing, it moves a few ticks a frame).
            if (cutscene_skipping != 0 || tick - dlg_last_tick > 60) {
                dlg_skipped = 1;
            }
            if (tick != dlg_last_tick || voice_now) {
                dlg_frozen = 0;
            } else {
                dlg_frozen++;
            }
            dlg_last_tick = tick;

            int event = -1;
            if (cutscene_state != CUTSCENE_PLAYING || (playing_ours && cutscene_script != script)) {
                event = VOICE_DIALOG_END;
            } else if (dlg_frames >= DLG_MAX_FRAMES) {
                event = VOICE_DIALOG_CAP;
            } else if (dlg_taps >= DLG_MAX_TAPS) {
                event = VOICE_DIALOG_STALLED;
            } else if (dlg_frozen >= DLG_FROZEN_FRAMES) {
                g_buttons_pressed = (u16)(g_buttons_pressed | BUTTON_B); // one frame: advance the bubble
                dlg_taps++;
                dlg_frozen = 0;
            }

            if (event == VOICE_DIALOG_END) {
                if (playing_ours) {
                    recomp_printf("[hollywood_subtitles] auto-play: scene 0x%02X script %u %s (%u clips, %u B taps)\n",
                        (unsigned)scene, (unsigned)script, dlg_skipped ? "skipped" : "ended",
                        (unsigned)clips, (unsigned)dlg_taps);
                    recomp_voice_log_dialog(dlg_skipped ? VOICE_DIALOG_SKIPPED : VOICE_DIALOG_END,
                        scene, script, (u32)(dlg_last_tick > 0 ? dlg_last_tick : 0), clips);
                    dlg_index++;
                }
                dlg_frames = 0;
                dlg_phase = DLG_SETTLE;
            } else if (event >= 0) {
                // Stuck: log it (ours only) and end it with L; if even L doesn't end it, reload the scene.
                if (playing_ours) {
                    recomp_printf("[hollywood_subtitles] auto-play: scene 0x%02X script %u %s (%u clips, %u B taps), ending it\n",
                        (unsigned)scene, (unsigned)script, event == VOICE_DIALOG_CAP ? "hit the cap" : "stalled",
                        (unsigned)clips, (unsigned)dlg_taps);
                    recomp_voice_log_dialog((unsigned long)event, scene, script, (u32)(tick > 0 ? tick : 0), clips);
                    dlg_index++;
                }
                dlg_frames = 0;
                dlg_phase = DLG_SKIP;
            }
        } else if (dlg_phase == DLG_SKIP) {
            // End a stuck cutscene with L (it skips a cutscene; Skip Any Cutscene lets it skip ones never
            // watched). If it still won't end, reload the scene to clear it.
            dlg_frames++;
            if (cutscene_state != CUTSCENE_PLAYING) {
                dlg_frames = 0;
                dlg_phase = DLG_SETTLE;
            } else if (dlg_frames >= DLG_CLEAR_TIMEOUT) {
                recomp_printf("[hollywood_subtitles] auto-play: a cutscene won't end, reloading the scene\n");
                dlg_reload = 1;
                dlg_frames = 0;
                dlg_phase = DLG_SETTLE;
            } else {
                g_buttons_pressed = (u16)(g_buttons_pressed | BUTTON_L);
            }
        } else if (dlg_phase == DLG_SETTLE) {
            if (dlg_index < DIALOG_COUNT && ++dlg_frames >= DLG_SETTLE_FRAMES) {
                dlg_phase = DLG_NEXT;
            }
        }
    }

    const char* text = 0;
    if (opt("enabled") && clip != 0 && (clip->is_intro ? opt("intro_subtitles") : opt("general_subtitles")) != 0 &&
        serial != 0 && tail <= CLIP_TAIL_FRAMES) {
        text = text_at(clip, ms);
    }

    // The debug view while any clip plays (with lines or not): its address and time, the measured speech
    // segment it's in (the throne monologue's), then the line (if any).
    if (opt("enabled") && opt("debug_segments") && serial != 0 && !stopped) {
        static char buf[160];
        int i = debug_prefix(buf, (int)sizeof(buf), rom, ms);
        if (rom == 0x015E1FD8) {
            for (int s = 0; s < (int)(sizeof(throne_segments) / sizeof(throne_segments[0])); s++) {
                if (ms >= throne_segments[s][0] && ms < throne_segments[s][1] && i < (int)sizeof(buf) - 10) {
                    const int n = s + 1;
                    const char* p = "[seg ";
                    while (*p) buf[i++] = *p++;
                    if (n >= 10) buf[i++] = (char)('0' + n / 10);
                    buf[i++] = (char)('0' + n % 10);
                    buf[i++] = ']';
                    buf[i++] = ' ';
                    break;
                }
            }
        }
        for (const char* t = text ? text : ""; *t && i < (int)sizeof(buf) - 1; t++) buf[i++] = *t;
        buf[i] = '\0';
        recomp_subtitle_show(buf, build_style(), text_color());
        debug_shown = 1;
        shown = text;
        return;
    }
    if (debug_shown) {
        debug_shown = 0;
        if (text == 0) {
            recomp_subtitle_hide();
        }
        shown = (const char*)0;
        if (text != 0) {
            recomp_subtitle_show(text, build_style(), text_color());
            shown = text;
        }
        return;
    }

    if (text != shown) {
        if (text != 0) {
            recomp_subtitle_show(text, build_style(), text_color());
        } else {
            recomp_subtitle_hide();
        }
        shown = text;
    }
}
