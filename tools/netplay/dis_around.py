#!/usr/bin/env python3
"""
A minimal MIPS disassembly of the instructions around each osGetCount call in the functions named, to
see what the game does with the clock (scan_clock.py finds them). Only the common instructions are
decoded; the rest are shown as words.

    dis_around.py recomp/conker.us.syms.toml image.bin <function> [<function> ...]
"""
import struct
import sys
import tomllib

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
I = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori",
     0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x28: "sb", 0x29: "sh", 0x2B: "sw",
     0x31: "lwc1", 0x39: "swc1", 0x37: "ld", 0x3F: "sd"}
B = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x14: "beql", 0x15: "bnel"}
SP = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x08: "jr", 0x09: "jalr", 0x10: "mfhi", 0x12: "mflo",
      0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x21: "addu", 0x23: "subu", 0x24: "and",
      0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2D: "daddu", 0x2F: "dsubu",
      0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32"}


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def dis(w, pc, names):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    if w == 0:
        return "nop"
    if op == 0:
        m = SP.get(fn, f"special.{fn:02x}")
        if fn in (0, 2, 3, 0x3C, 0x3E, 0x3F):
            return f"{m} {R[rd]}, {R[rt]}, {sa}"
        if fn in (0x10, 0x12):
            return f"{m} {R[rd]}"
        if fn in (0x18, 0x19, 0x1A, 0x1B):
            return f"{m} {R[rs]}, {R[rt]}"
        if fn in (8, 9):
            return f"{m} {R[rs]}"
        return f"{m} {R[rd]}, {R[rs]}, {R[rt]}"
    if op in (2, 3):
        t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return f"{'j' if op == 2 else 'jal'} {names.get(t, hex(t))}"
    if op == 0x0F:
        return f"lui {R[rt]}, 0x{imm:04X}"
    if op in B:
        return f"{B[op]} {R[rs]}, {R[rt]}, 0x{pc + 4 + (s16(imm) << 2):08X}"
    if op in I:
        m = I[op]
        if op >= 0x20:
            reg = f"f{rt}" if op in (0x31, 0x39) else R[rt]
            return f"{m} {reg}, {s16(imm)}({R[rs]})"
        return f"{m} {R[rt]}, {R[rs]}, {s16(imm) if op in (8, 9, 0x0A, 0x0B) else hex(imm)}"
    if op == 0x11:
        return f"cop1 0x{w:08X}"
    return f".word 0x{w:08X}"


syms = tomllib.load(open(sys.argv[1], "rb"))
img = open(sys.argv[2], "rb").read()
fns, names = {}, {}
for s in syms["section"]:
    for f in s.get("functions", []):
        if f.get("size"):
            fns[f["name"]] = (f["vram"], f.get("rom", s["rom"] + f["vram"] - s["vram"]), f["size"])
            names[f["vram"]] = f["name"]
gc = fns["osGetCount"][0]
for name in sys.argv[3:]:
    v, r, sz = fns[name]
    ws = struct.unpack_from(f">{sz // 4}I", img, r)
    print(f"=== {name}")
    shown = set()
    for i, w in enumerate(ws):
        pc = v + 4 * i
        if w >> 26 == 3 and (((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) == gc:
            for j in range(max(0, i - 8), min(len(ws), i + 14)):
                if j not in shown:
                    shown.add(j)
                    print(f"  {v + 4 * j:08X}: {dis(ws[j], v + 4 * j, names)}")
            print("  ...")
