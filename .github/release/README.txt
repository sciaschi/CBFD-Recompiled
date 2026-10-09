Conker's Bad Fur Day: Recompiled
https://github.com/sciaschi/CBFD-Recompiled
macOS port by nitrostemp: https://github.com/nitrostemp

This package contains no game data. You need your own copy of the US ROM of
Conker's Bad Fur Day (N64).

Playing
  1. Start ConkerRecomp (ConkerRecomp.exe on Windows, ConkerRecomp.app on macOS).
  2. The first time, the launcher asks for your ROM: pick your US .z64 ROM.
     It's checked, and remembered for later runs.
  3. Start Game.

ROM hacks that only change the game's assets, such as the uncensored one, work
too: load one with the launcher's Add ROM option. Version shows the ROM in
play and switches between the ROMs you've loaded.

Windows: Windows 10 or 11, 64-bit, with a Direct3D 12 or Vulkan graphics card.
Linux: x86-64 with Vulkan, and SDL2, GTK 3 and FreeType installed
(on Ubuntu or Debian: sudo apt install libsdl2-2.0-0 libgtk-3-0 libfreetype6).
macOS: Apple Silicon, macOS 15 or later. The app isn't signed with an Apple
developer ID, so macOS blocks it the first time: open it once, then in System
Settings > Privacy & Security choose Open Anyway. Or, in Terminal, run
xattr -dr com.apple.quarantine ConkerRecomp.app before opening it.

Save states: F5 saves the game as it is, anywhere, F7 loads it back, and F6
switches between the nine slots. The game's own saves are never touched. States
only load in the version of the program that made them. More in
docs/save-states.md on the GitHub page.

Texture packs: install a .rtz pack, or a GLideN64 pack's .htc file, with the Mods
menu's Install Mods button (or drop it onto the Mods menu, or put it in the mods
folder), and pick it in Settings > Graphics > Texture Pack. A .htc is unpacked the first
time, which takes a minute or so for a large pack.

Licenses: see LICENSES.txt. The program as a whole is under the GNU GPL version 3
(LICENSE-GPL-3.0.txt), with its source at the GitHub page above.

Saves, settings and the stored ROM are kept in your user folder, not here, so a
newer version can be unpacked anywhere.

If the game crashes, Windows writes crash.log next to ConkerRecomp.exe; please
attach it to an issue on the GitHub page.
