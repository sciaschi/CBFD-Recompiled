#!/bin/sh
# Netplay over a bad network on this one computer, in two windows: to see (or play) what lag, jitter and loss do
# to a match, and what rollback makes of them, without a second machine. The network is faked by the library on
# each copy's side (--netplay-fake-network: what it sends is delayed, jittered and lost), so a match between the
# two copies over loopback plays as one over the internet would. Run it after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/try-network.sh watch [--net <spec>] [--rollback <reads>] [--delay <reads>] [--minutes 2]
#   tools/netplay/try-network.sh play  [--net <spec>]
#   (both: [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64] [--window 960x540])
#
# watch  both windows play a made-up 2-player Deathmatch (make_match.py --mouse: everyone moves and aims)
#        from boot, on real time: the intro (about 1.5 minutes, it can't be skipped), the menus, then --minutes
#        of the match. Each copy has only its own player's inputs from the recording; the other's come over
#        the faked network. The host's window is player 1's, the joiner's player 2's. --rollback (0 to 12,
#        default 8; 0 is plain lockstep) and --delay (the input delay, default the automatic one) are the
#        host's, as on the menu's Host tab.
# play   both windows open at the launcher. Host in one (Netplay, Host a match: address 127.0.0.1, any players,
#        the Rollback you want; Copy the join code), join from the other (Join a match: Paste, Join). The
#        keyboard plays in the window in front; a controller plays in both at once (the game hears controllers
#        in the background), so play with the keyboard and click the other window to move its player.
#
# --net is --netplay-fake-network's: lag=<ms>,jitter=<ms>,loss=<%>,reorder=<%>,dup=<%>, applied on each
# side (default lag=60,jitter=15,loss=2: a so-so internet connection; lag=150,jitter=30,loss=5 is a bad one).
# Each copy keeps its data in a folder of its own (the ROM stored there from --rom), not the game's usual one.
# When both have quit, it prints each one's end-of-match lines: reads predicted and gone back, time sync, waits.
set -u

cd "$(dirname "$0")/../.."
mode=${1:-}
[ $# -gt 0 ] && shift
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
NET=lag=60,jitter=15,loss=2
ROLLBACK=8
DELAY=
MINUTES=2
WINDOW=960x540
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --net) NET=$2; shift 2 ;;
        --rollback) ROLLBACK=$2; shift 2 ;;
        --delay) DELAY=$2; shift 2 ;;
        --minutes) MINUTES=$2; shift 2 ;;
        --window) WINDOW=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$mode" in
    watch|play) ;;
    *) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM (give it with --rom)" >&2; exit 2; }
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-try.XXXXXX")
host_pid=
join_pid=
cleanup() {
    for pid in $host_pid $join_pid; do kill "$pid" 2>/dev/null; done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# code_of: the join code the host printed, once it has.
code_of() {
    i=0
    while [ $i -lt 600 ]; do
        code=$(sed -n 's/^\[netplay\] join code \([^ ]*\).*/\1/p' "$OUT/host.out" 2>/dev/null)
        [ -n "$code" ] && { echo "$code"; return; }
        kill -0 "$host_pid" 2>/dev/null || break
        sleep 0.1
        i=$((i + 1))
    done
    echo "no-code"
}

echo "faking on each side: $NET"
if [ "$mode" = watch ]; then
    python3 tools/netplay/make_match.py "$OUT/match.pads" --players 2 --mouse --play-reads $((MINUTES * 1800)) \
        --seed "$(date +%s)" || exit 2
    delay_option=
    [ -n "$DELAY" ] && delay_option="--input-delay $DELAY"
    echo "rollback $ROLLBACK, input delay ${DELAY:-automatic}: the intro first (about 1.5 minutes), then the menus and the match"
    # shellcheck disable=SC2086
    "$EXE" --rom "$ROM" --data "$OUT/data-host" --window "$WINDOW" --replay "$OUT/match.pads" \
        --netplay-host 27360 --netplay-address 127.0.0.1 --players 2 --rollback "$ROLLBACK" $delay_option \
        --netplay-fake-network "$NET" > "$OUT/host.out" 2>&1 &
    host_pid=$!
    code=$(code_of)
    "$EXE" --rom "$ROM" --data "$OUT/data-join" --window "$WINDOW" --replay "$OUT/match.pads" \
        --netplay-join "$code" --netplay-fake-network "$NET" > "$OUT/join.out" 2>&1 &
    join_pid=$!
    # Each quits at the recording's last read (a quit can hang: it's stopped once it says so).
    while :; do
        done_count=0
        for name in host join; do
            grep -q "was the last: quitting\|the netplay match is over" "$OUT/$name.out" 2>/dev/null && done_count=$((done_count + 1))
        done
        alive=0
        for pid in $host_pid $join_pid; do kill -0 "$pid" 2>/dev/null && alive=$((alive + 1)); done
        [ $done_count -eq 2 ] || [ $alive -eq 0 ] && break
        sleep 1
    done
    sleep 1
else
    "$EXE" --rom "$ROM" --data "$OUT/data-host" --window "$WINDOW" --netplay-fake-network "$NET" > "$OUT/host.out" 2>&1 &
    host_pid=$!
    "$EXE" --rom "$ROM" --data "$OUT/data-join" --window "$WINDOW" --netplay-fake-network "$NET" > "$OUT/join.out" 2>&1 &
    join_pid=$!
    echo "two windows at the launcher: host in one (address 127.0.0.1, Copy the code), join from the other (Paste)."
    echo "Play with the keyboard (a controller moves both). Close both windows when done."
    wait $host_pid $join_pid 2>/dev/null
fi

for name in host join; do
    echo "-- $name"
    grep -h "^\[netplay\] the match starts\|^\[netplay\] rollback:\|^\[netplay\] time sync:\|^\[netplay\] waits:\|^\[netplay\] the match is over\|played again, a read" \
        "$OUT/$name.out" | sed 's/^\[[a-z]*\] */  /'
done
rm -rf "$OUT/data-host" "$OUT/data-join"
echo "logs: $OUT"
