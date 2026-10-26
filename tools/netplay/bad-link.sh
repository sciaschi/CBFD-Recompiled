#!/bin/sh
# The bad link check (netplay): one player's bad connection mustn't make the match worse for the
# others. Each player's input delay is their own (the host sizes it for how their inputs reach the others), a
# LAN keeps a delay of 0, and a player whose inputs stop coming holds the others up only for a while: the host
# lets go of them (the others play on), and they come back by themselves. Run it after building with
# CONKER_NETPLAY=ON:
#
#   tools/netplay/bad-link.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 7] [--timeout 300] [--out <folder>]
#
# The matches run at the same time, headless, on the timeline's real time (30 reads a second, as a player's
# game), rollback 12 (the menu's), each copy with its own ports' pads (the others' scrambled):
#   lan        no lag: every port's delay is 0 from start to end, never changed
#   uneven     30 ms lag both ways, and 40 ms of jitter on the joiner's sends only (its inputs arrive unevenly,
#              the host's don't): the joiner's delay ends higher than the host's
#   short      the joiner's network out for 3 s at read 600: everyone waits, nobody is let go of
#   drop       ...out for 8 s: the host lets go of the joiner after 5 s (its first long stall) and plays on, and the
#              joiner comes back by itself once its network is
#   repeat     the joiner's game stopped for 4 s at read 600 (not let go of: under 5 s), then again at read 1200:
#              let go of after 2.5 s, as the second long stall within minutes
#   slow       the joiner's game stopped for 100 ms every 60 reads from read 300 (a computer that can't keep up):
#              the host slows everyone to keep level with it only for a while (time sync's cap), at most about 1.5%
#              of the match, then leaves it to catch up on its own
#   three      a 3-player Deathmatch (make_match.py --mouse): the second joiner with 30 ms lag, 40 ms jitter and its
#              network out for 8 s at read 1500, the others 10 ms: the bad one's delay ends the highest, the others
#              wait at most about 5 s, play on without it, and it comes back
# A match where someone was let go of must give, with that port idle from the read the host let go of it to the
# one it came back at, the game a replay of that alone does: the others throughout, the one let go of from when
# it was back. It exits 0 if every match gave each copy that game and the delays and waits went as above, 1 if
# not. The logs are kept in --out; without it they're in a temporary folder, deleted if all
# passed. It uses UDP ports 27370 to 27379.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=7
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
        -h|--help) sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-bad-link.XXXXXX"); KEEP=0; }
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

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

# match <name> <UDP port> <host's fake network> <joiner's> [<joiner's options>]: a 2-player match with rollback 12,
# on real time.
match() {
    paced=1 pads=1.pads start "$1-host" --netplay-host "$2" --netplay-address 127.0.0.1 --players 2 --rollback 12 \
        --netplay-fake-network "$3"
    # shellcheck disable=SC2086
    paced=1 pads=2.pads start "$1-join" --netplay-join "$(code_of "$1-host")" --netplay-fake-network "$4" ${5:-}
}
# let_go_count <name>: how many times the host <name> let go of a machine.
let_go_count() {
    grep -c "held everyone up for" "$OUT/$1.out"
}
# came_back <match> <port> <recording> <reads> <others...>: the host <match>-host let go of <port> once and it came back
# (its copy <match>-join, or the one named last): every copy named gives a replay of <recording> with <port> idle
# between, the one let go of from when it was back.
came_back() {
    match=$1 port=$2 recording=$3 count=$4
    shift 4
    from=$(left_at "$match-host" "$port")
    back=$(back_at "$match-host" "$port")
    if [ -z "$from" ] || [ -z "$back" ]; then
        echo "  $match: port $port wasn't let go of and brought back (idle from ${from:-?}, back at ${back:-?})"
        return 1
    fi
    echo "  $match: port $port let go of from read $from, back at read $back ($(sed -n 's/^\[netplay\] read [0-9]*: \(.*held everyone up for [0-9.]* s\).*/\1/p' "$OUT/$match-host.out"))"
    idle_from "$OUT/$recording" "$OUT/$match-solo.pads" "$port" "$from" "$back" || return 1
    pads=$match-solo.pads start "$match-solo"
    finish "was the last: quitting" "$match-solo" || return 1
    came_ok=0
    for name in "$@"; do
        if [ "$name" = "$match-join" ] || [ "$name" = "$match-join2" ]; then
            # (Its reads from when it was let go of to when it was back were its own game's.)
            same_from "$name" "$match-solo" "$back" || came_ok=1
        else
            same "$name" "$match-solo" "$count" || came_ok=1
        fi
    done
    return $came_ok
}
# delays_of <name>: the input delays the copy <name> ended with, by port ("0, 0").
delays_of() {
    sed -n 's/^\[netplay\] input delays by port: //p' "$OUT/$1.out"
}

echo "lan, uneven, short, drop, repeat, slow (at once, $READS reads each on real time)"
match lan 27370 "lag=0" "lag=0"
match uneven 27371 "lag=30" "lag=30,jitter=40"
match short 27372 "lag=30" "lag=30,outage=600:3"
match drop 27373 "lag=30" "lag=30,outage=600:8"
match repeat 27374 "lag=30" "lag=30" "--netplay-stall-at 600:4000 --netplay-stall-at 1200:4000"
slow_stalls=
read=300
while [ $read -lt $((READS - 60)) ]; do
    slow_stalls="$slow_stalls --netplay-stall-at $read:100"
    read=$((read + 60))
done
match slow 27376 "lag=30" "lag=30" "$slow_stalls"
names="lan-host lan-join uneven-host uneven-join short-host short-join drop-host drop-join repeat-host repeat-join slow-host slow-join"
# shellcheck disable=SC2086
if finish "was the last: quitting" $names; then
    for name in lan-host lan-join uneven-host uneven-join short-host short-join slow-host slow-join; do
        same "$name" || failed=1
        echo "    $(sed -n 's/^\[netplay\] \(rollback: .*\)/\1/p' "$OUT/$name.out")"
        echo "    input delays by port: $(delays_of "$name")"
    done
    lan_ok=1
    for name in lan-host lan-join; do
        if [ "$(delays_of "$name")" != "0, 0" ] || ! grep -q "input delay 0 reads" "$OUT/$name.out" ||
            grep -q "the input delay of" "$OUT/$name.out"; then
            echo "  $name: the delay wasn't 0 throughout"
            lan_ok=0
            failed=1
        fi
    done
    [ $lan_ok -eq 1 ] && echo "  lan: every delay 0 throughout ($(sed -n 's/^\[netplay\] waits: .*; \(caught up [0-9]* times\)/\1/p' "$OUT/lan-join.out" | head -n 1) the joiner, a hitch of its own)"
    # The joiner's inputs are the uneven ones: its own delay is the higher (as both copies say).
    for name in uneven-host uneven-join; do
        host_delay=$(delays_of "$name" | cut -d, -f1)
        join_delay=$(delays_of "$name" | cut -d, -f2 | tr -d ' ')
        if [ -z "$host_delay" ] || [ -z "$join_delay" ] || [ "$join_delay" -le "$host_delay" ]; then
            echo "  $name: the joiner's delay (${join_delay:-?}) isn't higher than the host's (${host_delay:-?})"
            failed=1
        else
            echo "  $name: the joiner's delay is $join_delay, the host's $host_delay"
        fi
    done
    if [ "$(let_go_count short-host)" -ne 0 ]; then
        echo "  short: the joiner was let go of for a 3 s outage"
        failed=1
    else
        echo "  short: nobody let go of for a 3 s outage"
    fi
    if [ "$(let_go_count drop-host)" -ne 1 ]; then
        echo "  drop: the joiner was let go of $(let_go_count drop-host) times, not once"
        failed=1
    fi
    came_back drop 2 inputs.pads "$READS" drop-host drop-join || failed=1
    # Once, at the second stall, the sooner.
    stalled=$(sed -n 's/^\[netplay\] read [0-9]*: .*held everyone up for \([0-9]*\)\.[0-9] s.*/\1/p' "$OUT/repeat-host.out")
    from=$(left_at repeat-host 2)
    if [ "$(let_go_count repeat-host)" -ne 1 ] || [ -z "$from" ] || [ "$from" -lt 1000 ] || [ "${stalled:-9}" -ge 4 ]; then
        echo "  repeat: the joiner wasn't let go of once, at its second stall, sooner than the first's grace (from read ${from:-?}, after ${stalled:-?} s)"
        failed=1
    fi
    came_back repeat 2 inputs.pads "$READS" repeat-host repeat-join || failed=1
    # Time sync: the host stops slowing everyone for the slow joiner once it has a while.
    slowed=$(sed -n 's/^\[netplay\] time sync: [0-9]* reads paced later, \([0-9]*\) ms in all.*/\1/p' "$OUT/slow-host.out")
    if ! grep -q "time sync goes by port 2 no more" "$OUT/slow-host.out" || [ -z "$slowed" ] || [ "$slowed" -gt $((READS * 1000 / 30 * 15 / 1000)) ]; then
        echo "  slow: the host slowed everyone ${slowed:-?} ms for the slow joiner, past its cap"
        failed=1
    else
        echo "  slow: the host slowed everyone $slowed ms for the slow joiner, then left it to catch up ($(grep -c "catching up" "$OUT/slow-join.out") catch-ups)"
    fi
else
    failed=1
fi

echo "three (a 3-player match on real time, $MOUSE_READS reads)"
paced=1 pads=mouse-1.pads start three-host --netplay-host 27375 --netplay-address 127.0.0.1 --players 3 --rollback 12 \
    --netplay-fake-network "lag=10"
code=$(code_of three-host)
paced=1 pads=mouse-2.pads start three-join1 --netplay-join "$code" --netplay-fake-network "lag=10"
wait_for three-join1 "in, on port 2"
paced=1 pads=mouse-3.pads start three-join2 --netplay-join "$code" --netplay-fake-network "lag=30,jitter=40,outage=1500:8"
if finish "was the last: quitting" three-host three-join1 three-join2; then
    for name in three-host three-join1 three-join2; do
        echo "    $name: input delays by port $(delays_of "$name"); $(sed -n 's/^\[netplay\] waits: .*\(the longest [0-9]* ms\).*/\1 waited/p' "$OUT/$name.out")"
    done
    bad=$(delays_of three-host | cut -d, -f3 | tr -d ' ')
    for port in 1 2; do
        good=$(delays_of three-host | cut -d, -f$port | tr -d ' ')
        if [ -z "$bad" ] || [ -z "$good" ] || [ "$good" -ge "$bad" ]; then
            echo "  three: port $port's delay (${good:-?}) isn't lower than the bad joiner's (${bad:-?})"
            failed=1
        fi
    done
    for name in three-host three-join1; do
        longest=$(sed -n 's/^\[netplay\] waits: .*the longest \([0-9]*\) ms.*/\1/p' "$OUT/$name.out")
        if [ -z "$longest" ] || [ "$longest" -gt 6000 ]; then
            echo "  three: $name waited ${longest:-?} ms at once, over about 5 s"
            failed=1
        fi
    done
    came_back three 3 mouse.pads "$MOUSE_READS" three-host three-join1 three-join2 || failed=1
else
    failed=1
fi

if [ $failed -eq 0 ]; then
    [ $KEEP -eq 0 ] && rm -rf "$OUT"
    echo "passed: each player's delay was their own, a LAN's stayed 0, and a player whose inputs stopped was let go of a while and came back"
    exit 0
fi
echo "logs: $OUT"
exit 1
