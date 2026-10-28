#!/usr/bin/env python3
"""
Compares two runs of the determinism harness (host/src/netplay/harness.cpp): their hash logs, read by
read, and, given the game's memory from each at the same read (--dump-at), where it differs.

    compare_hashes.py <run a.log> <run b.log>
    compare_hashes.py <run a.log> <run b.log> --dumps <a.log.N.bin> <b.log.N.bin> [--max-ranges 40]

The first prints how many reads match before the runs part (the read whose hash first differs is the
first one whose memory is different, so it's the reads before it that did something differently).
Rerun both with --dump-at <that read> to see the memory. The ranges that differ are named by the
nearest data symbol below them (mods/syms/conker.us.datasyms.toml); memory past the game's own data
(its heap) has none.
"""
import argparse
import bisect
import os
import sys
import tomllib

BASE = 0x80000000
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATASYMS = os.path.join(REPO, "mods", "syms", "conker.us.datasyms.toml")
NEAR = 0x1000  # a symbol further below a range than this doesn't name it


def read_log(path):
    hashes = []
    with open(path) as f:
        for number, line in enumerate(f):
            parts = line.split()
            if len(parts) != 2 or int(parts[0]) != number:
                sys.exit(f"{path}: line {number + 1} isn't read {number}'s hash")
            hashes.append(parts[1])
    return hashes


def load_symbols(path):
    with open(path, "rb") as f:
        syms = tomllib.load(f)
    found = sorted({(s["vram"], s["name"]) for sec in syms["section"] for s in sec.get("symbols", [])
                    if s["vram"] >= BASE})
    return [a for a, _ in found], [n for _, n in found]


def name_of(address, addresses, names):
    i = bisect.bisect_right(addresses, address) - 1
    if i < 0 or address - addresses[i] > NEAR:
        return "(no symbol within 4KB)"
    offset = address - addresses[i]
    return names[i] + (f"+0x{offset:X}" if offset else "")


def differing_ranges(a, b, gap=16):
    """The ranges where a and b differ, merging those less than `gap` bytes apart."""
    ranges = []
    start = end = None
    i, n = 0, min(len(a), len(b))
    step = 4096
    while i < n:
        if a[i:i + step] == b[i:i + step]:
            i += step
            continue
        for j in range(i, min(i + step, n)):
            if a[j] != b[j]:
                if start is not None and j - end <= gap:
                    end = j + 1
                else:
                    if start is not None:
                        ranges.append((start, end))
                    start, end = j, j + 1
        i += step
    if start is not None:
        ranges.append((start, end))
    return ranges


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log_a")
    parser.add_argument("log_b")
    parser.add_argument("--dumps", nargs=2, metavar=("A", "B"))
    parser.add_argument("--max-ranges", type=int, default=40)
    args = parser.parse_args()

    a, b = read_log(args.log_a), read_log(args.log_b)
    common = min(len(a), len(b))
    first = next((i for i in range(common) if a[i] != b[i]), None)
    if first is None:
        print(f"identical for all {common} reads both logs have" +
              (f" (the logs have {len(a)} and {len(b)})" if len(a) != len(b) else ""))
    else:
        matching = f"reads 0 to {first - 1} match" if first else "no read matches"
        print(f"first difference at read {first}: {matching} ({first} of {common})")
        later = sum(1 for i in range(first, common) if a[i] == b[i])
        if later:
            print(f"  {later} later reads match again")

    if args.dumps:
        da, db = (open(p, "rb").read() for p in args.dumps)
        if len(da) != len(db):
            sys.exit("the dumps are different sizes")
        addresses, names = load_symbols(DATASYMS)
        ranges = differing_ranges(da, db)
        total = sum(e - s for s, e in ranges)
        print(f"\n{len(ranges)} ranges differ, {total} bytes in all")
        for s, e in ranges[:args.max_ranges]:
            print(f"  0x{BASE + s:08X}-0x{BASE + e - 1:08X} ({e - s:6} bytes)  {name_of(BASE + s, addresses, names)}")
        if len(ranges) > args.max_ranges:
            print(f"  ... and {len(ranges) - args.max_ranges} more (--max-ranges)")

    # As cmp does: 1 if the runs differ (or one is longer), for scripts (check.sh).
    return 0 if first is None and len(a) == len(b) else 1


if __name__ == "__main__":
    sys.exit(main())
