#!/bin/sh
# Rollback across the game's own changes (netplay): every multiplayer mode, each going back often and
# deep across its scene changes, its pause and (in a Race) its match's end, must give exactly the game a plain
# replay of the same recording does, read by read. Run it after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/rollback-modes.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                                   [--recording <file>]... [--timeout 900] [--out <folder>]
#
# For each of make_match.py's seven modes (4 players, --mouse, --pause at the end; a Race ends its match by
# itself), headless and --fast, three replays of the same recording:
#   plain   the recording alone
#   test    --rollback-test 3:12: back 12 reads every 3 reads, each read played again must hash as it did
#   late    --rollback-late 8:3: ports 2 to 4 play as other machines whose inputs arrive up to 8 reads late,
#           predicted till then and gone back over when they differ
# and both must match plain. Each --recording (a real one, e.g. a long session; any port count) gets plain and
# test. It also prints how many loads couldn't be done at once (a thread waiting outside a safe point, at a
# scene change: done from an earlier state, or a read later) and fails if any rollback was given up or had no
# state, but the test's own at read 0 (the boot, which a match never predicts in). It exits 0 if all is as it
# should be, 1 if not. The logs are kept in --out; without it they're in a temporary folder, deleted if all
# passed. About a minute and a half, plus two minutes for a recording of 13 minutes.
set -u

# --recording is from the folder it's run in (the rest, as in the other scripts, from the repository's root).
here=$(pwd)
script=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
TIMEOUT=900
OUT=
RECORDINGS=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --recording)
            case "$2" in /*) recording=$2 ;; *) recording=$here/$2 ;; esac
            case "$recording" in *[[:space:]]*) echo "--recording: a path without spaces, please ($2)" >&2; exit 2 ;; esac
            [ -f "$recording" ] || { echo "no recording at $2" >&2; exit 2; }
            RECORDINGS="$RECORDINGS $recording"
            shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$script" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-rollback-modes.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
READS=0
. tools/netplay/lib.sh

modes="deathmatch war tank race beach raptor heist"
for mode in $modes; do
    python3 tools/netplay/make_match.py "$OUT/$mode.pads" --mode "$mode" --players 4 --pause --mouse --seed 7 > /dev/null || exit 2
done
n=0
for recording in $RECORDINGS; do
    n=$((n + 1))
    cp "$recording" "$OUT/recording$n.pads" || exit 2
done

failed=0
# checked <name> <plain> <reads>: same as plain, every read played again hashed as it did the first time, and
# every rollback could be done: none given up (but the test's own at read 0), with no state, or not loadable.
checked() {
    problems=$(grep -v "rollback to read 0 given up" "$OUT/$1.out" |
        grep "given up at\|no state for it\|COULDN'T\|couldn't be loaded\|played again hashes")
    given_up=$(sed -n 's/.*(rollbacks given up \([0-9]*\)), rollbacks with no state \([0-9]*\).*/\1 \2/p' "$OUT/$1.out")
    at_zero=$(grep -c "rollback to read 0 given up" "$OUT/$1.out")
    otherwise=$(sed -n 's/.*reads played again that hashed otherwise: \([0-9]*\).*/\1/p' "$OUT/$1.out")
    if [ -n "$problems" ] || [ "${given_up:-x}" != "$at_zero 0" ] || [ "${otherwise:-x}" != 0 ]; then
        echo "  $1: a rollback went wrong (given up, with no state: ${given_up:-?}; at read 0: $at_zero; played again" \
            "otherwise: ${otherwise:-?})"
        [ -n "$problems" ] && echo "$problems" | head -3 | sed 's/^/    /'
        failed=1
    fi
    same "$1" "$2" "$3" || failed=1
    not_now=$(sed -n 's/.*loads not now \([0-9]*\).*/\1/p' "$OUT/$1.out")
    echo "    $(sed -n 's/^\[harness\] rollback test: every [0-9]* reads back [0-9]*; \(.*\)/\1/p' "$OUT/$1.out"); loads not at once: ${not_now:-?}"
}
# A batch at a time (each copy has a game's memory and states).
for group in "deathmatch war tank" "race beach raptor heist"; do
    echo "$group: plain, test, late (at once)"
    names=
    for mode in $group; do
        pads=$mode.pads start "$mode-plain"
        pads=$mode.pads start "$mode-test" --rollback-test 3:12
        pads=$mode.pads start "$mode-late" --rollback-late 8:3
        names="$names $mode-plain $mode-test $mode-late"
    done
    # shellcheck disable=SC2086
    if finish "was the last: quitting" $names; then
        for mode in $group; do
            reads=$(( ($(wc -c < "$OUT/$mode.pads") - 40) / 73 ))
            checked "$mode-test" "$mode-plain" "$reads"
            # The late test logs a read once every input before it is known: the last few never are, so it's
            # compared with the plain replay's reads up to there.
            logged=$(wc -l < "$OUT/$mode-late.log")
            if [ "$logged" -lt $((reads - 8)) ]; then
                echo "  $mode-late logged $logged of $reads reads (all but the last 8 at least, as inputs come 8 late at most)"
                failed=1
            fi
            head -n "$logged" "$OUT/$mode-plain.log" > "$OUT/$mode-plain-cut.log"
            checked "$mode-late" "$mode-plain-cut" "$logged"
        done
    else
        failed=1
    fi
done
n=0
for recording in $RECORDINGS; do
    n=$((n + 1))
    echo "recording $n ($recording): plain, test (at once)"
    pads=recording$n.pads start "recording$n-plain"
    pads=recording$n.pads start "recording$n-test" --rollback-test 3:12
    if finish "was the last: quitting" "recording$n-plain" "recording$n-test"; then
        checked "recording$n-test" "recording$n-plain" "$(wc -l < "$OUT/recording$n-plain.log")"
    else
        failed=1
    fi
done

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: going back across every mode's scene changes, pause and match end gave the plain replay's game"
