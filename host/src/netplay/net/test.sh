#!/bin/sh
# Tests the netplay transport from a build of this folder (CMakeLists.txt): the packets' and join codes'
# tests, then matches between copies of netplay_probe over loopback (2 players; 3 with 20% of packets
# lost once the match starts, which also tests the host passing inputs on; 3 and 4 with two players on
# the host or on a joiner, also lossy; a joiner with more players than ports left, which must be
# refused), joiners with another match's key, another ROM or
# other mods, which must be refused, a joiner kicked or kept out (the host's block list), the host waiting
# for everyone to be Ready, joiners sending Inputs early or far ahead or a Hash twice, which
# must be dropped, as must one sending Inputs for a port that isn't its own; a spectator (all the pads, none of its own: one
# sending Inputs dropped; refused by a host that takes none); one that never says Hello, which must be dropped too, and one sending a message bigger
# than any packet, which the library must close. A match found on this network (LAN discovery: by build and
# platform, and the broadcast alone). Desync summaries the server logs, within its limits. Then a
# match through ICE, by a room on the signalling
# server netplay_server on loopback (directly: the relay needs a TURN server, tools/netplay/server/test.sh);
# a room gone with its host, the public room list (by game, mods and platform), a relay-only host on a
# server without one, the connection test (no relay, no server), an older version's games (which play together, apart from this one's), and
# guessing room codes, which the server stops.
#   sh host/src/netplay/net/test.sh <build folder>
set -u
bin=$1
probe=$bin/netplay_probe
failed=0

"$bin/packets_test" || failed=1
"$bin/join_code_test" || failed=1
"$bin/signal_packets_test" || failed=1
"$bin/turn_credentials_test" || failed=1
"$bin/lan_packets_test" || failed=1
"$bin/stun_test" || failed=1

# The join code a host printed in <file> (its key is random), once it has.
code_of() {
    i=0
    while [ $i -lt 50 ]; do
        code=$(sed -n 's/^join code //p' "$1")
        [ -n "$code" ] && { echo "$code"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}

# match <name> <port> <players> [probe options...]: a host and players - 1 joiners, by its code; all
# must succeed.
match() {
    name=$1; port=$2; players=$3; shift 3
    "$probe" host "$port" --players "$players" "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    to=$(code_of "$name.host.txt")
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "$to" "$@" > "$name.join$i.txt" 2>&1 &
        pids="$pids $!"
        sleep 0.5
        i=$((i + 1))
    done
    ok=1
    for pid in $pids; do
        wait "$pid" || ok=0
    done
    for f in "$name".*.txt; do
        echo "-- $f"
        grep -v '^\[netplay\]' "$f"
    done
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}

match two 27200 2 --reads 150 --own-view 1
# The host's own-view rule reaches the joiner with Start.
if grep -q "started (own view)" two.join1.txt; then echo "own view: passed"; else echo "own view: FAILED"; failed=1; fi
match three-lossy 27201 3 --reads 150 --loss 20

# couch <name> <port> <players> <host's players here> <each joiner's...>: a match with more than one
# player on some machines, lossy once started (joiners started half a second apart); all must succeed.
couch() {
    name=$1; port=$2; players=$3; host_local=$4; shift 4
    "$probe" host "$port" --players "$players" --local "$host_local" --reads 150 --loss 20 > "$name.host.txt" 2>&1 &
    pids=$!
    to=$(code_of "$name.host.txt")
    i=1
    for local in "$@"; do
        "$probe" join "$to" --local "$local" --reads 150 --loss 20 > "$name.join$i.txt" 2>&1 &
        pids="$pids $!"
        sleep 0.5
        i=$((i + 1))
    done
    ok=1
    for pid in $pids; do
        wait "$pid" || ok=0
    done
    for f in "$name".*.txt; do
        echo "-- $f"
        grep -v '^\[netplay\]' "$f"
    done
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}
couch couch-host 27210 3 2 1
grep -q "welcomed: ports 4 of 3" couch-host.join1.txt || { echo "couch-host: the joiner isn't on port 2"; failed=1; }
couch couch-join 27211 4 1 2 1
# Lowest free ports first, in the order the handshakes finish (either joiner may be first).
if ! { grep -q "welcomed: ports 6 of 4" couch-join.join1.txt && grep -q "welcomed: ports 8 of 4" couch-join.join2.txt; } &&
   ! { grep -q "welcomed: ports c of 4" couch-join.join1.txt && grep -q "welcomed: ports 2 of 4" couch-join.join2.txt; }; then
    echo "couch-join: the joiners' ports aren't the lowest free ones"
    failed=1
fi

# Another match's key (a code for this address with another key), another ROM, or other mods: refused,
# with the reason (a wrong key's alone, as nothing else is checked).
refused() {
    name=$1; port=$2; option=$3; reason=$4
    # shellcheck disable=SC2086
    "$probe" host "$port" --reads 30 ${host_options:-} > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    [ "$option" = "--key" ] && to=$("$probe" code "127.0.0.1:$port" 1234567890ABCDEF) && option=
    "$probe" join "$to" --reads 30 ${option:+$option 2} > "$name.join.txt" 2>&1
    if grep -q "ended: $reason" "$name.join.txt"; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
refused refused-key 27207 --key "not this match's join code"
refused refused-rom 27202 --rom "a different ROM"
refused refused-mods 27204 --mods "mods on"
# More players on a joiner than the match has ports left (a match of 2: the host and one).
refused refused-full 27212 --local "the match is full"
# The waiting room: the host kicks a joiner, or keeps one out (its block list: player id 2): told only that the
# host removed it.
host_options="--kick 1" refused refused-kicked 27216 --player "the host removed this player"
host_options="--block 2" refused refused-blocked 27217 --player "the host removed this player"
# Another build: told which release the host has.
host_options="--release v0.1.6-netplay.99" refused refused-build 27219 --build "a different build of the game (.*), host's release v0.1.6-netplay.99$"
host_options=
# The host waits for everyone to be Ready: a joiner that says so after 3 s starts the match then, not as it joins.
"$probe" host 27218 --reads 30 > ready.host.txt 2>&1 &
host=$!
if "$probe" join "$(code_of ready.host.txt)" --reads 30 --ready-after 3 > ready.join.txt 2>&1 && wait $host &&
    after=$(sed -n 's/^started after \([0-9.]*\) s$/\1/p' ready.host.txt) && [ -n "$after" ] &&
    awk -v a="$after" 'BEGIN { exit !(a >= 2.5) }'; then echo "ready: passed (started after $after s)"; else
    echo "ready: FAILED"; cat ready.host.txt ready.join.txt; failed=1; kill $host 2>/dev/null; fi

# A hostile joiner: Inputs before the start (it joins a lobby of 3 alone, so the match hasn't started),
# far beyond what the game could have reached, or the same Hash twice (in a started match of 2). The host
# must drop it as broken (the joiner hears why) and keep running.
hostile() {
    name=$1; port=$2; players=$3; mode=$4
    "$probe" host "$port" --players "$players" --reads 600 > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    "$probe" join "$to" --reads 600 --hostile "$mode" > "$name.join.txt" 2>&1
    if grep -q "ended: an unexpected or broken packet" "$name.join.txt" && kill -0 $host 2>/dev/null; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
hostile hostile-early 27205 3 early
hostile hostile-far 27206 2 far
hostile hostile-hash 27208 2 hash
hostile hostile-port 27213 2 port

# Spectators: a joiner with nobody playing (--local 0) watches, getting every port's pads; one that sends Inputs (of the
# host's port) is dropped as broken while the match goes on; a host that takes none refuses one.
"$probe" host 27222 --players 2 --reads 150 > watch.host.txt 2>&1 &
pids=$!
to=$(code_of watch.host.txt)
# (The spectators in first: the match starts as the player comes, and the probe's host brings none in under way.)
"$probe" join "$to" --reads 150 --local 0 > watch.spectator.txt 2>&1 &
pids="$pids $!"
"$probe" join "$to" --reads 150 --local 0 --hostile port > watch.hostile.txt 2>&1 &
hostile=$!
sleep 1
"$probe" join "$to" --reads 150 > watch.join.txt 2>&1 &
pids="$pids $!"
ok=1
for pid in $pids; do
    wait "$pid" || ok=0
done
wait $hostile
if [ $ok -eq 1 ] && grep -q "welcomed: ports 0 of 2" watch.spectator.txt && grep -q "^port 0: 150 of 150 reads, 0 wrong" watch.spectator.txt &&
    grep -q "^port 1: 150 of 150 reads, 0 wrong" watch.spectator.txt && grep -q "ended: an unexpected or broken packet" watch.hostile.txt; then
    echo "watch: passed"
else
    echo "watch: FAILED"
    cat watch.host.txt watch.spectator.txt watch.hostile.txt
    failed=1
fi
"$probe" host 27223 --reads 30 --spectators 0 > watch-refused.host.txt 2>&1 &
host=$!
"$probe" join "$(code_of watch-refused.host.txt)" --reads 30 --local 0 > watch-refused.join.txt 2>&1
if grep -q "ended: the host takes no spectators" watch-refused.join.txt; then echo "watch-refused: passed"; else
    echo "watch-refused: FAILED"; cat watch-refused.join.txt; failed=1; fi
kill $host 2>/dev/null

# A joiner that never says Hello: dropped after the handshake time (3 s). One that sends a message bigger
# than any packet: closed by the library at once. One of a newer protocol version: refused, told the host's release.
raw() {
    name=$1; port=$2; option=$3
    # shellcheck disable=SC2086
    "$probe" host "$port" --reads 30 ${host_options:-} > "$name.host.txt" 2>&1 &
    host=$!
    code_of "$name.host.txt" > /dev/null
    if "$probe" join "127.0.0.1:$port" "$option" > "$name.join.txt" 2>&1; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        failed=1
    fi
    grep -v '^\[netplay\]' "$name.join.txt"
    kill $host 2>/dev/null
}
raw silent 27203 --silent
raw oversize 27209 --oversize
host_options="--release v0.1.6-netplay.97" raw future 27214 --future
grep -q "^refused: a different version of netplay, host's release v0.1.6-netplay.97$" future.join.txt || { echo "future: FAILED (no release)"; failed=1; }

# LAN discovery: a host visible on this network (on a port of the test's own) is heard, and joined by the
# code the beacon makes; a game of another build hears nothing.
"$probe" host 27240 --lan 1 --lan-port 27241 --reads 60 > lan.host.txt 2>&1 &
lan_host=$!
sleep 1
"$probe" lan --lan-port 27241 > lan.list.txt 2>&1
lan_code=$(sed -n 's/^lan [^ ]* \([^ ]*\) 1\/2 rollback 0$/\1/p' lan.list.txt | head -1)
if [ -n "$lan_code" ] && "$probe" join "$lan_code" --reads 60 > lan.join.txt 2>&1 && wait $lan_host; then echo "lan: passed"; else
    echo "lan: FAILED"; cat lan.list.txt lan.host.txt lan.join.txt; failed=1; kill $lan_host 2>/dev/null; fi
# Another build hears nothing; this one, then, hears the host (so it was there to be heard).
# (As Linux's: the machine's own platform may be macOS.)
"$probe" host 27242 --lan 1 --lan-port 27241 --platform 1 --reads 30 > lan-other.host.txt 2>&1 &
lan_host=$!
sleep 1
if "$probe" lan --lan-port 27241 --platform 1 --build 2 2>&1 | grep -qx "matches 0" && "$probe" lan --lan-port 27241 --platform 3 2>&1 | grep -qx "matches 0" &&
    "$probe" lan --lan-port 27241 --platform 1 2>&1 | grep -qx "matches 1"; then
    echo "lan-other: passed"; else echo "lan-other: FAILED"; failed=1; fi
kill $lan_host 2>/dev/null
# The broadcast alone (not also sent to this machine): heard where a firewall lets it in. Required with
# LAN_BROADCAST=1 (the CI's Linux runner), only said otherwise (a firewall here may keep it out).
"$probe" host 27243 --lan 1 --lan-loopback 0 --lan-port 27244 --reads 30 > lan-broadcast.host.txt 2>&1 &
lan_host=$!
sleep 1
if "$probe" lan --lan-port 27244 2>&1 | grep -qx "matches 1"; then echo "lan-broadcast: passed"
elif [ "${LAN_BROADCAST:-0}" = 1 ]; then echo "lan-broadcast: FAILED (the broadcast wasn't heard)"; failed=1
else echo "lan-broadcast: not heard (a firewall here may keep broadcasts out; LAN_BROADCAST=1 requires it)"; fi
kill $lan_host 2>/dev/null

# Through ICE: the server, then a host makes a room and two joiners join by its code, lossy once the match starts.
# (Every probe connects from the same address: more than the server lets one make in a burst, or have at once.)
"$bin/netplay_server" --port 27220 --connection-burst 1000 --max-per-address 32 --max-listed-per-address 3 --desync-burst 8 \
    --status 127.0.0.1:27224 > ice.server.txt 2>&1 &
server=$!
room_of() {
    i=0
    while [ $i -lt 100 ]; do
        room=$(sed -n 's/^room //p' "$1")
        [ -n "$room" ] && { echo "$room"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}
"$probe" host room --players 3 --signal 127.0.0.1:27220 --direct 1 --reads 150 --loss 20 > ice.host.txt 2>&1 &
pids=$!
room=$(room_of ice.host.txt)
for i in 1 2; do
    "$probe" join "$room" --signal 127.0.0.1:27220 --direct 1 --reads 150 --loss 20 > "ice.join$i.txt" 2>&1 &
    pids="$pids $!"
    sleep 0.5
done
ok=1
for pid in $pids; do
    wait "$pid" || ok=0
done
for f in ice.host.txt ice.join*.txt; do
    echo "-- $f"
    grep -v '^\[netplay\]' "$f"
done
if [ $ok -eq 1 ] && grep -q "P2P ICE" ice.join1.txt; then echo "ice: passed"; else echo "ice: FAILED"; failed=1; fi
# A host that made a room and left, then hosts again from the same process (a new id: the server keeps the
# old one from reuse).
"$probe" host room --signal 127.0.0.1:27220 --direct 1 --reads 60 --again 1 > ice-again.host.txt 2>&1 &
pids=$!
room=$(room_of ice-again.host.txt)
"$probe" join "$room" --signal 127.0.0.1:27220 --direct 1 --reads 60 > ice-again.join.txt 2>&1 &
pids="$pids $!"
ok=1
for pid in $pids; do
    wait "$pid" || ok=0
done
if [ $ok -eq 1 ] && grep -q "^left room" ice-again.host.txt; then echo "ice-again: passed"; else
    echo "ice-again: FAILED"; cat ice-again.host.txt ice-again.join.txt; failed=1; fi
# Desync summaries: 6 from one connection and 1 from another, from this address: the server logs 5 (4, its
# limit for a connection, and the other's: this server allows 8 from an address), counting the others,
# with nothing of the sender's in them.
"$probe" desync --signal 127.0.0.1:27220 --platform 1 --count 6 > desync-report.txt 2>&1
"$probe" desync --signal 127.0.0.1:27220 --platform 1 > desync-report2.txt 2>&1
sleep 0.5
logged=$(grep -c "^netplay_server: desync: match 5eed1234, build 0123456789abcdef, Linux, 2 players, port 2 against port 1, read [0-9]*, regions 64, rollback 0, delay 2$" ice.server.txt)
if grep -qx "sent 6" desync-report.txt && grep -qx "sent 1" desync-report2.txt && [ "$logged" -eq 5 ] && ! grep "desync:" ice.server.txt | grep -q "127.0.0.1"; then
    echo "desync-reports: passed (5 logged)"; else echo "desync-reports: FAILED ($logged logged)"; grep "desync" ice.server.txt; failed=1; fi
# The status (--status): the rooms made and joined, the 7 summaries reported and the 5 logged, as the page, JSON
# and text; never an address or a room code; other paths and methods turned away.
curl -s http://127.0.0.1:27224/status.json > status.json.txt
curl -s http://127.0.0.1:27224/status.txt > status.txt
curl -s http://127.0.0.1:27224/ > status.page.txt
status_codes=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:27224/nope; curl -s -o /dev/null -w " %{http_code}" -X POST http://127.0.0.1:27224/)
recent=$(grep -o '"build":"0123456789abcdef"' status.json.txt | wc -l)
if grep -q '"desyncs":7,"desyncs_not_logged":2' status.json.txt && [ "$recent" -eq 5 ] && grep -q '"rooms_created":[3-9]' status.json.txt &&
    grep -q '"matches":[2-9]' status.json.txt && grep -q "^  desyncs: 7 (2 not logged)" status.txt && grep -q "<h1>Netplay server</h1>" status.page.txt &&
    [ "$status_codes" = "404 405" ] && ! grep -q "127\.0\.0\.1" status.json.txt status.txt status.page.txt &&
    ! grep -q -e "$room" -e "$(sed -n 's/^room //p' ice.host.txt)" status.json.txt status.txt status.page.txt; then
    echo "status: passed"; else echo "status: FAILED ($status_codes)"; cat status.json.txt status.txt; failed=1; fi
# expect <name> <text> <probe options...>: the probe fails, saying <text>.
expect() {
    name=$1; text=$2; shift 2
    if ! "$probe" "$@" > "$name.txt" 2>&1 && grep -q "$text" "$name.txt"; then echo "$name: passed"; else
        echo "$name: FAILED"; cat "$name.txt"; failed=1; fi
}
# The room closed with its host: the code finds nothing now.
expect ice-closed "no room $room on the server" join "$room" --signal 127.0.0.1:27220 --direct 1
# Relay-only, but this server has no relay.
expect ice-norelay "offers no TURN relay" host room --signal 127.0.0.1:27220
# The menu's connection test: this server (no relay), and one that doesn't answer (the relay's check is in
# tools/netplay/server/test.sh). It tells what it found, and the LAN port it opens.
"$probe" test --signal 127.0.0.1:27220 --lan-port 27245 > test-norelay.txt 2>&1
if grep -qx "verdict server good relay unknown direct unknown lan good" test-norelay.txt && grep -q "^The server offers no relay" test-norelay.txt &&
    grep -q "signalling version [0-9]" test-norelay.txt; then echo "test-norelay: passed"; else echo "test-norelay: FAILED"; cat test-norelay.txt; failed=1; fi
"$probe" test --signal 127.0.0.1:27229 --lan-port 27245 > test-noserver.txt 2>&1
if grep -qx "verdict server bad relay unknown direct unknown lan good" test-noserver.txt && grep -q "^The netplay server \(didn't answer\|couldn't be reached\)" test-noserver.txt; then
    echo "test-noserver: passed"; else echo "test-noserver: FAILED"; cat test-noserver.txt; failed=1; fi
# The public room list: a listed room of this game (3 players), one with other mods (counted, not listed),
# one on macOS (apart from Linux and Windows), one not listed, and one past the server's public rooms
# from one address (--max-listed-per-address 3 here: it stays private, so it isn't counted). The list
# shows the first as players join it, and as it's played.
# listed <name> <expected "rooms n, others n, older n"> <grep for a room line, or -> <probe options...>: asked
# again until it's so, for up to 15 s (or $tries half seconds). As Linux unless the options say (the
# machine's own platform may be macOS, which is apart).
listed() {
    name=$1; count=$2; line=$3; shift 3
    i=0
    while :; do
        "$probe" list --signal 127.0.0.1:27220 --platform 1 "$@" > "$name.txt" 2>&1
        if grep -qx "$count" "$name.txt" && { [ "$line" = - ] || grep -q "^listed $line" "$name.txt"; }; then echo "$name: passed"; return; fi
        [ $i -ge "${tries:-30}" ] && break
        sleep 0.5
        i=$((i + 1))
    done
    echo "$name: FAILED (wanted $count, $line)"; cat "$name.txt"; failed=1
}
"$probe" host room --players 3 --public 1 --platform 1 --player 10b --signal 127.0.0.1:27220 --direct 1 --reads 300 > lobby.host.txt 2>&1 &
lobby_host=$!
lobby_room=$(room_of lobby.host.txt)
others=
"$probe" host room --public 1 --platform 1 --mods 2 --signal 127.0.0.1:27220 --direct 1 > lobby.mods.txt 2>&1 &
others="$others $!"
"$probe" host room --public 1 --platform 3 --signal 127.0.0.1:27220 --direct 1 > lobby.mac.txt 2>&1 &
others="$others $!"
"$probe" host room --signal 127.0.0.1:27220 --direct 1 > lobby.private.txt 2>&1 &
others="$others $!"
room_of lobby.mods.txt > /dev/null; room_of lobby.mac.txt > /dev/null; room_of lobby.private.txt > /dev/null
listed lobby-list "rooms 1, others 1, older 0" "$lobby_room 1/3 direct rollback 0 host 000000000000010b release probe$"
"$probe" host room --public 1 --platform 1 --mods 3 --signal 127.0.0.1:27220 --direct 1 > lobby.capped.txt 2>&1 &
others="$others $!"
room_of lobby.capped.txt > /dev/null
sleep 1
listed lobby-capped "rooms 1, others 1, older 0" "$lobby_room 1/3"
listed lobby-windows "rooms 1, others 1, older 0" "$lobby_room 1/3" --platform 2
listed lobby-mac "rooms 1, others 0, older 0" - --platform 3
grep -q "^listed $lobby_room" lobby-mac.txt && { echo "lobby-mac: FAILED (Linux's room listed)"; failed=1; }
# Another build of the same ROM: the rooms of this one are listed as another build's, with their release
# (the one with other mods too: settings aside, it's another release).
listed lobby-build "rooms 2, others 0, older 0" "$lobby_room 1/3 direct rollback 0 host 000000000000010b release probe other-build$" --build 2
listed lobby-rom "rooms 0, others 0, older 0" - --build 2 --rom 2
"$probe" join "$lobby_room" --signal 127.0.0.1:27220 --direct 1 --reads 300 > lobby.join1.txt 2>&1 &
lobby_joiners=$!
listed lobby-joined "rooms 1, others 1, older 0" "$lobby_room 2/3"
"$probe" join "$lobby_room" --signal 127.0.0.1:27220 --direct 1 --reads 300 > lobby.join2.txt 2>&1 &
lobby_joiners="$lobby_joiners $!"
listed lobby-playing "rooms 1, others 1, older 0" "$lobby_room 3/3 playing"
# The status counts it: public and being played, of its release.
if curl -s http://127.0.0.1:27224/status.json | grep -q '"public_playing":[1-9][0-9]*,.*"probe":[1-9]'; then echo "status-playing: passed"; else
    echo "status-playing: FAILED"; curl -s http://127.0.0.1:27224/status.json; failed=1; fi
for pid in $lobby_host $lobby_joiners; do
    wait "$pid" || { echo "lobby-match: FAILED"; failed=1; }
done
# Killed, the others' connections only time out on the server (the library's 10 s): until then, their
# rooms are listed.
kill $others 2>/dev/null
tries=60 listed lobby-gone "rooms 0, others 0, older 0" -
# An older game's signalling (version 2, before public rooms): two of them play through a room, but
# neither version finds the other's rooms.
"$probe" host room --speak 2 --signal 127.0.0.1:27220 --direct 1 --reads 60 > old.host.txt 2>&1 &
old_host=$!
old_room=$(room_of old.host.txt)
expect old-from-new "no room $old_room on the server" join "$old_room" --signal 127.0.0.1:27220 --direct 1
if "$probe" join "$old_room" --speak 2 --signal 127.0.0.1:27220 --direct 1 --reads 60 > old.join.txt 2>&1 && wait $old_host; then
    echo "old: passed"; else echo "old: FAILED"; cat old.host.txt old.join.txt; failed=1; fi
"$probe" host room --signal 127.0.0.1:27220 --direct 1 --reads 30 > new.host.txt 2>&1 &
new_host=$!
new_room=$(room_of new.host.txt)
expect new-from-old "no room $new_room on the server" join "$new_room" --speak 2 --signal 127.0.0.1:27220 --direct 1
kill $new_host 2>/dev/null
# A public room of an older version (5: rooms without a release) is counted for this one, not listed; this
# one's room (another build) is listed for this one only.
"$probe" host room --speak 5 --public 1 --platform 1 --signal 127.0.0.1:27220 --direct 1 > old5.host.txt 2>&1 &
old5_host=$!
"$probe" host room --public 1 --platform 1 --build 3 --release v0.1.6-netplay.98 --signal 127.0.0.1:27220 --direct 1 > new6.host.txt 2>&1 &
new6_host=$!
room_of old5.host.txt > /dev/null
new6_room=$(room_of new6.host.txt)
listed older-rooms "rooms 1, others 0, older 1" "$new6_room 1/2 direct rollback 0 host [0-9a-f]* release v0.1.6-netplay.98 other-build$"
listed older-lister "rooms 1, others 0, older 0" "[A-Z0-9-]* 1/2 direct rollback 0" --speak 5
grep -q " release " older-lister.txt && { echo "older-lister: FAILED (a newer room's release)"; failed=1; }
kill $old5_host $new6_host 2>/dev/null
# This version on a server that only speaks version 2 (posing as an older one): it speaks that too, and
# plays through a room, without public rooms.
"$bin/netplay_server" --port 27221 --connection-burst 1000 --newest-version 2 > older.server.txt 2>&1 &
older_server=$!
"$probe" host room --public 1 --signal 127.0.0.1:27221 --direct 1 --reads 60 > older.host.txt 2>&1 &
older_host=$!
older_room=$(room_of older.host.txt)
if "$probe" join "$older_room" --signal 127.0.0.1:27221 --direct 1 --reads 60 > older.join.txt 2>&1 && wait $older_host &&
    grep -q "speaking its signalling version 2" older.join.txt; then echo "older-server: passed"; else
    echo "older-server: FAILED"; cat older.host.txt older.join.txt; failed=1; fi
expect older-list "has no public rooms" list --signal 127.0.0.1:27221
kill $older_server 2>/dev/null
# Guessing codes: after 10 that aren't there (one above; a slow machine's may be refilled, one each 6 s),
# the server closes the connection.
i=0
guessed=
while [ $i -lt 20 ] && [ -z "$guessed" ]; do
    "$probe" join "ZZZZ-ZZ$(printf '%02d' $i)" --signal 127.0.0.1:27220 --direct 1 > "ice-guess$i.txt" 2>&1
    grep -q "too many room codes" "ice-guess$i.txt" && guessed=$i
    i=$((i + 1))
done
if [ -n "$guessed" ] && [ "$guessed" -ge 9 ] && grep -q "no room ZZZZ-ZZ00" ice-guess0.txt; then echo "ice-guess: passed (refused at $((guessed + 1)))"; else
    echo "ice-guess: FAILED"; cat ice-guess0.txt "ice-guess$((i - 1)).txt"; failed=1; fi
kill $server 2>/dev/null

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
