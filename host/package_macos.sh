#!/bin/sh
# Packages the macOS build (host/build/ConkerRecomp, from build.sh) as a self-contained
# ConkerRecomp.app, which runs without Homebrew:
#   sh host/package_macos.sh [out_dir]      (default: host/build)
# The app holds the executable, assets/ in its Resources (where recompui looks on
# macOS), and in Frameworks the Homebrew libraries it uses: SDL2 (sdl2-compat), the
# SDL3 that sdl2-compat loads next to itself, and FreeType. It contains no ROM and no
# game data. It's signed ad hoc, so macOS asks before opening it the first time.
# Needs dylibbundler (brew install dylibbundler).
set -e
cd "$(dirname "$0")/.."
OUT=${1:-host/build}
BUILD=host/build
APP=$OUT/ConkerRecomp.app

fail() { printf 'package_macos: %s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || fail "this packages the macOS build; run it on macOS."
[ -x "$BUILD/ConkerRecomp" ] || fail "no $BUILD/ConkerRecomp: run ./build.sh first."
command -v dylibbundler >/dev/null 2>&1 || fail "needs dylibbundler (brew install dylibbundler)."

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BUILD/ConkerRecomp" "$APP/Contents/MacOS/"
# Local symbols only: the crash handler's backtrace names recompiled functions from
# the exported ones (-rdynamic).
strip -x "$APP/Contents/MacOS/ConkerRecomp"
cp -R host/assets "$APP/Contents/Resources/assets"

# The icon, from the launcher's thumbnail.
ICONSET=$(mktemp -d)/ConkerRecomp.iconset
mkdir -p "$ICONSET"
for size in 16 32 128 256; do
    sips -z $size $size host/assets/thumbnail.png --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) host/assets/thumbnail.png --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
cp host/assets/thumbnail.png "$ICONSET/icon_512x512.png"
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/ConkerRecomp.icns"
rm -rf "$(dirname "$ICONSET")"

# Homebrew's libraries into Frameworks, with the executable pointing at them there.
dylibbundler -of -b -x "$APP/Contents/MacOS/ConkerRecomp" -d "$APP/Contents/Frameworks" \
    -p @executable_path/../Frameworks/ >/dev/null
# sdl2-compat loads SDL3 at run time from its own folder (@loader_path/libSDL3.dylib),
# so dylibbundler can't see that one: add it by hand.
SDL3=$(brew --prefix sdl3)/lib/libSDL3.0.dylib
[ -f "$SDL3" ] || fail "no SDL3 at $SDL3 (brew install sdl3)."
cp "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
install_name_tool -id @executable_path/../Frameworks/libSDL3.dylib "$APP/Contents/Frameworks/libSDL3.dylib" 2>/dev/null
dylibbundler -of -b -x "$APP/Contents/Frameworks/libSDL3.dylib" -d "$APP/Contents/Frameworks" \
    -p @executable_path/../Frameworks/ >/dev/null

# Nothing may still point into Homebrew.
for f in "$APP/Contents/MacOS/ConkerRecomp" "$APP"/Contents/Frameworks/*.dylib; do
    if otool -L "$f" | tail -n +2 | grep -qE '/opt/homebrew|/usr/local'; then
        fail "$f still links to Homebrew: $(otool -L "$f" | grep -E '/opt/homebrew|/usr/local')"
    fi
done

# The oldest macOS it runs on: the newest any of its parts was built for.
MIN_MACOS=$(for f in "$APP/Contents/MacOS/ConkerRecomp" "$APP"/Contents/Frameworks/*.dylib; do
    otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b && $1=="minos"{print $2; exit}'
done | sort -t. -k1,1n -k2,2n | tail -1)
# The version, from the release tag: v0.1.1 or V0.1.1 (or with -something) is shown as
# 0.1.1 (macOS wants numbers there). An untagged build is 0.
VERSION=$(git describe --tags --match '[vV][0-9]*' 2>/dev/null | sed -e 's/^[vV]//' -e 's/[^0-9.].*//')

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ConkerRecomp</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.sciaschi.ConkerRecompiled</string>
    <key>CFBundleName</key>
    <string>ConkerRecomp</string>
    <key>CFBundleDisplayName</key>
    <string>Conker's Bad Fur Day: Recompiled</string>
    <key>CFBundleIconFile</key>
    <string>ConkerRecomp</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION:-0}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION:-0}</string>
    <key>LSMinimumSystemVersion</key>
    <string>${MIN_MACOS}</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# The game data guard, as in the package workflows: no ROM may end up in the app.
find "$APP" -type f | while IFS= read -r f; do
    if [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = "80371240" ]; then
        fail "$f is an N64 ROM"
    fi
done

# install_name_tool invalidated the libraries' signatures: sign everything again, ad hoc.
codesign --force --sign - "$APP"/Contents/Frameworks/*.dylib
# The app with the hardened runtime and the entitlements mods need, as Banjo: Recompiled signs
# its own (issue #81: enabling a mod killed the game). Mods patch the recompiled functions'
# code (disable-executable-page-protection, with host/macos/ld64 making __TEXT writable at all)
# and run code the live recompiler generates (allow-jit, allow-unsigned-executable-memory), and
# the bundled libraries are signed ad hoc, not by the same team (disable-library-validation).
ENTITLEMENTS="$(mktemp -t conker-entitlements).plist"
cat > "$ENTITLEMENTS" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.allow-jit</key>
	<true/>
	<key>com.apple.security.cs.allow-unsigned-executable-memory</key>
	<true/>
	<key>com.apple.security.cs.disable-executable-page-protection</key>
	<true/>
	<key>com.apple.security.cs.disable-library-validation</key>
	<true/>
</dict>
</plist>
PLIST
codesign --force --options=runtime --entitlements "$ENTITLEMENTS" --sign - "$APP"
rm -f "$ENTITLEMENTS"
codesign --verify --strict "$APP"
echo "Packaged $APP (macOS $MIN_MACOS or later)"
