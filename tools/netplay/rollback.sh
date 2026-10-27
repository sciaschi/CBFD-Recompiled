#!/bin/sh
# The rollback check (netplay): matches where the copies predict the others' inputs (the host's
# --rollback, which the joiners get with the match's start) and go back when a prediction was wrong, over a
# network made worse by the library, must still give each copy exactly what a replay of the same recording
# alone does, read by read (a copy that predicts logs a read's hash once every input before it is known and
# was given as it came). Run it after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/rollback.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 5] [--timeout 300] [--out <folder>]
#
# The matches run at the same time, headless and --fast, each copy with its own ports' pads (the others'
# scrambled):
#   both       rollback 8 reads; 100 ms lag, 10 ms jitter, 5% loss, 2% reordered, 1% duplicated
#   delay      rollback 12 (the most); 20 ms lag, then 150 ms from read 300: the host raises the input delay,
#              which a joiner that predicts must hear before it gets there
#   stall      rollback 8; input delay 4, 30 ms lag, and the joiner's game stopped for 1.5 s at read 300
#   three      a 3-player Deathmatch (make_match.py --mouse), rollback 4; 50 ms lag
#   leave      the same with rollback 8 and the second joiner quitting at read 1500: the host and the first
#              joiner play on with port 3 idle from the read the host says, and must match a solo replay so
# and a joiner given --rollback of its own must be refused (it's the host's choice).
# Then, on the timeline's real time (30 reads a second, as a player's game), 900 reads of their own:
#   paced      rollback 8, at input delay 2 with 50 ms lag and 10 ms jitter: the reads played again must not be
#              paced, so the match takes no longer than its reads (+10%, +2 s), and time sync must keep the copies
#              level: neither predicts over 3 times as many reads as the other, past 40 (without it, the copy a
#              little ahead predicts nearly every read and the other none)
#   paced-lockstep  the same without predicting, for comparison (it waits for the lag): not checked
# For each copy that predicts it prints how many reads it predicted and went back. It exits 0 if every match
# gave the solo replay's game, every copy in a rollback match went back at least once and never failed to, the
# delay change was the same on both sides, the joiner's own --rollback was refused, and the paced match kept to
# time with both copies predicting; 1 if not. The logs are kept in
# --out; without it they're in a temporary folder, deleted if all passed. It uses UDP ports 27330 to 27335.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=5
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
        -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-rollback.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
. tools/netplay/lib.sh

python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 --mouse --own-view || exit 2
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/inputs.pads" "$OUT/other.pads" 2 "$OUT/" || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse.pads" --mode deathmatch --players 3 --mouse --seed "$SEED" --own-view || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse-other.pads" --mode deathmatch --players 3 --mouse --seed "$((SEED + 1))" --own-view > /dev/null || exit 2
scramble "$OUT/mouse.pads" "$OUT/mouse-other.pads" 3 "$OUT/mouse-" || exit 2
MOUSE_READS=$(( ($(wc -c < "$OUT/mouse.pads") - 40) / 73 ))
PACED_READS=900
python3 tools/netplay/make_inputs.py "$OUT/paced.pads" --reads "$PACED_READS" --seed "$SEED" --ports 2 --mouse --own-view > /dev/null || exit 2
python3 tools/netplay/make_inputs.py "$OUT/paced-other.pads" --reads "$PACED_READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/paced.pads" "$OUT/paced-other.pads" 2 "$OUT/paced-" || exit 2

failed=0
echo "solo, mouse-solo, paced-solo"
start solo
pads=mouse.pads start mouse-solo
pads=paced.pads start paced-solo
finish "was the last: quitting" solo mouse-solo paced-solo || exit 1

bad="lag=100,jitter=10,loss=5,reorder=2,dup=1"
echo "refused"
# A joiner's own --rollback: refused before it connects (the code is never used).
if timeout 60 "$EXE" --headless --rom "$ROM" --data "$OUT/data-refused" --fast --netplay-join 127.0.0.1:27339 --rollback 8 2>&1 |
    grep -q "host's choice"; then
    echo "  a joiner's own --rollback is refused"
else
    echo "  a joiner's own --rollback wasn't refused"
    failed=1
fi
rm -rf "$OUT/data-refused"

echo "both, delay, stall, three, leave (at once)"
pads=1.pads start both-host --netplay-host 27330 --netplay-address 127.0.0.1 --players 2 --rollback 8 --netplay-fake-network "$bad"
pads=2.pads start both-join --netplay-join "$(code_of both-host)" --netplay-fake-network "$bad"
pads=1.pads start delay-host --netplay-host 27332 --netplay-address 127.0.0.1 --players 2 --rollback 12 --netplay-fake-network "lag=20,lag-at=300:150"
pads=2.pads start delay-join --netplay-join "$(code_of delay-host)" --netplay-fake-network "lag=20,lag-at=300:150"
pads=1.pads start stall-host --netplay-host 27333 --netplay-address 127.0.0.1 --players 2 --input-delay 4 --rollback 8 --netplay-fake-network lag=30
pads=2.pads start stall-join --netplay-join "$(code_of stall-host)" --netplay-fake-network lag=30 --netplay-stall-at 300:1500
# A joiner's port is the lowest free one: the second joins once the first is in.
wait_in() {
    i=0
    while ! grep -q "in, on port $2" "$OUT/$1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
}
pads=mouse-1.pads start three-host --netplay-host 27334 --netplay-address 127.0.0.1 --players 3 --rollback 4 --netplay-fake-network lag=50
code=$(code_of three-host)
pads=mouse-2.pads start three-join1 --netplay-join "$code" --netplay-fake-network lag=50
wait_in three-join1 2
pads=mouse-3.pads start three-join2 --netplay-join "$code" --netplay-fake-network lag=50
pads=mouse-1.pads start leave-host --netplay-host 27335 --netplay-address 127.0.0.1 --players 3 --rollback 8 --netplay-fake-network lag=50
code=$(code_of leave-host)
pads=mouse-2.pads start leave-join1 --netplay-join "$code" --netplay-fake-network lag=50
wait_in leave-join1 2
pads=mouse-3.pads start leave-join2 --netplay-join "$code" --netplay-fake-network lag=50 --stop-after 1500

# predicted <name>: the copy predicted and went back at least once, and every rollback could be done.
predicted() {
    line=$(sed -n 's/^\[netplay\] rollback: //p' "$OUT/$1.out")
    if grep -q "COULDN'T\|given up at\|no state for it" "$OUT/$1.out"; then
        echo "  $1: a rollback couldn't be done ($line)"
        return 1
    fi
    case "$line" in
        *" 0 rollbacks"*|"") echo "  $1 never went back (${line:-nothing printed})"; return 1 ;;
    esac
    echo "    rollback: $line"
}
# shellcheck disable=SC2086
if finish "was the last: quitting" both-host both-join delay-host delay-join stall-host stall-join three-host three-join1 \
    three-join2 leave-host leave-join1 leave-join2; then
    for name in both-host both-join delay-host delay-join stall-host stall-join; do
        same "$name" || failed=1
        predicted "$name" || failed=1
    done
    for name in three-host three-join1 three-join2; do
        same "$name" mouse-solo "$MOUSE_READS" || failed=1
        predicted "$name" || failed=1
    done
    # The delay changes, as the host made them and the joiner heard them ("<ports> becomes <d> from read <r>").
    # (Not `made`: lib.sh's list of the data folders to delete.)
    host_made=$(sed -n 's/^\[netplay\] read [0-9]*: the input delay of \(.*\) becomes \([0-9]*\) from read \([0-9]*\) .*/\1: \2 \3/p' "$OUT/delay-host.out")
    heard=$(sed -n 's/^\[netplay\] read [0-9]*: the input delay of \(.*\) becomes \([0-9]*\) from read \([0-9]*\) .*/\1: \2 \3/p' "$OUT/delay-join.out")
    if [ -z "$host_made" ] || [ "$host_made" != "$heard" ]; then
        echo "  delay: the delay changes differ or there are none: host: ${host_made:-none}; joiner: ${heard:-none}"
        failed=1
    else
        echo "  delay: the input delay became $(echo "$host_made" | sed 's/\(.*\) \([0-9]*\)$/\1 from read \2/' | paste -sd, - | sed 's/,/, /g'), the same on both"
    fi
    # Port 3 leaving: both say the same read, and the game is the solo replay's with port 3 idle from there.
    from=$(left_at leave-host 3)
    if [ -z "$from" ] || [ "$(left_at leave-join1 3)" != "$from" ]; then
        echo "  leave: the host and the first joiner don't say the same read for port 3 leaving"
        failed=1
    else
        echo "  leave: port 3 idle from read $from"
        idle_from "$OUT/mouse.pads" "$OUT/leave-solo.pads" 3 "$from" || exit 2
        pads=leave-solo.pads start leave-solo
        if finish "was the last: quitting" leave-solo; then
            same leave-host leave-solo "$MOUSE_READS" || failed=1
            predicted leave-host || failed=1
            same leave-join1 leave-solo "$MOUSE_READS" || failed=1
            predicted leave-join1 || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "paced, paced-lockstep (at once, $PACED_READS reads each on real time)"
net="lag=50,jitter=10"
paced=1 pads=paced-1.pads start paced-host --netplay-host 27330 --netplay-address 127.0.0.1 --players 2 --input-delay 2 --rollback 8 --netplay-fake-network "$net"
paced=1 pads=paced-2.pads start paced-join --netplay-join "$(code_of paced-host)" --netplay-fake-network "$net"
paced=1 pads=paced-1.pads start paced-lockstep-host --netplay-host 27331 --netplay-address 127.0.0.1 --players 2 --input-delay 2 --netplay-fake-network "$net"
paced=1 pads=paced-2.pads start paced-lockstep-join --netplay-join "$(code_of paced-lockstep-host)" --netplay-fake-network "$net"
if finish "was the last: quitting" paced-host paced-join paced-lockstep-host paced-lockstep-join; then
    # Its time from the first read to the last (the rollback summary's) against the reads' own.
    most=$(( PACED_READS * 11 / 300 + 2 ))
    # Time sync: both copies predict, about as much (a copy behind has the other's inputs already and needn't).
    # Below 40 reads either way the counts are scheduling noise (level, inputs come with a few ms to spare).
    host_predicted=$(sed -n 's/^\[netplay\] rollback: \([0-9]*\) reads predicted.*/\1/p' "$OUT/paced-host.out")
    join_predicted=$(sed -n 's/^\[netplay\] rollback: \([0-9]*\) reads predicted.*/\1/p' "$OUT/paced-join.out")
    if [ -z "$host_predicted" ] || [ -z "$join_predicted" ] ||
        { [ "$host_predicted" -gt 40 ] && [ "$host_predicted" -gt $((3 * join_predicted)) ]; } ||
        { [ "$join_predicted" -gt 40 ] && [ "$join_predicted" -gt $((3 * host_predicted)) ]; }; then
        echo "  paced: the copies predicted ${host_predicted:-?} and ${join_predicted:-?} reads: time sync didn't keep them level"
        failed=1
    else
        echo "  paced: the copies predicted $host_predicted and $join_predicted reads (time sync keeps them level)"
    fi
    for name in paced-host paced-join; do
        same "$name" paced-solo "$PACED_READS" || failed=1
        if grep -q "COULDN'T\|given up at\|no state for it" "$OUT/$name.out"; then
            echo "  $name: a rollback couldn't be done"
            failed=1
        fi
        echo "    $(sed -n 's/^\[netplay\] \(time sync: .*\)/\1/p' "$OUT/$name.out")"
        took=$(sed -n 's/^\[harness\] rollback test: .* rollbacks in \([0-9.]*\) s$/\1/p' "$OUT/$name.out")
        if [ -z "$took" ] || awk "BEGIN { exit !($took > $most) }"; then
            echo "  $name took ${took:-?} s for $PACED_READS reads (at most $most s): the reads played again were paced"
            failed=1
        else
            echo "    took $took s for $PACED_READS reads ($(( PACED_READS / 30 )) s of game)"
        fi
    done
    for name in paced-lockstep-host paced-lockstep-join; do
        same "$name" paced-solo "$PACED_READS" || failed=1
        echo "    waits: $(sed -n 's/^\[netplay\] waits: //p' "$OUT/$name.out")"
    done
else
    failed=1
fi

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: every match with copies that predict gave each copy the same game as the replay alone, and on real time kept to it"
