#!/bin/sh
# The spectators check (netplay): a spectator (--netplay-watch) gets every input of a match and sends none,
# and must see exactly the game the players do (its hash log a solo replay's), never holding them up. Run it after
# building with CONKER_NETPLAY=ON:
#
#   tools/netplay/spectators.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                               [--reads 1800] [--seed 7] [--timeout 300] [--out <folder>]
#
# Each match is a host and a joiner, each with its own port's pads from a made-up 2-port recording (the other's
# scrambled), and a spectator given a recording of other pads altogether (it must play none of its own), all
# headless and --fast:
#   watch      a spectator from the start: all three must match the solo replay
#   late       the spectator joins as the host stops a while at read 900 (--netplay-stall-at): it must load the
#              host's game and match the solo replay from the read it loaded before; the players, all of it
#   desync     the spectator changes a byte of its memory at read 600: the host must send it its game alone (the
#              players never told), which it loads; the players match solo, the spectator from the read it loaded
#   rollback   the host's --rollback 8, the joiner 40 ms of lag: the spectator, which never predicts, matches solo
#   leave      the spectator quits at read 900: the players play on to the end, and match solo
#   refused    a host with --netplay-no-spectators refuses a spectator, and the players play on
# Then seat changes, in a 3-player Deathmatch (make_match.py --mouse; each copy's file has the others' ports scrambled,
# a spectator's every port but the one it takes):
#   claim      the host starts with port 3 open (--netplay-start-with 2); the spectator asks for it at read 600
#              (--netplay-claim-at) and plays it from the read the host says
#   swap       the host seats the spectator in port 3 at read 600 (--netplay-seat-at): its player steps out first,
#              watching from then on, and the spectator plays it from the read the host says
#   takeover   port 3's player quits at read 500; the host seats the spectator there at read 800, its hold not over
#   step-out   in a 2-player match the joiner steps out at read 700 (--netplay-step-out-at), and watches to the end
#   behind     as claim, the spectator stopped for 2 s just before it asks: the host seats it only once its game is
#              near the host's (or gives up), never ending the match
#   couch-out  a joiner with ports 2 and 3 (--local-players 2) steps out at read 700: both idle (open) from then on
# and before the start (the host makes the change at once):
#   pre-claim  a spectator asks for port 3 (--netplay-claim-at 0:3) and plays it, the match starting full
#   pre-out    a joiner steps out (--netplay-step-out-at 0), and two more take ports 2 and 3; it watches the match
#   pre-kept   a joiner steps out before the start of a match that takes no spectators: the host refuses (says so), and
#              it plays the match
# Every copy, players and spectators, must match a solo replay of the recording with the port idle while nobody had it.
# And through a room on a netplay server (beside the game's build), in public: its list says one is watching.
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out; without it they're in a temporary
# folder, deleted if all passed. It uses UDP ports 27340 to 27355.
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
        -h|--help) sed -n '2,/^set -u$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-spectators.XXXXXX"); KEEP=0; }
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
# A machine with ports 2 and 3 shows their views two to a window, which widens their culls: its solo replays are the
# recording marked so (the header's view pairs), as lockstep.sh's couch matches.
python3 -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[36:40]=(6).to_bytes(4,'little'); open(sys.argv[2],'wb').write(d)" \
    "$OUT/mouse.pads" "$OUT/couch-solo.pads" || exit 2
scramble_own "$OUT/couch-solo.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-23.pads" 23 || exit 2
scramble_own "$OUT/couch-solo.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-1.pads" 1 || exit 2

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

# loaded <name>: the read before which the spectator <name> last loaded the host's game.
loaded() {
    sed -n "s/^\[netplay\] .*the host's game was loaded before read \([0-9]*\).*/\1/p" "$OUT/$1.out" | tail -n 1
}

# match <name> <port> [host options] -- [spectator options]: the host and the joiner, then the spectator.
match() {
    m=$1 m_port=$2; shift 2
    host_options=
    while [ $# -gt 0 ] && [ "$1" != -- ]; do host_options="$host_options $1"; shift; done
    [ $# -gt 0 ] && shift
    # (lib.sh's start uses $name: these have names of their own.)
    # shellcheck disable=SC2086
    pads=1.pads start "$m-host" --netplay-host "$m_port" --netplay-address 127.0.0.1 $host_options
    code=$(code_of "$m-host")
    # ($wait_pattern, $join_options and $watch_first: set before, and cleared after, the one match they're for. A
    # spectator first comes before the joiner, and is refused or in before the match can start.)
    if [ -n "${watch_first:-}" ]; then
        pads=other.pads start "$m-watch" --netplay-join "$code" --netplay-watch "$@"
        wait_for "$m-watch" "$watch_first"
    fi
    # shellcheck disable=SC2086
    pads=2.pads start "$m-join" --netplay-join "$code" ${join_options:-}
    [ -n "${wait_pattern:-}" ] && wait_for "$m-host" "$wait_pattern"
    [ -z "${watch_first:-}" ] && pads=other.pads start "$m-watch" --netplay-join "$code" --netplay-watch "$@"
}

echo "watch, late, desync"
watch_first="watching, a match"
match watch 27340
watch_first=
wait_pattern="the game stops"
match late 27341 --netplay-stall-at 900:6000
wait_pattern=
# (From the start: a state it got coming in would hold up the next, states for spectators going 10 s apart at most.)
watch_first="watching, a match"
match desync 27342 -- --netplay-desync-at 600
watch_first=
if finish "was the last: quitting" watch-host watch-join watch-watch late-host late-join late-watch desync-host desync-join desync-watch; then
    for copy in watch-host watch-join watch-watch late-host late-join desync-host desync-join; do
        same "$copy" || failed=1
    done
    if ! grep -q "the match starts: 2 players, this is a spectator" "$OUT/watch-watch.out"; then
        echo "  watch-watch didn't watch from the start"
        failed=1
    fi
    at=$(loaded late-watch)
    if [ -z "$at" ] || ! grep -q "watching, a match of 2 under way" "$OUT/late-watch.out"; then
        echo "  late-watch didn't come in under way"
        failed=1
    else
        same_from late-watch solo "$at" || failed=1
    fi
    at=$(loaded desync-watch)
    if [ -z "$at" ] || ! grep -q "spectator 1's game memory isn't this one's" "$OUT/desync-host.out"; then
        echo "  desync-watch wasn't set right"
        failed=1
    elif grep -q "desync recovery\|will load" "$OUT/desync-join.out"; then
        echo "  the joiner was told of the spectator's desync"
        failed=1
    else
        same_from desync-watch solo "$at" || failed=1
    fi
else
    failed=1
fi

echo "rollback, leave, refused"
join_options="--netplay-fake-network lag=40"
match rollback 27343 --rollback 8
join_options=
match leave 27344 -- --stop-after 900
watch_first="couldn.t join"
match refused 27345 --netplay-no-spectators
watch_first=
if finish "was the last: quitting\|couldn't join" rollback-host rollback-join rollback-watch leave-host leave-join leave-watch \
    refused-host refused-join refused-watch; then
    for copy in rollback-host rollback-join rollback-watch leave-host leave-join refused-host refused-join; do
        same "$copy" || failed=1
    done
    if ! grep -q "spectator 1 left" "$OUT/leave-host.out"; then
        echo "  the host didn't see the spectator leave"
        failed=1
    fi
    if ! grep -q "couldn't join: the host takes no spectators" "$OUT/refused-watch.out"; then
        echo "  the spectator wasn't refused"
        failed=1
    fi
else
    failed=1
fi

# took <name> <port>: the read from which the host <name> said a spectator plays <port>.
took() {
    sed -n "s/^\[netplay\] read [0-9]*: spectator [0-9]* takes port $2 from read \([0-9]*\).*/\1/p" "$OUT/$1.out" | tail -n 1
}
# stepped <name> <port>: the read from which the host <name> said <port>'s player stepped out (idle).
stepped() {
    sed -n "s/^\[netplay\] port $2 stepped out to watch (spectator [0-9]*), idle from read \([0-9]*\).*/\1/p" "$OUT/$1.out" | tail -n 1
}
# solo_idle <name> <ports> <from> <to> [<file>]: a solo replay of the mouse recording (or <file>) with <ports> idle from
# <from> to <to>, as $OUT/<name>.log; true if it ran to the end.
solo_idle() {
    idle_from "$OUT/${5:-mouse.pads}" "$OUT/$1.pads" "$2" "$3" "$4" || exit 2
    pads=$1.pads start "$1"
    finish "was the last: quitting" "$1"
}

pads=mouse.pads start solo-mouse
finish "was the last: quitting" solo-mouse || failed=1

echo "claim, swap, takeover, step-out"
pads=mouse-1.pads start claim-host --netplay-host 27346 --netplay-address 127.0.0.1 --players 3 --netplay-start-with 2
code=$(code_of claim-host)
pads=mouse-3.pads start claim-watch --netplay-join "$code" --netplay-watch --netplay-claim-at 600:3
wait_for claim-watch "watching, a match"
pads=mouse-2.pads start claim-join --netplay-join "$code"
pads=mouse-1.pads start swap-host --netplay-host 27347 --netplay-address 127.0.0.1 --players 3 --netplay-seat-at 600:3:1
code=$(code_of swap-host)
pads=mouse-3.pads start swap-watch --netplay-join "$code" --netplay-watch
wait_for swap-watch "watching, a match"
pads=mouse-2.pads start swap-join1 --netplay-join "$code"
wait_for swap-join1 "in, on port 2"
pads=mouse-3.pads start swap-join2 --netplay-join "$code"
pads=mouse-1.pads start take-host --netplay-host 27348 --netplay-address 127.0.0.1 --players 3 --netplay-seat-at 800:3:1
code=$(code_of take-host)
pads=mouse-3.pads start take-watch --netplay-join "$code" --netplay-watch
wait_for take-watch "watching, a match"
pads=mouse-2.pads start take-join1 --netplay-join "$code"
wait_for take-join1 "in, on port 2"
pads=mouse-3.pads start take-join2 --netplay-join "$code" --stop-after 500
pads=mouse-1.pads start behind-host --netplay-host 27350 --netplay-address 127.0.0.1 --players 3 --netplay-start-with 2
code=$(code_of behind-host)
pads=mouse-3.pads start behind-watch --netplay-join "$code" --netplay-watch --netplay-stall-at 550:2000 --netplay-claim-at 600:3
wait_for behind-watch "watching, a match"
pads=mouse-2.pads start behind-join --netplay-join "$code"
pads=1.pads start out-host --netplay-host 27349 --netplay-address 127.0.0.1
code=$(code_of out-host)
pads=2.pads start out-join --netplay-join "$code" --netplay-step-out-at 700
if finish "was the last: quitting" claim-host claim-join claim-watch swap-host swap-join1 swap-join2 swap-watch take-host take-join1 \
    take-watch behind-host behind-join behind-watch out-host out-join; then
    at=$(took claim-host 3)
    if [ -z "$at" ] || ! grep -q "this machine takes it from read $at" "$OUT/claim-watch.out"; then
        echo "  the spectator didn't take port 3 (from read '$at')"
        failed=1
    elif solo_idle claim-solo 3 0 "$at"; then
        for copy in claim-host claim-join claim-watch; do same "$copy" claim-solo "$MOUSE_READS" || failed=1; done
    else
        failed=1
    fi
    from=$(stepped swap-host 3)
    at=$(took swap-host 3)
    if [ -z "$from" ] || [ -z "$at" ] || ! grep -q "You're watching now" "$OUT/swap-join2.out"; then
        echo "  no swap (port 3 idle from read '$from', the spectator from '$at')"
        failed=1
    elif solo_idle swap-solo 3 "$from" "$at"; then
        echo "  port 3 idle from read $from, the spectator's from $at"
        for copy in swap-host swap-join1 swap-join2 swap-watch; do same "$copy" swap-solo "$MOUSE_READS" || failed=1; done
    else
        failed=1
    fi
    from=$(left_at take-host 3)
    at=$(took take-host 3)
    if [ -z "$from" ] || [ -z "$at" ]; then
        echo "  no takeover (port 3 left at read '$from', the spectator from '$at')"
        failed=1
    elif solo_idle take-solo 3 "$from" "$at"; then
        for copy in take-host take-join1 take-watch; do same "$copy" take-solo "$MOUSE_READS" || failed=1; done
    else
        failed=1
    fi
    # (Seated or not: either way everyone's game is the same.)
    at=$(took behind-host 3)
    echo "  the spectator behind: $(grep -c "takes it from read" "$OUT/behind-watch.out") seat, $(grep -o "is at read [0-9]*: it takes\|still too far behind" "$OUT/behind-host.out" | tr '\n' ' ')"
    if solo_idle behind-solo 3 0 "${at:-$MOUSE_READS}"; then
        for copy in behind-host behind-join behind-watch; do same "$copy" behind-solo "$MOUSE_READS" || failed=1; done
    else
        failed=1
    fi
    from=$(stepped out-host 2)
    if [ -z "$from" ]; then
        echo "  the joiner didn't step out"
        failed=1
    elif solo_idle out-solo 2 "$from" "$READS" inputs.pads; then
        for copy in out-host out-join; do same "$copy" out-solo || failed=1; done
    else
        failed=1
    fi
else
    failed=1
fi

echo "couch-out, pre-claim, pre-out"
pads=couch-1.pads start couch-host --netplay-host 27352 --netplay-address 127.0.0.1 --players 3
code=$(code_of couch-host)
pads=couch-23.pads start couch-join --netplay-join "$code" --local-players 2 --netplay-step-out-at 700
pads=mouse-1.pads start pre-host --netplay-host 27353 --netplay-address 127.0.0.1 --players 3
code=$(code_of pre-host)
pads=mouse-2.pads start pre-join --netplay-join "$code"
wait_for pre-join "in, on port 2"
pads=mouse-3.pads start pre-watch --netplay-join "$code" --netplay-watch --netplay-claim-at 0:3
pads=mouse-1.pads start out-pre-host --netplay-host 27354 --netplay-address 127.0.0.1 --players 3
code=$(code_of out-pre-host)
pads=mouse-other.pads start out-pre-gone --netplay-join "$code" --netplay-step-out-at 0
wait_for out-pre-gone "the host moved this machine: watching"
pads=mouse-2.pads start out-pre-join1 --netplay-join "$code"
wait_for out-pre-join1 "in, on port 2"
pads=mouse-3.pads start out-pre-join2 --netplay-join "$code"
pads=1.pads start kept-host --netplay-host 27355 --netplay-address 127.0.0.1 --netplay-no-spectators
code=$(code_of kept-host)
pads=2.pads start kept-join --netplay-join "$code" --netplay-step-out-at 0
if finish "was the last: quitting" couch-host couch-join pre-host pre-join pre-watch out-pre-host out-pre-gone out-pre-join1 out-pre-join2 \
    kept-host kept-join; then
    same kept-host || failed=1
    same kept-join || failed=1
    from=$(stepped couch-host 2)
    if [ -z "$from" ] || [ "$from" != "$(stepped couch-host 3)" ]; then
        echo "  the couch didn't step out of ports 2 and 3 (from read '$from')"
        failed=1
    elif solo_idle couch-out-solo 23 "$from" "$MOUSE_READS" couch-solo.pads; then
        for copy in couch-host couch-join; do same "$copy" couch-out-solo "$MOUSE_READS" || failed=1; done
    else
        failed=1
    fi
    if ! grep -q "the host moved this machine: on port 3" "$OUT/pre-watch.out"; then
        echo "  the spectator didn't take port 3 before the start"
        failed=1
    fi
    if ! grep -q "the match starts: 3 players, this is a spectator" "$OUT/out-pre-gone.out"; then
        echo "  the joiner that stepped out isn't watching"
        failed=1
    fi
    for copy in pre-host pre-join pre-watch out-pre-host out-pre-gone out-pre-join1 out-pre-join2; do
        same "$copy" solo-mouse "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi

echo "listed"
# Through a room on a netplay server (beside the game's build, on loopback), in public: the server's list says how
# many watch it, and that it takes spectators.
"$(dirname "$EXE")/src/netplay/net/netplay_server" --port 27351 > "$OUT/listed-server.out" 2>&1 &
pids="$pids $!"
room="--netplay-server 127.0.0.1:27351 --netplay-direct"
# shellcheck disable=SC2086
pads=1.pads start listed-host --netplay-host room --netplay-public $room --players 2
code=$(room_of listed-host)
# shellcheck disable=SC2086
pads=other.pads start listed-watch --netplay-join "$code" --netplay-watch $room
wait_for listed-watch "watching, a match"
hashes=$(sed -n 's/^\[harness\] build .*: build \([0-9a-f]*\), ROM \([0-9a-f]*\), profile \([0-9a-f]*\)$/--build \1 --rom \2 --profile \3/p' "$OUT/listed-host.out")
listed=
i=0
while [ $i -lt 20 ] && [ -z "$listed" ]; do
    # shellcheck disable=SC2086
    "$(dirname "$EXE")/src/netplay/net/netplay_probe" list --signal 127.0.0.1:27351 $hashes --mods 0 > "$OUT/listed-list.out" 2>&1
    listed=$(grep "^listed $code 1/2 .* spectators watching 1$" "$OUT/listed-list.out")
    i=$((i + 1))
    [ -z "$listed" ] && sleep 0.5
done
# shellcheck disable=SC2086
pads=2.pads start listed-join --netplay-join "$code" $room
if [ -z "$listed" ]; then
    echo "  the list doesn't say one watches:"
    sed 's/^/    /' "$OUT/listed-list.out"
    failed=1
else
    echo "  $listed"
fi
finish "was the last: quitting" listed-host listed-join listed-watch || failed=1

if [ $failed -ne 0 ]; then
    echo "FAILED (logs: $OUT)"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: every spectator saw the players' game, coming in under way and after a desync too, nobody waited for one, and spectators and players changed places"
