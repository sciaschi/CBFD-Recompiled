# Netplay tools

Work in progress for online multiplayer (issue #126, discussion #137). Lockstep netplay only works if
the same inputs always give the same game on every machine, so the first step is a determinism
harness: record a game's inputs, replay them, and compare a hash of the game's memory read by read.

## The harness

Built only with the CMake option `CONKER_NETPLAY` (off by default). With the build scripts, on a
clean build folder (they keep what it was configured with):

    CONKER_CMAKE_ARGS=-DCONKER_NETPLAY=ON ./build.sh             (Linux, macOS)
    set CONKER_CMAKE_ARGS=-DCONKER_NETPLAY=ON && build.cmd       (Windows)

or by hand, `cmake -S host -B host/build -DCONKER_NETPLAY=ON`. Netplay also needs protobuf and, but on
Windows, OpenSSL (for GameNetworkingSockets):

- **Linux:** `libprotobuf-dev protobuf-compiler libssl-dev` (on Ubuntu); `build.sh` checks for them.
- **macOS:** `brew install protobuf openssl@3`; `build.sh` checks for them and points CMake at OpenSSL.
- **Windows:** nothing to install: protobuf comes from the vcpkg that Visual Studio 2022 (17.6 and later)
  installs with its C++ workload (or `VCPKG_ROOT`'s), as `host/src/netplay/net/vcpkg.json` asks, built
  static so the game needs no more DLLs. The first build takes about 20 minutes longer; vcpkg keeps it,
  so later ones don't.

Its options (see `host/src/netplay/harness.cpp`):

| Option | |
|---|---|
| `--record <file>` | records the pads the game reads, at each controller read |
| `--replay <file>` | gives the game the recorded pads instead (starts the game directly), and quits after the last read |
| `--hash-log <file>` | logs the XXH3-64 of the game's memory at each controller read |
| `--dump-at <n>` | writes the game's memory at read `n` to `<hash-log>.<n>.bin` (can be repeated) |
| `--ignore <addr>:<size>` | leaves a known difference out of the hash (hex, e.g. `800BE6F0:8`; can be repeated) |
| `--stop-after <n>` | quits after read `n` |
| `--ports <n>` | the game sees controllers on ports 1 to `n`, without paks (a replay uses its recording's) |
| `--fast` | runs the timeline as fast as the host can, without waiting for real time (headless) |
| `--jitter <us>` | sleeps at random, up to `us` microseconds, at the game's OS calls and in the graphics thread: a test that the host's speed doesn't change the game |
| `--state-test <s>:<l>` | takes a save state after read `s` and loads it at read `l`, so the game goes back and on again: the hash log then holds the reads after the load, which should match a run without it |
| `--real-time` | runs the game on the time, as normally, instead of the timeline, to compare |
| `--own-view` | records or hashes as a match whose rule is that every player sees only their own view (it widens the split screen's culls, see `host/src/netplay/view.cpp`); the recording says so, and its replay does the same (a replay can't be given it: it goes by its recording's) |
| `--view-pairs <ports>` | ...with those ports' views shown two to a window (e.g. `12`), as on a machine with two players; recorded too |

While any of these are used, the game starts from an empty save of its own (`saves/harness/`, emptied
each run; your own save isn't touched) and sees fixed controllers. Reads are counted from the first,
so read `n` of one run is read `n` of another, and the pads are recorded as the N64 reads them (after
the sticks are converted), which is what netplay will send. The runtime's own host pointers in game
memory (each thread's context in its `OSThread`) are always left out of the hash.

While the harness is used, the game runs on the runtime's virtual timeline (deterministic mode,
`ultramodern/timeline.hpp`, in `recomp/n64modernruntime.patch`): game code takes no virtual time, and its VIs (exactly 60 a second), timers and task
completions are delivered only when the game is idle, so the same inputs give the same game however
fast the host is. Its clock (`osGetCount`, `osGetTime`) is the timeline's, plus about a microsecond
each time it's read. A headless replay with `--fast` takes about 1.8 s for 2,700 reads (90 s of game).
`--real-time` turns it off: then two replays differ at the deadlines of two timers the game sets from
the clock (`--ignore 8003A598:8 --ignore 800BE6F0:8`), and at the order events arrive in.

The sound plays on the timeline too (`host/src/audio_clock.cpp`). The game's audio thread sizes
each buffer by how much is left of the one playing (`osAiGetLength`), and the MP3 player that cutscenes
and lip-sync wait on is paced by it. Normally that comes from the sound device's queue, whose pace
differs from run to run, and headless has none. While the harness is used, each buffer the game hands
over (`osAiSetNextBuffer`) instead ends at a fixed time on the timeline after the one before, as the
N64's audio interface plays them, and the length is what's left of the buffer playing at the
timeline's time, headless and in a window alike. The sound device only plays what it's given: it
waits for about 70 ms of sound before it starts (and again if it runs dry), and drops buffers while it's
over about 270 ms behind. A save state holds the buffers still playing.

A window replay now matches a headless one. Of the session recorded on 2026-10-14, the first 12,000
reads (6.7 minutes: the opening cutscenes and the first chapter) give the same hashes in a window
and headless; in the window the device ran dry once, before the first buffer started, and dropped
nothing. Before the sound went on the timeline, two windows split from each other at about read 360,
and while the renderer still wrote into game memory a window split from headless at read 2.

Runs from the same build share the data folder (the stored ROM, which `--rom` writes, and
`saves/harness/`), so two at once break each other: give each its own with `--data <folder>` (made if
need be; pass `--rom` too the first time). Two `--fast` replays run at once that way give the same
hashes as one run alone.

### The netplay settings profile

While the harness is used, the settings that change the game or what's in its memory are fixed
(`host/src/netplay/profile.cpp`), whatever the menus say, so a recording made in a window replays the
same headless or on another machine:

- **Each player's own:** the mouse and gyro (R-Look, the aiming modes, the orbit camera) and the
  settings that change what they and the stick do there: each response (Smooth or Direct), each
  invert, and Mouse: Turn the Camera (`host/src/netplay/motion.cpp`); Camera: Field of View, which
  widens or narrows that player's camera (`host/src/field_of_view.cpp`: it changes the game's culls and
  aiming, so every machine applies every player's own); and Aiming: Swap Sticks, so a player can move
  while they aim (`host/src/look_aim.cpp`: it turns their character in game memory, so every machine
  applies every player's own, and it counts them as aiming for two reads after their camera last aimed,
  by the game's reads rather than the time between frames). A player's movement is part of
  their input for each controller read, like their pad: every poll's movement (with their sensitivity)
  is added up and taken at the read on the game thread, in fixed point, so every machine gives the game
  the same, and each player's camera takes their own (in a match, every player's camera turns by its
  player's mouse). Their settings (their *Controls*) go with it, every read: one changed in a match
  takes effect at the same read on every machine. The camera code then uses `same_math.hpp`'s trigonometry, as the C libraries'
  differ in the last bit.
- **Off:** Right Stick: Free Camera and the mouse in the bank's leaps: single player's, and the first
  goes by the time between frames.
- **At their defaults:** the Camera tab's others (Invert Turning, Turning Speed, Lock-On), Conker's Detail, Cutscene Aspect Ratio (Expand) and the sound volumes (100%).
- **Widescreen:** the culls and sprites are widened for 16:9 whatever the window (a wider window can
  lose a little at its sides). While the match's rule is that everyone sees only their own view (below),
  each split-screen camera's culls are also made as tall and wide as "my view only" draws it, on every
  machine.
- **Save states** (F5, F7) are off. **Mods:** none, in netplay matches, recordings and replays: a mod
  is native code the other players would have to trust, and a recording doesn't say which were on.
  Netplay won't host or join with one on, and the handshake still carries a hash of each mod on (its
  id, version, file and saved settings, in load order), so the host refuses a player with any ("mods
  on"). Texture packs only change the picture: they're left out, and can differ.
- **The renderer** writes nothing into game memory. RT64's Render to RAM copies each frame's
  colour and depth images back into the game's memory, as the N64's RDP did, from the graphics thread
  whenever it gets to the display list. Headless, with no renderer, nothing is written. So a window
  run differed from a headless one from read 2 (the depth image at `0x800E9D80`), and the copies raced
  the game. RT64 still draws everything on the GPU; only the copy back is off
  (`RT64::setRenderToRAMOff`, `rt64.patch`), so what the game's own code reads back from those images
  is whatever was there before (as headless).

What only changes the picture or the sound outside game memory (the renderer's settings, Overscan
Borders, Show FPS, the reticle, texture packs) and what only changes the pads or the movement (the
controls, the sensitivities) is left to the player: those are what's recorded, and what netplay sends.

### Recordings

A recording (version 4) holds the ROM's XXH3-64 and the profile's hash: a replay with another ROM or
another profile is refused before the game starts. `make_inputs.py` writes 0 for both, which isn't
checked. It holds each player's mouse and gyro movement and Controls beside their pad at each read,
whether the match's rule was that everyone sees only their own view (`--own-view`), and which ports'
views were shown two to a window (`--view-pairs`), which change the split screen's culls. Version 3
recordings (16-bit Controls, no rule) replay as split screen;
version 2 recordings (before the mouse) replay with no movement and the default settings;
version 1 recordings (before the profile) are replayed unchecked.

### The determinism check

After a change, run `tools/netplay/check.sh` (with the game built with `CONKER_NETPLAY=ON`). It makes a
recording of 2,700 made-up reads (`make_inputs.py --seed 1`), replays it headless four ways (twice, with
random sleeps in the host, and through a save state taken and loaded on the way) and compares each
hash log with the first. Each run is stopped once its last read is logged. `--mouse` gives the made-up
recording mouse and gyro movement and settings of its own; `--recording <file>` checks another
recording instead, e.g. a match where everyone moves their mouse and aims (`make_match.py --mouse`). The CI runs it on pushes
that touch what decides determinism (`.github/workflows/netplay.yml`), where the ROM secrets are set,
as for the other builds.

A typical comparison, of two replays of the same recording:

    host/build/ConkerRecomp --headless --replay run.pads --hash-log a.log
    host/build/ConkerRecomp --headless --replay run.pads --hash-log b.log
    tools/netplay/compare_hashes.py a.log b.log

and, at the read where they first differ, the memory itself:

    ... --hash-log a.log --dump-at 900    (and the same for b)
    tools/netplay/compare_hashes.py a.log b.log --dumps a.log.900.bin b.log.900.bin

## The transport

`host/src/netplay/net/` holds what goes over the network, and builds without the game or the ROM
(its `CMakeLists.txt`). The game links it with `CONKER_NETPLAY`, for the lockstep (below).

- **GameNetworkingSockets** (v1.6.1, downloaded while configuring): encrypted UDP connections, direct by
  address, or through its own ICE client. The host listens; the others connect to it, and it passes
  their inputs on. Direct by address, the host may have to forward the port. Through ICE, the host makes
  a room on a signalling server and the others join it by its room code; the peers find each other by id
  there, and the library finds a way: directly through NAT, or through the server's TURN relay. By default
  it's relay-only, so no peer is told another's address. The servers: `tools/netplay/server/` (below).
- **`signal_packets.cpp`, `signalling.cpp`**: signalling's own packets (Register, Registered, Signal, Unreachable,
  Refused, CreateRoom, RoomCreated, FindRoom, RoomFound, NoRoom, Turn: versioned, each field checked,
  fuzzed with the others) and a peer's connection to the signalling server. It keeps rooms (a host's,
  for as long as it stays connected: the whole match) and passes the library's ICE signals between peer
  ids, and never sees the game's packets. `netplay_server` (`signal_server.cpp`) is the server: one
  registration per connection, ids and room codes not reused for an hour, limits per address and per
  second, at most 10 lookups of rooms that aren't there from an address (then one each 6 s), and it logs
  counts only. With `--turn`, it hands each peer TURN credentials of its own that expire
  (`turn_credentials.cpp`: coturn's shared-secret scheme, HMAC-SHA1), so the relay isn't open to anyone.
- **`packets.cpp`**: every packet (Hello, Welcome, Refuse, Start, Inputs, Hash, Bye), fixed-size and
  versioned, each field checked; the only code that reads from the network. `packets_test.cpp` tests
  it and `fuzz_packets.cpp` fuzzes it.
- **`session.cpp`**: the host makes a random 64-bit key for each match (from the system's random
  source), which is in the join code. A joiner's Hello must carry it: a wrong key is closed at once and
  told nothing else, so a stranger who finds the port can't take a place, or learn whether their build
  or ROM would have done. With the right key, the handshake refuses another build, ROM, settings
  profile or mods, a full or started match. A joiner has 3 s to say Hello, and the host takes at most 8
  connections at once. A joiner's Hello says how many players are on its machine, and it's given that many
  of the lowest free ports (the host's players have the first). Then each peer sends only its own ports' pads and memory hashes, reliably (lost
  packets are resent: lockstep needs every input). Each Inputs also goes unreliably with its port's last 8 reads
  (while the connection's send queue is near empty), so one lost packet doesn't hold up the inputs after it until
  it's resent: the reads that came already are cut from whatever comes, and a copy is taken only once every other
  reliable message sent before it (a Delay, a Gone...) has arrived. Inputs and hashes before the start, beyond the reads
  a peer could have reached, or a hash repeated, drop the peer. The library closes a connection that
  sends a message bigger than the largest packet, and buffers little from each peer.
- **`lan.cpp`, `lan_packets.cpp`**: LAN discovery's beacon (27 bytes: the fit hash, platform, port, key,
  players, joined, playing, rollback; every field checked) and its plain UDP sockets.
- **`join_code.cpp`**: the host's address, port and the match's key as a code with a typo check
  (`SC07-21V9-GW0J-6HB7-H6NW-VVTT`), to join directly; and room codes (`7KQ2-M9XD`: a room's random
  number, 40 bits, with no address in it), to join through a server.
- **What players should know:** the join code is the host's IP address and port, so it's for people the
  host trusts, and joining shows the host the joiner's IP address (the menu says so). A room code holds
  no address, and relay-only (the default) nobody learns another's address; with direct connections
  allowed, the players may. The connections
  are encrypted, but not authenticated: there's no certificate authority, so someone on the network path
  between two players could pose as the host. Joining by a code from someone you know keeps that to
  people who can already see your traffic.
- **`netplay_probe`**: two or more copies play a made-up match (`host <port>`, which prints its join
  code for 127.0.0.1, `join <code>`, `--loss <percent>`) and check every pad arrived right; `--hostile`
  joiners must be dropped, `join --silent` checks a joiner that never says Hello is dropped and `join
  --oversize` one that sends too big a message; `code <address:port> <key>` prints a code. Through a
  room: `host room --signal <server:port>` prints its room code, `join <room code> --signal ...`, with
  `--direct 1` to allow direct connections. `test.sh` runs the tests and a few matches (also through a
  room, directly), and checks a closed room, a relay-only host on a server without a relay and guessed
  codes; `tools/netplay/server/test.sh` plays relay-only matches through the servers' Docker setup,
  checks no peer is told an address but the relay's, and that coturn refuses a password the server
  didn't make. `.github/workflows/netplay-transport.yml` runs them on Windows, Linux and macOS (the relay on
  Linux; no ROM needed).

## The servers

`tools/netplay/server/`: the signalling server (rooms) and coturn (the TURN relay) in one `docker compose`
file, for anyone to host. See its README.

## Lockstep

`host/src/netplay/lockstep.cpp`, part of the harness: every peer runs the whole game, and at each
controller read the game waits until it has every player's pad (and mouse and gyro movement) for that
read. A peer sends its own `--input-delay` reads ahead to hide the time it takes to arrive: the host
chooses it for everyone and sends it with Start. By default it's `auto`: from the round trips to the
players as the match starts, enough reads to cover the slowest way an input goes (half of each of the
two slowest round trips, as the host passes joiners' inputs on), less the few ms a read can wait without
the game falling behind. On a local network that's 0, so the others' input reaches the game as soon as
this player's own, which matters for the mouse. Every
30 reads the peers compare hashes of the game's memory, and a difference is a desync (below).
Each peer starts from boot with an empty save, on the timeline and with the settings profile.

**Desync recovery**. When the host finds that a joiner's game went another way, it sets it right
rather than ending the match: it picks a read no joiner has made yet (as it does for a delay change) and
tells everyone (Resync). At the safe point before that read it takes its game's state (as a save state
holds it, with the host's own part), compresses it (zstd, about 0.5 to 1.5 MB) and sends it to that
joiner, faster than the rest while it lasts (1 MB/s). That joiner stops after the read before it until
the state has come, checks it (its hash, its layout, and that no thread's registers could reach past the
game's memory), loads it and plays on; the others play on too, waiting for its inputs meanwhile (a second
or so). Inputs go on as before: nothing else in the match changes. With rollback the host sends the state
once every input before its read is known and right. The host's game is the one that counts: a joiner
that finds a desync waits for the host's Resync. The match still ends as a desync if the same machine's
game goes another way again within 10 s of being set right, if no Resync comes (a host with
`--netplay-no-recovery`), and a joiner that can't load the state leaves the match (its ports go idle).
A report is written for each desync either way. Only the host's state is ever loaded: a joiner takes
larger messages from the host alone, and a malicious host could at worst crash it (the game memory's
allocation has guard pages on both sides, so the game's code can't reach past it, whatever's in memory).

**Coming back**. A player who left a match under way (quit, crashed, or lost the connection; not
kicked) can get back in: they start the game again and join the same room or code, with the same player id
(the menu keeps one; `--netplay-player` on the command line) and as many players on their computer. Anyone
else is refused, as before. The host welcomes them as coming back, and their game starts at once. The host
picks a read past the furthest any input can have reached, tells the others their ports play again from
there (Back: idle pads until then), takes its game's state before it, and sends it to them with the match
as it is there (Rejoin: the input delay, each port's last input, which ports are idle) and the inputs since.
Their game waits for it about a second after it starts, loads it, and plays on from that read; the others
wait for their inputs meanwhile, as for desync recovery. Players coming back at about the same time are
brought back together.

**Joining a match under way**. The host can start before every place is taken (the waiting room's
Start now, or `--netplay-start-with <n>`): the free ports are open, idle from the first read (Start says
which). A new player who joins the room or code then is welcomed on the lowest free ports and brought in as
a player coming back is: Back to the others, the host's game and the match with Rejoin. Free ports are the
open ones, and those of a player who left over 2 minutes ago (`--netplay-seat-hold`) without coming back,
who then can't come back to them. In the game they're a plugged-in controller from then on: they join a
round at the next character select, as on a console. The host keeps the room's listing (and the LAN beacon)
up to date with how many are in, so the room list offers Join on a match being played with free places.
Two players on one computer joining late see their own two views, but aren't counted as a pair for the culls
(Start's pairs can't change mid-match), so the edges of their views may show things a moment late.

**Spectators**. Someone with nobody playing on their computer can watch (the menu's Watch, by a code or on
a listed match; `--netplay-watch` with `--netplay-join`): the host welcomes them on no ports, with a spectator
number of its own (up to 8, besides the players), unless it takes none (the Host tab's Spectators: None;
`--netplay-no-spectators`). A spectator gets every input the players do and sends none, only its hashes; nobody
waits for it, so its game runs a little behind theirs (catching up when it falls well behind) and it never
predicts, whatever the match's rollback. Watching from the start, it gets Start like the players; joining a
match under way, it gets the host's game as a new player does, without the Back (nobody waits for its inputs).
A spectator whose game goes another way is sent the host's game alone (a Resync to no ports, to it only), and
removed if it goes another way again soon after; it never ends the match. The waiting room and the Esc menu's
Netplay tab list the spectators, and the host can kick them. The public room list (from signalling version 7: the
server must be that new) and the LAN beacon (version 2) say how many watch a match, and whether it takes spectators:
Watch is offered only where it does. A spectator sees the whole split screen, or one
player's view across the window, whatever the match's rule for the players: L and R on its first controller (or the
D-pad's left and right) go from everyone's to each player's in turn (view.cpp, lockstep's `watched_ports`).

**Changing places**. Spectators and players can change places, before the match (the waiting room) and in it
(the Esc menu's Netplay tab). A spectator can take a free place (Play as; `--netplay-claim-at`), and the host can seat
one: in a free place, in one held for a player who left (who then can't come back to it), or in a joiner's (a swap:
Seat as, Swap with; `--netplay-seat-at`). A joiner's players (every one on that computer) can step out to watch, of
their own accord (Watch instead; `--netplay-step-out-at`) or as the host asks (Make spectator, or a swap). Before the
start nothing has been played: the host just makes the change, and its roster tells everyone (a joiner finds where it
is in it). In the match, a spectator's game is the match's already, so no state is sent: the host picks a read past
any input sent and tells everyone (Back, naming the spectator's player id), and it plays from there, once its game is
near the host's. A machine stepping out sends no more inputs and says so (Unseat, with all its ports), and the host
tells everyone its ports are idle, and open, from the read after its last (Gone's 2); it watches from then on. A swap
is the two in turn, the place idle a moment between. Spectators take one place each. The view pairs stay as they were at the start (Start's): a
port of a two-player computer that's taken by someone else in the match is culled as a pair's still, so the edges of
its view may show things a moment late (as for a late joiner; it can't desync).

**Desync reports.** The hash a peer sends comes with a hash of each 64 KB region of memory (128 of them,
16 bits each), so a desync names the regions that differ. Each peer whose game differs from another's
writes a report to `netplay-desyncs/` in the game's data folder, and tells the player where:
`desync-<date>-<time>-read<n>-port<p>.json` holds the read, the regions, the build, ROM, settings profile
and match settings, every region's full hash, and each player's last 120 reads of input (pads, movement
and Controls); `.bin` beside it is the game's memory at that read (8 MB, as `--dump-at` writes it). With
the other player's report, `compare_desyncs.py` checks they're comparable (same build, read and inputs)
and names the ranges of memory that differ:

    tools/netplay/compare_desyncs.py desync-...-port1.json desync-...-port2.json

In a match of three or four the joiners only compare with the host, so the reports come from the host
and the player whose game went another way.

    ConkerRecomp --netplay-host 27015 --netplay-address 203.0.113.7 --players 2   (prints the join code)
    ConkerRecomp --netplay-join SC07-21V9-GW0J-6HB7-H6NW-VVTT                    (the host's code)

The game waits at its start for every player. The host has to forward its UDP port if it's behind a
router. Or through a room on a netplay server, with no port to forward (relay-only unless
`--netplay-direct`):

    ConkerRecomp --netplay-host room --netplay-server play.example.org:27100 --players 2   (prints the room code)
    ConkerRecomp --netplay-join 7KQ2-M9XD --netplay-server play.example.org:27100

**More than one player on a machine.** `--local-players <n>` (1 to 3, hosting or joining; the menu's
Players on this computer) puts n players on this machine, each with a port of their own: the lowest free
ones, the host's from port 1 (so `--players 3 --local-players 2` on the host gives it players 1 and 2,
and the one who joins is player 3). Each plays with this machine's controller of the same number: the
frontend numbers the controllers in the order they first press a button, and the first has the keyboard,
mouse and gyro too. The machine's settings that go with each read (its Controls: responses, inverts,
field of view, Swap Sticks) are the same for all its players. With two players on a machine, "my view only" (below) shows both of their views,
one across the top half of the window and one across the bottom, like the game's own 2-player split;
with three, the whole split screen is shown.

In a window, the launcher's **Netplay** menu (`host/src/netplay/menu.cpp`) does the same: host directly (port and
the host's own address, from which it shows the join code, and visible on this network), or through a room on a
netplay server, which shows the room code to send once hosting. There's no server by default: the Settings tab
takes one's address (with Relay only / Allow direct; kept in `netplay.json` in the config folder), and until a
player sets one, rooms, the public room list and Quick Match say so instead (tools/netplay/server/ runs one); players, input delay; or join by either code. It says how the connecting goes and why a peer was refused, and the game starts on every
machine once all are in. When a match started there ends (a desync, the host gone), a prompt says why
instead of the game quitting.

**The waiting room**. Once hosting or in, everyone sees who's there: each port's
player by a name made from their player id (random, kept in `netplay.json`; `--netplay-player <hex>` on
the command line), whether they're ready, their platform and round trip to the host, and the match's
settings (the host's Roster, sent as it changes; joiners say Ready). The match starts once every port is
taken and everyone's ready; the host's Ready is its Start. From the command line every copy is ready by
itself, as before. The host can **kick** a player; anyone can **block** one (kept in `netplay.json`;
`--netplay-block <hex>`): a host refuses them (told only "the host removed this player"), and a joiner
leaves a room they're in (Quick Match tries the next).

The menu finds matches too:
- **Public rooms** (`--netplay-public`): a room hosted Public is in the server's room list, under a name
  made from its number ("Grumpy Squirrel"). The Join tab lists the rooms this game can join (the same
  build, ROM, settings and mods, on platforms that play together), refreshed every 5 s, and how many more
  have other settings or mods.
- **Quick Match** (`--netplay-quick`): joins the first open public room that fits (trying up to 5), or
  hosts one in public with the Host tab's settings. A host still alone that sees an older open room leaves
  its own and joins that, so two players who pressed it at once meet.
- **On this network** (`--netplay-lan`, hosting on a port): the host broadcasts a beacon each second (UDP
  port 27016, and to its own machine), and games on the network list the match and join it with no code
  and no server. Anyone on the network can join it, and any machine on it could pose as the match (it's
  for trusted networks: a home, a LAN party). A firewall on the joiner's machine may need UDP 27016
  open to hear other machines.

**Screens.** Every machine draws the whole split screen (that keeps the games the same), but a window can
show only its player's view, drawn across the whole window ("my view only", `host/src/netplay/view.cpp`),
or its two players' views, across the top and bottom halves (in a 3- or 4-player match the game draws them as
quarters: each is drawn anew in its half, its projection scaled so it isn't stretched, and its HUD moved with
it; the timer and scores the views share stay on the line between them). Replaying a recording,
`--my-view-only --my-view-ports 12` shows it as a machine with players 1 and 2 would (for `shot.sh`).
The host sets the match's rule: by default everyone sees only their own view, so no one can look at the
others' (the Netplay menu's Screens, or `--split-screen` to let each player choose with their menu's Your
screen or `--my-view-only`). The rule reaches every peer with Start. It can't be enforced against a
modified build, which could still show every view: it keeps honest players on the same footing.
A view drawn this way shows more of the world than the game culled it for: a 2-player half across the
window, about 1.6 times as tall (a 232 by 108 view across 292 by 216); a quarter across half the
window, about 1.4 times as wide. So while the rule is on, every machine makes every camera's culls as much taller or wider (`own_view_scale` in
`host/src/widescreen.cpp`). It changes the game's memory, so it goes by what every machine has: the
match's rule, and the ports shown two to a window (each machine with two players'), which the host
sends with Start. Never by a player's own choice: a player who chooses "my view only" in a split-screen
match can still see the world cut off at the edges of their view, as can a replay shown with
`--my-view-ports` whose recording doesn't have those view pairs.

With `--replay`, a peer's own pads come from its port in the recording instead: `lockstep.sh` plays two
copies against each other on this machine (`--data` gives each its own folder) and checks that each
gets exactly the game a replay alone does (each copy's recording has the other's port scrambled, so it
only does if the pads came over the network), with input delay 0 and `--jitter` on one side too; a
3-player Deathmatch where everyone moves their mouse and aims, at the automatic delay (through the
host's relay); the same match with two players on one copy (the host's, then a joiner's); a joiner leaving partway, quitting or killed, and two
players on one joiner leaving together (the others must match a solo replay of the recording with those
ports idle from the read the host says); players coming back (once, twice, two machines at once, and two
players on one joiner: everyone must match a solo replay with those ports idle while they were away, and
the one back must from the read it came back at; another player is refused); and that a
desync (`--netplay-desync-at <read>`, which changes a byte in region 64) is set right by the host's game
(a joiner's, in the 3-player match: the others match the solo replay, it does from the read it loaded at),
and with `--netplay-no-recovery` is caught, with reports that name the region and compare to that byte.
About 100 s.

**Bad networks.** `--netplay-fake-network <spec>` has the library make this peer's network worse: `lag=<ms>`,
`jitter=<ms>`, `loss=<%>`, `reorder=<%>` and `dup=<%>` on what it sends, `outage=<read>:<s>` (nothing sent or
received for that long from that read) and `lag-at=<read>:<ms>` (the lag changes), separated by commas;
`--netplay-stall-at <read>:<ms>` stops the game at a read for that long, as a hitch would (again for more). As a match ends,
each peer prints how many of its reads waited for the others' input (the time in all and the longest), and
how many came over 50 ms after the one before: what a player feels, as a read that waits where the game
would have slept until its time anyway costs nothing. `network.sh` plays matches at once on real time (as a
player's game, not `--fast`): on loopback as it is, with 100 ms lag, jitter, 5% loss, reordering and
duplicates, with a 3 s outage, with the lag going from 20 to 150 ms partway and from 150 to 20, and with a
1.5 s stall. Each must give the solo replay's game, the delay must go up and down with the lag (both copies
switching at the same read), and the stalled copy must catch up; the waits are printed, and
`--record <file>` keeps them, a line per copy with the date and commit, to compare over time. About 70 s.

**A player leaving.** When a joiner's machine leaves (saying goodbye, or not answering for about 10 s: a
crash, a lost connection), the others play on. The host decides from which read its ports get no input: the
read after the last of its inputs the host had, as every joiner's inputs reach the others only through the
host, in order, so nobody can be past it. It tells the joiners still in (`Gone`, sent after those inputs), and
from that read every machine gives those ports an idle pad: nothing pressed, nothing moved, the player's last
Controls. Several players on the leaving machine leave together. The host leaving still ends the match (it's
the hub). `Gone` says whether they said goodbye or stopped answering, so every machine can say which on
screen; the log says who left and from when.

**Each player's own delay, following their connection** (#18). Each port has its own input delay (a
machine's ports share one): every peer sends its own pads its own delay ahead, and reads each other port's
by that port's. With `auto` the host chooses each machine's, so one player's bad link doesn't raise everyone's.
Each machine times every input that comes against when it made the read the input was sent at (by the
library's receive time: the game thread only polls at its reads), against a rolling one-second median (a trip
that changes for good isn't unevenness): how much later the latest tenth came is that port's spread, sent to
the host with its hashes, beside how many reads it waited for each port (and how many of those waited over
8 ms). The host goes by the receiver that
has a port the most evenly and waited for it the least (a receiver with a bad link gets everyone's late). It
checks every 30 reads. Without rollback every pad must arrive in time along the slowest way one goes, so the
round trips' delay is everyone's, plus what the machine's spread needs past a read; it raises a machine's
after 2 checks in a row wanting more (or 3 reads late from waiting for it since the last check, or half the
reads waiting over 8 ms for it: two machines half a frame out of step, as on a LAN at delay 0, keep 30 frames
a second but show them unevenly, which frame interpolation makes judder; then one read more, up to 2 more than
the trips need), and lowers it only after 10 with 12 ms to spare and none waiting for it.
With rollback, a machine's delay is for its one-way trip to the host (the host's own: to the nearest
joiner), less the 3 reads predicting covers (at least 1 unless a LAN's trip needs none), plus what its spread
needs past a read: the others' predicting covers the rest. It's chosen at the start from the round trips,
then follows the median of the last 30 checks a read at a time, at most every 300 reads: up as that wants,
or for reads waiting for it, and down only after 90 checks in a row with nothing waiting for it. A LAN's is 0
from start to end. Not within 90 (300) reads of a machine's last change either, so it doesn't go back and
forth. The host tells the joiners `Delay`: the delay of these ports becomes D from read R, where R is past
any read a joiner can have sent its pads at before it hears (a joiner is at most the delay ahead of the host,
and the Delay comes after the host's pads for those reads). Every peer switches at R: from there that machine
sends its pads up to R + D. Raised, the reads in between get the pad its controllers give then too (held;
the mouse and gyro movement goes with the first); lowered, its pads for the reads up to R - 1 + the old delay
are already sent, and the live ones for those reads aren't sent (a button pressed and let go within them is
lost). The pads are sent either way, so every machine gives the game the same. A fixed `--input-delay` is
everyone's, and stays.

**A player whose inputs stop**. A player whose inputs stop coming (a dropout) holds the others up only
for a while. Once the host has waited for a machine's inputs 5 s (reads waiting for it a quarter second or
more, one after another), it lets go of it: `Gone` (its 3) tells everyone, the machine too, that its ports are
idle from the read after the last of its inputs, as for a player who left, and the others play on ("Player 2
lost connection: the others play on."). A stall that ends sooner lets go of nobody. A machine that had a stall
of 2.5 s or more within the last 3 minutes is let go of after 2.5 s (its connection keeps failing); short
waits, often, are for its delay to cover instead. The machine stays connected: once the `Gone` reaches it, it
asks for the host's game (`Claim` of its first port) and comes back with it as a player coming back does
(`Rejoin` and the state): the host brings it in from a read past any input sent ("Player 2 is back"). Ports
getting a state, or whose first inputs since coming back haven't come, are waited for as ever. If its
connection doesn't come back, the library drops it after about 10 s, and it's a player who left.

**Catching up.** A peer that falls behind (a hitch: loading, the system) lets the others run on until they
need its input, up to the input delay ahead; from then on they'd wait on it at every read, the delay used
up. So when the others' inputs reach 2 or more reads further than they would if the others were at this
peer's read, it runs the timeline faster than real time (`ultramodern::timeline::set_catching_up`, as
`--fast`) until they no longer do, then goes back to real time, paced from where the game is then (from
where it was before, it would sleep the time it gained away and fall behind again). The sound queued meanwhile is capped as
always (`audio_output.cpp`), and nothing else needs to change: the game is the same at any speed.

**On screen.** What the match is doing is shown over the game (`host/src/netplay/status.cpp`, a recompui
overlay like the notices, never in game memory): at the top, "Waiting for Player 2... 4 s" once a read has
waited a second for the others, "Catching up..." once that's gone on half a second, and for a few seconds
each, a player leaving ("Player 3 left.", or "stopped answering (they may have quit)", or "lost connection:
the others play on"), coming back, and the host playing on when everyone else has left. With Netplay Details
on (the in-game menu's Netplay tab), a corner shows the input delay, how many frames this machine is ahead of the others (or behind),
and each player's round trip (a joiner knows only the host's: the others' inputs come through it) and input
delay. Lockstep keeps what's shown (`lockstep::status`, `take_news`)
under a lock of its own, so the overlay reads it while the game thread waits at a read. While the game waits,
the timeline gives no VI, and RT64 only presents a frame when the game makes one, so nothing drawn over the
game (the overlay, the menus) would change: once no VI has come for 100 ms, the runtime's VI thread asks the
renderer to present the last frame again every 50 ms (`RendererContext::present_again`, RT64's
`Application::presentAgain`: the last present repeated, paused, as its debugger's pause does, without
drawing anything of the game's again).

**Rollback (under way).** With the host's `--rollback <reads>` (1 to 12; in the menu, the Host
tab's Rollback), no peer waits at a read for the others' inputs: each predicts each player's (their last
input it has) up to that many reads past the last read it has every input for, and plays on. When an input
comes that isn't what it predicted, the harness loads the state it took before that read (it takes one
before every read, at the first safe point) and the game plays on again from there, with the inputs as
they're known now; the reads made again send nothing and wait for nothing. A peer sends its hash of a read
only once every input before it is known and was given as it came, so peers compare the game as it really
went, and the hash log does the same. It's the match's, the same for everyone: the host sends it with
`Start` (a joiner's own `--rollback` is refused). The boot's first 30 reads aren't predicted (the game
changes its main loop there), and near a replay's end every input is waited for, so it ends right. The
host's `Delay` comes the rollback window further ahead, as a joiner that predicts can be that much further on. The
reads played again are replayed (the runtime's `timeline::set_replay_until`, up to the time the game had
reached): they don't wait for real time and the load doesn't move the pacing, so the game is back where it
was on time; their graphics tasks aren't drawn, their VIs show no frame and do no host work (rumble), and
their sound isn't played (it still counts for the game's audio timing). Time sync keeps the machines level:
each sends, with its inputs, how many reads its game is past each player's latest input it has (its gap,
measured once a read's inputs are in); half the difference between two machines' gaps to each other is how
far one is ahead, the latency cancelling. Averaged over about a second, the machine more than a tenth of a
read ahead is paced up to 2 ms later a read (`timeline::pace_later`) till they're level: else the one a
little ahead predicts nearly every read and the other none (the inputs come with only a few milliseconds to
spare). One player can't drag everyone down: the host goes by a port no more once it has paced
everyone later for it over 300 ms in about the last half minute (about 1%), till that's half again; a player
kept behind for good (a slow computer) catches up on its own. `--rollback-test <every>:<back>` and `--rollback-late <late>[:<seed>]` measure and test going back
alone (harness.cpp). `rollback.sh` plays rollback matches over a bad network, and `lockstep.sh
--host-options "--rollback 8" --match-options "--netplay-fake-network lag=40"` plays every lockstep match
so.

## The scripts

| Script | |
|---|---|
| `check.sh` | the determinism check: replays a made-up recording headless twice, with `--jitter` and with `--state-test`, and fails unless all four give the same hashes (about 30 s; the CI runs it, `.github/workflows/netplay.yml`) |
| `lockstep.sh` | the lockstep check: copies in a match over loopback give the same game as a replay alone (2 players, 3 with the mouse, and 3 with two on one copy), players leaving partway, through a room on a local netplay server (public: the server lists it), Quick Match (joining a public host; two at once meeting in one room), block lists (a host keeping a player out, a joiner leaving, Quick Match passing over a blocked host's room), a kick during a match, a joiner's desync set right by the host's game, players coming back and new players joining a match under way (an open port, a seat someone left), and a desync is caught and reported, by the players and, through a room, in the server's log (a few minutes); `--match-options` gives every copy in a match more options, `--host-options` every hosting one (e.g. `--rollback 8`) |
| `network.sh` | the bad-network check: matches with lag, jitter, loss, an outage and a change of lag give the same game as a replay alone, and how long each waited (about 70 s) |
| `spectators.sh` | the spectators check: a spectator watching from the start, joining a match under way, set right after a desync, in a rollback match and leaving partway sees the players' game (the same as a replay alone) and never holds them up, a host that takes none refuses one, spectators and players change places (taking an open place, a swap, a place someone left, a player or two on one computer stepping out, before the start too), and the server's room list says how many watch (about 2 minutes) |
| `rollback.sh` | the rollback check: matches where the copies predict the others' inputs (the host's `--rollback`), over a bad network, with a change of delay, a stall and a player leaving, give the same game as a replay alone; then a match on real time must keep to time, with time sync keeping both copies predicting (about 90 s) |
| `rollback-modes.sh` | rollback across the game's own changes: every multiplayer mode (4 players, a pause, a Race's match end) replayed going back every 3 reads, 12 deep, and with other machines' inputs up to 8 reads late, must give the plain replay's game; `--recording` adds a real one (about 90 s) |
| `bad-link.sh` | the bad link check, on real time with rollback 12: a LAN keeps every delay 0, a joiner whose inputs arrive unevenly gets a higher delay than the host, a 3 s outage lets go of nobody, an 8 s one is let go of after 5 s and comes back by itself, a second long stall is let go of after 2.5 s, time sync stops slowing everyone for a slow joiner, and in a 3-player match the bad one's delay is the highest, the others wait at most 5 s and play on; each giving the same game as a replay alone (with the port idle while it was out) (about 5 minutes) |
| `try-network.sh` | netplay over a faked bad network on this one computer, in two windows: `watch` (a made-up 2-player Deathmatch, each window one player's, with `--net`, `--rollback`, `--delay`) or `play` (both at the launcher: host, join, play with the keyboard); prints each side's rollback, time sync and waits |
| `lib.sh` | what `lockstep.sh`, `network.sh`, `rollback.sh`, `spectators.sh` and `bad-link.sh` share: starting copies, waiting for them, comparing their hash logs, a recording with players idle from a read |
| `compare_hashes.py` | the first read two hash logs differ at (it exits 1 if they differ); with dumps, the ranges that differ, by data symbol |
| `compare_desyncs.py` | two peers' desync reports: whether they're comparable, the regions and (with their memory) the ranges that differ, by data symbol |
| `make_match.py` | a recording that plays from boot into a multiplayer match (`--mode`, `--players`, `--variant`, `--pause`; `--mouse` has everyone move, aim and use their mouse and gyro), the same every time: for looking at multiplayer (split screen, "my view only") without the menus |
| `shot.sh` | replays a recording in a window as fast as it goes and saves a screenshot of its end (X11; `SDL_VIDEODRIVER=x11` under Wayland), e.g. `make_match.py heist.pads --mode heist && shot.sh heist.pads heist.png --my-view-only` |
| `xwin.py` | drives a game window from a script: screenshots, clicks, keys (X11, python-xlib) |
| `make_inputs.py` | a recording of made-up inputs from a seed, to replay without anyone playing |
| `scan_clock.py` | every read of the game's clock in its code, and whether it's in a loop (needs your ROM, unpacked with `recomp/unpack_rom.py`) |
| `dis_around.py` | the instructions around a function's clock reads |

