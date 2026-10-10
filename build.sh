#!/bin/sh
# Builds Conker's Bad Fur Day: Recompiled from a clone and your ROM, in one go:
#   ./build.sh [path/to/rom.z64]
# The ROM is copied to conker/baserom.us.z64 (needed once). Safe to run again after
# `git pull`: every step skips what's already done or only rebuilds what changed.
#   --decomp  for developers: also build the decompilation (checked against your ROM)
#             and regenerate the committed symbols from it (recomp/run.sh), which
#             needs the decompilation's tools too (IDO runs through binutils-mips)
# Works on Linux and macOS (Apple Silicon or Intel, with Xcode and Homebrew).
set -e

cd "$(dirname "$0")"
ROOT=$(pwd)
DECOMP=
ROM=
for arg in "$@"; do
    case "$arg" in
        --decomp) DECOMP=1 ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) ROM=$arg ;;
    esac
done
MACOS=
if [ "$(uname -s)" = Darwin ]; then
    MACOS=1
    # The decompilation's Makefiles need GNU coreutils (sha1sum --check, split
    # --additional-suffix): put Homebrew's ahead of macOS's own.
    if command -v brew >/dev/null 2>&1; then
        GNUBIN="$(brew --prefix)/opt/coreutils/libexec/gnubin"
        [ -d "$GNUBIN" ] && PATH="$GNUBIN:$PATH" && export PATH
    fi
    # Apple's clang (/usr/bin's, which finds the macOS SDK) for everything built
    # here: Python packages without a wheel, gzip, the recompiler and the game,
    # whatever other clang is first on the PATH.
    CC=/usr/bin/clang
    CXX=/usr/bin/clang++
    export CC CXX
fi
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nError: %s\n' "$*" >&2; exit 1; }

case "$ROOT" in
    *\'*) fail "the folder path contains an apostrophe ($ROOT). Some of RT64's build steps break on one: move the clone somewhere else." ;;
esac

# ---------------------------------------------------------------- prerequisites
step "Checking the tools"
missing=
if [ -n "$MACOS" ]; then
    # Xcode provides clang; the file dialogs use Cocoa rather than GTK.
    cmds="git python3 cmake ninja pkg-config xcrun"
    libs="sdl2 freetype2"
else
    cmds="git python3 cmake ninja clang pkg-config"
    libs="sdl2 dbus-1 freetype2"
fi
for cmd in $cmds; do
    command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
done
if command -v pkg-config >/dev/null 2>&1; then
    for lib in $libs; do
        pkg-config --exists "$lib" || missing="$missing $lib"
    done
fi
if [ -n "$MACOS" ]; then
    packages="git python3 cmake ninja pkg-config sdl2 freetype"
else
    packages="git python3 cmake ninja-build clang pkg-config libsdl2-dev libdbus-1-dev libfreetype-dev"
fi
if [ -n "$DECOMP" ]; then
    for cmd in make mips-linux-gnu-as; do
        command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
    done
    python3 -c 'import venv, ensurepip' 2>/dev/null || missing="$missing python3-venv"
    if [ -n "$MACOS" ]; then
        command -v curl >/dev/null 2>&1 || missing="$missing curl"
        split --version >/dev/null 2>&1 || missing="$missing coreutils"
        packages="$packages coreutils mips-linux-gnu-binutils"
    else
        command -v gcc >/dev/null 2>&1 || missing="$missing gcc"
        packages="$packages build-essential binutils-mips-linux-gnu python3-venv"
    fi
fi
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    if [ -n "$MACOS" ]; then
        command -v brew >/dev/null 2>&1 || fail "install Homebrew (https://brew.sh), then run this again."
        echo "They're in these Homebrew packages:"
        echo "  brew install $packages"
        printf 'Install them now? [Y/n] '
        read -r answer || answer=n
        case "$answer" in
            [nN]*) fail "install the packages above, then run this again." ;;
        esac
        brew install $packages || fail "couldn't install the packages."
        # Again, now with coreutils on the PATH.
        exec sh "$ROOT/build.sh" "$@"
    elif command -v apt-get >/dev/null 2>&1; then
        echo "They're in these packages:"
        echo "  sudo apt install $packages"
        printf 'Install them now? [Y/n] '
        read -r answer || answer=n
        case "$answer" in
            [nN]*) fail "install the packages above, then run this again." ;;
        esac
        sudo apt-get update && sudo apt-get install -y $packages || fail "couldn't install the packages."
    else
        fail "install the missing tools with your distribution's package manager (on Ubuntu: $packages), then run this again."
    fi
fi
# RT64 compiles its shaders for Metal with Xcode's metal compiler, which Xcode 26 and
# later download separately.
if [ -n "$MACOS" ] && ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
    xcrun -sdk macosx --find metal >/dev/null 2>&1 || fail "the game needs Xcode (from the App Store), not only the Command Line Tools.
Install it, run \`sudo xcode-select -s /Applications/Xcode.app\`, then run this again."
    echo "Xcode's Metal Toolchain isn't installed."
    printf 'Download it now (about 850 MB)? [Y/n] '
    read -r answer || answer=n
    case "$answer" in
        [nN]*) fail "run \`xcodebuild -downloadComponent MetalToolchain\`, then run this again." ;;
    esac
    xcodebuild -downloadComponent MetalToolchain || fail "couldn't download the Metal Toolchain."
fi

# ---------------------------------------------------------------- the ROM
BASEROM=conker/baserom.us.z64
if [ -n "$ROM" ]; then
    [ -f "$ROM" ] || fail "no file at $ROM."
    step "Copying your ROM to $BASEROM"
    cp "$ROM" "$BASEROM"
fi
[ -f "$BASEROM" ] || fail "no ROM yet. Run this with the path to your ROM, for example:
  ./build.sh ~/Downloads/conker.z64"
# Checks it's the right ROM (it says so if not).
python3 -c "import sys; sys.path.insert(0, 'recomp'); import unpack_rom; unpack_rom.unpack(open('$BASEROM', 'rb').read())"

# ---------------------------------------------------------------- tools
step "Getting the submodules"
# A pull can move a patched tool to another commit, which the patch would block.
for tool in tools/N64Recomp tools/N64ModernRuntime tools/rt64 tools/RecompFrontend; do
    if [ -e "$tool/.git" ]; then
        want=$(git ls-tree HEAD "$tool" | awk '{print $3}')
        if [ "$want" != "$(git -C "$tool" rev-parse HEAD)" ]; then
            echo "  $tool moved to another commit: resetting it (its patch is applied again below)."
            git -C "$tool" reset --hard -q
            git -C "$tool" clean -fdq
        fi
    fi
done
# RmlUi is patched too, and is RecompFrontend's submodule, so it moves when either does.
RMLUI=tools/RecompFrontend/recompui/lib/RmlUi
if [ -e "$RMLUI/.git" ]; then
    want=$(git ls-tree HEAD tools/RecompFrontend | awk '{print $3}')
    want_rmlui=$(git -C tools/RecompFrontend/recompui/lib ls-tree HEAD RmlUi | awk '{print $3}')
    if [ "$want" != "$(git -C tools/RecompFrontend rev-parse HEAD)" ] ||
       [ "$want_rmlui" != "$(git -C "$RMLUI" rev-parse HEAD)" ]; then
        echo "  $RMLUI moved to another commit: resetting it (its patch is applied again below)."
        git -C "$RMLUI" reset --hard -q
        git -C "$RMLUI" clean -fdq
    fi
fi
git submodule update --init --recursive

# Applies a patch unless it's already applied. If the tool holds an older version of
# the patch (after a git pull that changed it), the tool is reset and patched again.
apply_patch() {
    tool=$1; patch=$2
    if git -C "$tool" apply --reverse --check "$ROOT/$patch" 2>/dev/null; then
        return
    fi
    if ! git -C "$tool" apply --check "$ROOT/$patch" 2>/dev/null; then
        echo "  $tool has changes that aren't $patch (probably an older version of it): resetting it."
        git -C "$tool" reset --hard -q
        git -C "$tool" clean -fdq
    fi
    git -C "$tool" apply "$ROOT/$patch" || fail "couldn't apply $patch to $tool."
    echo "  patched $tool"
}
step "Patching the tools"
apply_patch tools/N64Recomp recomp/n64recomp.patch
apply_patch tools/N64ModernRuntime recomp/n64modernruntime.patch
apply_patch tools/rt64 recomp/rt64.patch
# RecompFrontend: mouse buttons can be bound (with the keyboard's controls).
apply_patch tools/RecompFrontend recomp/recompfrontend.patch
# RmlUi's fix for GCC 15 and later (RmlUi #766), which isn't in the RmlUi that RecompFrontend
# uses: its robin_hood.h uses uint64_t without including <cstdint>. Unneeded, and skipped as
# already applied, once RecompFrontend uses RmlUi 6.2 or later.
apply_patch "$RMLUI" recomp/rmlui.patch

step "Building the recompiler"
if [ ! -f tools/N64Recomp/build/build.ninja ]; then
    cmake -S tools/N64Recomp -B tools/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build tools/N64Recomp/build --target N64RecompCLI RSPRecomp RecompModTool

# ---------------------------------------------------------------- recompilation
if [ -n "$DECOMP" ] && [ -n "$MACOS" ]; then
    # The decompilation ships two Linux programs: IDO 5.3 (the N64 C compiler, as a
    # static recompilation) and a gzip patched to compress exactly as Rare's did. On
    # macOS, get the same IDO built for macOS, and build that gzip from GNU's release.
    # Both land in tools/macos/.
    step "Getting the macOS builds of the decompilation's tools"
    MAC_TOOLS=$ROOT/tools/macos
    mkdir -p "$MAC_TOOLS"
    # fetch URL FILE SHA256
    fetch() {
        if [ ! -f "$2" ] || [ "$(shasum -a 256 "$2" | cut -d' ' -f1)" != "$3" ]; then
            curl -fL --retry 3 -o "$2.part" "$1" || fail "couldn't download $1."
            [ "$(shasum -a 256 "$2.part" | cut -d' ' -f1)" = "$3" ] || fail "$1 doesn't have the expected SHA-256."
            mv "$2.part" "$2"
        fi
    }
    IDO_URL=https://github.com/decompals/ido-static-recomp/releases/download/v1.2/ido-5.3-recomp-macos.tar.gz
    IDO_SHA256=5b1ca006ee4b158ffba0422fc3f00b9b330f9036f279bae2ea1b4703317ad9c0
    if [ "$(cat "$MAC_TOOLS/ido5.3/.sha256" 2>/dev/null)" != "$IDO_SHA256" ]; then
        fetch "$IDO_URL" "$MAC_TOOLS/ido5.3.tar.gz" "$IDO_SHA256"
        rm -rf "$MAC_TOOLS/ido5.3" && mkdir "$MAC_TOOLS/ido5.3"
        tar -xzf "$MAC_TOOLS/ido5.3.tar.gz" -C "$MAC_TOOLS/ido5.3"
        echo "$IDO_SHA256" > "$MAC_TOOLS/ido5.3/.sha256"
        echo "  IDO 5.3: $MAC_TOOLS/ido5.3"
    fi
    # mkst/gzip is GNU gzip with fill_window()'s memzero of the window's tail taken
    # out (--enable-rare-deflate), as in gzip before 1.5: the same change on 1.10.
    GZIP_URL=https://ftp.gnu.org/gnu/gzip/gzip-1.10.tar.xz
    GZIP_SHA256=8425ccac99872d544d4310305f915f5ea81e04d0f437ef1a230dc9d1c819d7c0
    if [ "$(cat "$MAC_TOOLS/gzip.sha256" 2>/dev/null)" != "$GZIP_SHA256" ] || [ ! -x "$MAC_TOOLS/gzip" ]; then
        fetch "$GZIP_URL" "$MAC_TOOLS/gzip-1.10.tar.xz" "$GZIP_SHA256"
        rm -rf "$MAC_TOOLS/gzip-1.10"
        tar -xJf "$MAC_TOOLS/gzip-1.10.tar.xz" -C "$MAC_TOOLS"
        (
            cd "$MAC_TOOLS/gzip-1.10"
            grep -q 'memzero (window + strstart + lookahead, MIN_MATCH - 1);' deflate.c
            sed -i.orig '/memzero (window + strstart + lookahead, MIN_MATCH - 1);/d' deflate.c
            ./configure --quiet --disable-dependency-tracking
            make -j"$JOBS"
        ) > "$MAC_TOOLS/gzip-build.log" 2>&1 || fail "couldn't build gzip (see $MAC_TOOLS/gzip-build.log)."
        cp "$MAC_TOOLS/gzip-1.10/gzip" "$MAC_TOOLS/gzip"
        rm -rf "$MAC_TOOLS/gzip-1.10"
        echo "$GZIP_SHA256" > "$MAC_TOOLS/gzip.sha256"
        echo "  gzip: $MAC_TOOLS/gzip"
    fi
    export RAREZIP_GZIP="$MAC_TOOLS/gzip"
fi
if [ -n "$DECOMP" ]; then
    step "Setting up Python for the decompilation (.venv)"
    if [ ! -x .venv/bin/python3 ]; then
        python3 -m venv .venv
    fi
    if [ ! -f .venv/requirements.stamp ] || [ requirements.txt -nt .venv/requirements.stamp ]; then
        .venv/bin/pip install -q -r requirements.txt
        touch .venv/requirements.stamp
    fi
    . .venv/bin/activate
    step "Extracting the game from your ROM"
    make -C conker extract
    # splat writes the assembly for every #pragma GLOBAL_ASM, so it runs again whenever
    # the decompiled sources or its configuration change.
    SPLAT_STAMP=conker/conker/build/.splat.stamp
    if [ ! -f "$SPLAT_STAMP" ] || [ -n "$(find conker/conker/src conker/conker/conker.us.yaml -newer "$SPLAT_STAMP" -print -quit)" ]; then
        make -C conker/conker extract
        mkdir -p conker/conker/build
        touch "$SPLAT_STAMP"
    fi
    step "Building the decompilation (checked against your ROM)"
    if [ -n "$MACOS" ]; then
        # macOS's cpp takes the linker script (.ld) for a file to link, not to preprocess.
        make -C conker/conker -j"$JOBS" CC="$MAC_TOOLS/ido5.3/cc" CPP="$CC -E -x c"
    else
        make -C conker/conker -j"$JOBS"
    fi
    step "Regenerating the symbols from the decompilation, and recompiling"
    sh recomp/run.sh
else
    step "Recompiling the game from your ROM"
    python3 recomp/recompile.py
fi

# ---------------------------------------------------------------- the game
step "Building the game"
if [ -n "$MACOS" ]; then
    # RT64 and RecompFrontend have Objective-C++ parts and use Apple's frameworks.
    HOST_CC=$CC
    HOST_CXX=$CXX
else
    HOST_CC=clang
    HOST_CXX=clang++
fi
if [ ! -f host/build/build.ninja ]; then
    cmake -S host -B host/build -G Ninja -DCMAKE_C_COMPILER="$HOST_CC" -DCMAKE_CXX_COMPILER="$HOST_CXX" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build host/build

# macOS: the linker's ad-hoc signature on the fresh binary is sometimes rejected by AMFI
# ("invalid, or not a simple adhoc signature"), which kills the app at launch (zsh: killed).
# Re-sign it ad-hoc so it runs. Harmless if already valid.
if [ "$(uname -s)" = Darwin ] && [ -f host/build/ConkerRecomp ]; then
    codesign --force --sign - host/build/ConkerRecomp >/dev/null 2>&1 && \
        printf 'Re-signed host/build/ConkerRecomp (ad-hoc) for macOS.\n'
fi

printf '\nDone. Run the game with:\n  %s/host/build/ConkerRecomp\n' "$ROOT"
printf 'The first time, pick Load ROM in the launcher and select your ROM (%s works).\n' "$BASEROM"
