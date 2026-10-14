#!/bin/sh
# Builds a mod and installs its .nrm into the game's data folder so the game loads it.
# Usage: sh install_mod.sh mods/hollywood_subtitles
set -e
MOD_DIR="$1"
if [ -z "$MOD_DIR" ]; then echo "usage: sh install_mod.sh <mod dir>"; exit 1; fi
sh mods/build_mod.sh "$MOD_DIR"
# macOS data-folder mods path (unless a portable.txt sits next to the exe).
DEST="$HOME/Library/Application Support/ConkerRecompiled/mods"
mkdir -p "$DEST"
NRM=$(ls "$MOD_DIR"/build/*.nrm | head -1)
cp "$NRM" "$DEST/"
echo "installed $(basename "$NRM") -> $DEST"
