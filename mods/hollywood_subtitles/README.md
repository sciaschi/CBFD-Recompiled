# Hollywood Subtitles

Movie-style subtitles for **Conker's Bad Fur Day: Recompiled**.

The game is fully voice acted and draws no dialogue text: cutscenes show comic speech bubbles, not
words. This mod shows the spoken lines along the screen, like a film. The wording comes from a
transcript in the mod; **the timing comes from the game's own voice playback**: which recording it's
playing, and how far into it the voice is.

Covered so far:

- **Chainsaw boot** (scene `0x21`): "Stupid logo!", "There, much better.", "How can… get it… Ah!",
  "Marvelous."
- **Throne-room monologue** (scene `0x18`, the campaign intro), all of it: "Well. There I am. Conker the
  King…" through "It's what I call… a bad fur day."


## Options (Mods menu)

| Option | What it does | Default |
|---|---|---|
| Enable Subtitles | Master switch. | On |
| Intro Cutscene Subtitles | The chainsaw boot and the throne-room monologue. | On |
| General Game Subtitles | The rest of the game's cutscenes (as the transcript grows). | On |
| Hide Speech Bubbles | Doesn't draw the game's comic speech bubbles, so the subtitles take their place. The dialogue still plays (and waits for B) as before. | On |
| Position / Text Size / Background Box / Text Color | How the subtitle looks. | Bottom / Normal / Medium / White |
| Debug: Voice Clips | Shows the recording's ROM address and how far into it the voice is (`[0x015E1FD8 12.34s]`) before each line, for writing the timings. | Off |
| Dev: Capture Voice Clips to File | Logs every voice clip that plays (scene, cutscene script, ROM address, order, length, cutscene tick) to `voice_segments.txt` in the data folder, while you play. | Off |
| Dev: Auto Play All Dialogues | Teleports straight to every dialogue in the game, one after another, and logs its clips and how it ended (no save progress needed). See below. | Off |
| Dev: Auto Play Skips Captured Dialogues | With Auto Play: passes over dialogues the log already has played to their end, so a capture can be collected over several sittings. | On |

## How it works

The game's own code is untouched. Two parts:

1. **Host** (`host/src/subtitles.cpp`): a subtitle overlay, a recompui Label like the FPS counter. It
   exports `recomp_subtitle_show(text, style, color)` and `recomp_subtitle_hide()` to mods (see
   `mods/include/recompsubtitles.h`) and holds no subtitle logic of its own.
2. **Mod** (`src/hollywood_subtitles.c`): all the subtitle logic. It watches the game's MP3 voice player
   (Rare's library, named in [DevOldSchool's decomp](https://github.com/DevOldSchool/conkers-bfd-decomp)):
   - `mp3_play_file(romaddr, filesize)` (`func_151F2960`) starts each recording, with its ROM address.
     The mod hooks it to know which recording is playing.
   - `mp3main_continue_file` (`func_151F86B0`) decodes the recording's next MP3 frame: 576 samples at
     22,050 Hz, 26.1 ms. The mod counts them, so it knows how far into the recording the voice is,
     exactly. The count stops while the game is paused, as the player does.
   - Each game frame (the hook on `func_1501BBB8`), it shows the line whose time holds that position.

   The transcript lists each recording by ROM address, with its lines as times (ms) from the
   recording's start. A recording that is one line (most of the chainsaw boot) shows it for as long as
   it plays.

Measured on Windows: the throne monologue is one 38.8 s recording (ROM `0x015E1FD8`, 1,484 frames) in
every run, with two pauses or none. Its 20 speech segments were measured from its decoded audio in the
game, and each one's words noted by ear from the game's own output, cut at them. (The first version
decoded the recording offline past its end, to 55.75 s, and spread the speech over that, and timed lines
with the cutscene timer, which falls behind the voice whenever the game draws fewer than 30 frames a
second: its lines drifted later and later.)

Two rules the code keeps:

- **The player's hooks only store numbers.** They run on the game's audio thread, which the runtime
  schedules with the game's other threads; printing or reading a clock there hung the game in testing.
- **No hook on a function `conker.toml` hooks** (a mod's hook replaces the host's): so none on
  `func_15095D34`, the speech bubbles' pieces, which the host hooks for widescreen and a crash fix.

**Hide Speech Bubbles** hooks the bubble drawer one level up instead, `func_1516B6BC` (found from the
recompiled call chain `func_15095D34` <- `func_15095A90` <- `func_15095A48` <- `func_1516B6BC`). It
takes the display list in `$a0` and returns its new end in `$v0`; the mod notes the start on entry and,
on return, turns what it wrote into no-ops (`G_NOOP`), only within a sane bubble-sized range. The
drawer still runs, so the game's bubble logic and the host's hooks are untouched. Tested in game:
the bubbles are gone and the dialogue plays as before.

## Adding lines

### By hand (one scene)

1. Turn on **Debug: Voice Clips** and play the scene. Each recording that starts is logged
   (`[hollywood_subtitles] clip 0x… starts (no lines for it)`), and while one with lines plays, its
   address and the voice's time show on screen.
2. Add the recording to `clips[]` with its lines as `{ start_ms, end_ms, "text" }`, or one
   `{ 0, WHOLE_CLIP, "text" }` for a recording that is one line.

### With the tools (the speech-bubble scenes)

The game's speech bubbles are text in the ROM, so for the scenes that have them the lines don't have
to be typed by ear. Two tools build the transcript (both are Python, standard library only, and both
read only your own ROM / your own game's data; neither ships game text):

1. **`extract_bubble_text.py`** pulls the speech-bubble text out of your ROM into `bubble_text/`
   (one file per scene). See its header for details.

   ```sh
   python mods/hollywood_subtitles/extract_bubble_text.py "path/to/conker.z64" \
       --out mods/hollywood_subtitles/bubble_text
   ```

2. **In game**, capture the voice. Either play the scenes with **Dev: Capture Voice Clips to File**
   on, or, faster, turn on **Dev: Auto Play All Dialogues**: it teleports straight to every dialogue
   of the game, one after another, without any save progress. Every conversation is a scene script
   (bank 6: scene N's script S, the `[S.T]` blocks in `bubble_text/`), and the game plays one with
   `func_1501D348(scene, script, 0, 0, 0)`, what its own trigger zones and the Chapters menu call.
   The run loads each script's scene if it isn't in play (`func_1501C730(6, scene, 0, 0, 1)`, as Skip
   Intro does) and starts the script; the list of all 483 scripts is `src/dialog_list.h`, generated
   from the ROM by `make_dialog_list.py` (numbers only, no text).

   A dialogue is left to play to its own end, voiced or not, while the cutscene clock moves. When the
   clock stops and nothing speaks, it's waiting for input (a bubble's "press B"), and the run taps B
   once. Only if B stops helping (or after 3 minutes) is it ended with L (keep **Skip Any Cutscene**
   on). Every clip is logged with its scene, cutscene script, ROM address, order, length and the
   cutscene tick it started at, and each dialogue with how it ended: `end`, `skipped` (by hand),
   `stalled`, `cap` or `nostart`. The run saves its place before each dialogue, so if one crashes the
   game a relaunch carries on past it; dialogues already captured to their end are skipped.

   To start a run over: delete `voice_nav_progress.txt` in the data folder (from the first dialogue,
   still skipping captured ones), and move `voice_segments.txt` aside too to replay everything.

3. **`match_voice_to_text.py`** pairs the logged clips with the bubble text, dialogue by dialogue,
   and writes a C fragment for `clips[]` and a report of the run:

   ```sh
   python mods/hollywood_subtitles/match_voice_to_text.py \
       "$HOME/Library/Application Support/ConkerRecompiled/voice_segments.txt" \
       --bubbles mods/hollywood_subtitles/bubble_text --out clips_generated.c --report dialog_report.txt
   ```

   Of each dialogue played more than once, the best run is kept (played to its end, over stalled,
   over skipped). Where a dialogue's clip count equals its bubble count, clip N is bubble N, emitted
   as a ready-to-paste single-line entry (`{ 0, WHOLE_CLIP, "the bubble's text" }`). Where they differ
   (many dialogues play one recording over several bubbles), the dialogue is listed with its clips
   and bubbles side by side, to split by hand. Paste the entries into `clips[]` and rebuild.

The intro cutscenes (the chainsaw boot and the throne monologue) have no speech bubbles, so they stay
hand-written. `bubble_text/`, `voice_segments.txt` and the generated C are your ROM's text and are
git-ignored: keep them to yourself.

## Status / open questions

- **One recording, several bubbles.** Most dialogues play a single recording over a few bubbles, so
  the matcher can't time each bubble yet. The game moves the bubbles in time with the voice, so
  logging the recording's position as each new bubble shows would give every line's start and end.
- Whether Hide Speech Bubbles also hides Conker's thought bubbles is still to be checked.
- The game's lip-sync records (`L:` records in 220 of the 453 recordings, passed to a callback at
  `0x800E0E00`) could fine-tune where lines start, but the monologue has none, so the timing doesn't
  rely on them.
- The transcript is built into the mod (a mod's MIPS code can't read files).

## Building

From the repository root (macOS needs `brew install llvm lld`):

```sh
sh install_mod.sh mods/hollywood_subtitles
```

builds the `.nrm` (via `mods/build_mod.sh`) and copies it into the game's data-folder `mods`
directory, which is where the game loads mods from. The host needs the subtitle overlay, so build it
with `./build.sh` too.
