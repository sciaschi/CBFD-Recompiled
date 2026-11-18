#!/usr/bin/env python3
"""
Compares two peers' desync reports (lockstep.hpp): when a netplay match ends in a desync, each peer whose
game differs from another's writes desync-<date>-<time>-read<n>-port<p>.json to netplay-desyncs/ in its
data folder, with its game memory at that read beside it (.bin).

    compare_desyncs.py <report a.json> <report b.json> [--max-ranges 40]
    compare_desyncs.py <report.json>

With two, it checks they're from the same build, settings and read, and the same inputs, then lists the
regions of memory that differ (from the reports' region hashes) and, with both memory files, the ranges
that differ in them, named by the nearest data symbol below (as compare_hashes.py does). With one, it
says what that report holds: the regions its peer found differing, by the 16 bits of each region's hash
the other peer sent.
"""
import argparse
import json
import os
import sys

from compare_hashes import BASE, DATASYMS, differing_ranges, load_symbols, name_of

INPUT_FIELDS = ["buttons", "stick x", "stick y", "mouse x", "mouse y", "gyro x", "gyro y", "wheel", "Controls"]


def load(path):
    with open(path) as f:
        report = json.load(f)
    if report.get("report") != "Conker netplay desync" or report.get("version") != 1:
        sys.exit(f"{path}: not a version 1 desync report")
    report["path"] = path
    return report


def addresses(region, size):
    start = BASE + region * size
    return f"0x{start:08X}-0x{start + size - 1:08X}"


def ports(report):
    return ", ".join(str(p) for p in report["ports"])


def summary(r):
    print(f"{r['path']}: port(s) {ports(r)}, read {r['read']}, compared with port {r['compared_with_port']}")
    print(f"  build {r['build_id']}, {r['players']} players, input delay {r['input_delay']}")
    left = [f"port {p + 1} from read {at}" for p, at in enumerate(r.get("idle_from", [])) if at is not None]
    if left:
        print(f"  players that left, idle: {', '.join(left)}")
    regions = r["differing_regions"]
    if regions:
        print(f"  regions that differ from port {r['compared_with_port']}'s: {len(regions)}")
        for region in regions:
            print(f"    {region:3}  {addresses(region, r['region_size'])}")
    else:
        print("  no region's 16-bit hash tells the two apart (the whole memory's hash does)")
    print(f"  memory: {r['memory'] or 'not kept'}")


def compare_inputs(a, b):
    """Lockstep gives every peer the same inputs: a difference here is a bug in passing them on."""
    start = max(a["inputs_from_read"], b["inputs_from_read"])
    if len(a["inputs"]) != len(b["inputs"]):
        print(f"  inputs: for {len(a['inputs'])} players in one, {len(b['inputs'])} in the other")
        return False
    compared = 0
    for port in range(len(a["inputs"])):
        ia = a["inputs"][port][start - a["inputs_from_read"]:]
        ib = b["inputs"][port][start - b["inputs_from_read"]:]
        compared += min(len(ia), len(ib))
        for i, (x, y) in enumerate(zip(ia, ib)):
            if x != y:
                fields = [f"{name} {u} vs {v}" for name, u, v in zip(INPUT_FIELDS, x, y) if u != v]
                print(f"  port {port + 1}'s input differs at read {start + i}: {', '.join(fields)}")
                return False
    if compared == 0:
        print("  inputs: no read in both reports to compare")
        return False
    print(f"  inputs: the same in both from read {start}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reports", nargs="+", metavar="report.json")
    parser.add_argument("--max-ranges", type=int, default=40)
    args = parser.parse_args()
    if len(args.reports) > 2:
        parser.error("one or two reports")
    reports = [load(p) for p in args.reports]
    for r in reports:
        summary(r)
    if len(reports) == 1:
        return 0

    a, b = reports
    print()
    same = True
    # (rollback and characters: none in reports from before they were the match's)
    for key in ("build_id", "rom", "profile", "mods", "players", "input_delay", "own_view", "view_pairs", "region_size", "idle_from",
                "rollback", "characters"):
        if a.get(key) != b.get(key):
            print(f"  {key} differs: {a.get(key)} vs {b.get(key)}")
            same = False
    if a["read"] != b["read"]:
        print(f"  the reports are of different reads ({a['read']} and {b['read']}): their memory can't be compared")
        return 1
    if a["hash"] == b["hash"]:
        print(f"  their memory hashes are the same ({a['hash']}): these two peers didn't differ")
        return 1
    same = compare_inputs(a, b) and same

    size = a["region_size"]
    regions = [i for i, (x, y) in enumerate(zip(a["regions"], b["regions"])) if x != y]
    print(f"\nregions that differ: {len(regions)}")
    for region in regions:
        print(f"  {region:3}  {addresses(region, size)}")

    if a["memory"] and b["memory"]:
        da, db = (open(os.path.join(os.path.dirname(os.path.abspath(r["path"])), r["memory"]), "rb").read() for r in reports)
        if len(da) != len(db):
            sys.exit("the memory files are different sizes")
        symbols, names = load_symbols(DATASYMS)
        ranges = differing_ranges(da, db)
        total = sum(e - s for s, e in ranges)
        print(f"\nranges of memory that differ: {len(ranges)}, {total} byte{'' if total == 1 else 's'} in all")
        for s, e in ranges[:args.max_ranges]:
            print(f"  0x{BASE + s:08X}-0x{BASE + e - 1:08X} ({e - s:6} bytes)  {name_of(BASE + s, symbols, names)}")
        if len(ranges) > args.max_ranges:
            print(f"  ... and {len(ranges) - args.max_ranges} more (--max-ranges)")
    else:
        print("\n(a report has no memory file: only the regions are known)")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
