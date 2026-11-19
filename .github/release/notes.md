This one is mostly about multiplayer, controls and polish, and once again nearly all of it came from people playing, reporting and testing. Thank you, credits are at the bottom. 🐿️

## New

- **Multiplayer Characters.** A new General option for up to 12 or 16 characters in a match, players and bots together. With 3 or 4 players there's now room for bots in every mode, where the game allowed one or none. (#167)
- **A controller for each player.** Assign each controller to a player slot. Each player gets their own controls profile, Swap Sticks and Free Camera. (#160, #164)
- **Controller presets and profiles.** New default controls, ready-made presets (including a modern layout), and custom profiles of your own. (#149)
- **21 new achievements, 58 in all.**
  - **Story (9):** bosses without losing a life, a whole chapter or the whole game without dying, nine tails at once, and $2,000 in one game.
  - **Multiplayer (12):** winning each mode, a flawless Deathmatch, sharpshooting, winning against a crowd, team play, and a full 4-player match.
  - Matches count for anyone playing on your computer. (#171)
- **Remember Cheat Codes.** Cheat codes you've entered, like the multiplayer characters, stay on between sessions. (#166, #154)
- **Sound:**
  - separate **Music**, **Sound Effects** and **Speech** volumes;
  - **Sound Mode** (mono, stereo or surround) is in the Sound tab, kept in sync with the game's own menu. (#159)
- **Start Game Directly** goes straight into the game without the launcher. **Return to Launcher** in the Quit prompt gets you back to it. (#133)
- **Hide notifications.** Turn off the achievement and save state messages for a vanilla look. (#128, #133)
- **Texture packs:**
  - a hotkey switches between the pack and the game's own textures, to compare;
  - hotkeys can be rebound in Controls;
  - GLideN64 `.hts` packs install and unpack like `.htc` ones. (#124)
- **Reticle styles.** Ring, Crosshair or Dot, in several sizes. (#124)
- **The sniper scope with Swap Sticks.** Conker walks any way while scoped, and the D-pad or mouse wheel zooms. (#84)
- **The mouse** turns itself on the first time you use it to play.
- **ROM hacks:** the German and Brazilian Portuguese fan translations play, and show by name. (#129, #131)
- **The launcher** has a new look in the game's autumn colours.

## Fixed

**Crashes and hangs**
- **Linux and Steam Deck:** quitting no longer hangs or crashes as the program closes. (#127, #148, #157, #191)
- **Windows:**
  - mods that patch the game no longer cause random crashes, such as when moving the left stick. (#178)
  - Return to Launcher no longer crashes. (#149)
- **Multiplayer:** Tank, and Heist from the main menu, no longer stop the game by running out of memory.

**Graphics**
- **It's War's opening newspaper** is in black and white again, as on the N64. (#162)
- **The pause screen** has a sharp, full-width blurred background. On Direct3D 12 it's no longer black, and the renderer no longer runs out of memory at pauses.
- **Widescreen** (#138):
  - bullet holes and screen cracks reach the screen's edges, and so do the wasp hive turret's frame and the Haybot's red view;
  - the tank's timer hangs at the right edge;
  - the bazooka's sight lines up for players 1 and 3 in 3- and 4-player split screen. (#173)
- **Split screen:**
  - no flicker, and no jittering player 2 camera, with frame interpolation;
  - Beach's top views are widened too;
  - when a player dies, the closing circle and the fade cover their whole widened view.
- **Shadows** ease toward their light instead of snapping with it.

**Controls**
- **The Nintendo Switch Online N64 controller** works over Bluetooth on Windows: the C-buttons act as the right stick, and Capture opens the menu. (#188)

## Known issues

- **Rock Solid with modded-audio ROMs:** the sliding rock cutscene can still get stuck on some ROM hacks. huguitocloud has since pinned down the exact bytes involved, so a fix is closer. (#79)

## Thank you

Every item above started with someone taking the time to report, explain, record or test:

- **[@urbangangsta101](https://github.com/urbangangsta101)** had another huge release:
  - **Reported:**
    - the Steam Deck exit crash (#127);
    - a long list of widescreen overlays cut off at 4:3: cracks, bullet holes, the wasp hive, the Terminator's view, the tank timer (#138);
    - shadows that followed the camera.
  - **Asked for:** controller presets (#149), Start Game Directly and hiding notifications (#133).
  - **Tested** every build for those on Linux, the Steam Deck and Windows, and found the Return to Launcher crash along the way. They were ready to test the newspaper fix (#162) the moment it landed.
- **[@easymod0](https://github.com/easymod0)** contributed:
  - Multiplayer Characters (#182);
  - the Tank and Heist memory fix;
  - Beach's widened views;
  - the Direct3D 12 pause fix;
  - faster CI builds.
  They also tracked down why the bazooka's sight broke in split screen (#173). They've also built online multiplayer (#126), which is in the source behind a build option while it's tested, and isn't in these packages.
- **[@magbean328-ship-it](https://github.com/magbean328-ship-it)** brought the multiplayer ideas behind this release: a controller and settings for each player (#164), saved cheat codes (#166), more bots (#167) and multiplayer achievements (#171). They also reported the bazooka's sight in split screen (#173).
- **[@dankor66](https://github.com/dankor66)** narrowed the bazooka sight bug down to players 1 and 3, with a video (#173). They also shared a character unlock mod while Remember Cheat Codes was on its way (#154).
- **[@TopFlightLawter](https://github.com/TopFlightLawter)** reported the Windows mod crash (#178) with a complete diagnosis and repair tools, and tested the fix.
- **[@drencoss](https://github.com/drencoss)** tested the texture pack changes and asked for the texture hotkey and reticle styles (#124). They also reported aim inversion.
- **[@Succulent-Puppet](https://github.com/Succulent-Puppet)** reported the N64 controller over Bluetooth (#188), sent the log that cracked it, and tested the fix.
- **[@Tasosgemah](https://github.com/Tasosgemah)** asked for a way to hide achievements (#128) and tested it. They also reported the audio crackle (#136).
- **[@IceBaronTwinAcc](https://github.com/IceBaronTwinAcc)** suggested Sound Mode in the settings (#159) and tested it.
- **[@silbinarywolf](https://github.com/silbinarywolf)**, **[@rdutta](https://github.com/rdutta)** and **[@iPooPaLot](https://github.com/iPooPaLot)** reported the Linux hang on exit (#148, #157, #191). silbinarywolf and iPooPaLot confirmed the fix.
- **[@portlover](https://github.com/portlover)** asked for the German translation (#129) and tested it, and **[@Malucococo](https://github.com/Malucococo)** did the same for Brazilian Portuguese (#131).
- **[@fufiplayyy](https://github.com/fufiplayyy)** asked for multiplayer characters that stay unlocked (#154).
- **[@huguitocloud](https://github.com/huguitocloud)** kept digging into the Rock Solid softlock (#79), and **[@jesusen904](https://github.com/jesusen904)** reported it.
- **[@ncflagg](https://github.com/ncflagg)** retested surround sound (#32), and **[@austin98989](https://github.com/austin98989)** checked fullscreen frame rates (#163).
- **GameBeast92**'s texture pack is what the `.hts` support was built and tested with.

## How to play

Ready-to-play builds for Windows, Linux and macOS. **You need your own US ROM of Conker's Bad Fur Day**: these packages contain no game data.

1. Download the package for your system and unpack it anywhere.
2. Run `ConkerRecomp` (`ConkerRecomp.exe` on Windows, `ConkerRecomp.app` on macOS).
3. The first time, the launcher asks for your ROM: pick your US `.z64`. Then Start Game.

**Save states:** F5 saves, F7 loads, F6 picks the slot (1 to 9). **Achievements** of the program's own are in the settings' Achievements tab, kept on your computer.

ROM hacks that only change the game's assets, such as the uncensored one or the Russian translation, work too: load one with the launcher's **Add ROM** option. **Version** shows the ROM in play and switches between the ROMs you've loaded.

Linux needs SDL2, GTK 3 and FreeType (Ubuntu/Debian: `sudo apt install libsdl2-2.0-0 libgtk-3-0 libfreetype6`) and a Vulkan driver.

macOS needs Apple Silicon and macOS 15 or later. The app isn't signed with an Apple developer ID, so macOS blocks it the first time: open it once, then choose **Open Anyway** in System Settings > Privacy & Security (or run `xattr -dr com.apple.quarantine ConkerRecomp.app` in Terminal first).

The included mods (Skip Intro, Skip Any Cutscene and Cheats) are in the `Mods` zip, for every system. Unpack it and drop the `.nrm` files onto the launcher's **Mods** menu (or copy them into the `mods` folder of your data folder), then enable them there.

To build it yourself instead, see the [README](https://github.com/sciaschi/CBFD-Recompiled#readme).

macOS port by [nitrostemp](https://github.com/nitrostemp).
