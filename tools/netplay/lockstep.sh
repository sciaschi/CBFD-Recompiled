#!/bin/sh
# The lockstep check (netplay): two copies of the game on this machine play a match over
# loopback, each giving its own port's pads from the same made-up 2-port recording, and must give the
# game exactly what a replay of that recording alone does, read by read. Run it after building with
# CONKER_NETPLAY=ON:
#
#   tools/netplay/lockstep.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 2] [--timeout 300] [--out <folder>]
#                             [--match-options "<options>"] [--host-options "<options>"]
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
#   room       the match through a room on a netplay server (netplay_server, beside the game's build, on
#              loopback): the host makes the room, public, which the server must list for the game's hashes;
#              the joiner joins by its code (direct connections: there's no relay here,
#              tools/netplay/server/test.sh tests that); both must match solo's
#   quick      Quick Match (--netplay-quick) finds a public room's host and joins it; both must match solo's
#   quick-meet two Quick Matches at once, with no room: they must end up in one match, without a desync
#   jittery    the same with input delay 0 and random sleeps (--jitter 500) on the joiner, so the two
#              are out of step and wait for each other all the time
#   leave      the mouse match, the second joiner quitting at read 1500: the host and the first joiner play on,
#              port 3 idle from the read the host says (both must say the same), and must match a solo replay
#              of the recording with port 3 idle from there
#   leave-crash the same with the second joiner killed at about read 1500 (no goodbye): the host lets go of it after
#              5 s of holding everyone up, and the two play on as above
#   leave-couch the couch match with ports 2 and 3 on a joiner that quits at read 1500: the host plays on, both
#              ports idle from the same read, and must match a solo replay so
#   kick       the host kicks the joiner during the match: the joiner is told, the host plays to the end
#   block      a host keeps out a joiner on its block list, and a joiner leaves a host on its own; Quick
#              Match passes over a public room whose host is on it
#   recover    desync recovery: the mouse match, the second joiner changing a byte of its memory at reads 900
#              and 2000: each time the host must send it its game, which it loads, and all play on: the host and the
#              first joiner must match the solo replay, the second from the read it last loaded at
#   rejoin     coming back: the mouse match, the second joiner quitting at read 900 and coming back (the same
#              player id) as the host stops a while (--netplay-stall-at): it must load the host's game and play on from
#              the read the host says. The host and the first joiner must match a solo replay of the recording with port 3
#              idle from the read it left to that one, and the second joiner, back, the same from that read
#   rejoin-twice the same, the first joiner quitting at read 600 for good and the second at 900, coming back, quitting
#              at 2000 and coming back again; meanwhile another player (another id) must be refused port 3
#   rejoin-both both joiners quitting (at reads 800 and 900) and coming back as the host stops: two machines brought
#              back together, from the same read
#   rejoin-couch the couch match with ports 2 and 3 on a joiner that quits at read 900 and comes back
#   late       joining a match under way: the mouse match started with two of its three players
#              (--netplay-start-with 2), port 3 open (idle from read 0); a new player joins it as the host stops a
#              while: it must load the host's game and play from the read the host says. The host and the first
#              joiner must match a solo replay of the recording with port 3 idle up to that read, the new player the
#              same from it; another new player before it quits as soon as it's in, which must leave port 3 free
#   late-seat  a new player taking a seat someone left (--netplay-seat-hold 0): the second joiner quits at read 900,
#              another player joins on port 3, and the one who left is then refused it
#   recover-again the 2-player match, the joiner changing its memory at reads 300 and 400: the second desync, so soon
#              after the first was set right, must end the match
#   desync     the host (with --netplay-no-recovery) changes a byte of its memory at read 300: both must stop for a desync, and each
#              write a desync report naming the region changed (64, at 0x80400000) with its memory, which
#              compare_desyncs.py must find the byte in
#   desync-room the same through a room: the netplay server must log a summary of it (region 64)
# --match-options gives every copy in a match those options too, and --host-options every hosting copy, e.g.
# --host-options "--rollback 8" --match-options "--netplay-fake-network lag=40" (rollback is the
# host's choice: every run must still come out the same).
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out; without it they're in a
# temporary folder, deleted if all passed. The data folders it makes for the copies (about 1 GB in all) are
# always deleted. As in check.sh, a run is judged by what it prints and then stopped. It uses UDP ports
# 27310 to 27324 and 27326 on 127.0.0.1 (netplay servers on 27320 to 27323).
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=2
TIMEOUT=300
OUT=
MATCH_OPTIONS=
HOST_OPTIONS=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --match-options) MATCH_OPTIONS=$2; shift 2 ;;
        --host-options) HOST_OPTIONS=$2; shift 2 ;;
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

echo "room"
"$(dirname "$EXE")/src/netplay/net/netplay_server" --port 27320 > "$OUT/room-server.out" 2>&1 &
server=$!
pids="$pids $server"
room="--netplay-server 127.0.0.1:27320 --netplay-direct"
# shellcheck disable=SC2086
pads=1.pads start room-host --netplay-host room --netplay-public $room --players 2 --input-delay 2
# shellcheck disable=SC2086
code=$(room_of room-host)
# The host listed it in public: the server lists it for a game of the same hashes (the harness prints them).
hashes=$(sed -n 's/^\[harness\] build .*: build \([0-9a-f]*\), ROM \([0-9a-f]*\), profile \([0-9a-f]*\)$/--build \1 --rom \2 --profile \3/p' "$OUT/room-host.out")
# shellcheck disable=SC2086
"$(dirname "$EXE")/src/netplay/net/netplay_probe" list --signal 127.0.0.1:27320 $hashes --mods 0 > "$OUT/room-list.out" 2>&1
grep -q "^listed $code 1/2" "$OUT/room-list.out" || { echo "room: FAILED (not in the public list)"; cat "$OUT/room-list.out"; failed=1; }
# shellcheck disable=SC2086
pads=2.pads start room-join --netplay-join "$code" $room
if finish "was the last: quitting" room-host room-join; then
    same room-host || failed=1
    same room-join || failed=1
else
    failed=1
fi

echo "quick"
# A server of their own (finish stops the room case's with its copies).
"$(dirname "$EXE")/src/netplay/net/netplay_server" --port 27321 > "$OUT/quick-server.out" 2>&1 &
server=$!
servers="${servers:-} $server"     # stopped by cleanup too, if this ends early
room="--netplay-server 127.0.0.1:27321 --netplay-direct"
# shellcheck disable=SC2086
pads=1.pads start quick-host --netplay-host room --netplay-public $room --players 2 --input-delay 2
room_of quick-host > /dev/null
# shellcheck disable=SC2086
pads=2.pads start quick-join --netplay-quick $room --players 2
if finish "was the last: quitting" quick-host quick-join && grep -q "Quick Match: joining" "$OUT/quick-join.out"; then
    same quick-host || failed=1
    same quick-join || failed=1
else
    echo "quick: FAILED"
    failed=1
fi

echo "quick-meet"
# Both at once: each may host (the list was empty for both), then the newer room's host leaves it for the
# older. Which machine hosts isn't known, so they're checked against each other (their hash logs), not solo.
# shellcheck disable=SC2086
pads=1.pads start meet-a --netplay-quick $room --players 2
# shellcheck disable=SC2086
pads=2.pads start meet-b --netplay-quick $room --players 2
# If both hosted (both lists were empty), one must have left its room for the older one's.
if finish "was the last: quitting" meet-a meet-b && both_hosted=$(cat "$OUT/meet-a.out" "$OUT/meet-b.out" | grep -c "no room to join, so hosting one" || true) &&
    grep -q "the match starts" "$OUT/meet-a.out" &&
    grep -q "the match starts" "$OUT/meet-b.out" && ! grep -q "\[netplay\] desync at read" "$OUT/meet-a.out" "$OUT/meet-b.out" &&
    same meet-b meet-a &&
    { [ "$both_hosted" -lt 2 ] || grep -q "is older, so leaving" "$OUT/meet-a.out" "$OUT/meet-b.out"; }; then
    grep -h "Quick Match" "$OUT/meet-a.out" "$OUT/meet-b.out" | sed 's/^/  /'
else
    echo "quick-meet: FAILED"
    failed=1
fi
kill $server 2>/dev/null

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
# The 3-player match again, the second joiner killed at read 1500 (no goodbye): the host lets go of it once it has
# held everyone up 5 s (the library notices it stopped answering later), and the two play on, as above.
# (The host stops 8 s at read 1700, after it lets go of it, so the library notices it's gone before the match ends: the
# first joiner must then be told it left.)
pads=mouse-1.pads start crash-host --netplay-host 27319 --netplay-address 127.0.0.1 --players 3 --netplay-stall-at 1700:8000
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
        echo "  port 3 stopped answering, let go of; idle from read $from"
        # Let go of first, then gone for good: the first joiner is told it left, not still "lost connection".
        if ! grep -q "(on screen) Player 3 stopped answering" "$OUT/crash-join1.out"; then
            echo "  the first joiner wasn't told port 3 stopped answering, after it was let go of"
            failed=1
        fi
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
    # The automatic delay counts the joining machine's round trip (one, on loopback: 0), not its players
    # (unless --match-options fakes a slower network).
    case " $MATCH_OPTIONS " in
        *" --netplay-fake-network "*) ;;
        *) grep -q "this is ports 1 and 2, input delay 0" "$OUT/couch-host2.out" || { echo "  couch-host2's automatic input delay isn't 0"; failed=1; } ;;
    esac
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

echo "block"
# The block list: a host keeps out a joiner on it (told only that the host removed it), and a joiner leaves a
# host on it. (Every match above went through the waiting room too: from the command line, every copy is ready
# by itself, and the host starts once all are in.)
start block-host --netplay-host 27313 --netplay-address 127.0.0.1 --players 2 --netplay-block b10c
start block-join --netplay-join "$(code_of block-host)" --netplay-player b10c
finish "the host removed this player" block-join || failed=1
start block-host2 --netplay-host 27314 --netplay-address 127.0.0.1 --players 2 --netplay-player 4057
start block-join2 --netplay-join "$(code_of block-host2)" --netplay-block 4057
finish "is on the block list: leaving" block-join2 || failed=1
# Quick Match passes over a public room whose host is on its block list (the server says who hosts it), and
# hosts its own instead.
# (Every copy and probe comes from 127.0.0.1: a burst of new connections big enough for the probe's tries below,
# each a connection of its own, and Quick Match after them.)
"$(dirname "$EXE")/src/netplay/net/netplay_server" --port 27323 --connection-burst 100 > "$OUT/block-server.out" 2>&1 &
server=$!
servers="${servers:-} $server"
start block-public --netplay-host room --netplay-public --netplay-server 127.0.0.1:27323 --netplay-direct --players 2 --netplay-player 4058
room_of block-public > /dev/null
# It must see the room (listed first) and pass over it: not join it and then leave (that's the other check).
hashes=$(sed -n 's/^\[harness\] build .*: build \([0-9a-f]*\), ROM \([0-9a-f]*\), profile \([0-9a-f]*\)$/--build \1 --rom \2 --profile \3/p' "$OUT/block-public.out")
i=0
# shellcheck disable=SC2086
while [ $i -lt 50 ] && ! "$(dirname "$EXE")/src/netplay/net/netplay_probe" list --signal 127.0.0.1:27323 $hashes --mods 0 2>&1 | grep -q " host 0000000000004058$"; do
    sleep 0.2
    i=$((i + 1))
done
start block-quick --netplay-quick --netplay-server 127.0.0.1:27323 --netplay-direct --players 2 --netplay-block 4058
if finish "no room to join, so hosting one" block-quick && ! grep -q "Quick Match: joining\|is on the block list" "$OUT/block-quick.out"; then :; else
    echo "  block (Quick Match): FAILED"; failed=1; fi
kill $server 2>/dev/null

echo "kick"
# The host kicks the joiner during the match (the Esc menu's Kick): the joiner is told, the host plays on to
# the end.
pads=1.pads start kick-host --netplay-host 27315 --netplay-address 127.0.0.1 --players 2 --netplay-kick-at 300:2
pads=2.pads start kick-join --netplay-join "$(code_of kick-host)"
# (The joiner's match ends there, and with it that copy; the host's goes on to the replay's end.)
if finish "was the last: quitting\|the host ended the match: the host removed this player" kick-host kick-join &&
    grep -q "kicking port 2" "$OUT/kick-host.out" && grep -q "was the last: quitting" "$OUT/kick-host.out" &&
    grep -q "the host ended the match: the host removed this player" "$OUT/kick-join.out"; then :; else echo "  kick: FAILED"; failed=1; fi

echo "recover"
pads=mouse-1.pads start recover-host --netplay-host 27326 --netplay-address 127.0.0.1 --players 3
code=$(code_of recover-host)
pads=mouse-2.pads start recover-join1 --netplay-join "$code"
i=0
while ! grep -q "in, on port 2" "$OUT/recover-join1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
pads=mouse-3.pads start recover-join2 --netplay-join "$code" --netplay-desync-at 900 --netplay-desync-at 2000
if finish "was the last: quitting" recover-host recover-join1 recover-join2; then
    same recover-host mouse-solo "$MOUSE_READS" || failed=1
    same recover-join1 mouse-solo "$MOUSE_READS" || failed=1
    # The second joiner's game is the host's from the read it loaded at: its hashes from there are the solo replay's.
    loaded=$(sed -n "s/^\[netplay\] the host's game was loaded before read \([0-9]*\).*/\1/p" "$OUT/recover-join2.out" | tail -n 1)
    if [ -n "$loaded" ] && [ "$loaded" -gt 2000 ] && [ "$(wc -l < "$OUT/recover-join2.log")" -eq "$MOUSE_READS" ] &&
        [ "$(tail -n +"$((loaded + 1))" "$OUT/mouse-solo.log" | md5sum)" = "$(tail -n +"$((loaded + 1))" "$OUT/recover-join2.log" | md5sum)" ] &&
        grep -q "desync recovery: 2, each set right" "$OUT/recover-host.out"; then
        echo "  recover-join2 loaded the host's game twice, last before read $loaded, and is the same as mouse-solo from there"
    else
        echo "  recover-join2 didn't load the host's game, or isn't the same as mouse-solo after it (loaded: ${loaded:-none})"
        failed=1
    fi
else
    failed=1
fi

echo "rejoin"
pads=mouse-1.pads start rejoin-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-stall-at 1000:8000
code=$(code_of rejoin-host)
pads=mouse-2.pads start rejoin-join1 --netplay-join "$code"
wait_for rejoin-join1 "in, on port 2"
pads=mouse-3.pads start rejoin-join2 --netplay-join "$code" --netplay-player 3e70 --stop-after 900
wait_for rejoin-join2 "was the last: quitting"
pads=mouse-3.pads start rejoin-back --netplay-join "$code" --netplay-player 3e70
if finish "was the last: quitting" rejoin-host rejoin-join1 rejoin-back; then
    from=$(left_at rejoin-host 3)
    back=$(back_at rejoin-host 3)
    loaded=$(sed -n "s/^\[netplay\] back in the match: the host's game was loaded before read \([0-9]*\).*/\1/p" "$OUT/rejoin-back.out")
    if [ -z "$from" ] || [ -z "$back" ] || [ "$loaded" != "$back" ]; then
        echo "  port 3 didn't come back (left from read '$from', back from '$back', loaded before '$loaded')"
        failed=1
    else
        echo "  port 3 idle from read $from, back from read $back"
        idle_from "$OUT/mouse.pads" "$OUT/rejoin-solo.pads" 3 "$from" "$back" || exit 2
        pads=rejoin-solo.pads start rejoin-solo
        if finish "was the last: quitting" rejoin-solo; then
            same rejoin-host rejoin-solo "$MOUSE_READS" || failed=1
            same rejoin-join1 rejoin-solo "$MOUSE_READS" || failed=1
            # Back, its game is the host's from that read (before it, its own game's as it started).
            same_from rejoin-back rejoin-solo "$back" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "rejoin-twice"
# (Ports held for whoever left them all the while, however slow the run: the stranger must find none free.)
pads=mouse-1.pads start twice-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-stall-at 1000:8000 \
    --netplay-stall-at 2200:8000 --netplay-seat-hold 3600
code=$(code_of twice-host)
pads=mouse-2.pads start twice-join1 --netplay-join "$code" --stop-after 600
wait_for twice-join1 "in, on port 2"
pads=mouse-3.pads start twice-join2 --netplay-join "$code" --netplay-player 3e71 --stop-after 900
wait_for twice-join2 "was the last: quitting"
pads=mouse-3.pads start twice-back1 --netplay-join "$code" --netplay-player 3e71 --stop-after 2000
wait_for twice-back1 "was the last: quitting"
# Port 3 free again, but only for its player: anyone else is refused.
pads=mouse-3.pads start twice-stranger --netplay-join "$code" --netplay-player 5a5a
pads=mouse-3.pads start twice-back2 --netplay-join "$code" --netplay-player 3e71
if finish "was the last: quitting\|couldn't join" twice-host twice-back2 twice-stranger; then
    gone2=$(left_at twice-host 2)
    from1=$(left_at twice-host 3 1)
    back1=$(back_at twice-host 3 1)
    from2=$(left_at twice-host 3 2)
    back2=$(back_at twice-host 3 2)
    if ! grep -q "couldn't join: the match has already started" "$OUT/twice-stranger.out"; then
        echo "  another player was let into port 3"
        failed=1
    elif [ -z "$gone2" ] || [ -z "$from1" ] || [ -z "$back1" ] || [ -z "$from2" ] || [ -z "$back2" ]; then
        echo "  port 3 didn't leave and come back twice (port 2 left from '$gone2'; port 3 '$from1' to '$back1', '$from2' to '$back2')"
        failed=1
    else
        echo "  port 2 idle from read $gone2; port 3 from $from1 to $back1, and from $from2 to $back2; another player refused"
        idle_from "$OUT/mouse.pads" "$OUT/twice-solo.pads" 2 "$gone2" || exit 2
        idle_from "$OUT/twice-solo.pads" "$OUT/twice-solo.pads" 3 "$from1" "$back1" || exit 2
        idle_from "$OUT/twice-solo.pads" "$OUT/twice-solo.pads" 3 "$from2" "$back2" || exit 2
        pads=twice-solo.pads start twice-solo
        if finish "was the last: quitting" twice-solo; then
            same twice-host twice-solo "$MOUSE_READS" || failed=1
            same_from twice-back1 twice-solo "$back1" 2000 || failed=1
            same_from twice-back2 twice-solo "$back2" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "rejoin-both"
pads=mouse-1.pads start both-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-stall-at 1000:8000
code=$(code_of both-host)
pads=mouse-2.pads start both-join1 --netplay-join "$code" --netplay-player b071 --stop-after 800
wait_for both-join1 "in, on port 2"
pads=mouse-3.pads start both-join2 --netplay-join "$code" --netplay-player b072 --stop-after 900
wait_for both-join2 "was the last: quitting"
# Both while the host has stopped: it takes them in at once, and brings them back together.
wait_for both-host "the game stops"
pads=mouse-2.pads start both-back1 --netplay-join "$code" --netplay-player b071
pads=mouse-3.pads start both-back2 --netplay-join "$code" --netplay-player b072
if finish "was the last: quitting" both-host both-back1 both-back2; then
    from2=$(left_at both-host 2)
    from3=$(left_at both-host 3)
    back2=$(back_at both-host 2)
    back3=$(back_at both-host 3)
    if [ -z "$from2" ] || [ -z "$from3" ] || [ -z "$back2" ] || [ "$back3" != "$back2" ]; then
        echo "  ports 2 and 3 didn't come back together (port 2 '$from2' to '$back2', port 3 '$from3' to '$back3')"
        failed=1
    else
        echo "  port 2 idle from read $from2 to $back2, port 3 from $from3 to $back3"
        idle_from "$OUT/mouse.pads" "$OUT/both-solo.pads" 2 "$from2" "$back2" || exit 2
        idle_from "$OUT/both-solo.pads" "$OUT/both-solo.pads" 3 "$from3" "$back3" || exit 2
        pads=both-solo.pads start both-solo
        if finish "was the last: quitting" both-solo; then
            same both-host both-solo "$MOUSE_READS" || failed=1
            same_from both-back1 both-solo "$back2" || failed=1
            same_from both-back2 both-solo "$back3" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "rejoin-couch"
pads=couch-1.pads start couch-back-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-stall-at 1000:8000
code=$(code_of couch-back-host)
pads=couch-23.pads start couch-back-join --netplay-join "$code" --local-players 2 --netplay-player c0c4 --stop-after 900
wait_for couch-back-join "was the last: quitting"
pads=couch-23.pads start couch-back-again --netplay-join "$code" --local-players 2 --netplay-player c0c4
if finish "was the last: quitting" couch-back-host couch-back-again; then
    from=$(left_at couch-back-host 2)
    back=$(back_at couch-back-host 2)
    if [ -z "$from" ] || [ "$(left_at couch-back-host 3)" != "$from" ] || [ -z "$back" ] || [ "$(back_at couch-back-host 3)" != "$back" ]; then
        echo "  ports 2 and 3 didn't leave and come back together"
        failed=1
    else
        echo "  ports 2 and 3 idle from read $from, back from read $back"
        idle_from "$OUT/couch-solo23.pads" "$OUT/couch-back-solo.pads" 23 "$from" "$back" || exit 2
        pads=couch-back-solo.pads start couch-back-solo
        if finish "was the last: quitting" couch-back-solo; then
            same couch-back-host couch-back-solo "$MOUSE_READS" || failed=1
            same_from couch-back-again couch-back-solo "$back" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "late"
pads=mouse-1.pads start late-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-start-with 2 \
    --netplay-stall-at 1000:8000
code=$(code_of late-host)
pads=mouse-2.pads start late-join1 --netplay-join "$code"
wait_for late-host "the game stops"
# A first new player quits as soon as it's in (before or after its Back, never in the match): port 3 must be free again.
pads=mouse-3.pads start late-gone --netplay-join "$code" --netplay-player 9a9e --stop-after 1
wait_for late-gone "was the last: quitting"
pads=mouse-3.pads start late-join2 --netplay-join "$code" --netplay-player 1a7e
if finish "was the last: quitting" late-host late-join1 late-gone late-join2; then
    # (The last Back for port 3: the first new player's may have come before it left.)
    back=$(back_at late-host 3 '$')
    loaded=$(sed -n "s/^\[netplay\] in the match: the host's game was loaded before read \([0-9]*\).*/\1/p" "$OUT/late-join2.out")
    if ! grep -q "the match starts: 3 players, .*, port 3 open" "$OUT/late-host.out" ||
        ! grep -q "the match starts: 3 players, .*, port 3 open" "$OUT/late-join1.out"; then
        echo "  the match didn't start with port 3 open"
        failed=1
    elif ! grep -q "in, on port 3" "$OUT/late-gone.out"; then
        echo "  the first new player wasn't let in"
        failed=1
    elif [ -z "$back" ] || [ "$loaded" != "$back" ]; then
        echo "  port 3 didn't join (from read '$back', loaded before '$loaded')"
        failed=1
    else
        echo "  port 3 open, joined from read $back"
        idle_from "$OUT/mouse.pads" "$OUT/late-solo.pads" 3 0 "$back" || exit 2
        pads=late-solo.pads start late-solo
        if finish "was the last: quitting" late-solo; then
            same late-host late-solo "$MOUSE_READS" || failed=1
            same late-join1 late-solo "$MOUSE_READS" || failed=1
            same_from late-join2 late-solo "$back" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "late-seat"
pads=mouse-1.pads start seat-host --netplay-host 27324 --netplay-address 127.0.0.1 --players 3 --netplay-seat-hold 0 \
    --netplay-stall-at 1000:8000
code=$(code_of seat-host)
pads=mouse-2.pads start seat-join1 --netplay-join "$code"
wait_for seat-join1 "in, on port 2"
pads=mouse-3.pads start seat-join2 --netplay-join "$code" --netplay-player 5e01 --stop-after 900
wait_for seat-join2 "was the last: quitting"
pads=mouse-3.pads start seat-new --netplay-join "$code" --netplay-player 5e02
wait_for seat-new "in, on port 3"
pads=mouse-3.pads start seat-old --netplay-join "$code" --netplay-player 5e01
if finish "was the last: quitting\|couldn't join" seat-host seat-join1 seat-new seat-old; then
    from=$(left_at seat-host 3)
    back=$(back_at seat-host 3)
    if ! grep -q "couldn't join: the match has already started" "$OUT/seat-old.out"; then
        echo "  the player who left got port 3 back after another took it"
        failed=1
    elif ! grep -q "port 3 joined the match under way" "$OUT/seat-host.out" || [ -z "$from" ] || [ -z "$back" ]; then
        echo "  another player didn't take port 3 (left from read '$from', joined from '$back')"
        failed=1
    else
        echo "  port 3 idle from read $from, another player's from read $back; the one who left refused"
        idle_from "$OUT/mouse.pads" "$OUT/seat-solo.pads" 3 "$from" "$back" || exit 2
        pads=seat-solo.pads start seat-solo
        if finish "was the last: quitting" seat-solo; then
            same seat-host seat-solo "$MOUSE_READS" || failed=1
            same seat-join1 seat-solo "$MOUSE_READS" || failed=1
            same_from seat-new seat-solo "$back" || failed=1
        else
            failed=1
        fi
    fi
else
    failed=1
fi

echo "recover-again"
start again-host --netplay-host 27326 --netplay-address 127.0.0.1 --players 2
start again-join --netplay-join "$(code_of again-host)" --netplay-desync-at 300 --netplay-desync-at 400
if finish "the netplay match is over" again-host again-join &&
    grep -q "went another way again" "$OUT/again-host.out" && grep -q "the host's game was loaded before read" "$OUT/again-join.out"; then
    echo "  $(grep -m1 "went another way again" "$OUT/again-host.out" | sed 's/^\[netplay\] //')"
else
    echo "  recover-again: FAILED (the second desync didn't end the match after the first was set right)"
    failed=1
fi

echo "desync"
start desync-host --netplay-host 27312 --netplay-address 127.0.0.1 --players 2 --netplay-desync-at 300 --netplay-no-recovery
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

echo "desync-room"
# The same through a room: the netplay server logs a summary of it (the regions; no addresses).
"$(dirname "$EXE")/src/netplay/net/netplay_server" --port 27322 > "$OUT/desync-server.out" 2>&1 &
server=$!
servers="${servers:-} $server"
room="--netplay-server 127.0.0.1:27322 --netplay-direct"
# shellcheck disable=SC2086
start desync-room-host --netplay-host room $room --players 2 --netplay-desync-at 300 --netplay-no-recovery
# shellcheck disable=SC2086
start desync-room-join --netplay-join "$(room_of desync-room-host)" $room
if finish "the netplay match is over" desync-room-host desync-room-join &&
    grep -q "^netplay_server: desync: match [0-9a-f]*, build [0-9a-f]*, [A-Za-z]*, 2 players, port [12] against port [12], read 3[0-9][0-9], regions 64," "$OUT/desync-server.out"; then
    echo "  $(grep -m1 "desync: match" "$OUT/desync-server.out")"
else
    echo "  desync-room: FAILED (the server logged no summary: desync-server.out)"
    failed=1
fi
kill $server 2>/dev/null

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: the match gave each copy the same game as the replay alone, a desync was set right, and one was caught"
