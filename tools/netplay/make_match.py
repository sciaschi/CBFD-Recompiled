#!/usr/bin/env python3
"""
Writes a recording that plays from boot into a multiplayer match, for the determinism harness to replay
(host/src/netplay/harness.cpp): the inputs only, the same every time, so a replay always reaches the same
moment of the same match. For trying multiplayer things (netplay's "my view only", split-screen rendering)
without playing through the menus each time. Replay it with --fast, in a window with shot.sh.

    make_match.py <out.pads> [--mode war] [--players 4] [--variant 0] [--pause] [--mouse] [--play-reads 900]
                  [--seed 1] [--own-view] [--view-pairs 12]

From boot: the intro (it can't be skipped), Start on the title, the main menu's MULTI, the mode's barrel in
the cellar, its list's --variant'th entry (War's 1 is Colors, Race's 1 is Race B, Deathmatch's are maps),
every other player pressing Start once (twice takes them out again), then player 1 starting the match and
some 30 seconds of it with nobody moving. --pause pauses it at the end. A Race is started with Start
alone (more presses pause it and quit). --mouse has every player play in those 30 seconds instead (or for
--play-reads reads, 30 a second): move and turn with the stick, hold R to aim at times, and move their
mouse, gyro and scroll wheel (from --seed, as make_inputs.py --mouse does), with settings (Controls) of
their own. --own-view marks it as a match whose rule is that everyone sees only their own view (it changes
the split screen's culls); --view-pairs, with the views of those ports (from 1) shown two to a window, as
on a machine with two players.

The recording's format is the harness's (see its header comment), with its ROM and settings profile hashes
0 (not checked). The timings were measured with the US ROM on the timeline: a change to what the game does
before the match (the boot, the menus) can make them miss.
"""
import argparse
import random
import struct

from make_inputs import NO_MOTION, Movement, header

R = 0x0010
START, A = 0x1000, 0x8000
NO_RESPONSE = 8
# The cellar's barrels, as many presses right from the first (Deathmatch's skull); 4 is the way back.
MODES = {"deathmatch": 0, "war": 1, "tank": 2, "race": 3, "beach": 5, "raptor": 6, "heist": 7}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out")
    parser.add_argument("--mode", choices=MODES, default="deathmatch")
    parser.add_argument("--players", type=int, default=4, choices=range(2, 5))
    parser.add_argument("--variant", type=int, default=0)
    parser.add_argument("--pause", action="store_true")
    parser.add_argument("--mouse", action="store_true")
    parser.add_argument("--play-reads", type=int, default=900)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--own-view", action="store_true")
    parser.add_argument("--view-pairs", default="")
    args = parser.parse_args()
    rng = random.Random(args.seed)
    motions = {}  # read: each port's 13 bytes

    reads = []

    def pads(port=None, buttons=0, stick_x=0, stick_y=0):
        read = bytearray([4])
        for p in range(4):
            mine = p == port
            read += struct.pack("<HbbB", buttons if mine else 0, stick_x if mine else 0, stick_y if mine else 0,
                                0 if p < args.players else NO_RESPONSE)
        return read

    def wait(count):
        reads.extend(pads() for _ in range(count))

    def press(port, buttons=0, stick_x=0, stick_y=0, count=5):
        reads.extend(pads(port, buttons, stick_x, stick_y) for _ in range(count))

    # To the main menu: Start past the attract screen, Start on the title, the menu (GAME1) ready by 1700.
    wait(1140); press(0, START, count=4)
    wait(1522 - len(reads)); press(0, START)
    wait(1700 - len(reads))
    # MULTI, one to the right; A into the cellar once the menu has stopped turning.
    press(0, stick_x=82, count=15); wait(200); press(0, A); wait(160)
    # The mode's barrel, then A for its list.
    for _ in range(MODES[args.mode]):
        press(0, stick_x=80, count=8); wait(40)
    press(0, A); wait(100)
    for _ in range(args.variant):
        press(0, stick_y=-80, count=8); wait(30)
    press(0, A); wait(100)
    # Character select: the others in, then player 1 on into the match.
    for port in range(1, args.players):
        press(port, START); wait(40)
    wait(60)
    starts = (START, A) if args.mode == "race" else (START, A, START, A, A)
    for buttons in starts:
        press(0, buttons); wait(120 if args.mode != "race" else 60)
    # Some 30 seconds in (a Race 10: its bikes move by themselves, so with nobody steering they crash until
    # they run out of lives, and it ends).
    if args.mouse and args.mode != "race":
        moving = [Movement(rng) for _ in range(4)]
        held = [[0, 0, 0, 0] for _ in range(4)]  # buttons, stick x, stick y, reads left
        for _ in range(args.play_reads):
            read = bytearray([4])
            for p in range(4):
                if p >= args.players:
                    read += struct.pack("<HbbB", 0, 0, 0, NO_RESPONSE)
                    continue
                h = held[p]
                if h[3] == 0:
                    h[0] = R if rng.random() < 0.4 else 0
                    h[1] = rng.randint(-70, 70) if rng.random() < 0.5 else 0
                    h[2] = rng.randint(-70, 70) if rng.random() < 0.5 else 0
                    h[3] = rng.randint(20, 90)
                h[3] -= 1
                read += struct.pack("<HbbB", h[0], h[1], h[2], 0)
            motions[len(reads)] = b"".join(moving[p].next() if p < args.players else NO_MOTION for p in range(4))
            reads.append(read)
    else:
        wait(300 if args.mode == "race" else 900)
    if args.pause:
        press(0, START); wait(200)

    with open(args.out, "wb") as f:
        f.write(header(args.players, args.own_view, args.view_pairs))
        for i, read in enumerate(reads):
            f.write(read + motions.get(i, NO_MOTION * 4))
    print(f"{args.out}: {len(reads)} reads, {args.mode} ({args.variant}), {args.players} players")


if __name__ == "__main__":
    main()
