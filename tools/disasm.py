#!/usr/bin/env python3
"""Disassemble one function from SpaceEngine.exe with imports, names and
strings resolved where analysis/ has them.

usage: disasm.py <address> [byte_count]

Without byte_count every .pdata chunk of the function is printed; leaf
functions with no .pdata entry are disassembled up to their first ret/jmp
followed by padding.
"""
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from pe import Image  # noqa: E402

root = Path(__file__).resolve().parent.parent
img = Image()

def table(name, skip_header=False):
    """Lines of an analysis table, if it has been generated (recomp's build makes imports.tsv)."""
    path = root / "analysis" / name
    lines = path.read_text(errors="replace").splitlines() if path.exists() else []
    return lines[1:] if skip_header else lines


names = {}
for line in table("names.tsv", skip_header=True):
    va, name = line.split("\t")[:2]
    names[int(va, 16)] = name
imports = {}
for line in table("imports.tsv"):
    va, name = line.split("\t")[:2]
    imports[int(va, 16)] = name
strings = {}
for line in table("strings.tsv"):
    va, _, val = line.partition("\t")
    strings[int(va, 16)] = val

start = int(sys.argv[1], 16)
ranges = img.function_ranges()
if len(sys.argv) > 2:
    spans = [(start, start + int(sys.argv[2], 0))]
elif start in ranges:
    spans = ranges[start]
else:
    spans = [(start, start + 0x400)]

rdata = next(s for s in img.sections if s[0] == ".rdata")
rd_lo, rd_hi = img.base + rdata[1], img.base + rdata[1] + rdata[2]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
leaf = start not in ranges and len(sys.argv) <= 2


def annotate(ins):
    note = ""
    for op in ins.operands:
        tgt = None
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            tgt = ins.address + ins.size + op.mem.disp
        elif op.type == capstone.x86.X86_OP_IMM and img.base <= op.imm < img.base + img.size:
            tgt = op.imm
        if tgt is None:
            continue
        if tgt in names:
            note = names[tgt]
        elif tgt in imports:
            note = imports[tgt]
        elif tgt in strings:
            note = repr(strings[tgt][:60])
        elif rd_lo <= tgt < rd_hi and ins.mnemonic not in ("call", "jmp", "lea"):
            # A read-only constant: show it both as double and as float.
            raw = img.read(tgt, 8)
            note = f"{struct.unpack('<d', raw)[0]:.17g} | f32 {struct.unpack('<f', raw[:4])[0]:.9g}"
        if op.type == capstone.x86.X86_OP_MEM:
            note = f"{tgt:#x} {note}".strip()
    return note


for n, (addr, end) in enumerate(spans):
    if n:
        print(f"; --- chunk {addr:#x}..{end:#x}")
    done = False
    while addr < end and not done:
        progressed = False
        for ins in md.disasm(img.read(addr, end - addr), addr):
            progressed = True
            note = annotate(ins)
            print(f"{ins.address:x}  {ins.mnemonic:9s} {ins.op_str:44s} {'; ' + note if note else ''}".rstrip())
            addr = ins.address + ins.size
            if leaf and ins.mnemonic in ("ret", "jmp") and img.read(addr, 1) in (b"\xcc", b"\x90"):
                done = True
                break
        if not progressed:
            print(f"{addr:x}  (undecoded byte {img.read(addr, 1).hex()})")
            addr += 1
