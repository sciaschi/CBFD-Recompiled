#!/bin/sh
# Shared by lockstep.sh and network.sh (sourced, from the repo's root): running copies of the game against
# each other on this machine, each with its own data folder, and comparing their hash logs. They set EXE, ROM,
# OUT, READS and TIMEOUT first.

# However the check ends (Ctrl-C too): the copies still running are stopped, and the data folders this
# made deleted (only those: one already in an --out folder is left alone).
made=
pids=
procs=
cleanup() {
    for pid in $pids; do kill -9 "$pid" 2>/dev/null; done
    for name in $made; do rm -rf "$OUT/data-$name"; done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# scramble <recording> <other> <ports> <prefix>: <prefix><n>.pads for each port n (from 1), the recording
# with every other port's pads and movement taken from <other> (the same length, from another seed).
scramble() {
    python3 - "$@" <<'PY'
import sys
real_path, other_path, ports, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
real = open(real_path, "rb").read()
other = open(other_path, "rb").read()
assert len(real) == len(other)
header, read_size, pad_size, motion_size = 40, 73, 5, 13   # version 4 (harness.cpp)
for own in range(ports):
    scrambled = bytearray(real)
    for port in range(ports):
        if port == own:
            continue
        for start in range(header, len(real), read_size):
            pad = start + 1 + port * pad_size
            motion = start + 1 + 4 * pad_size + port * motion_size
            scrambled[pad:pad + pad_size] = other[pad:pad + pad_size]
            scrambled[motion:motion + motion_size] = other[motion:motion + motion_size]
    assert scrambled != real
    open(f"{prefix}{own + 1}.pads", "wb").write(scrambled)
PY
}

# scramble_own <recording> <other> <ports> <out> <own>: <out>, the recording with every port but those in
# <own> (port numbers from 1, e.g. 12) scrambled from <other>, for a copy playing several ports.
scramble_own() {
    python3 - "$@" <<'PY'
import sys
real_path, other_path, ports, out, own = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
real = open(real_path, "rb").read()
other = open(other_path, "rb").read()
assert len(real) == len(other)
header, read_size, pad_size, motion_size = 40, 73, 5, 13   # version 4 (harness.cpp)
scrambled = bytearray(real)
for port in range(ports):
    if str(port + 1) in own:
        continue
    for start in range(header, len(real), read_size):
        pad = start + 1 + port * pad_size
        motion = start + 1 + 4 * pad_size + port * motion_size
        scrambled[pad:pad + pad_size] = other[pad:pad + pad_size]
        scrambled[motion:motion + motion_size] = other[motion:motion + motion_size]
assert scrambled != real
open(out, "wb").write(scrambled)
PY
}

# start <name> [options...]: one copy in the background, with its own data folder, its hash log in
# $OUT/<name>.log and its output in $OUT/<name>.out; its PID is added to $pids.
# The recording is $OUT/inputs.pads, or $OUT/$pads if that's set; it runs --fast unless $paced is set (on
# the timeline's real time, as a player would).
start() {
    name=$1; shift
    rm -f "$OUT/$name.log" "$OUT/$name.out"
    [ -e "$OUT/data-$name" ] || made="$made $name"
    speed=--fast
    [ -n "${paced:-}" ] && speed=
    # shellcheck disable=SC2086
    "$EXE" --headless --rom "$ROM" --data "$OUT/data-$name" $speed --replay "$OUT/${pads:-inputs.pads}" \
        --hash-log "$OUT/$name.log" "$@" > "$OUT/$name.out" 2>&1 &
    pids="$pids $!"
    procs="$procs $name:$!"
}

# code_of <name>: the join code the hosting copy <name> printed (its key is random each time), once it has.
code_of() {
    i=0
    while [ $i -lt 600 ]; do
        code=$(sed -n 's/^\[netplay\] join code \([^ ]*\).*/\1/p' "$OUT/$1.out" 2>/dev/null)
        [ -n "$code" ] && { echo "$code"; return; }
        sleep 0.1
        i=$((i + 1))
    done
    echo "no-code-from-$1"
}

# finish <pattern> <name...>: waits until each named copy prints <pattern> (true), or one quits or the
# time runs out first (false); then kills the copies started.
finish() {
    pattern=$1; shift
    began=$(date +%s)
    result=
    while [ -z "$result" ]; do
        all=1
        for name in "$@"; do
            grep -q "$pattern" "$OUT/$name.out" 2>/dev/null || all=0
        done
        if [ $all -eq 1 ]; then
            result=done
        elif [ $(( $(date +%s) - began )) -ge "$TIMEOUT" ]; then
            result="not done in $TIMEOUT s"
        else
            # A copy that printed <pattern> may have quit since (several matches at once end at different times).
            for proc in $procs; do
                kill -0 "${proc##*:}" 2>/dev/null || grep -q "$pattern" "$OUT/${proc%%:*}.out" 2>/dev/null ||
                    result="${proc%%:*} quit early"
            done
            sleep 0.2
        fi
    done
    sleep 0.3
    for pid in $pids; do
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    pids=
    procs=
    if [ "$result" != done ]; then
        echo "  $result"
        for name in "$@"; do
            echo "  -- $name.out (last lines)"
            grep -v '^\[rt64\]' "$OUT/$name.out" | tail -n 8 | sed 's/^/    /'
        done
        return 1
    fi
    echo "  in $(( $(date +%s) - began )) s"
}

# same <name> [<solo> <reads>]: whether <name>'s hash log is solo's (or <solo>'s, of <reads> reads).
same() {
    ref=${2:-solo}
    reads=${3:-$READS}
    if result=$(python3 tools/netplay/compare_hashes.py "$OUT/$ref.log" "$OUT/$1.log"); then
        lines=$(wc -l < "$OUT/$1.log")
        if [ "$lines" -eq "$reads" ]; then
            echo "  $1 same as $ref: $result"
            return 0
        fi
        echo "  $1 has $lines reads, not $reads"
        return 1
    fi
    echo "  $1 NOT the same as $ref: $result"
    return 1
}
