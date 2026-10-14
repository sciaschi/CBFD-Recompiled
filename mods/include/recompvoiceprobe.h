#ifndef __RECOMPVOICEPROBE_H__
#define __RECOMPVOICEPROBE_H__

#include "modding.h"
#include "PR/ultratypes.h"

// Voice-clip probe, exported by the host (host/src/voice_probe.cpp, registered in host/src/mod_api.cpp).
//
// Dev-only tooling for building the subtitle transcript. A mod can't write files from its MIPS code, so
// it hands each finished voice clip and each auto-play event to the host, which appends a line to
// voice_segments.txt in the game's config folder. mods/hollywood_subtitles/match_voice_to_text.py then
// pairs those lines with the game's extracted speech-bubble text to generate the mod's clips[] table.
//
// Call these from the per-frame hook, never from the audio-thread hooks (they must only store numbers).

// recomp_voice_log_clip(scene, script, rom, order, duration_ms, serial, at_tick): one finished clip.
//   scene        the game's scene (D_800BE9F0) when the clip started.
//   script       the cutscene script playing then (0xFF: none), the id func_1501D348 takes.
//   rom          the clip's ROM address (as mp3_play_file / func_151F2960 receives it).
//   order        its place among the clips of that (scene, script), from 1.
//   duration_ms  how long it played, from the decoded MP3-frame count.
//   serial       clips started so far this run.
//   at_tick      the cutscene clock (2 ticks a game frame) when it started; -1 outside a cutscene.
RECOMP_IMPORT("*", void recomp_voice_log_clip(unsigned long scene, unsigned long script, unsigned long rom,
    unsigned long order, unsigned long duration_ms, unsigned long serial, long at_tick));

// recomp_voice_log_dialog(event, scene, script, a, b): an auto-play event (see voice_probe.cpp for the
// meaning of a and b per event).
#define VOICE_DIALOG_START   0
#define VOICE_DIALOG_END     1
#define VOICE_DIALOG_NOSTART 2
#define VOICE_DIALOG_STALLED 3
#define VOICE_DIALOG_CAP     4
#define VOICE_DIALOG_RESUME  5
#define VOICE_DIALOG_DONE    6
#define VOICE_DIALOG_SKIPPED 7 // ended early: skipped by hand (or its clock jumped to the end)
RECOMP_IMPORT("*", void recomp_voice_log_dialog(unsigned long event, unsigned long scene, unsigned long script,
    unsigned long a, unsigned long b));

// recomp_voice_dialog_done(scene, script): 1 if voice_segments.txt already has this dialogue played to
// its end (dialog=end), from any earlier run or this one; auto-play passes over those.
RECOMP_IMPORT("*", unsigned long recomp_voice_dialog_done(unsigned long scene, unsigned long script));

// Auto-play resume, so a run survives a dialogue that crashes the game: the mod saves the next index
// before each one; after a relaunch it loads it and carries on past the one that crashed.
//   recomp_voice_nav_save(next_index), recomp_voice_nav_load() (0 if none), recomp_voice_nav_clear().
RECOMP_IMPORT("*", void recomp_voice_nav_save(unsigned long next_index));
RECOMP_IMPORT("*", unsigned long recomp_voice_nav_load(void));
RECOMP_IMPORT("*", void recomp_voice_nav_clear(void));

#endif
