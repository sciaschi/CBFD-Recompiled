#!/bin/sh
# The bad-network check (netplay): matches of two copies of the game on this machine over loopback, with
# the network made worse by the library (--netplay-fake-network), must still give each copy exactly what a
# replay of the same recording alone does, read by read; and it says how long each copy's game waited for
# the other's input, so a change that makes matches feel worse shows up. Run it after building with
# CONKER_NETPLAY=ON:
#
#   tools/netplay/network.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                            [--reads 900] [--seed 3] [--timeout 300] [--out <folder>] [--record <file>]
#
# The matches run at the same time, on the timeline's real time (30 reads a second, as a player's game:
# not --fast), each copy with its own port's pads from a made-up 2-port recording (the other's scrambled, so
# it only gets the right game if the other's come over the network), at the automatic input delay:
#   lan        loopback as it is
#   bad        both send with 100 ms lag, 10 ms jitter, 5% loss, 2% reordered and 1% duplicated
#   outage     30 ms lag, and the joiner's network out for 3 s from read 300
#   lag-change 20 ms lag, then 150 ms from read 300 (the delay chosen at the start no longer covers it):
#              the host must raise the delay, and both copies switch at the same read
#   lag-drop   150 ms lag, then 20 ms from read 300: the host must lower the delay
#   stall      input delay 4, 30 ms lag, and the joiner's game stopped for 1.5 s at read 300 (a hitch): the
#              host runs on until it needs the joiner's input, so the joiner is behind and must catch up
# For each copy it prints how many reads waited for the other, the time in all and the longest wait;
# --record <file> also adds them to <file>, a line per copy with the date and commit, to compare runs over
# time. It exits 0 if every match gave the solo replay's game (and the delay and catching up went as
# above), 1 if not. The logs are kept in --out; without it they're in a temporary folder, deleted if all
# passed. It uses UDP ports 27320 to 27325.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=900
SEED=3
TIMEOUT=300
OUT=
RECORD=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --record) RECORD=$2; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-network.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
. tools/netplay/lib.sh

python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 --mouse --own-view || exit 2
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/inputs.pads" "$OUT/other.pads" 2 "$OUT/" || exit 2

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

# match <name> <UDP port> <host's fake network> <joiner's> [<host's options> [<joiner's>]]: starts a match (paced).
match() {
    # shellcheck disable=SC2086
    paced=1 pads=1.pads start "$1-host" --netplay-host "$2" --netplay-address 127.0.0.1 --players 2 --netplay-fake-network "$3" ${5:-}
    # shellcheck disable=SC2086
    paced=1 pads=2.pads start "$1-join" --netplay-join "$(code_of "$1-host")" --netplay-fake-network "$4" ${6:-}
}
echo "lan, bad, outage, lag-change, lag-drop, stall (at once, $READS reads each on real time)"
match lan 27320 "lag=0" "lag=0"
match bad 27321 "lag=100,jitter=10,loss=5,reorder=2,dup=1" "lag=100,jitter=10,loss=5,reorder=2,dup=1"
match outage 27322 "lag=30" "lag=30,outage=300:3"
match lag-change 27323 "lag=20,lag-at=300:150" "lag=20,lag-at=300:150"
match lag-drop 27325 "lag=150,lag-at=300:20" "lag=150,lag-at=300:20"
match stall 27324 "lag=30" "lag=30" "--input-delay 4" "--netplay-stall-at 300:1500"
names="lan-host lan-join bad-host bad-join outage-host outage-join lag-change-host lag-change-join lag-drop-host lag-drop-join
    stall-host stall-join"
# shellcheck disable=SC2086
if finish "was the last: quitting" $names; then
    commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    for name in $names; do
        same "$name" || failed=1
        waits=$(sed -n 's/^\[netplay\] waits: //p' "$OUT/$name.out")
        echo "    waits: ${waits:-none printed}"
        [ -n "$RECORD" ] && echo "$(date +%Y-%m-%dT%H:%M:%S) $commit $name $waits" >> "$RECORD"
    done
    # changes <match> <up|down>: the input delay the match ended with is higher (lower) than it started with,
    # and the joiner made the same changes as the host (each "<ports> becomes <d> from read <r>").
    changes() {
        host_made=$(sed -n 's/^\[netplay\] read [0-9]*: the input delay of \(.*\) becomes \([0-9]*\) from read \([0-9]*\) .*/\1: \2 \3/p' "$OUT/$1-host.out")
        heard=$(sed -n 's/^\[netplay\] read [0-9]*: the input delay of \(.*\) becomes \([0-9]*\) from read \([0-9]*\) .*/\1: \2 \3/p' "$OUT/$1-join.out")
        was=$(sed -n 's/^\[netplay\] the match starts: .*input delay \([0-9]*\) reads.*/\1/p' "$OUT/$1-host.out")
        now=$(sed -n 's/^\[netplay\] waits: .*; input delay \([0-9]*\),.*/\1/p' "$OUT/$1-host.out")
        if [ -z "$host_made" ] || [ "$host_made" != "$heard" ]; then
            echo "  $1: the delay changes differ or there are none: host: ${host_made:-none}; joiner: ${heard:-none}"
            return 1
        fi
        if [ -z "$was" ] || [ -z "$now" ] || { [ "$2" = up ] && [ "$now" -le "$was" ]; } || { [ "$2" = down ] && [ "$now" -ge "$was" ]; }; then
            echo "  $1: the input delay went from ${was:-?} to ${now:-?}, not $2"
            return 1
        fi
        echo "  $1: the input delay went from $was to $now ($(echo "$host_made" | sed 's/\(.*\) \([0-9]*\)$/\1 at read \2/' | paste -sd, - | sed 's/,/, /g')), the same on both"
    }
    changes lag-change up || failed=1
    changes lag-drop down || failed=1
    # Once: catching up must leave it level with the others, not fall behind again.
    if grep -q 'caught up (' "$OUT/stall-join.out" && [ "$(grep -c ': catching up$' "$OUT/stall-join.out")" = 1 ]; then
        echo "  stall-join: $(grep -m1 'reads behind the others' "$OUT/stall-join.out" | sed 's/^\[netplay\] //'); $(grep -m1 'caught up (' "$OUT/stall-join.out" | sed 's/^\[netplay\] //')"
    else
        echo "  stall-join didn't catch up after its stall, or caught up more than once ($(grep -c ': catching up$' "$OUT/stall-join.out") times)"
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
echo "passed: every match gave each copy the same game as the replay alone, the delay followed the lag, and a stalled copy caught up"
