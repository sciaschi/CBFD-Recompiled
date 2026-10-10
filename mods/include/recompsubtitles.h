#ifndef __RECOMPSUBTITLES_H__
#define __RECOMPSUBTITLES_H__

#include "modding.h"
#include "PR/ultratypes.h"

// Subtitle overlay, exported by the host (host/src/subtitles.cpp, registered in host/src/mod_api.cpp).
// A mod shows and hides a line of text drawn over the game, like a film's subtitle, choosing how it
// looks. The host holds no subtitle logic: it only renders what the mod last asked for.
//
// recomp_subtitle_show(text, style, color_rgb):
//   text       a zero-terminated UTF-8 string (the host copies it; it need not outlive the call).
//   style      a packed word:
//                bits 0-1  position: 0 bottom, 1 top, 2 middle
//                bits 2-3  size:     0 small,  1 normal, 2 large
//                bits 8-15 background box opacity 0-255 (0 = no box)
//   color_rgb  text color as 0xRRGGBB.
// recomp_subtitle_hide(): hides it.
//
// Call show every frame the line should stay up (it's cheap and idempotent: the host only rebuilds
// its UI when the text, style or color actually change), and hide when nothing should show.

RECOMP_IMPORT("*", void recomp_subtitle_show(const char* text, unsigned long style, unsigned long color_rgb));
RECOMP_IMPORT("*", void recomp_subtitle_hide(void));

// How many MP3 voice-clip starts the host has detected (host/src/subtitles.cpp). Read it each
// frame; when it increases, a new voice line has started playing, so advance to the next subtitle.
// This drives event-driven subtitles synced to the actual voice instead of a timer.
RECOMP_IMPORT("*", unsigned long recomp_subtitle_voice_event(void));

// ROM address of the voice clip playing now (its first MP3 frame): stable per recorded line.
RECOMP_IMPORT("*", unsigned long recomp_subtitle_voice_clip(void));

// Counts the current clip's streaming reads: rising while the voice plays, still once it ends.
RECOMP_IMPORT("*", unsigned long recomp_subtitle_voice_activity(void));

// Style helpers.
#define SUBTITLE_POS_BOTTOM 0u
#define SUBTITLE_POS_TOP    1u
#define SUBTITLE_POS_MIDDLE 2u

#define SUBTITLE_SIZE_SMALL  0u
#define SUBTITLE_SIZE_NORMAL 1u
#define SUBTITLE_SIZE_LARGE  2u

// Builds the packed style word from position (0-2), size (0-2) and background opacity (0-255).
#define SUBTITLE_STYLE(pos, size, bg_opacity) \
    (((unsigned long)(pos) & 0x3u) | (((unsigned long)(size) & 0x3u) << 2) | (((unsigned long)(bg_opacity) & 0xFFu) << 8))

#endif
