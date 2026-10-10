// Hollywood Subtitles: movie-style subtitles for Conker's Bad Fur Day's voice acting.
//
// All the logic lives here, in the mod. The host only draws the text, through the subtitle overlay
// it exports (recomp_subtitle_show / recomp_subtitle_hide, see mods/include/recompsubtitles.h). The
// game's own code is untouched.
//
// Each frame (a hook on func_1501BBB8, which the game runs once per frame to read the controllers;
// the recomp README calls it out as the per-frame hook), the mod:
//   1. reads the mod's options (the master switch and the per-area toggles);
//   2. reads which cutscene the game is playing and how far into it (read-only: the mod never skips
//      or changes a cutscene, it only watches the state to time the subtitles. These are the same
//      memory addresses the host's cutscene code reads; the Skip Cutscene mod reads them too, but
//      this mod shares nothing else with it and changes no playback);
//   3. looks the current line up in the transcript below;
//   4. asks the host to show it (with the chosen position, size, color and background) or to hide it.
//
// The transcript is built in for now (the mod's MIPS code can't open files on its own). This first
// version covers the intro (scene 0x21: the chainsaw opening and the throne-room monologue) and the
// start of the campaign; the rest of the game's script is added over time (see SCRIPT.MD).

#include "modding.h"
#include "PR/ultratypes.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompsubtitles.h"

// --- Game state the mod reads (read-only; the mod never skips or alters any cutscene) -----------
// (Addresses are the ones the host's cutscene code uses; the Skip Cutscene mod happens to read the
// same ones. This mod only observes them to time the subtitles.)

// A byte per cutscene slot, 1 while that slot is playing. Two slots are used.
#define g_cutscene_playing  ((volatile u8*)0x800C35EA)
// The scene id. Scene 0x21 is the intro (the chainsaw opening into the throne room).
#define g_scene_id          (*(volatile s32*)0x800BE9F0)
// A byte per slot: the cutscene id within the scene.
#define g_cutscene_ids      ((volatile u8*)0x800C35E8)
// An s32 per slot: how many frames into the cutscene the game is (30 per second).
#define g_cutscene_timers   ((volatile s32*)0x800C35B0)

#define SLOT_COUNT 2
// Hide a line this many frames after its voice stops streaming (the hook runs ~60 times a second;
// the voice streams several reads a frame while it plays). Leaves a short tail after the voice.
#define VOICE_QUIET_FRAMES 20
// Cue scenes: keep a cue up this many cutscene-timer ticks after its voice segment ends (0.3 s).
#define CUE_TAIL_TICKS 18
// Scene ids (confirmed on hardware), in the order the game plays them:
//   0x21 = the chainsaw boot (startup, first)
//   0x1D = the hub/menu (the bar)
//   0x18 = the campaign intro, where Conker speaks the throne-room monologue
//
// Known scene/cutscene ids catalogued while testing (scene, cut) -> what it is, in play order:
//   (0x21, 1)  chainsaw boot: "Stupid logo!", "There, much better.", "Marvelous." (7 voice clips)
//   (0x1D, 3)  the bar: Berri dancing while Conker calls to her
//   (0x1D, 4)  the bar: Conker drunk
//   (0x18, 1)  campaign intro: the throne-room monologue ("Well, here I am. King...")
// (0x1D hosts several cutscenes told apart by the cut value, so each 0x1D transcript entry sets its
// own cutscene_id. Add more here as they're found.)
#define SCENE_CHAINSAW 0x21
#define SCENE_HUB      0x1D
#define SCENE_INTRO    0x18

// --- The transcript -----------------------------------------------------------------------------
//
// Subtitles are event-driven: the host counts each MP3 voice-clip start (recomp_subtitle_voice_event)
// and the mod shows the matching line in order. So a cutscene's lines are just an ordered list - the
// n-th voice clip in the scene shows the n-th line. No frame timings needed; it stays in sync with
// the actual voice.

// One cutscene's lines, keyed by scene id (and optionally the cutscene id within it; -1 = any).
// is_intro marks the lines the Intro Cutscene Subtitles toggle controls; the rest are controlled by
// General Game Subtitles. Two kinds:
//   - lines: one per voice clip, in order (the n-th clip of the scene shows lines[n]).
//   - cues:  for a scene whose speech is one long clip, lines placed inside that clip at the times
//            the voice actually speaks, measured from the clip's own audio. Times are from the
//            moment the clip starts, in the game's cutscene timer (60 per second).
typedef struct {
    s32 start;  // cutscene-timer ticks after the clip starts
    s32 end;
    const char* text;
} SubCue;

typedef struct {
    s32 scene_id;
    s32 cutscene_id;
    int is_intro;
    const char* const* lines; // ordered: lines[0] on the 1st voice clip, lines[1] on the 2nd, ...
    int line_count;
    const SubCue* cues;       // or, for one long clip: timed cues inside it (NULL if unused)
    int cue_count;
} SubScene;

// Scene 0x21: the chainsaw boot. Conker saws the N64 logo, swaps in the Rareware logo and winks.
// Seven voice clips play here (t=60,168,284,556,846,942,1500 at 60 per second). The first three
// are the "N" looking around and the chainsaw, so they show nothing (NULL hides the subtitle).
// "Marvelous." on clip 7 is confirmed in sync; the others follow the scene's pacing.
static const char* const chainsaw_lines[] = {
    0,                              // clip 1, t=60   the "N" / sound effect
    0,                              // clip 2, t=168  chainsaw
    0,                              // clip 3, t=284  chainsaw cutting the "N"
    "Stupid logo!",                 // clip 4, t=556
    "There, much better.",          // clip 5, t=846
    "How can... get it... Ah!",     // clip 6, t=942
    "Marvelous.",                   // clip 7, t=1500 (confirmed)
};

// Scene 0x18: Conker's throne-room monologue (the campaign intro). The game plays it as ONE voice
// clip (ROM 0x115E1FD8, ~56 s), so lines can't advance per clip. The cues follow the clip's own audio:
// each one spans the voice segments the game's recording actually has (pauses found by decoding the
// clip), and only the wording comes from the script. Ticks are 60 per second from the clip's start.
static const SubCue intro_cues[] = {
    {    6,   33, "Well." },
    {   93,  144, "There I am." },
    {  216,  309, "Conker the King." },
    {  363,  483, "King of all the land." },
    {  534,  768, "Who'd a thought that?" },
    {  837, 1056, "But how did I come to this," },
    { 1089, 1146, "I hear you say." },
    { 1233, 1764, "And who are those strange fellows that surround my throne," },
    { 1800, 1875, "I hear you also say." },
    { 1929, 2004, "It's a long story." },
    { 2046, 2346, "Come closer, and I'll tell you." },
    { 2463, 2514, "It all started..." },
    { 2550, 2565, "yesterday," },
    { 2598, 2784, "and what a day that was!" },
    { 2841, 3024, "It's what I like to call..." },
    { 3066, 3345, "a bad fur day." },
};

// The 25 voice segments the throne-monologue clip actually has (speech between pauses, found by
// decoding the clip), in the same ticks. Used by the Debug: Voice Segments option to label what's
// on screen and in the log, so each segment's words can be matched to the right cue.
static const s32 intro_segments[][2] = {
    {    6,   33 }, {   93,  144 }, {  216,  309 }, {  363,  483 }, {  534,  582 },
    {  657,  768 }, {  837, 1056 }, { 1089, 1146 }, { 1233, 1260 }, { 1317, 1386 },
    { 1434, 1533 }, { 1611, 1686 }, { 1731, 1764 }, { 1800, 1875 }, { 1929, 2004 },
    { 2046, 2109 }, { 2142, 2187 }, { 2325, 2346 }, { 2463, 2514 }, { 2550, 2565 },
    { 2598, 2652 }, { 2700, 2712 }, { 2751, 2784 }, { 2841, 3024 }, { 3066, 3345 },
};
#define INTRO_SEGMENT_COUNT ((int)(sizeof(intro_segments) / sizeof(intro_segments[0])))

static const SubScene scenes[] = {
    // In play order.
    // 1. Chainsaw boot (0x21): its own lines (not the throne monologue).
    { SCENE_CHAINSAW, -1, 1, chainsaw_lines, (int)(sizeof(chainsaw_lines) / sizeof(chainsaw_lines[0])), 0, 0 },
    // 2. Hub/bar (0x1D): several cutscenes by cut id (cut 3 Berri dancing, cut 4 drunk Conker), to
    //    be added with their lines.
    // 3. Campaign intro (0x18): the throne-room monologue, one long clip with timed cues.
    { SCENE_INTRO, -1, 1, 0, 0, intro_cues, (int)(sizeof(intro_cues) / sizeof(intro_cues[0])) },
    // Nothing else is mapped yet, so no stray lines appear elsewhere. Each 0x1D entry must set its own
    // cutscene_id rather than -1, so one bar cutscene's lines don't bleed into another.
};
#define SCENE_COUNT ((int)(sizeof(scenes) / sizeof(scenes[0])))

// --- Options ------------------------------------------------------------------------------------

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

// --- Per-frame logic ----------------------------------------------------------------------------

// Finds the scene entry playing now, or returns 0. Fills *slot with the playing cutscene slot.
static const SubScene* playing_scene(int* slot_out) {
    int playing_slot = -1;
    for (int s = 0; s < SLOT_COUNT; s++) {
        if (g_cutscene_playing[s] == 1) {
            playing_slot = s;
            break;
        }
    }
    if (playing_slot < 0) {
        return 0;
    }
    *slot_out = playing_slot;
    s32 scene = g_scene_id;
    s32 cutscene = (s32)g_cutscene_ids[playing_slot];
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (scenes[i].scene_id != scene) {
            continue;
        }
        if (scenes[i].cutscene_id != -1 && scenes[i].cutscene_id != cutscene) {
            continue;
        }
        return &scenes[i];
    }
    return 0;
}

// Whether this scene's lines are allowed by the per-area toggle.
static int scene_allowed(const SubScene* sc) {
    if (sc->is_intro) {
        return opt("intro_subtitles") != 0;
    }
    return opt("general_subtitles") != 0;
}

// func_1501BBB8 runs once per frame (reads the controllers). Event-driven: the host counts MP3
// voice-clip starts (recomp_subtitle_voice_event); each new one advances to the next line of the
// scene playing now, so the subtitle appears exactly when the voice does.
RECOMP_HOOK_RETURN("func_1501BBB8") void hollywood_on_frame(void) {
    // State across frames: which scene we're subtitling, the voice-event count when it began, and
    // the last event count we acted on.
    static const SubScene* active = 0;
    static unsigned long scene_base_events = 0; // voice-event count when the scene started
    static unsigned long last_events = 0;       // last count seen
    static int line_index = -1;                 // which line is showing (-1 = none yet)
    static unsigned long last_activity = 0;     // the clip's streaming-read count last frame
    static int voice_quiet_frames = 0;          // frames since the voice last streamed
    static s32 clip_t0 = -1;                    // cue scenes: cutscene timer when the clip started
    static int cue_index = -1;                  // cue scenes: which cue is showing
    static int seg_index = -1;                  // cue scenes: which voice segment is playing

    const unsigned long events = recomp_subtitle_voice_event();

    if (!opt("enabled")) {
        active = 0;
        line_index = -1;
        last_events = events;
        recomp_subtitle_hide();
        return;
    }

    int slot = 0;
    const SubScene* sc = playing_scene(&slot);

    // Not in a mapped, allowed cutscene: clear and wait.
    if (sc == 0 || !scene_allowed(sc)) {
        if (active != 0) {
            active = 0;
            line_index = -1;
            recomp_subtitle_hide();
        }
        last_events = events;
        return;
    }

    // Entered a new mapped scene: start fresh. The first voice clip of the scene shows line 0, so
    // align the base one event back if a clip is already counted this frame.
    if (sc != active) {
        active = sc;
        line_index = -1;
        clip_t0 = -1;
        cue_index = -1;
        seg_index = -1;
        scene_base_events = events;
        last_events = events;
        recomp_subtitle_hide();
    }

    // A new voice clip started since last frame: advance the line.
    if (events != last_events) {
        last_events = events;
        long n = (long)(events - scene_base_events) - 1; // 1st clip -> line 0
        recomp_printf("[hollywood_subtitles] voice clip %d in scene 0x%X (t=%d) rom=0x%08X\n",
            (int)(n + 1), g_scene_id, g_cutscene_timers[slot], (unsigned)recomp_subtitle_voice_clip());
        voice_quiet_frames = 0;
        last_activity = recomp_subtitle_voice_activity();
        if (sc->cues != 0) {
            // One long clip: anchor its timed cues to the moment the clip starts.
            if (n == 0) {
                clip_t0 = g_cutscene_timers[slot];
                cue_index = -1;
            }
            return;
        }
        if (n >= 0 && n < (long)sc->line_count) {
            line_index = (int)n;
            if (sc->lines[line_index] != 0) {
                recomp_subtitle_show(sc->lines[line_index], build_style(), text_color());
            } else {
                recomp_subtitle_hide(); // a non-speech clip (e.g. the chainsaw)
            }
        } else if (n >= (long)sc->line_count) {
            line_index = -1;
            recomp_subtitle_hide();
        }
        return;
    }

    // Timed cues inside one long clip: show the cue whose span holds the time since the clip
    // started (plus a short tail after the voice), hide between them.
    if (sc->cues != 0) {
        if (clip_t0 < 0) {
            return; // the scene's clip hasn't started yet
        }
        const s32 elapsed = g_cutscene_timers[slot] - clip_t0;
        int want = -1;
        for (int i = 0; i < sc->cue_count; i++) {
            if (elapsed >= sc->cues[i].start && elapsed < sc->cues[i].end + CUE_TAIL_TICKS) {
                want = i;
                break;
            }
        }

        // Log the voice segments as they start and end, and each cue change, with the time into the
        // clip. With Debug: Voice Segments on, the segment number is also shown on screen.
        const int debug = (int)opt("debug_segments");
        if (sc->cues == intro_cues) {
            int seg = -1;
            for (int i = 0; i < INTRO_SEGMENT_COUNT; i++) {
                if (elapsed >= intro_segments[i][0] && elapsed < intro_segments[i][1]) {
                    seg = i;
                    break;
                }
            }
            if (seg != seg_index) {
                if (seg >= 0) {
                    recomp_printf("[hollywood_subtitles] segment %d starts at +%d.%02ds (cue %d: %s)\n",
                        seg + 1, elapsed / 60, (elapsed % 60) * 100 / 60, want + 1,
                        want >= 0 ? sc->cues[want].text : "-");
                } else if (seg_index >= 0) {
                    recomp_printf("[hollywood_subtitles] segment %d ends at +%d.%02ds\n",
                        seg_index + 1, elapsed / 60, (elapsed % 60) * 100 / 60);
                }
                seg_index = seg;
                if (debug) {
                    cue_index = -2; // force a redraw with the segment label
                }
            }
        }

        if (want != cue_index) {
            if (want != cue_index && cue_index != -2) {
                recomp_printf("[hollywood_subtitles] cue %d %s at +%d.%02ds\n",
                    (want >= 0 ? want : cue_index) + 1, want >= 0 ? "shows" : "hides",
                    elapsed / 60, (elapsed % 60) * 100 / 60);
            }
            cue_index = want;
            if (want >= 0 || (debug && seg_index >= 0)) {
                const char* text = want >= 0 ? sc->cues[want].text : "";
                if (debug && seg_index >= 0) {
                    static char buf[128];
                    int i = 0;
                    const char* p = "[seg ";
                    while (*p) buf[i++] = *p++;
                    int n = seg_index + 1;
                    if (n >= 10) buf[i++] = (char)('0' + n / 10);
                    buf[i++] = (char)('0' + n % 10);
                    buf[i++] = ']'; buf[i++] = ' ';
                    while (*text && i < (int)sizeof(buf) - 1) buf[i++] = *text++;
                    buf[i] = '\0';
                    recomp_subtitle_show(buf, build_style(), text_color());
                } else {
                    recomp_subtitle_show(text, build_style(), text_color());
                }
            } else {
                recomp_subtitle_hide();
            }
        }
        return;
    }

    // While a line is up, hide it once its voice stops streaming (no new reads for a short while), so
    // it doesn't linger until the next clip.
    if (line_index >= 0) {
        const unsigned long activity = recomp_subtitle_voice_activity();
        if (activity != last_activity) {
            last_activity = activity;
            voice_quiet_frames = 0;
        } else if (++voice_quiet_frames == VOICE_QUIET_FRAMES) {
            line_index = -1;
            recomp_subtitle_hide();
        }
    }
}

// --- Hiding the speech bubbles ------------------------------------------------------------------
//
// func_15095D34 draws each piece of a cutscene's speech bubble. Hiding those (so only the subtitles
// show) while keeping Conker's thought-bubble tips needs to tell the two apart, which can't be
// confirmed from the recompiled code alone yet. Until it is, this hook is intentionally inert: it
// never hides a bubble, so no thought bubble is ever lost. The option is wired and documented
// (README) so the behaviour can be turned on once the discriminator is known. A hook (not a patch)
// also can't by itself stop the function drawing; suppressing the bubble will be done host-side
// through the same subtitle primitive once confirmed.
//
// (Kept as a return hook so the mod references the function and the plumbing is in place; it reads
// the option but takes no action.)
RECOMP_HOOK_RETURN("func_15095D34") void hollywood_on_bubble(void) {
    volatile u32 hide = opt("hide_bubbles"); // read so the intent is wired; no action yet (see above)
    (void)hide;
}
