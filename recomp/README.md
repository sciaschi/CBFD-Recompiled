# Static recompilation (N64Recomp)

Turns the game's code into C with [N64Recomp](../tools/N64Recomp) and runs it on
[N64ModernRuntime](../tools/N64ModernRuntime) through the host application in
[`host/`](../host). The code comes from your ROM; which functions there are, and
where, comes from the decompilation, through the committed
[`conker.us.syms.toml`](conker.us.syms.toml).

**Status:** the game runs on Windows in a window, rendered by
[RT64](../tools/rt64), and can be played with a game controller or the
keyboard. The boot screens, the 3D intro, the attract-mode cutscenes and the
menus render correctly, and there is sound: music, effects and the MP3
voice acting. The same window build (RT64 over Vulkan, input and sound) also
runs on Linux; it has been tested under WSL with software Vulkan.

## Building and running

`build.cmd` (Windows) and `build.sh` (Linux) in the repo root do everything; see the
main README. The recompilation step is [`recompile.py`](recompile.py), which only
needs Python's standard library, N64Recomp and RSPRecomp:

1. [`unpack_rom.py`](unpack_rom.py) unpacks the code from `conker/baserom.us.z64`
   into `recomp/build/`: the header, boot code and `.init` as they are, `.game`
   decompressed (Rare's rzip: an XORed table of raw deflate blocks, then the data),
   and `.debugger`. That's the same image the decompilation's extraction makes, and
   its SHA-1 is checked. A copy gets the words in
   [`code_rewrites.txt`](code_rewrites.txt) (see `prepare_elf.py` below).
2. N64Recomp recompiles it with `conker.toml`, which reads the functions from
   `conker.us.syms.toml`.
3. `emit_tlb_pages.py` writes `RecompiledFuncs/tlb_pages.c` (see the memory layout).
4. RSPRecomp recompiles the audio microcode.

It records hashes of the files that shape `RecompiledFuncs/` (`conker.toml`, the
symbols, the N64Recomp patch, ...), and CMake stops with "RecompiledFuncs/ is out of
date" when any of them has changed since: run the build script again after pulling.

### Developers: regenerating the symbols

`conker.us.syms.toml`, `code_rewrites.txt` and `mods/syms/` are generated from the
decompilation, on Linux or in WSL, after building it (`make` in `conker/conker`,
or `./build.sh --decomp`, which does all of this):

```sh
sh recomp/run.sh
```

It runs `prepare_elf.py` on the decomp's ELF, has N64Recomp dump the prepared ELF's
functions and data symbols (`--dump-context`, with `conker.toml` pointed at the
ELF), and turns those into the symbols file with [`make_syms.py`](make_syms.py):
every ELF section in the ELF's order, so the section indices stay the same;
`recomp_entrypoint`, at the KSEG0 alias 0x80001000 of `.init`'s first function,
with its ROM address given directly (the N64Recomp patch reads a `rom` field for
that); and where two names share an address, the descriptive one first. The
rewrites are the words where the prepared ELF's code differs from the ROM's. Then
it recompiles with `recompile.py`. Recompiling from the symbols file gives the same
code as from the ELF, function for function
([`compare_recomp_output.py`](compare_recomp_output.py) compares two outputs).
Commit the regenerated files.

`tools/N64Recomp` (ffb39cd) and `tools/N64ModernRuntime` (cdf5abb) are
untracked checkouts, so their changes live in the patch files here.

`tools/rt64` is an untracked checkout of rt64/rt64 at 4337374, and
`tools/RecompFrontend` one of N64Recomp/RecompFrontend at b1a1477, both with their
submodules (`git submodule update --init --recursive`). `build_windows.cmd` finds
Visual Studio, sets up the x64 compiler environment and builds `host/build-win`
(RelWithDebInfo, so the crash handler can name functions). Extra arguments go to
`cmake --build`, e.g. `host\build_windows.cmd -- -j 8 -k 0`. SDL2 and DXC come
from RT64's bundled dependencies and are copied next to the exe, and so is
`host/assets/` (the menus' fonts, icons and the launcher picture).

The game opens on RecompFrontend's launcher: Start Game (or Load ROM, the first
time), Controls, Settings, Mods and Exit. Settings has the General (rumble),
Graphics (resolution, aspect ratio, fullscreen, anti-aliasing, framerate, HUD
placement), Controls (remapping for keyboard and controller), Sound (volume) and
Mods tabs; Esc or the controller's menu button opens it in game too. The ROM,
saves and settings live in `%LOCALAPPDATA%\ConkerRecompiled` (or next to the exe
when a `portable.txt` is there); a `conker_data/` from older builds next to the exe
is copied over once. `--rom PATH` stores a ROM from the command line, `--seconds N`
starts the game directly (no launcher) and quits after N seconds, and `--headless`
runs without a window, input or sound (then with `conker_data/` next to the exe).

`host/capture_run.ps1` runs the game for a while, saves screenshots of the window
at given times and can press keys, for checking a build without watching it:
`powershell -File host\capture_run.ps1 -Seconds 60 -Shots "20,40" -Keys "30:Enter"`
(`-Launcher` opens the launcher instead of starting the game). It sets
`CONKER_NO_CONTROLLER=1`, which makes the game ignore controllers, so a test run
doesn't pick up someone playing on the same machine.

## Debugging tools

- `sh host/debug_run.sh [SECONDS]`: runs under gdb and prints the crashing
  thread's backtrace, or every game thread's after SECONDS. Recompiled
  functions are named after their vram.
- `sh host/difftest.sh <func> [hit]`: snapshots RAM and registers at a call in
  the recompiled game, then runs the **original machine code** for the same
  call in [`mipsinterp.py`](mipsinterp.py) (a small VR4300 interpreter) and
  compares `$v0` and every RDRAM word written. This found most of the bugs below.
- `sh recomp/check_unresolved.sh`: calls that neither the output nor the
  runtime defines.
- The host prints a backtrace on SIGSEGV.

## Memory layout

| section      | vram       | notes |
|--------------|------------|-------|
| `.init`      | 0x10001000 | TLB entry 0 aliases KSEG0: the boot code runs at 0x80001000, then jumps to 0x1000xxxx |
| `.init_data` | 0x800290D0 | |
| `.game`      | 0x15000000 | demand-paged: a TLB-miss handler (`func_10005C2C`) pages code in from compressed ROM |
| `.game_data` | 0x80082B20 | `.game`'s rodata, data and jump tables |
| `.debugger`  | 0x16000000 | code plus rodata |

Most data is in KSEG0, but the hand-written math code keeps lookup tables
inside its own `.game` pages (e.g. 0x150AA318), and the debugger reads its
rodata at 0x1600xxxx. The runtime reserves 4 GB for game memory and maps only
RDRAM, so the host maps those ranges and copies the original segments in
(`emit_tlb_pages.py` -> `RecompiledFuncs/tlb_pages.c`).

The boot code sets `Status.FR` and the hand-written math uses odd FPRs, so
`uses_mips3_float_mode = true`, and the host puts every thread context into FR=1
mode (which also points `ctx->f_odd` at the odd registers).

## `prepare_elf.py`

Runs before N64Recomp.
- Overlays the code sections with the **original** bytes
  (`conker/assets/*.us.bin`). Drift is 0, so every function is at its original
  address, and decomp functions whose C doesn't match yet can't change
  behaviour (e.g. `func_15125690`, whose C reconstruction adds a load).
- Sizes the asm `glabel` functions, extends functions that fall through into
  the next label, and re-homes glabels that `undefined_funcs_auto.txt` turned
  into ABS symbols.
- Creates function starts for branch targets in the middle of other routines,
  for code addresses the game takes as values (callbacks, `D_` continuation
  labels in hand-written asm, a bare `jr $ra` used as an empty callback), and
  for `EMBEDDED_DATA` boundaries.
- Recompiled calls never set `$ra`, so hand-written asm that treats it as a
  value is rewritten:
  - `jr rX` where rX is a copy of `$ra`, or a link register every caller loads
    with its return point, becomes `jr $ra`.
  - `$ra = ret; j F` becomes `jal F`, and the function is re-sized to take in
    the code the call returns into (`func_150A7A00` stores a transform's W
    there; cut off, the camera spun around Conker at the start of Hungover).
  - A loop head kept in `$ra` moves to `$k1`, and its `jr $ra` gotos become
    `jr $k1`.
- Gives .game's `<name>2` libultra duplicates their libultra names when
  N64Recomp skips or replaces them.

## `conker.toml`

- Stubs the TLB paging system, the exception/thread dispatch code and the
  debugger's TLB dump.
- Nops the boot code's TLB and Status writes and the pre-NMI thread's
  `__osViInit`, and keeps `func_10005B04`'s page-pool allocations while
  dropping its cop0 writes.
- Replaces direct hardware and KSEG1 loads with hooks into the host: the
  audio thread's `AI_LEN` read, Rare's PIO ROM copy (`func_1000480C`) and an
  anti-piracy ROM read (`func_15001A08`), plus an uncached RDRAM pointer
  retargeted to KSEG0.
- Creates libultra's `__osEepromTimerQ` after the game's `osContInit` (the
  runtime's `osContInit` doesn't). Rare's EEPROM code paces its accesses with
  timers on it; without it the game froze on a black screen at the first save,
  after the intro cutscene.
- Turns the script interpreter's abort (`func_150AE280`, which reloads the `$sp`
  saved by `func_150ADAF0` and jumps to its epilogue) into a `setjmp`/`longjmp`.
  Recompiled as plain calls, it returned only from the innermost function and
  crashed in the Panther King cutscene.
- Makes the fall-through into `func_150AB1F0`'s internal subroutine at
  0x150AC1C4 a call followed by a jump to 0x150AB6F0, in `func_150AC1B4` and
  in `func_150AC0F8` (which falls through `func_150AC1B4` into the same code).
  The subroutine returns with `jr $t0`, which `prepare_elf.py` rewrites to
  `jr $ra` for its jal caller; on the fall-through path (`$t0` = 0x150AB6F0, no
  jal) that return left the routine without its epilogue, `$sp` ended up 0x268
  low, and the camera code's saved registers came back as garbage (a spinning
  camera, then a crash in `func_1512BB10` shortly after Hungover starts).
  `prepare_elf.py` warns about any such fall-through entry.
- Makes `func_10008CE8`'s busy-wait yield (issue #66). It starts a song on a
  sequence player: it stops the player, then counts up to 2,000,000 (and then
  4,000,000) until the audio thread reports it stopped. On the N64 the audio
  thread preempts the loop; the runtime switches game threads only when one
  waits or yields, so the loop ran out with the player still playing, and the
  new song went unplayed (the bar's music and chatter played on after loading a
  save from the menu a game over returns to). A hook at the head of each loop
  (`conker_spin_wait_pass`, `host/src/ultra_extras.cpp`) yields for up to 1 ms
  and counts it as the ~3,000 passes the N64 would make in that time, so the
  loop still gives up after about as long.
- Makes a song just started read as playing (issue #66). Starting one only
  queues an event for the audio thread, and the player reads as stopped until
  the audio thread has taken it up, which here can be a frame or more later. The
  game's music manager then took the player for free while its song played on:
  the outside ambience went on in the bar, later songs went to the wrong
  players, and in the stone dragon's mouth the level's music played on, very
  loud. Waiting for the audio thread when the song starts doesn't work (it takes
  the song up only once the game goes on), so a player whose song was just
  started reads as playing until it plays, is stopped, or 500 ms pass
  (`conker_song_started`, `conker_song_state`, `conker_song_stopped`).

## Local changes to the tools

N64Recomp (`n64recomp.patch`):
- finds a jump table in the section that holds it (Conker's are in
  `.game_data`), and treats a `jr` through a struct table as an indirect jump.
- tolerates stack accesses below `$sp`.
- **only turns branch targets inside a function into labels.** Out-of-function
  targets were shifting every following label, which broke loops in
  hand-written asm.
- `N64RECOMP_KEEP_GOING=1` reports every failing function.
- division by zero gives the VR4300's results instead of trapping on the host
  (the hand-written `func_150A3CBC` divides by zero during the attract mode).

N64ModernRuntime (`n64modernruntime.patch`):
- PI DMA completion posts the request's `OSIoMesg` pointer, as libultra does,
  instead of 0 (Conker's audio code reads it).
- `osPiStartDma` fills in the `OSIoMesg` (`dramAddr`, `devAddr`, `size`,
  `retQueue`) as libultra does. Rare's instrument cache (`func_10009CBC` loads,
  `func_1000A03C` collects) matches finished DMAs by `mb->dramAddr`; without it
  no instrument ever finished loading, every MIDI channel waited forever, and
  only the MP3 voices played.
- `ultramodern::set_running_thread_variable` keeps the game's
  `__osRunningThread` pointing at the running thread (Conker reads it
  directly).
- GCC-only warning flags are skipped under MSVC.
- A mod's patch keeps the patched page executable while it's written. It was
  made read-write only, and another thread running code on the same page (RT64's
  idle thread, every millisecond) crashed as the game started with mods on.
  macOS keeps read-write only, as Apple Silicon doesn't allow both.

RT64 (`rt64.patch`): Conker's graphics microcode, F3DEX2 with Rare's changes,
as in GLideN64's `F3DEX2CBFD`. RT64 identifies a microcode by a hash of its
text and data. Conker's two builds ("F3DEXBG.NoN fifo 2.08" and "F3DEX.NoN fifo
2.08") are added to the database and mapped to a new `GBI_F3DEX2CBFD`
(`src/gbi/rt64_gbi_f3dex2cbfd.cpp`). It handles:
- a four-triangle command at opcodes 0x10-0x1F;
- light counts in bytes (`w1/48`);
- up to 12 lights, with point lights and a separate per-vertex normal array
  (`G_MV_NORMALES`);
- a coordinate modifier (`G_MW_COORD_MOD`);
- `G_LOAD_UCODE` reused to switch on the "advanced" lighting.

RT64's shaders can't see those normals, so lighting is computed on the CPU into
the vertex colours (`RSP::lightVerticesCBFD`), and `G_LIGHTING` is hidden from
the shaders. The point lights' positions are in clip space, as GLideN64 has
them: each vertex is transformed by the modelview and projection, then scaled by
the coordinate modifier, before its distance to a light is measured (with the
untransformed position, the lights missed what they light: the lantern and walls
outside the bar, the bar's inside, the N64 logo). Texture generation
(`G_TEXTURE_GEN`, environment mapping: the gold of the Rareware logo, glass),
which needs the same normals, is done on the CPU too
(`RSP::textureGenVerticesCBFD`): each vertex's normal against the look-at
vectors, turned into model space as the lights' directions are, gives its texture
coordinates, written into the vertex's s and t (32 times the generated
coordinate, so the texture scale then applies as it does with texture
generation), and `G_TEXTURE_GEN` is hidden from the shaders as well.

The patch also changes widescreen. RT64 widens a projection only if its
scissor covers the whole width of its framebuffer's combined scissor. Conker's
3D scissor stops 2 pixels short of each edge of its 292-wide frame. Some effects
(water, diving) also draw a rectangle with a full-frame scissor. In those frames
the 3D fell back to 4:3, so the picture flickered between the two, and the water
surface was cut at the 4:3 edges. Both checks, the wide viewport in
`FramebufferRenderer` and the projection adjustment in `ProjectionProcessor`,
now allow 4 pixels of slack (`CoversWidthSlack`). They must agree, or the 3D is
stretched.

A framebuffer pair whose triangles were all culled has no combined scissor, but
its projection can share its transforms with a pair that did draw. Comparing
with the null scissor turned the widening off for both, and the 3D flickered
between 4:3 and widescreen (the hub, near the naughty/nice sign). Both checks
now compare with the projection's own scissor in that case.

The patch adds a rect aspect, `G_EX_ASPECT_ZOOM`: stretched to the width like
`G_EX_ASPECT_STRETCH`, and scaled as much vertically about the middle of the
scissor, so the rectangle keeps its proportions and loses its top and bottom.
The pause menu's background, a saved copy of the 4:3 frame, is drawn with it.

A rectangle whose scissor spans the frame (with the same slack) is clipped at
the edges of the widened frame, as widened 3D is, instead of at the 4:3 area.
Together with the game-side hooks in `host/src/widescreen.cpp`, that keeps
screen-space sprites (bubbles, bees) whole past the 4:3 edges. The hooks emit
each sprite as RT64's extended texture rectangle with signed corners. It takes
the same three commands as the game's `G_TEXRECT`. The enable goes where the
sprite's pipe sync was, so the display lists don't grow; they're allocated to
fit what the game writes.

Cutscene speech bubbles (`func_15095D34`, issue #59) are clamped at the screen's
left edge the same way. A piece near or past it is drawn that many whole pixels
to the right, so the game doesn't clamp it, and moved back as an extended
rectangle. They write no sync to hold the enable, so it's put early in the
frame, in place of the pipe sync each camera's pass starts with.

A camera's background fill (`func_151103C8`, its colour under everything it
draws) ends exactly at the scissor's right edge, and RT64 lines such a rectangle
up with the window's: its last column went out to the window's right side. The
widened 3D covers it in play, but on the screens before the N64 logo, which draw
only a picture over the 4:3 frame, it showed as a thin blue line near the right
edge. In widescreen that fill ends a pixel short, inside the frame
(`conker_camera_background_fill`).

A fill rectangle in fill mode is drawn as a clear of that part of the framebuffer
(`FramebufferRenderer`, `InstanceDrawCall::Type::FillRect`). The clear's rectangle
is now clipped to the framebuffer first. A clear outside it is undefined in Vulkan
(`vkCmdClearAttachments`), and the cutscene pillarbox bars
(`host/src/cutscene_aspect.cpp`) reach past both sides of the window. NVIDIA's
drivers clipped it as wanted, but on the Steam Deck (RADV) the left bar, which
starts left of the window, wasn't drawn: the picture showed widescreen on that
side, and old frames stayed in the strip (issue #82).

The game's 3D and its fill rectangles stop 2 pixels short of each side of its
292-pixel frame (its camera scissor, and `func_1501A6CC` clamps its fills the
same): a black border the N64 left for the TV's overscan, which showed down
both sides of the picture (issue #72). The Graphics tab's Overscan Borders
(Hidden by default, or Shown) hides it as the picture is shown: the game sets
how much of the picture's width to crop off each side each frame
(`RT64::setPresentationCrop`, 2 of the frame's width in pixels), and the VI
renderer zooms the picture about its middle by as much both ways, so it isn't
stretched and as much comes off its top and bottom. Widening the 3D scissor to
the full frame instead pillarboxed the picture.

The patch also changes frame interpolation (a frame rate above the game's 30).
RT64 draws frames between the game's by pairing each transform with last
frame's; without help it guesses, from draw calls that look alike. Conker's
characters were paired part with part at random, and the game batches their
triangles differently from frame to frame, so they vibrated and came apart. The
game now tells RT64 which is which (`host/src/interpolation.cpp`): each object
drawn by `func_1502CCFC` is wrapped in a matrix group naming it, matched in the
order drawn. In RT64:
- a group with a different number of transforms than last frame can't be paired
  in order (the parts after a change would pair with their neighbours', and the
  model flew apart for a frame). Each transform is paired with last frame's
  closest instead (`GameFrame::alignTransforms`: its position moved on by its
  velocity, plus how differently it's turned), several with the same one if need
  be, and its vertices aren't interpolated. A character's mesh is drawn in
  chunks, one bone's matrix each, cut differently as its animation changes; left
  unmatched, Conker snapped each time it did (issue #76).
- pushing or popping a group starts a new transform, even with the same matrix,
  so the vertices drawn after a group don't count as its own.
- a change of direction alone no longer counts as a teleport (`RigidBody`):
  animations swing parts back and forth, and snapping them on each reversal
  while the rest was interpolated made the model shake.
- the camera is interpolated as a camera (the view matrix's inverse), not as a
  view matrix: lerping the view's translation while the camera turns moves the
  in-between camera off its path.
- vertex motion is interpolated only if it's plausible (under 64 units a frame).
- a camera cut isn't interpolated (`RigidBody::updateCameraCut`). A game that doesn't
  say how its camera interpolates gets it always interpolated, so the frames between
  two cutscene shots showed the camera partway from one to the other, inside the
  scenery (issue #59). A cut is a camera moving over 150 units or turning over 50
  degrees in one game frame, and four times as far as the frame before.
- a frame the game's motion blur is drawn in (`func_151D6778`, Conker drunk)
  isn't interpolated (`Workload::skipInterpolation`, set by the game each frame
  through `RT64::setInterpolationPaused`). The blur blends each frame with a
  copy of the last; blended on the frames drawn between the game's, that copy
  was only a fraction of a frame old, and the blur was all but gone (issue #77).

A character's shadow (`func_15186794`) is the ground under it, clipped anew
every frame and drawn with the shadow's texture projected onto it from the
light. Its vertices can't be paired with last frame's, so it stepped at 30 fps.
Its own group asks RT64 to interpolate only its texture coordinates: RT64 fits
last frame's projection (a perspective map from world position to texture
coordinates, by least squares) and takes each vertex's coordinates last frame
from it, if the fit is close, covers the texture and moves no vertex more than
a quarter of the texture.

The shadow's texture is a silhouette the game draws first, into a 64 by 64
8-bit image (0x800DE080), with a camera it aims at the character from the light
anew every frame, moving it nearer or further and zooming to match (standing
still, its distance and zoom both doubled between two frames, the silhouette the
same size in each). Its view matrix is scaled and skewed, not a rotation: RT64
interpolated it as a camera, taken apart into position, rotation and scale and
put together halfway, and the shadow was drawn larger on the frames between
(issue #76; only at more than 30 fps). A scene drawn into a 64 pixel wide 8-bit
image has its camera interpolated whole instead (`ProjectionProcessor`): each
frame's view and projection together, scaled so the place it's aimed at
(halfway between its near and far planes) is at w = 1, then blended. The
silhouette's points on the texture then move as the texture coordinates on the
ground do. Drawing the silhouettes uninterpolated stopped the expanding but
didn't match the interpolated coordinates, and the shadow shook.

A shadow isn't drawn on the first game frame it's back after frames without it
(`interpolation.cpp`, issue #75): walking into a room, the game draws no shadow
while the room loads, then for one frame lays the shadow from where Conker was
before the room switched over onto the new room's ground, a dark patch (at 30
fps too). And the shadow's texture coordinates are pulled in 2 texels from each
edge (`rt64_rsp.cpp`): on an AMD GPU under Linux, at high resolutions, the
texture's outermost texels came out dark and drew a thin line along the patch's
edge next to Conker (issue #73).

Texture packs named for Rice (issue #63): GLideN64 and Rice Video name each
replacement by its texture's Rice hash (`<name>#<crc>#<format>#<size>[#<palette
crc>]_all.png`), which RT64 can only use through an `rt64.json` pairing it with
RT64's own hash, made from textures dumped while playing (its `texture_hasher`
tool). The patch works the Rice hash out live instead
(`src/hle/rt64_rice_hash.cpp`): a pack whose database's auto path is Rice keeps
its files by Rice hash (`ReplacementDatabase::resolvePaths`), and the first time
a texture is seen while one is loaded (`TextureManager::checkRiceReplacement`),
its Rice names are worked out from the RDRAM its load read, as `dumpTexture`
dumps it and `texture_hasher` hashes it. The texture cache
(`TextureCache::addRiceReplacement`) then gives the texture that file, unless a
database already replaces it; a texture uploaded before the pack was loaded is
checked again. GLideN64 names a palette only for CI textures, but Conker loads a
palette for some RGBA ones too, so both names are tried. The host unpacks a
GLideN64 cache (`.htc`) in the mods folder into such a pack when the launcher
opens or the Mods menu rescans the folder (`host/src/gliden64_packs.cpp`).

`rt64_rice_hash.cpp` is under the **GPL, version 2 or later**, unlike the rest of
RT64 (MIT): its hashing is `texture_hasher`'s, which RT64 keeps under the GPL
because it imitates GLideN64's and Rice Video's. The Rice CRC is GlideHQ's
(Hiroshi Morii, in GLideN64's `TxUtil.cpp`, GPL 2 or later), and the sizes it
hashes (`ReverseDXT`, `CalculateMaxCI`, the tile and block dimensions) are Rice
Video's (mupen64plus-video-rice, GPL 2 or later). Its header says so; see
[the licenses](../README.md#license) for what that means for builds.

RecompFrontend (`recompfrontend.patch`): mouse buttons can be bound, with the
keyboard's controls (and in single player with the controller's too, as both are
read then). recompinput already had a mouse input type, but reading it
was left to do (`// TODO mouse support`). While binding a keyboard control, a
mouse button press is bound (the click that starts binding has gone by by then;
Escape still cancels). The buttons' state is read with the keyboard's each poll
(`SDL_GetMouseState`), so, like the keyboard, they don't reach the game while a
menu is open. They're shown as PromptFont's mouse glyphs: left, middle and
right, and numbers from the side buttons (4, 5) on.

RecompFrontend also lets the game take files the mod loader doesn't, for the
GLideN64 texture packs: `register_mod_file_extension` has the mod installer
(Install Mods, or files dropped on the Mods menu) copy a file with that extension
(`.htc`) into the mods folder as it is, where it rejected anything but a zip, and
`register_mod_scan_callback` is called before the Mods menu rescans the mods
folder (its refresh button, which also follows an install), where the game starts
unpacking a new `.htc`.

## Audio

Conker's audio microcode is an ABI-style ucode like libultra's `aspMain`
(n_audio command numbers), except that command 7 decodes MP3 (the voices). Its
text is at ROM 0x291A0 and its data at 0x2C960. The first 0xF70 bytes are the
main code (IMEM 0x1080). The 0x9C0-byte MP3 overlay (text offset 0xF70) is
DMAed over the main code from IMEM 0x1238 and swaps it back when it's done.
[`audio_ucode.toml`](audio_ucode.toml) describes this to RSPRecomp, which
`recompile.py` runs to produce `RecompiledFuncs/rsp/audio_ucode.cpp`.

The output is 22020 Hz stereo. `host/src/audio_output.cpp` queues it on an SDL
device. The audio thread (`func_100095A0`) sizes each buffer from AI_LEN: 736
frames, or 552 when 249 or more samples are still playing. So the host reports
only what is queued beyond one buffer, as AI_LEN counts only the buffer playing
now. Reporting the whole queue made the game fall behind and pop.

## Host (`host/`)

`main.cpp` registers the game (ROM hash, entrypoint, 16Kbit EEPROM), sets FR
mode, `osCicId` = 6105 (the idle thread won't start the game otherwise) and
`__osRunningThread`. It also registers the TLB-mapped code sections and maps
the code pages. `ultra_extras.cpp` provides `osPiRawReadIo`/`osPiReadIo` (Rare's
anti-piracy checks read real ROM words), `osPfsInit` (Rare's rumble detection),
the KSEG1 read helper and `recomp_syscall_handler` (Conker halts with
`syscall` on fatal errors). `frontend.cpp` wires RecompFrontend in: the SDL
window, recompui's renderer (RT64 plus the menus drawn over it), recompinput for
the keyboard and controllers, and the launcher entry (`supported_games`, which
recompui declares extern). `conker_config.cpp` sets up the settings tabs and
Conker's control descriptions, and `audio_output.cpp` plays the sound at the
Sound tab's volume. `cutscene_aspect.cpp` is the Graphics tab's Cutscene Aspect
Ratio: with 4:3, cutscenes (anything the cutscene system plays, during which the
player can't move: the game marks no difference between a story scene and a B
pad's hint) get black bars over the picture beyond the 4:3 frame, drawn at the
end of the frame's display list by a call to a list of its own. The picture
itself stays widescreen: switching the renderer's aspect ratio instead made RT64
remake every framebuffer at each switch, and a player's game crashed there in
fullscreen at 4K (issue #21). `texture_packs.cpp` registers RT64 texture packs (`.rtz`
files and folders with an `rt64.json`) with the mod loader and adds the Texture
Packs settings tab, and `gliden64_packs.cpp` unpacks GLideN64 texture caches
(`.htc`) in the mods folder into packs RT64 matches by their Rice names (see RT64's
changes above). `mouse_camera.cpp`
is a free orbit camera around Conker, turned by the mouse and, with Right Stick:
Free Camera, the right stick, which then presses no C-buttons while it turns it
(`frontend.cpp`); its settings, with Camera: Field of View, are in
`look_aim.cpp`. `field_of_view.cpp` widens the normal camera's field of view as
func_1510B128 sets it, and works the level's cull scales out for the wider view.
`fps_counter.cpp` is the Graphics tab's Show FPS: a corner counter of the frames
RT64 presented and the game's own, a context of its own that takes no input.
`reticle.cpp` is the General tab's Aiming: Reticle (issue #74, off by default): a
green ring like Conker: Live & Reloaded's, drawn the same way, while Conker aims
the slingshot or the throwing knives (the look mode, `func_15120158`, in its
states 0x2 and 0xA; plain R-Look is 0x0) or in the second aiming mode
(`func_15126378`, the magnum), not zoomed in (the sniper scope has its own) or in
split screen. Both modes aim the camera itself, but the thrown shots leave
Conker's hand below it, pitched up: in the distance they land 20% of the screen's
height above the middle, where the reticle is (closer, a little lower).
Overscan Borders, in the same tab, crops the N64's black border off each side
of the picture (`widescreen.cpp`, with RT64's changes above).
Conker's Detail (Always Highest by default) keeps Conker on his most detailed
model (`model_detail.cpp`): every game frame `func_1502C6E8` picks each object's
model by its distance from the camera (level 0 under 500 units, then further
levels, one closer when nearly still) and stores the level at +0x1C8, so he
flipped between levels 0, 1 and 2 as the camera pulled back and as he started and
stopped walking; a hook sets the level to 0 for him before it's compared with his
last. The multiplayer arenas (`D_800BE616`) keep the original's level 1.
`patches/` holds the headers recompui includes for the
game-side patch code that mods will use. `null_renderer.cpp` is used with
`--headless`, and in a build configured with `-DCONKER_RT64=OFF` (no window, input
or sound). On Linux the window build creates the SDL window with
`SDL_WINDOW_VULKAN` for RT64 (`RT64_SDL_WINDOW_VULKAN`, as in Banjo: Recompiled),
and the recompiled audio microcode is compiled with `-msse4.1`. A crash prints a backtrace (SIGSEGV on Linux; on
Windows an unhandled-exception filter with DbgHelp, which also writes
`crash.log` next to the exe and shows a message box).

## Mods

Mods are `.nrm` files for N64ModernRuntime, installed by dropping them into
`mods/` in the data folder (or through the launcher's Mods menu, which also
enables and configures them). Each mod in `mods/` here has a `mod.toml` and C
sources in `src/`; build one in WSL from the repo root with

    sh mods/build_mod.sh mods/skip_cutscenes

which compiles for MIPS with clang, links with `mips-linux-gnu-ld` (`ld.lld`
isn't needed) and runs RecompModTool, leaving the `.nrm` in the mod's `build/`.
Mods link against `mods/syms/conker.us.{syms,datasyms}.toml`, which are
committed and which `recomp/run.sh` regenerates with N64Recomp's `--dump-context`;
rebuild mods after the game's function layout changes.

Mods can replace a game function (`RECOMP_PATCH`) or run code before it or
when it returns (`RECOMP_HOOK`, `RECOMP_HOOK_RETURN`). Patching rewrites the
start of the recompiled host function, which is why the exe links with
`/OPT:NOICF`. A hook makes the runtime recompile the hooked function again,
with LiveRecomp, from its instructions in the ROM. `.game` is compressed in the
real ROM, so `conker::decompress_rom` (`host/src/overlays.cpp`) builds the ROM
the recompiler saw instead: the original `.game` and `.debugger` code (which
the exe already carries for the TLB pages) at their ROM addresses in the
recompiled layout, plus the words `prepare_elf.py` rewrote
(`code_rewrites.txt`; `emit_tlb_pages.py` emits both). Limits of hooks: a function with a jump
table can't be hooked (its table is in `.game_data`, which the regenerated
code can't see), and hooking a function that `conker.toml` hooks drops the
toml hook. `func_1501BBB8` (reads the controllers once per game frame) makes
a good per-frame hook.

The game exports `recomp_printf` (`host/src/mod_api.cpp`), and the runtime
provides the `recomp_get_config_*` functions for a mod's config options.

`mods/skip_cutscenes` lets L skip any cutscene the first time it plays (the game
normally only lets you skip ones you've watched), and optionally the ones the
game's script never lets you skip, like the opening. `mods/cheats` has infinite
health, infinite lives and a full wallet, each an option.

## Next steps

1. Play further into the game: saves (EEPROM), rumble, the other microcode build.
2. Play-testing on Linux with a real GPU driver (so far only WSL with software
   Vulkan, where the sound crackles while rendering loads the CPU).
