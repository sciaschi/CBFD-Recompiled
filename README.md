<img width="1360" height="768" alt="image" src="https://github.com/user-attachments/assets/abb979d7-24a5-44f8-98d3-088ba2054a74" />

# Conker's Bad Fur Day: Recompiled

A native PC port of **Conker's Bad Fur Day** (N64, US version). It's built by
statically recompiling the game with [N64Recomp](https://github.com/N64Recomp/N64Recomp),
starting from the [Conker decompilation](https://github.com/mkst/conker). It runs on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and is rendered by
[RT64](https://github.com/rt64/rt64).

> **This repository contains no game data.** You need your own legally obtained
> copy of the US ROM. Everything from the game is extracted from that ROM on
> your machine during the build.

## Download

Ready-to-play Windows, Linux and macOS (Apple Silicon, macOS 15 or later) builds
are on the [Releases page](https://github.com/sciaschi/CBFD-Recompiled/releases).
Unpack one anywhere, run `ConkerRecomp`, and pick your US ROM in the launcher the
first time. The packages contain no game data: you still need your own ROM. The
macOS app isn't signed with an Apple developer ID, so the first time, open it and
then choose **Open Anyway** in System Settings > Privacy & Security. To build it
yourself instead, read on.

## Features

- Runs natively on Windows (Direct3D 12 or Vulkan), Linux (Vulkan) and macOS
  (Metal), rendered by RT64. Supports higher resolutions, widescreen,
  anti-aliasing and high frame-rate presentation.
- Game controllers and keyboard, with remappable controls and rumble.
- Full audio: music, sound effects and the voice acting.
- Saving (EEPROM), stored per user.
- A launcher with settings, controls and a mod menu (RecompFrontend, as in
  Zelda 64: Recompiled and Banjo: Recompiled).
- Mod support (`.nrm` mods: function patches and hooks). Three mods are
  included: **Skip Intro**, **Skip Any Cutscene** and **Cheats**.
- Texture packs (RT64's `.rtz`), for HD textures and other replacements.

## Status

The game is playable, and has been played through to the end. Known issues:

- Widescreen: the pause menu's blurred background is the 4:3 frame scaled up to
  the full width, so its top and bottom are cropped.
- Environment-mapped (reflective) surfaces render without their reflection texture.
- Frame rates above 30: characters snap once when the game changes how many parts
  it draws them with, a few times a minute, and the camera can blend across a cut.
- Only the US ROM is supported (and ROM hacks that only change its assets: see
  [ROM hacks](#rom-hacks)).
- Linux: the build and the game have been tested on Ubuntu 24.04 under WSL, with
  software Vulkan (llvmpipe) and sound. It hasn't been played on Linux with a
  real GPU driver yet, so reports are welcome, especially about performance or
  audio (under WSL the sound crackles while the software renderer loads the CPU).
- macOS: the build has been tested on an Apple M3 Pro with macOS 27 and Xcode 27,
  and the game starts and runs with Metal. It hasn't been played through on
  macOS yet, and Intel Macs haven't been tried.

If the game crashes on Windows, a report is written to `crash.log` next to the
executable and shown in a message box. On Linux and macOS, the crash report is printed
to the terminal. Please include it when reporting a problem.

## What you need to build it

- The **US** ROM of Conker's Bad Fur Day in big-endian `.z64` format, with
  SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`. It is never committed: the
  build extracts what it needs from your copy.
- A folder path without an apostrophe (`'`) to clone into. Some of RT64's build
  steps break on one.

Then follow the guide for your system: [Windows](#building-on-windows),
[Linux](#building-on-linux) or [macOS](#building-on-macos). Each is one command
once the tools are installed. The first full build takes a while, because the
recompiled game is a lot of C code.

## Building on Windows

Windows 10 or 11 (x64). Everything builds natively; `build.cmd` does all of it.

### 1. Install the tools (once)

- [Git for Windows](https://git-scm.com/download/win).
- [Python 3](https://www.python.org/downloads/). In the installer, tick *Add
  python.exe to PATH*.
- **Visual Studio 2022 or later** with the *Desktop development with C++*
  workload. The free *Build Tools for Visual Studio* edition is enough.

### 2. Get the code and build

In a Command Prompt, in the folder you want it in (for example `D:\Games`), with
the path to your ROM:

```bat
git clone --recursive https://github.com/sciaschi/CBFD-Recompiled.git
cd CBFD-Recompiled
build.cmd "C:\path\to\your\conker.z64"
```

The first build takes a while. When it's done, the game is
`host\build-win\ConkerRecomp.exe` (see [Playing](#playing)).

### Updating

```bat
git pull
build.cmd
```

The script updates the submodules and their patches, and only rebuilds what
changed.

## Building on Linux

Tested on Ubuntu 24.04 (x86-64). You need a Vulkan driver: Mesa's, which most
distributions install by default, or NVIDIA's.

```sh
git clone --recursive https://github.com/sciaschi/CBFD-Recompiled.git
cd CBFD-Recompiled
./build.sh ~/path/to/your/conker.z64
```

On Ubuntu and Debian, the script lists the packages it's missing and offers to
install them. On other distributions, install the equivalents of `git python3
cmake ninja-build clang pkg-config libsdl2-dev libdbus-1-dev libfreetype-dev`
yourself. When it's done, run `host/build/ConkerRecomp` (see [Playing](#playing)).

To update: `git pull`, then `./build.sh` again.

## Building on macOS

macOS on Apple Silicon or Intel. The game renders with Metal. `build.sh` does all
of it, as on Linux.

### 1. Install the tools (once)

- **Xcode**, from the App Store. The Command Line Tools alone aren't enough: RT64
  compiles its shaders with Xcode's Metal compiler. Open Xcode once to finish its
  setup. If `xcode-select -p` doesn't print a path inside `Xcode.app`, run
  `sudo xcode-select -s /Applications/Xcode.app`.
- [**Homebrew**](https://brew.sh).
- Optionally, for building mods: `brew install llvm` (Apple's clang can't compile
  for the N64's MIPS processor).

### 2. Get the code and build

In Terminal, in the folder you want it in, with the path to your ROM:

```sh
git clone --recursive https://github.com/sciaschi/CBFD-Recompiled.git
cd CBFD-Recompiled
./build.sh ~/path/to/your/conker.z64
```

Put the ROM's path in quotes if it has spaces or an apostrophe, for example
`./build.sh "$HOME/Downloads/Conker's Bad Fur Day (USA).z64"`.

The first build takes a while. If some tools are missing, the script lists the
Homebrew packages (`cmake ninja pkg-config sdl2 freetype`) and offers to install
them. It also offers to download Xcode's Metal Toolchain (about 850 MB), which
Xcode 26 and later install separately. When it's done, the game is
`host/build/ConkerRecomp` (see [Playing](#playing)).

To turn it into a standalone `ConkerRecomp.app` that runs without Homebrew (as the
release workflow does), run `sh host/package_macos.sh` after `brew install
dylibbundler`. The app ends up in `host/build/`.

### Updating

```sh
git pull
./build.sh
```

The script updates the submodules and their patches, and only rebuilds what
changed.

## What the build does

`build.sh` holds the full sequence, if you'd rather run the steps yourself:

1. Checks your ROM (copied to `conker/baserom.us.z64`) and applies the patches in
   `recomp/` to N64Recomp, N64ModernRuntime and RT64.
2. Builds N64Recomp.
3. Runs `recomp/recompile.py`, which unpacks the game's code from your ROM and
   recompiles it into `RecompiledFuncs/`. Which functions there are, and where,
   comes from `recomp/conker.us.syms.toml`: names, addresses and sizes, but no
   code. The code itself only ever comes from your ROM.
4. Builds the game (`host/`) with CMake.

### Working on the decompilation

`recomp/conker.us.syms.toml` (and `mods/syms/`) are generated from the
decompilation in `conker/`, which needs its own Linux tools: IDO, which runs
through the MIPS binutils, and the Python packages in `requirements.txt`. On
Linux, macOS, or in WSL on Windows, `./build.sh --decomp` builds the decompilation
too, checks that it rebuilds your ROM's code byte for byte, and regenerates those
files from it (`recomp/run.sh`). The decompilation is a work in progress: about 9%
of the code is C so far, and the rest is still the original assembly.

On macOS, `--decomp` also needs `coreutils` and `mips-linux-gnu-binutils` from
Homebrew (the script offers to install them). The decompilation comes with two
Linux programs, the IDO compiler and a patched gzip that compresses exactly as the
original game's did, so the script puts macOS replacements in `tools/macos/`:
IDO's macOS build from
[ido-static-recomp](https://github.com/decompals/ido-static-recomp), and GNU gzip
1.10 built with the same one-line change.

## Playing

Run `host/build/ConkerRecomp` (Linux and macOS) or `host\build-win\ConkerRecomp.exe`
(Windows). The first time, pick **Load ROM** in the launcher and select your ROM
(the same `baserom.us.z64` works). After that it's remembered, so just choose
**Start Game**.

- **Settings** (in the launcher, or Esc / the controller's menu button in game)
  has its options by tab: General (deadzone, rumble), Camera (turning, the free
  camera, field of view, aiming with the stick), Mouse & Gyro, Graphics
  (resolution, aspect ratio, anti-aliasing, frame rate, texture packs), Controls,
  Sound, Achievements and Mods. The tabs scroll sideways when they don't all fit.
- **Achievements** are the program's own, for reaching the game's scenes (the
  ones its Chapters menu lists), from the first death to the end credits. They're
  kept on this computer, in `achievements.txt` with the settings, and the
  Achievements tab lists them. A save that's already further along unlocks what
  it has reached when you play it.
- Saves, settings and the stored ROM live in `~/.config/ConkerRecompiled` on
  Linux, `~/Library/Application Support/ConkerRecompiled` on macOS and
  `%LOCALAPPDATA%\ConkerRecompiled` on Windows. Put an empty `portable.txt` next
  to the executable to keep them there instead.
- Default keyboard controls: move with WASD, A = Space, B = Left Shift,
  Z = Q, L = E, R = R, Start = Enter, C buttons = arrow keys, D-pad = IJKL.
  Everything can be remapped in Controls.
- N64 pads and adapters for real N64 controllers (raphnet-tech's, Mayflash,
  Hyperkin, the NSO and 8BitDo 64 controllers) are mapped from
  `assets/controllerdb.txt`, with the C buttons as the right stick. A controller
  SDL doesn't know can be given a mapping in the `SDL_GAMECONTROLLERCONFIG`
  environment variable, which takes precedence.

## ROM hacks

A ROM hack that only changes the game's assets (its audio, textures, models or
text), such as an uncensored patch that restores the bleeped words, plays like the
US ROM. The recompiled code only comes from the ROM's code, which has to be the US
ROM's, so hacks that change the game's code are refused. A hack that repacks the
game's compressed code without changing it, as the Russian fan translation does, is
checked unpacked and plays too (**Version: US Russian**).

The launcher's **Version** option says which ROM is in play, with its region (for
example **Version: US Original** or **Version: US Uncensored**), and so does the
window's title. **Add ROM** loads another: pick the hack's `.z64`, and it's put in
play. After that, selecting Version switches between the ROMs you've loaded. Other
regions' ROMs (such as the European one) are refused, with a message naming the
region. Each is kept in `rom_versions/` in the data
folder (64 MB apiece). Saves are shared between them.

## Mods

Mods are `.nrm` files. Install one by copying it into the `mods` folder of the
data folder above (or dropping it onto the Mods menu), then enable it in the
**Mods** menu. Some mods have options there too.

Each release has the included mods built, in its `Mods` zip (the same `.nrm`
files work on every system). Their source is in `mods/`. Build one on Linux, macOS
(which also needs `brew install llvm lld`, as Apple's clang can't compile for MIPS)
or in WSL, from the repository root, after `recomp/run.sh`. It links with `ld.lld`
(`sudo apt install lld`), or GNU ld (`mips-linux-gnu-ld`) where there's none, but
GNU ld can't link a mod that calls the game's functions, such as Skip Intro:

```sh
sh mods/build_mod.sh mods/skip_cutscenes
```

The `.nrm` ends up in the mod's `build/` folder.

- **Skip Intro** (`mods/skip_intro`): boots straight to the main menu, skipping
  the notices, logos and the chainsaw opening.
- **Skip Any Cutscene** (`mods/skip_cutscenes`): L skips a cutscene even the
  first time you see it. An option also allows skipping the ones the game never
  lets you skip.
- **Cheats** (`mods/cheats`): infinite health, infinite lives and a full wallet,
  each toggled in the mod's options.

Writing your own is covered in [recomp/README.md](recomp/README.md#mods).

### Texture packs

Texture packs replace the game's textures with new ones, such as HD textures.
They're RT64 texture packs, the same format as Zelda 64: Recompiled's and Banjo:
Recompiled's: an `rt64.json` listing each texture's hash and its replacement
image. Install a `.rtz` pack like a mod (the `mods` folder, or dropped onto the
Mods menu), then pick it in **Settings > Graphics > Texture Pack**, which turns it on and
the other packs off. Packs installed while the game runs are listed there from
the next start. Its default, **Set in the Mods Menu**, leaves them to the
**Mods** menu instead, where several can be on at once (later ones in the list
take precedence).

GLideN64 texture packs (for Project64 and RetroArch) work too. Install the
pack's `.htc` file the same way (**Install Mods** or dropped onto the Mods menu),
or put it in the `mods` folder and start the game or press the Mods menu's
refresh button: it's unpacked, once, into a pack folder beside it, with its
progress shown (a large pack takes a minute or so), and turned on. It's then in
the **Mods** menu, and listed in **Settings > Graphics > Texture Pack**. GLideN64 names each texture by a
different hash than RT64 (Rice's), which the game's RT64 works out as each
texture is loaded. Only packs stored as RGBA8 are unpacked (not those built with
texture compression).

[`tools/texture_packs/gliden64_to_rt64.py`](tools/texture_packs/gliden64_to_rt64.py)
makes a `.rtz` from a GLideN64 pack (a `.htc`, or a folder of
`...#crc#format#size_all.png` files) to share:

```sh
python tools/texture_packs/gliden64_to_rt64.py pack.htc --out my_pack --name "My Pack" --author Me --rtz my_pack.rtz
```

Given folders of textures dumped while playing (RT64's developer mode, below:
F1, **Start dumping textures**), it instead lists the dumped textures with
RT64's hashes, as RT64 without live matching needs (see the tool).

To make a pack from scratch, put a folder in `mods` with a `mod.json` (`"game_id": "conker"`,
plus an `id`, `version`, `display_name`, `authors` and `minimum_recomp_version`)
and an `rt64.json`:

```json
{ "configuration": { "configurationVersion": 3, "hashVersion": 5 }, "textures": [] }
```

With it enabled, and RT64's developer mode on (`"developer_mode": true` in
`graphics.json` in the data folder), RT64's inspector (F1) shows each draw's
texture hash and has a **Replace** button that picks an image and adds it to the
folder's `rt64.json`. Zip the folder's contents (without `mod.json`) and rename
it to `.rtz` to share it.

## How it works

[recomp/README.md](recomp/README.md) documents the whole pipeline:

- how the decomp's ELF is prepared for N64Recomp (`recomp/prepare_elf.py`);
- the recompiler configuration and hooks (`conker.toml`);
- the audio microcode;
- the changes to N64Recomp, N64ModernRuntime and RT64 (Conker's graphics
  microcode, widescreen);
- the host application in `host/`;
- debugging tools.

## AI assistance

This project was made with heavy use of an AI coding assistant (Claude, through
Claude Code). That covers the recompilation setup, the patches to the tools, the
host application, the mods, and the functions decompiled to C in this repository.

What's checked, and how:

- **Decompiled C** is only kept when it compiles to exactly the original
  instructions. The build fails unless the rebuilt code is byte-for-byte
  identical to the ROM's.
- **The port** is checked by playing it, and by comparing its behaviour with
  the original running in an emulator.

What isn't checked: names, types and comments don't change the compiled bytes,
so a byte-for-byte match says nothing about whether they're right. Names that
end in an address (for example `resetSlotState_150104F0`) are best guesses based
on what the code appears to do. Treat them as hints, not established facts.

This is an independent project. The AI-assisted work here isn't part of the
upstream decompilation, and the people behind that project and other N64
decompilation communities aren't responsible for it. Please report problems here,
not to them.

## Contributing

Bug reports, fixes and patches are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- The [Conker's Bad Fur Day decompilation](https://github.com/mkst/conker) project,
  whose work this is built on.
- The macOS port, and support for ROM hacks, by
  [nitrostemp](https://github.com/nitrostemp).
- [N64Recomp](https://github.com/N64Recomp/N64Recomp),
  [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and
  [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) by Wiseguy and
  contributors.
- [RT64](https://github.com/rt64/rt64) by Darío and contributors.
- The mod headers follow the
  [Banjo: Recompiled mod template](https://github.com/BanjoRecomp/BKRecompModTemplate).
- Fonts: [Inter](https://rsms.me/inter/), [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji)
  and [promptfont](https://shinmera.github.io/promptfont/).

## License

This project's own code is under the [MIT License](LICENSE). The submodules keep
their own licenses, and a build combines them:

- N64ModernRuntime (librecomp and ultramodern) is under the
  [GPL, version 3](tools/N64ModernRuntime/COPYING). As the program includes it,
  the program as a whole is distributed under the GPL version 3, which gives
  everyone who gets it the right to its source: this repository, at the release's
  tag. The release packages include the GPL's text (`LICENSE-GPL-3.0.txt`) and
  [`LICENSES.txt`](.github/release/LICENSES.txt), which lists them.
- RT64 is under the MIT License, but the live texture pack matching that
  [`recomp/rt64.patch`](recomp/rt64.patch) adds (`src/hle/rt64_rice_hash.cpp`) is
  under the GPL, version 2 or later: it imitates the Rice texture hashing of
  GlideHQ (Hiroshi Morii, as in GLideN64) and Rice Video, both under the GPL
  version 2 or later ([recomp/README.md](recomp/README.md#local-changes-to-the-tools)).
  That's compatible with the GPL version 3.
- The macOS linker wrapper, [`host/macos/ld64`](host/macos/ld64), which lets mods
  patch the program's code there, follows Banjo: Recompiled's
  ([github.com/BanjoRecomp/BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp),
  `.github/macos/ld64`) and is under the GPL, version 3, as that is. It's only
  used to build, and isn't part of the program.

The game itself is not included and not covered by them.

Conker's Bad Fur Day is © Rare Ltd. This project is not affiliated with or
endorsed by Rare, Microsoft or Nintendo.
