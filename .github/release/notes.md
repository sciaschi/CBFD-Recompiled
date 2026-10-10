Ready-to-play builds for Windows, Linux and macOS. **You need your own US ROM of Conker's Bad Fur Day**: these packages contain no game data.

1. Download the package for your system and unpack it anywhere.
2. Run `ConkerRecomp` (`ConkerRecomp.exe` on Windows, `ConkerRecomp.app` on macOS).
3. The first time, the launcher asks for your ROM: pick your US `.z64`. Then Start Game.

ROM hacks that only change the game's assets, such as the uncensored one or the Russian translation, work too: load one with the launcher's **Add ROM** option. **Version** shows the ROM in play and switches between the ROMs you've loaded.

Linux needs SDL2, GTK 3 and FreeType (Ubuntu/Debian: `sudo apt install libsdl2-2.0-0 libgtk-3-0 libfreetype6`) and a Vulkan driver.

macOS needs Apple Silicon and macOS 15 or later. The app isn't signed with an Apple developer ID, so macOS blocks it the first time: open it once, then choose **Open Anyway** in System Settings > Privacy & Security (or run `xattr -dr com.apple.quarantine ConkerRecomp.app` in Terminal first).

The included mods (Skip Intro, Skip Any Cutscene and Cheats) are in the `Mods` zip, for every system. Unpack it and drop the `.nrm` files onto the launcher's **Mods** menu (or copy them into the `mods` folder of your data folder), then enable them there.

To build it yourself instead, see the [README](https://github.com/sciaschi/CBFD-Recompiled#readme).

macOS port by [nitrostemp](https://github.com/nitrostemp).
