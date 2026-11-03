# Netplay's servers

Players who play through a room (rather than by a join code with the host's address) need two servers
(and a third container sets up the machine's network for the relay):

- **The signalling server** (`netplay_server`, from `host/src/netplay/net/signal_server.cpp`): players
  register a random id with it. A host makes a room there and gets a room code (`7KQ2-M9XD`, no address
  in it) to send the others; they look the code up to find the host. The room lasts as long as the host
  stays connected. A host may make its room public: the server lists it for players whose
  game is the same (build, ROM, settings and mods, by one hash it only compares) on platforms that play
  together (Linux with Windows; macOS apart), under a name the game makes from the room's number, so
  nothing a player typed is shown. The server passes GameNetworkingSockets' ICE signals between ids so that two players
  can find a way to each other, and hands each player TURN credentials of their own. It never sees the
  game's packets. It uses little bandwidth.
- **coturn**, the TURN relay: when the players' networks don't let them connect directly, or when they
  choose relay-only (the default, so nobody sees anybody's address), their packets go through it.
  Netplay only sends inputs, so a whole match uses a few tens of KB/s.

Anyone can run them. The game takes the server's address (`host:27100`) as a setting: the Netplay menu's
Server, or `--netplay-server`.

## Running them

On a Linux machine with Docker (rootful: not rootless, nor with user namespace remapping) and nftables,
with a public IPv4 address (on the machine, as on most VPSes, or forwarded to it by a router: below),
you need only `compose.yaml` and `.env.example`
from this folder: the server's image is published to GitHub's container registry
(`ghcr.io/sciaschi/conker-netplay-server`, for amd64 and arm64) by the `netplay-server` workflow. (A
new package on GHCR is private: after its first publish, its owner makes it public once, in the
package's settings, or `docker compose pull` is refused.)

```sh
mkdir conker-netplay && cd conker-netplay
curl -fsSLO https://raw.githubusercontent.com/sciaschi/CBFD-Recompiled/main/tools/netplay/server/compose.yaml
curl -fsSL https://raw.githubusercontent.com/sciaschi/CBFD-Recompiled/main/tools/netplay/server/.env.example -o .env
# A random TURN_SECRET, written into .env:
sed -i "s/^TURN_SECRET=.*/TURN_SECRET=$(openssl rand -hex 24)/" .env
nano .env                 # set PUBLIC_HOST, e.g. conker.example.org (or PUBLIC_IP)
docker compose up -d
docker compose logs -f    # each says "public address <ip>": check it's this server's
```

**Updating:** the fixes are in `compose.yaml` as well as the image, and each needs the other as of the
same commit: download `compose.yaml` again (the same `curl` line), check `.env.example` for new settings,
then `docker compose pull && docker compose up -d`, and check `docker compose logs net` says `ready`.
Pull first: an older image with a newer `compose.yaml` may not know an option it now passes (`--state`), and
the signalling server then stops at once, again and again (`docker compose logs signal` says `usage`).

**Stopping:** `docker compose down`, which keeps the status's numbers (in the `data` volume); `docker
compose down -v` deletes them too. `net`'s guard stays (a firewall rule that only affects coturn's
user); to remove it too, `sudo nft delete table ip conker_netplay_guard`. If `net` was killed rather than
stopped, also `sudo nft delete table ip conker_netplay_nat` and `sudo ip addr del <public IP>/32 dev lo`.

`TURN_SECRET` is 48 random hex characters (any 16 or more letters and digits will do). Only coturn and the
signalling server use it; players never see it, and nobody needs it to play. Without `openssl`:
`head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n'`. Changing it (then `docker compose up -d`) stops
every relay credential given out so far.

**The server's address.** Players connect by name (`PUBLIC_HOST`, e.g. `conker.example.org`; the game's
Server setting is `conker.example.org:27100`), but the relay has to tell them its IP address: the TURN
protocol carries an address, not a name. So each container looks `PUBLIC_HOST` up as it starts, unless
`PUBLIC_IP` is set, which it uses instead (for a name that doesn't point here, or none). If the address
changes (a home connection), update the DNS record, then `docker compose restart`. Behind Cloudflare, the
record must be **DNS only** (grey cloud): its proxy carries web traffic, not UDP.

To build the server from a clone of this repository instead:
`docker compose -f compose.yaml -f compose.build.yaml up -d --build`.

Open in the firewall (all UDP: the game uses TURN over UDP only, and coturn relays nothing over TCP):

| Port | What |
|---|---|
| UDP 27100 | signalling (`SIGNAL_PORT`) |
| UDP 3478 | TURN: players get their relays here |
| UDP 49160-49659 | the relays themselves (one per player and network interface while they play) |

All use the host's network, as coturn's relay ports are a range that Docker's port mapping handles
badly. Relays are IPv4 only.

**`net`**, the third container (the server's image, as root with network-admin rights only), sets up
this machine's network for the relay, in nftables tables of its own; coturn starts once it's ready, and
restarts with it:
- **A guard** (`conker_netplay_guard`), always: coturn may relay to the public address (this machine's)
  only on the relay ports, i.e. to other players' relays, never to its other services. (Its other
  addresses are private ones, which coturn denies; a second public address on the machine isn't
  guarded.) It stays when `net` stops, so it never lapses while coturn runs. So nobody can play on the
  server machine itself.
- **Behind NAT** (at home behind a router, or a cloud machine whose public address isn't on its own
  network interface, as on AWS or Google Cloud), which it notices itself: every relay-only match has both
  players' relays on this server, and relay to relay goes to the public address; with coturn's usual
  setup it would go between LAN addresses instead, and the players wouldn't recognise each other's
  traffic. So `net` puts the public address on this machine (on `lo`) for the relays, and translates
  (`conker_netplay_nat`) between it and the machine's LAN address (the one its default route goes from,
  or `HOME_LAN_IP` in `.env`) for traffic through the router. Its log says `behind NAT: public address
  <public> ... to and from <LAN address> (on <interface>)`: check that's the address your router
  forwards to, or set `HOME_LAN_IP` and `docker compose up -d`. A VPN as the machine's default route
  isn't supported (the relays' replies would go out through it). While it runs, this machine reaches its
  public address on itself, not through the router (say, a port forward of it to another machine).

`net` needs a kernel with nf_tables (most Linux distributions; some NAS systems lack it, and then `net`
says `couldn't set up nftables` and coturn doesn't start). Host firewalls (firewalld, ufw) must let the
ports above in; after the translation, they arrive for the public address, not the LAN one. The relay
ports are in Linux's range for other programs' outgoing ports too: to keep them for the relay,
`sudo sysctl -w net.ipv4.ip_local_reserved_ports=49160-49659` (and in `/etc/sysctl.d/` to keep it).

**Hosting behind a router** (at home): forward the ports above to the server machine, at the same
numbers (don't remap them), and give it a fixed address on your network (a DHCP reservation); `net` does
the rest.

`PUBLIC_HOST`'s record points at your public IP; if your provider changes it, update the record (a
dynamic DNS updater can) and `docker compose restart`. Playing from the same network as the server reaches
it through the public IP from inside, which needs the router's NAT loopback (hairpinning): the relay
is always given out at the public address.

`docker compose down` stops them.

## Versions

A game update doesn't need a new server: the server only passes signals on, keeps rooms and compares
hashes it doesn't read, so it works for any build of the game. Only a change to signalling itself (a
new kind of packet, for something new the game needs from the server) makes a new signalling version,
and then the server still speaks the older ones (from version 2), each to the games that speak it, and
keeps them apart: a game finds rooms of its own version only. A game newer than its server speaks the
server's version instead, and goes without what's newer (public rooms need version 3: on an older
server, rooms by code still work; desync summaries need version 4, and hiding blocked players' rooms
version 5, which adds each listed room's host player id). The image is only rebuilt when the
server's code changes.

## Desyncs

When a match through a room desyncs (the players' games went different ways), each player whose game
found it sends the server a short summary, which it logs: at most 4 from each connection, 4 from each
address (then one each 5 minutes) and 30 in all (then one each 30 s), so the log can't be filled; the rest
are only counted:

    netplay_server: desync: match 1a2b3c4d, build 5e6f..., Windows, 2 players, port 2 against port 1, read 4512, regions 64,65, rollback 8, delay 2

`match` tells one match's summaries from another's (made from its key, which it can't be turned back
into); `build` is the game's build hash (the release); the regions are the 64 KB parts of game memory
that differed. No addresses, memory or inputs: the full reports stay on the players' machines
(`netplay-desyncs/` in the config folder), for them to send when they report the problem. The 10-minute
count line says how many came in, and how many weren't logged. Matches by join code or on a LAN don't go through the server, so they
send nothing.

## The status page

Set `STATUS_PORT` in `.env` (e.g. `27180`) and `docker compose up -d`, and the signalling server serves a
status on that TCP port, for you to open in a browser: `http://<server>:27180/`. It refreshes itself every
30 s; `/status.json` has the same as JSON (for a script or a dashboard) and `/status.txt` as plain text.

- **Now:** players online, rooms (private, public: being played or waiting, and how many watch them),
  public rooms by the release they say, and the relays coturn has open (read from coturn's own metrics,
  every 15 s, which it serves on 127.0.0.1 only, at `TURN_METRICS_PORT`, 9641 by default).
- **Today** (UTC): players (each time one goes online), the most at once, rooms made and joined, desyncs.
- **Since** the date it began counting: rooms made, found and codes not found, signals, room lists, relay
  credentials, desyncs (and how many weren't logged).
- **The latest desyncs:** the last 20 summaries logged, as in the log (below).

It never shows an address or a room code, and there's nothing to log in with: it's for your own network.
It listens on all this machine's addresses (`STATUS_ADDRESS`, `0.0.0.0` by default): at home, behind a router
that forwards only the UDP ports above, only your network reaches it. **On a VPS**, where the machine's
address is public, set `STATUS_ADDRESS` to a private one (a VPN's, say), or keep the port closed in its
firewall. Without `STATUS_PORT` it's off.

**Kept across restarts:** today's numbers, the counts and the latest desyncs (no address nor room code) are
written to `/var/lib/conker-netplay/state` in the signal container every minute they change and as it stops,
and read as it starts. That's the `data` volume by default; to keep them in a folder of your own instead,
set `DATA_DIR` in `.env` to it and let the container's user (65534) write there: `sudo mkdir -p /srv/conker
&& sudo chown 65534:65534 /srv/conker`. (A path needs a `/` in it: `./conker` for a folder beside `compose.yaml`; a bare
name is taken as a volume's.) If it can't write, it says so in its log and runs on (the numbers then
start again at a restart). To start counting afresh: `docker compose stop signal`, delete the file
(`state`), `docker compose start signal`. The 10-minute log line has the same headline numbers either way.

## What's limited

- **Signalling:** a peer must register within 5 s, with an id nobody else has (nor had in the last
  hour). At most `MAX_PER_ADDRESS` connections at once from one IP address (8; an IPv6 address's /64
  counts as one), 20 new ones in a burst then one each 2 s, 4096 in all, and 20 packets a second each
  (bursts of 60). A connection that neither hosts a room nor found one that's still open (a player in a match), and is idle for 10 minutes, is closed, and any after 12 hours. One room each. From one IP address, at most 10 lookups of room codes that
  aren't there, then one each 6 s, so codes can't be guessed (a code is one of a trillion, and isn't
  given out again for an hour after its room closes). At most 6 room lists in a burst, then one each 3 s,
  of 32 rooms at most; at most 2 public rooms from one address at once (more stay private). Anything else closes the connection. The server
  logs counts, never addresses or codes.
- **coturn:** only the signalling server's credentials; UDP only (no TCP listener, and no TCP relay,
  which could open connections anywhere); IPv4 relays only; at most 200 KB/s per relay, `TURN_USER_QUOTA` relays per player (32: one per
  network interface and other player) and `TURN_TOTAL_QUOTA` in all (400); and never into private,
  loopback, link-local (cloud metadata), multicast, documentation or IPv6-to-IPv4 ranges, so it can't be
  used to reach the server's own network; to the server itself, only on the relay ports (`net`'s
  guard). Its secret is in a file only it can read, not on its command line.

## The relay's credentials

coturn takes no fixed user: it checks credentials made from `TURN_SECRET`, which only it and the
signalling server know (coturn's `use-auth-secret`). The signalling server gives each player a user
`<expiry>:<id>` and its password (HMAC-SHA1 of the user, base64) as they register; they work for
`TURN_HOURS` (12 by default), and coturn refuses them after. So only players of this server can use the
relay, and only for a while. Changing the secret stops every credential given out so far.

## Testing

`test.sh` starts these servers on this machine, built from here, twice: behind NAT (a made-up public
address, which `net` puts on this machine, as it would a router's) and as on a VPS (that address already
on this machine). It plays relay-only matches through a room with `netplay_probe` (with the credentials
the server hands out) and checks that no player is told an address but the relay's; that coturn refuses
a password the server didn't make, and denies private addresses, the machine's LAN one and cloud
metadata; `net`'s guard and its translation (a packet from a container on Docker's bridge); and that the
address is gone once they stop:

```sh
cmake -S host/src/netplay/net -B build-net -G Ninja && cmake --build build-net
sh tools/netplay/server/test.sh build-net /tmp/relay-test
```

It needs Linux with rootful Docker, nftables, python3 and openssl. It uses the project name
`conker-netplay-test` but the same ports (27100, 3478, 49160-49659), adds and removes `5.5.5.5` on `lo`
and replaces, then removes, the `conker_netplay_*` nftables tables, so don't run it on a machine that is hosting.
