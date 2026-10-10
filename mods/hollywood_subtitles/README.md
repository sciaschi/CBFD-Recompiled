# Hollywood Subtitles

Movie-style subtitles for **Conker's Bad Fur Day: Recompiled**.

The game is fully voice acted and draws no dialogue text: cutscenes show comic speech bubbles, not
words. This mod shows the spoken lines along the screen, like a film. The wording comes from a
transcript in the mod; **the timing comes from the game's own voice playback**, not from timers.

Covered so far:

- **Chainsaw boot** (scene `0x21`): "Stupid logo!", "There, much better.", "How can… get it… Ah!",
  "Marvelous." In sync.
- **Throne-room monologue** (scene `0x18`, the campaign intro): "Well. There I am. Conker the King…"
  through "…a bad fur day." Lines 1–5 are in sync; from line 6 the mapping to the voice segments
  still needs fixing (see Status).

## Options (Mods menu)

| Option | What it does | Default |
|---|---|---|
| Enable Subtitles | Master switch. | On |
| Intro Cutscene Subtitles | The chainsaw boot and the throne-room monologue. | On |
| General Game Subtitles | The rest of the game's cutscenes (as the transcript grows). | On |
| Hide Speech Bubbles | Wired but inert for now (see Status). | On |
| Position / Text Size / Background Box / Text Color | How the subtitle looks. | Bottom / Normal / Medium / White |
| Debug: Voice Segments | Shows `[seg N]` before each line during the monologue, for tuning. | Off |

## How it works

The game's own code is untouched. Three parts:

1. **Runtime (one passive line).** `recomp/n64modernruntime.patch` adds a weak hook,
   `conker_on_rom_read(addr, size)`, at the top of `do_rom_read` in
   `librecomp/src/pi.cpp`. It only observes ROM→RAM DMAs. Left undefined, it does nothing.
2. **Host** (`host/src/subtitles.cpp`). Defines that hook and tracks the MP3 voice playback:
   - When a voice clip starts, the MP3 decoder loads its work buffers with two big reads
     (~17 KB and ~42 KB). The next 2048-byte read is the clip's first MP3 frame block, and its ROM
     address identifies the clip (stable per recorded line).
   - While the clip plays, its MP3 streams in 2048-byte reads; when they stop, the voice has ended.
   - The hook does only relaxed atomic stores: it runs on the audio/DMA thread, which is
     timing-sensitive (`func_1000A03C`).

   It exports to mods: `recomp_subtitle_show(text, style, color)`, `recomp_subtitle_hide()`,
   `recomp_subtitle_voice_event()` (clips started), `recomp_subtitle_voice_clip()` (ROM address of
   the clip playing) and `recomp_subtitle_voice_activity()` (rises while the voice streams). The
   text is drawn as a recompui Label, like the FPS counter. See `mods/include/recompsubtitles.h`.
3. **Mod** (`src/hollywood_subtitles.c`). Holds all the subtitle logic, run from the per-frame hook
   on `func_1501BBB8`:
   - **One clip per line** (chainsaw boot): the n-th voice clip of the scene shows the n-th line;
     clips that aren't speech (the chainsaw) show nothing. Each line hides when its voice stops.
   - **One long clip** (the throne monologue is a single ~56 s clip at ROM `0x115E1FD8`): timed
     cues inside the clip, anchored to the moment the game starts it and counted in the cutscene
     timer (60 per second). The cue times are the clip's real speech segments, found by decoding
     the clip's MP3 and detecting the pauses (25 segments). Only the wording comes from the script.

Scene ids seen while testing: `0x25` logos, `0x21` chainsaw boot, `0x1D` the bar (several cutscenes
by cut id: 3 = Berri dancing, 4 = drunk Conker), `0x18` campaign intro.

## Status / open questions

- **Monologue mapping:** the segment *times* are measured, but which words fall in which segment was
  estimated from the length of each line. Lines 1–5 ("Well." → "Who'd a thought that?") are
  confirmed in sync; from line 6 ("But how did I come to this,", segment 7) they drift, e.g. "And who
  are those strange fellows…" spans segments 9–13, about 9 s. Needs the words per segment, by ear
  (Debug: Voice Segments) or by offline speech recognition.
- **Hide Speech Bubbles** is inert: telling `func_15095D34`'s speech bubbles from Conker's thought
  bubbles isn't confirmed yet, so nothing is hidden (no thought bubble is ever lost).
- The transcript is built into the mod (a mod's MIPS code can't read files).

## Building

From the repository root (macOS needs `brew install llvm lld`):

```sh
sh install_mod.sh mods/hollywood_subtitles
```

builds the `.nrm` (via `mods/build_mod.sh`) and copies it into the game's data-folder `mods`
directory, which is where the game loads mods from. The host needs the subtitle exports, so build it
with `./build.sh` too.
