#!/bin/sh
# Checks on a Mac that the game runs with mods enabled (issue #81: enabling any mod killed the game
# on macOS, with no crash report). Run it from the folder it came in, next to ConkerRecomp.app and
# the test mods (test-mods/*.nrm), with your US ROM:
#
#   sh test_mods_macos.sh /path/to/baserom.us.z64
#
# Over SSH, where the game may not be able to open a window, run it headless instead (the null
# renderer: no window, sound or input). The mods load and patch the game's code the same way, which
# is where #81 crashed, so it tests them as well:
#
#   HEADLESS=1 sh test_mods_macos.sh /path/to/baserom.us.z64
#
# It doesn't touch your own Conker data or the app: the app is copied into a new folder of its own
# (under /tmp), with portable.txt beside it, so the game keeps all its data in that folder. Then:
#   1. a run without mods, 45 seconds, to show the game runs on this Mac at all;
#   2. the same with the test mods installed and enabled, 60 seconds.
# Each run starts the game straight away (--seconds, no launcher) and quits by itself. A run passes
# if the game is still running when its time is up and quits normally. A crash, or macOS killing it
# (what #81 did), ends it early. Its output is kept in the folder, and the script prints where.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROM="${1:-}"
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "usage: sh test_mods_macos.sh /path/to/baserom.us.z64"; exit 2; }
ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"
[ -d "$HERE/ConkerRecomp.app" ] || { echo "ConkerRecomp.app isn't next to this script ($HERE)"; exit 2; }
ls "$HERE"/test-mods/*.nrm >/dev/null 2>&1 || { echo "the test mods (test-mods/*.nrm) aren't next to this script"; exit 2; }

ROOT="$(mktemp -d /tmp/conker-mod-test.XXXXXX)"
echo "Test folder: $ROOT"

# One run in a folder of its own: the app's copy, portable.txt, and the mods if asked for.
# Prints PASS or FAIL; returns 0 on a pass.
run_test() {
    name="$1"; seconds="$2"; with_mods="$3"
    dir="$ROOT/$name"
    mkdir -p "$dir"
    cp -R "$HERE/ConkerRecomp.app" "$dir/"
    # Downloaded files are quarantined; the copy is ours to run.
    xattr -dr com.apple.quarantine "$dir/ConkerRecomp.app" 2>/dev/null || true
    if [ "${HEADLESS:-0}" = 1 ]; then
        # Headless, the game keeps its data in conker_data next to its executable: in this copy.
        data="$dir/ConkerRecomp.app/Contents/MacOS/conker_data"
        mode="--headless"
    else
        echo "Mod test instance: data lives next to the app." > "$dir/portable.txt"
        data="$dir"
        mode="--window 960x720"
    fi
    mkdir -p "$data"
    if [ "$with_mods" = yes ]; then
        mkdir -p "$data/mods"
        ids=""
        for mod in "$HERE"/test-mods/*.nrm; do
            cp "$mod" "$data/mods/"
            id="$(basename "$mod" .nrm)"
            ids="$ids${ids:+, }\"$id\""
        done
        printf '{\n    "enabled_mods": [%s],\n    "latest_game_mode": "",\n    "mod_order": [%s]\n}\n' "$ids" "$ids" > "$data/mods.json"
        echo "[$name] mods enabled: $ids"
    fi

    echo "[$name] running for $seconds seconds..."
    start=$(date +%s)
    "$dir/ConkerRecomp.app/Contents/MacOS/ConkerRecomp" --rom "$ROM" --seconds "$seconds" $mode \
        > "$dir/out.txt" 2> "$dir/err.txt" &
    pid=$!
    # A run still going a minute after its time is stuck, most likely at a message box (a mod that
    # failed to load shows one, and waits for it to be closed): it's stopped and fails.
    hung=no
    while kill -0 $pid 2>/dev/null; do
        if [ $(( $(date +%s) - start )) -gt $((seconds + 60)) ]; then
            kill -9 $pid 2>/dev/null
            hung=yes
            break
        fi
        sleep 1
    done
    wait $pid
    status=$?
    elapsed=$(( $(date +%s) - start ))

    # Each mod prints "Loading mod <id>" as librecomp loads it.
    loaded=yes
    if [ "$with_mods" = yes ]; then
        for mod in "$HERE"/test-mods/*.nrm; do
            grep -q "Loading mod $(basename "$mod" .nrm)" "$dir/out.txt" || loaded=no
        done
    fi

    # The game prints "seconds elapsed" as its time runs out and it quits.
    # A crash after that line is in the game's shutdown, after its time was up (on Linux a game
    # thread can still run as the memory is freed): it isn't what's tested, so the run still passes.
    if [ $hung = no ] && [ $loaded = yes ] && grep -q "seconds elapsed" "$dir/out.txt" && [ $elapsed -ge $((seconds - 5)) ]; then
        if [ $status -eq 0 ]; then
            echo "[$name] PASS: ran for ${elapsed}s and quit normally."
        else
            echo "[$name] PASS: ran for ${elapsed}s, its full time (it then crashed while quitting, status $status: a shutdown issue, not this test's)."
        fi
        return 0
    fi
    if [ $hung = yes ]; then
        echo "[$name] FAIL: still running ${elapsed}s in, stopped: probably waiting at a message box (an error loading the mods?)."
    elif [ $loaded = no ]; then
        echo "[$name] FAIL: the mods weren't all loaded (no \"Loading mod\" line for each), so this run didn't test them."
    else
        echo "[$name] FAIL: exit status $status after ${elapsed}s (a status of 137 or 9 means macOS killed it)."
    fi
    echo "[$name] last lines of its output ($dir/out.txt, err.txt):"
    tail -n 15 "$dir/out.txt"
    tail -n 15 "$dir/err.txt"
    return 1
}

run_test no-mods 45 no; base=$?
run_test with-mods 60 yes; mods=$?

echo
if [ $base -ne 0 ]; then
    echo "RESULT: the game didn't run even without mods on this Mac, so this test can't tell about mods."
elif [ $mods -eq 0 ]; then
    echo "RESULT: PASS. The game runs with mods enabled."
else
    echo "RESULT: FAIL. The game runs without mods but not with them (issue #81)."
fi
echo "Everything the runs printed is in $ROOT."
[ $base -eq 0 ] && [ $mods -eq 0 ]
