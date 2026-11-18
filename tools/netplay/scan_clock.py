#!/usr/bin/env python3
"""
Finds every read of the game's clock (calls to osGetCount and osGetTime, and reads of the CPU's Count
register) in the game's code, and whether each is inside a loop (a branch back over it): a loop that
waits on the clock would hang under netplay's virtual clock (issue #126). Small functions that read the
clock are counted as clocks themselves, one level up.

    python3 recomp/unpack_rom.py conker/baserom.us.z64 image.bin
    scan_clock.py recomp/conker.us.syms.toml image.bin

The image is the one N64Recomp reads (unpack_rom.py, from your ROM).
"""
import struct
import sys
import tomllib

ROOTS = {"osGetCount", "osGetTime"}
BRANCH_OPS = {0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def load(syms_path, image_path):
    with open(syms_path, "rb") as f:
        syms = tomllib.load(f)
    image = open(image_path, "rb").read()
    funcs = []
    for sec in syms["section"]:
        for fn in sec.get("functions", []):
            if fn.get("size", 0) == 0:
                continue
            rom = fn.get("rom", sec["rom"] + fn["vram"] - sec["vram"])
            words = struct.unpack_from(f">{fn['size'] // 4}I", image, rom)
            funcs.append(dict(name=fn["name"], vram=fn["vram"], section=sec["name"], words=words))
    return funcs


def analyse(fn):
    """Calls (pc, target), Count reads (pc), loops (start, end) and the syscall-ish calls."""
    calls, counts, loops = [], [], []
    base = fn["vram"]
    end = base + 4 * len(fn["words"])
    for i, w in enumerate(fn["words"]):
        pc = base + 4 * i
        op = w >> 26
        if op == 0x03:  # jal
            calls.append((pc, ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)))
        elif op == 0x02:  # j
            t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if base <= t <= pc:
                loops.append((t, pc))
        elif op == 0x10 and ((w >> 21) & 0x1F) == 0 and ((w >> 11) & 0x1F) == 9:  # mfc0 rt, Count
            counts.append(pc)
        else:
            is_branch = op in BRANCH_OPS or (op == 0x01 and ((w >> 16) & 0x1F) in (0, 1, 2, 3)) \
                or (op == 0x11 and ((w >> 21) & 0x1F) == 8)
            if is_branch:
                t = pc + 4 + (s16(w & 0xFFFF) << 2)
                if base <= t <= pc:
                    loops.append((t, pc))
    return calls, counts, loops


def main():
    funcs = load(sys.argv[1], sys.argv[2])
    by_vram = {f["vram"]: f for f in funcs}
    info = {f["vram"]: analyse(f) for f in funcs}
    name_of = lambda a: by_vram[a]["name"] if a in by_vram else f"0x{a:08X}"

    clock = {f["vram"] for f in funcs if f["name"] in ROOTS}
    # Small functions that read the clock are treated as clocks themselves (one level).
    for f in funcs:
        calls, counts, _ = info[f["vram"]]
        reads = counts or any(t in clock for _, t in calls)
        if reads and len(f["words"]) <= 24 and f["name"] not in ROOTS:
            print(f"wrapper: {f['name']} ({f['section']}, {4 * len(f['words'])} bytes)")
            clock.add(f["vram"])

    print()
    total = 0
    for f in funcs:
        if f["vram"] in clock and f["name"] in ROOTS:
            continue
        calls, counts, loops = info[f["vram"]]
        sites = [(pc, name_of(t)) for pc, t in calls if t in clock] + [(pc, "mfc0 Count") for pc in counts]
        for pc, what in sites:
            total += 1
            inside = [(a, b) for a, b in loops if a <= pc <= b]
            if inside:
                a, b = min(inside, key=lambda l: l[1] - l[0])
                others = sorted({name_of(t) for p, t in calls if a <= p <= b and t not in clock})
                tag = f"IN LOOP 0x{a:08X}-0x{b:08X} ({(b - a) // 4 + 1} instrs), other calls: {', '.join(others) or 'none'}"
            else:
                tag = "no loop"
            print(f"{f['section']:10} {f['name']:28} 0x{pc:08X} {what:22} {tag}")
    print(f"\n{total} clock reads in {len(funcs)} functions")


main()
