#!/bin/sh
# The determinism check (netplay, issue #126): replays one made-up recording headless four ways and
# checks that each gives the game the same memory, read by read. The CI runs it (netplay.yml); run it
# yourself after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/check.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                          [--reads 2700] [--seed 1] [--timeout 300] [--out <folder>]
#                          [--mouse] [--recording <file.pads>]
#
# The runs, all on the timeline as fast as the host can (--fast):
#   a, b     the same replay twice
#   jitter   with random sleeps in the game's OS calls and graphics thread (a slower, uneven host)
#   state    a save state taken at a third of the way and loaded at two thirds (it goes back and on again)
# --mouse gives the made-up recording mouse and gyro movement and settings of its own; --recording replays
# that recording instead of a made-up one (e.g. a match from make_match.py --mouse), and --reads is then
# its length.
# b, jitter and state must each match a. The logs are kept in --out; without it they're in a temporary
# folder, printed at the end and deleted if everything matched. It exits 0 if everything matches, 1 if not.
#
# A run is judged by what it prints, not by its exit code: once it says its last read was made (its hash
# log is written out by then) it's stopped, rather than waiting for it to quit. The runs share the game's data folder, so they're made one at a time, and nothing else may
# use the same build at the time.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=2700
SEED=1
TIMEOUT=300
OUT=
MOUSE=
RECORDING=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --mouse) MOUSE=--mouse; shift ;;
        --recording) RECORDING=$2; shift 2 ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-check.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

if [ -n "$RECORDING" ]; then
    cp "$RECORDING" "$OUT/inputs.pads" || exit 2
    # Its reads: the header (40 bytes in version 4, 32 in 2 and 3, 16 in 1), then 73 bytes a read in version 4 (65 in 3, 21 in 2).
    READS=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); v=struct.unpack_from('<I',d,8)[0]; print((len(d)-{4: 40, 1: 16}.get(v, 32))//{4: 73, 3: 65}.get(v, 21))" "$OUT/inputs.pads") || exit 2
else
    python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" $MOUSE || exit 2
fi

# run <name> [harness options...]: one replay, its hash log in $OUT/<name>.log and its output in
# $OUT/<name>.out. Fails if it doesn't reach its last read within $TIMEOUT seconds, or its log is short.
run() {
    name=$1; shift
    out=$OUT/$name.out
    rm -f "$OUT/$name.log"
    start=$(date +%s)
    "$EXE" --headless --rom "$ROM" --fast --replay "$OUT/inputs.pads" --hash-log "$OUT/$name.log" "$@" > "$out" 2>&1 &
    pid=$!
    result=
    while [ -z "$result" ]; do
        if grep -q "was the last: quitting" "$out"; then
            result=done
        elif ! kill -0 $pid 2>/dev/null; then
            result="quit before its last read"
        elif [ $(( $(date +%s) - start )) -ge "$TIMEOUT" ]; then
            result="didn't reach its last read in $TIMEOUT s"
        else
            sleep 0.1
        fi
    done
    sleep 0.3
    kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null
    if [ "$result" != done ]; then
        echo "$name: $result (its output: $out)"
        tail -n 20 "$out" | sed 's/^/    /'
        return 1
    fi
    lines=$(wc -l < "$OUT/$name.log")
    if [ "$lines" -ne "$READS" ]; then
        echo "$name: its hash log has $lines reads, not $READS"
        return 1
    fi
    echo "$name: $READS reads in $(( $(date +%s) - start )) s"
}

# same <name>: whether <name>'s hash log matches a's.
same() {
    if result=$(python3 tools/netplay/compare_hashes.py "$OUT/a.log" "$OUT/$1.log"); then
        echo "  same as a: $result"
    else
        echo "  NOT the same as a: $result"
        return 1
    fi
}

failed=0
run a || exit 1
run b && same b || failed=1
run jitter --jitter 500 && same jitter || failed=1
run state --state-test $((READS / 3)):$((READS * 2 / 3)) && same state || failed=1

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED: the same inputs didn't give the same game"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: the same inputs gave the same game every time"
