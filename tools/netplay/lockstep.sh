#!/bin/sh
# The lockstep check (netplay): two copies of the game on this machine play a match over
# loopback, each giving its own port's pads from the same made-up 2-port recording, and must give the
# game exactly what a replay of that recording alone does, read by read. Run it after building with
# CONKER_NETPLAY=ON:
#
#   tools/netplay/lockstep.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 2] [--timeout 300] [--out <folder>]
#
# The runs, all headless and --fast, each copy with a data folder of its own (--data):
#   solo       the recording replayed alone
#   match      host and joiner, input delay 2: both hash logs must match solo's. Each is given a copy
#              of the recording with the other's port scrambled, so it only gets the right game if the
#              other's pads come over the network
#   mouse      a 3-player Deathmatch where everyone moves their mouse, gyro and scroll wheel and aims
#              (make_match.py --mouse), alone and as a match of three at the automatic input delay, each
#              copy with the others' pads and movement scrambled: all three must match the solo replay
#   couch      the same 3-player Deathmatch with two players on one copy (--local-players 2): the host
#              with ports 1 and 2 and a joiner with port 3, then the host with port 1 and a joiner with 2
#              and 3; each copy's file has the other copy's ports scrambled. Two players' views are shown two
#              to a window, which widens their culls: each match must match a solo replay with those view pairs
#   tall       a 2-player Deathmatch (make_match.py --mouse): the split screen's two views are drawn taller in
#              "my view only", the match's rule, so every copy widens each camera's culls up and down: both copies
#              of a match of two must match its solo replay
#   jittery    the same with input delay 0 and random sleeps (--jitter 500) on the joiner, so the two
#              are out of step and wait for each other all the time
#   leave      the mouse match, the second joiner quitting at read 1500: the host and the first joiner play on,
#              port 3 idle from the read the host says (both must say the same), and must match a solo replay
#              of the recording with port 3 idle from there
#   leave-crash the same with the second joiner killed at about read 1500 (no goodbye): the host notices it stopped
#              answering, and the two play on as above
#   leave-couch the couch match with ports 2 and 3 on a joiner that quits at read 1500: the host plays on, both
#              ports idle from the same read, and must match a solo replay so
#   desync     the host changes a byte of its memory at read 300: both must stop for a desync, and each
#              write a desync report naming the region changed (64, at 0x80400000) with its memory, which
#              compare_desyncs.py must find the byte in
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out; without it they're in a
# temporary folder, deleted if all passed. The data folders it makes for the copies (about 1 GB in all) are
# always deleted. As in check.sh, a run is judged by what it prints and then stopped. It uses UDP ports
# 27310 to 27319 on 127.0.0.1.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=2
TIMEOUT=300
OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-lockstep.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
. tools/netplay/lib.sh

# The recordings say everyone sees only their own view, as the matches below are (the host's rule by default),
# so the solo replays have the same culls.
python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 --mouse --own-view || exit 2
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/inputs.pads" "$OUT/other.pads" 2 "$OUT/" || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse.pads" --mode deathmatch --players 3 --mouse --seed "$SEED" --own-view || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse-other.pads" --mode deathmatch --players 3 --mouse --seed "$((SEED + 1))" --own-view > /dev/null || exit 2
scramble "$OUT/mouse.pads" "$OUT/mouse-other.pads" 3 "$OUT/mouse-" || exit 2
# The couch matches show two players' views two to a window (ports 1 and 2, then 2 and 3), which changes their
# culls: their solo replays are the mouse recording marked so (the header's view pairs).
for pairs in 12 23; do
    python3 -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[36:40]=sum(1<<(int(p)-1) for p in sys.argv[2]).to_bytes(4,'little'); open(sys.argv[3],'wb').write(d)" \
        "$OUT/mouse.pads" $pairs "$OUT/couch-solo$pairs.pads" || exit 2
done
scramble_own "$OUT/couch-solo12.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-12.pads" 12 || exit 2
scramble_own "$OUT/couch-solo12.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-3.pads" 3 || exit 2
scramble_own "$OUT/couch-solo23.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-23.pads" 23 || exit 2
scramble_own "$OUT/couch-solo23.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-1.pads" 1 || exit 2
MOUSE_READS=$(( ($(wc -c < "$OUT/mouse.pads") - 40) / 73 ))
python3 tools/netplay/make_match.py "$OUT/tall.pads" --mode deathmatch --players 2 --mouse --seed "$SEED" --own-view || exit 2
python3 tools/netplay/make_match.py "$OUT/tall-other.pads" --mode deathmatch --players 2 --mouse --seed "$((SEED + 1))" --own-view > /dev/null || exit 2
scramble "$OUT/tall.pads" "$OUT/tall-other.pads" 2 "$OUT/tall-" || exit 2
TALL_READS=$(( ($(wc -c < "$OUT/tall.pads") - 40) / 73 ))


failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

echo "match"
pads=1.pads start match-host --netplay-host 27310 --netplay-address 127.0.0.1 --players 2 --input-delay 2
pads=2.pads start match-join --netplay-join "$(code_of match-host)"
if finish "was the last: quitting" match-host match-join; then
    same match-host || failed=1
    same match-join || failed=1
else
    failed=1
fi

echo "jittery"
start jittery-host --netplay-host 27311 --netplay-address 127.0.0.1 --players 2 --input-delay 0
start jittery-join --netplay-join "$(code_of jittery-host)" --input-delay 0 --jitter 500
if finish "was the last: quitting" jittery-host jittery-join; then
    same jittery-host || failed=1
    same jittery-join || failed=1
else
    failed=1
fi

echo "mouse"
pads=mouse.pads start mouse-solo
finish "was the last: quitting" mouse-solo || exit 1
pads=mouse-1.pads start mouse-host --netplay-host 27313 --netplay-address 127.0.0.1 --players 3
code=$(code_of mouse-host)
pads=mouse-2.pads start mouse-join1 --netplay-join "$code"
# Ports go to joiners in the order they're let in, and each copy plays its own file's port: the second waits
# for the first to be in (port 2).
i=0
while ! grep -q "in, on port 2" "$OUT/mouse-join1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
pads=mouse-3.pads start mouse-join2 --netplay-join "$code"
if finish "was the last: quitting" mouse-host mouse-join1 mouse-join2; then
    for name in mouse-host mouse-join1 mouse-join2; do
        same $name mouse-solo "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi

# idle_from <recording> <out> <ports> <read>: <out>, the recording with each of <ports> (port numbers from 1, e.g.
# 23) idle from <read> on, as a match gives a player that left: nothing pressed or moved, their last Controls.
idle_from() {
    python3 - "$@" <<'PY'
import sys
src, out, ports, first = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
data = bytearray(open(src, "rb").read())
header, read_size, pad_size, motion_size = 40, 73, 5, 13   # version 4 (harness.cpp)
reads = (len(data) - header) // read_size
assert 0 < first <= reads
for port in (int(p) - 1 for p in ports):
    last = header + (first - 1) * read_size + 1 + 4 * pad_size + port * motion_size
    controls = data[last + 9:last + 13]
    for start in range(header + first * read_size, len(data), read_size):
        pad = start + 1 + port * pad_size
        motion = start + 1 + 4 * pad_size + port * motion_size
        data[pad:pad + pad_size] = bytes(pad_size)
        data[motion:motion + motion_size] = bytes(9) + controls
open(out, "wb").write(data)
PY
}

# left_at <name> <port>: the read from which <name> said port <port> plays no more.
left_at() {
    sed -n "s/^\[netplay\] port $2 plays no more: no input from read \([0-9]*\).*/\1/p" "$OUT/$1.out" | head -n 1
}

echo "leave"
# The same 3-player match, the second joiner (port 3) quitting at read 1500: the host and the first joiner play on
# with port 3 idle from the read the host says, and must match a solo replay of the recording so.
pads=mouse-1.pads start leave-host --netplay-host 27317 --netplay-address 127.0.0.1 --players 3
code=$(code_of leave-host)
pads=mouse-2.pads start leave-join1 --netplay-join "$code"
i=0
while ! grep -q "in, on port 2" "$OUT/leave-join1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
pads=mouse-3.pads start leave-join2 --netplay-join "$code" --stop-after 1500
if finish "was the last: quitting" leave-host leave-join1 leave-join2; then
    from=$(left_at leave-host 3)
    if [ -z "$from" ] || [ "$(left_at leave-join1 3)" != "$from" ]; then
        echo "  the host and the first joiner don't say the same read for port 3 leaving ('$from', '$(left_at leave-join1 3)')"
        failed=1
    else
        echo "  port 3 idle from read $from"
        idle_from "$OUT/mouse.pads" "$OUT/leave-solo.pads" 3 "$from" || exit 2
        pads=leave-solo.pads start leave-solo
        if finish "was the last: quitting" leave-solo; then
            same leave-host leave-solo "$MOUSE_READS" || failed=1
            same leave-join1 leave-solo "$MOUSE_READS" || failed=1
            # Port 3 idle must make a difference, or this shows nothing.
            if python3 tools/netplay/compare_hashes.py "$OUT/mouse-solo.log" "$OUT/leave-host.log" > /dev/null; then
                echo "  leave-host is the same as mouse-solo: port 3 leaving changed nothing"
                failed=1
            fi
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "leave-couch"
# Two players on one joiner (ports 2 and 3) leaving together at read 1500: the host plays on alone.
pads=couch-1.pads start leave-couch-host --netplay-host 27318 --netplay-address 127.0.0.1 --players 3
pads=couch-23.pads start leave-couch-join --netplay-join "$(code_of leave-couch-host)" --local-players 2 --stop-after 1500
if finish "was the last: quitting" leave-couch-host leave-couch-join; then
    from=$(left_at leave-couch-host 2)
    if [ -z "$from" ] || [ "$(left_at leave-couch-host 3)" != "$from" ]; then
        echo "  ports 2 and 3 didn't leave at the same read"
        failed=1
    else
        echo "  ports 2 and 3 idle from read $from"
        idle_from "$OUT/couch-solo23.pads" "$OUT/leave-couch-solo.pads" 23 "$from" || exit 2
        pads=leave-couch-solo.pads start leave-couch-solo
        if finish "was the last: quitting" leave-couch-solo; then
            same leave-couch-host leave-couch-solo "$MOUSE_READS" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "leave-crash"
# The 3-player match again, the second joiner killed at read 1500 (no goodbye): the host notices it stopped answering
# (in about 10 s), and the two play on, as above.
pads=mouse-1.pads start crash-host --netplay-host 27319 --netplay-address 127.0.0.1 --players 3
code=$(code_of crash-host)
pads=mouse-2.pads start crash-join1 --netplay-join "$code"
i=0
while ! grep -q "in, on port 2" "$OUT/crash-join1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
pads=mouse-3.pads start crash-join2 --netplay-join "$code"
crashed=${pids##* }
i=0
while [ "$(cat "$OUT/crash-join2.log" 2>/dev/null | wc -l)" -lt 1500 ] && [ $i -lt 1200 ]; do sleep 0.1; i=$((i + 1)); done
kill -9 "$crashed" 2>/dev/null
wait "$crashed" 2>/dev/null
# It isn't waited for to finish.
procs=$(echo "$procs" | tr ' ' '\n' | grep -v '^crash-join2:' | tr '\n' ' ')
if finish "was the last: quitting" crash-host crash-join1; then
    from=$(left_at crash-host 3)
    if [ -z "$from" ] || [ "$(left_at crash-join1 3)" != "$from" ]; then
        echo "  the host and the first joiner don't say the same read for port 3 ('$from', '$(left_at crash-join1 3)')"
        failed=1
    else
        echo "  port 3 stopped answering; idle from read $from"
        idle_from "$OUT/mouse.pads" "$OUT/crash-solo.pads" 3 "$from" || exit 2
        pads=crash-solo.pads start crash-solo
        if finish "was the last: quitting" crash-solo; then
            same crash-host crash-solo "$MOUSE_READS" || failed=1
            same crash-join1 crash-solo "$MOUSE_READS" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "couch"
# One at a time: finish counts a copy that quits after its last read, before the other's, as quitting early.
pads=couch-solo12.pads start couch-solo12
finish "was the last: quitting" couch-solo12 || exit 1
pads=couch-solo23.pads start couch-solo23
finish "was the last: quitting" couch-solo23 || exit 1
pads=couch-12.pads start couch-host2 --netplay-host 27314 --netplay-address 127.0.0.1 --players 3 --local-players 2
pads=couch-3.pads start couch-join1 --netplay-join "$(code_of couch-host2)"
if finish "was the last: quitting" couch-host2 couch-join1; then
    grep -q "this is ports 1 and 2" "$OUT/couch-host2.out" || { echo "  couch-host2 isn't on ports 1 and 2"; failed=1; }
    # The automatic delay counts the joining machine's round trip (one, on loopback: 0), not its players.
    grep -q "this is ports 1 and 2, input delay 0" "$OUT/couch-host2.out" || { echo "  couch-host2's automatic input delay isn't 0"; failed=1; }
    for name in couch-host2 couch-join1; do
        same $name couch-solo12 "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi
pads=couch-1.pads start couch-host1 --netplay-host 27315 --netplay-address 127.0.0.1 --players 3 --input-delay 2
pads=couch-23.pads start couch-join2 --netplay-join "$(code_of couch-host1)" --local-players 2
if finish "was the last: quitting" couch-host1 couch-join2; then
    grep -q "this is ports 2 and 3" "$OUT/couch-join2.out" || { echo "  couch-join2 isn't on ports 2 and 3"; failed=1; }
    for name in couch-host1 couch-join2; do
        same $name couch-solo23 "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi

echo "tall"
pads=tall.pads start tall-solo
finish "was the last: quitting" tall-solo || exit 1
pads=tall-1.pads start tall-host --netplay-host 27316 --netplay-address 127.0.0.1 --players 2
pads=tall-2.pads start tall-join --netplay-join "$(code_of tall-host)"
if finish "was the last: quitting" tall-host tall-join; then
    for name in tall-host tall-join; do
        same $name tall-solo "$TALL_READS" || failed=1
    done
else
    failed=1
fi

echo "desync"
start desync-host --netplay-host 27312 --netplay-address 127.0.0.1 --players 2 --netplay-desync-at 300
start desync-join --netplay-join "$(code_of desync-host)"
if finish "the netplay match is over" desync-host desync-join; then
    for name in desync-host desync-join; do
        # The desync itself: this peer's comparison found it ("desync at read"), or the other's did and
        # said so (a desync); not merely the host's note that it changed its memory.
        if grep -q '\[netplay\] desync at read\|(a desync)' "$OUT/$name.out"; then
            echo "  $name: $(grep -m1 '\[netplay\] desync at\|the match is over' "$OUT/$name.out")"
        else
            echo "  $name stopped, but not for a desync"
            failed=1
        fi
        # Its report: the changed byte is half way into game memory, region 64.
        report=$(ls "$OUT/data-$name/netplay-desyncs/"desync-*.json 2>/dev/null | head -n 1)
        if [ -z "$report" ]; then
            echo "  $name wrote no desync report"
            failed=1
        elif ! python3 -c 'import json, sys; r = json.load(open(sys.argv[1])); sys.exit(r["differing_regions"] != [64] or not r["memory"])' "$report"; then
            echo "  $name's report doesn't name region 64 alone, or has no memory: $report"
            failed=1
        fi
    done
    reports=$(ls "$OUT"/data-desync-host/netplay-desyncs/desync-*.json "$OUT"/data-desync-join/netplay-desyncs/desync-*.json 2>/dev/null)
    # shellcheck disable=SC2086
    if python3 tools/netplay/compare_desyncs.py $reports > "$OUT/desync-compare.out" 2>&1 &&
        grep -q '^regions that differ: 1$' "$OUT/desync-compare.out" && grep -q '^  0x80400003-0x80400003 (' "$OUT/desync-compare.out"; then
        echo "  the reports compare: $(grep -A1 '^ranges of memory that differ' "$OUT/desync-compare.out" | tail -n 1 | sed 's/^ *//')"
    else
        echo "  the reports don't compare as they should (desync-compare.out)"
        failed=1
    fi
else
    failed=1
fi

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: the match gave each copy the same game as the replay alone, and a desync was caught"
