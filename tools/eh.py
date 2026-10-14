#!/usr/bin/env python3
"""Where execution can resume after an exception is caught.

A caught exception does not return to its thrower: the Windows unwinder
restarts the catching function in the middle, at the "continuation" of the
catch block. Translated functions can only be entered at their start, so the
translator needs every such address ahead of time. They are all in the
executable's exception tables:

  C++ (__CxxFrameHandler4)   the compressed FuncInfo of each function and catch
                             funclet lists its try blocks, their catch funclets
                             and up to two continuation addresses per catch
  C++ (__CxxFrameHandler3)   the catch funclet returns the address itself, so
                             its `lea rax, [rip+x]` instructions are collected
                             (also done for FH4 catches that list no address)
  SEH (__C_specific_handler) each scope with an __except block names it

Writes analysis/eh_resume.tsv: resume address, kind, the .pdata entry it came
from. `python eh.py` prints a summary; importing gives resume_points().
"""
import collections
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86 as X

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# Language handlers, identified by reading them: each wrapper checks the stack
# cookie and then calls the handler it is named after.
FH4 = {0x14012B1B0: 0,            # __CxxFrameHandler4
       0x1400CAF24: 0}            # __GSHandlerCheck_EH4
FH3 = {0x14012B128: 0,            # __CxxFrameHandler3
       0x14015EE14: 0}            # __GSHandlerCheck_EH
SEH = {0x14012BA38: 0,            # __C_specific_handler
       0x14015EE94: 0,            # __C_specific_handler_noexcept
       0x14015ED8C: 0}            # __GSHandlerCheck_SEH
PLAIN = {0x1400CAEA4}             # __GSHandlerCheck: no exception data


class Reader:
    """Cursor over the image for the FH4 tables."""
    def __init__(self, img, va):
        self.img, self.va = img, va

    def u8(self):
        v = self.img.read(self.va, 1)[0]
        self.va += 1
        return v

    def i32(self):
        v = struct.unpack("<i", self.img.read(self.va, 4))[0]
        self.va += 4
        return v

    def unsigned(self):
        """FH4's variable-length unsigned: the low bits of the first byte give the length."""
        first = self.img.read(self.va, 1)[0]
        n = 1 if not first & 1 else 2 if not first & 2 else 3 if not first & 4 else 4 if not first & 8 else 5
        raw = int.from_bytes(self.img.read(self.va, n), "little")
        self.va += n
        return raw >> 8 if n == 5 else raw >> n


def pdata(img):
    rva, size = img.dirs[3]
    o = img.off(img.base + rva)
    for i in range(size // 12):
        b, e, uw = struct.unpack_from("<III", img.data, o + i * 12)
        if b:
            yield img.base + b, img.base + e, img.base + uw


def handler_of(img, unwind):
    """(handler va, va of its data) for an UNWIND_INFO with a handler, else None."""
    flags = img.read(unwind, 1)[0] >> 3
    if not flags & 3:
        return None
    count = img.read(unwind + 2, 1)[0]
    at = unwind + 4 + ((count + 1) & ~1) * 2
    return img.base + img.u32(at), at + 4


def fh4(img, begin, data):
    """(continuations, catch funclets without a listed continuation) for one .pdata entry."""
    info = img.u32(data)
    if not info:
        return [], []
    r = Reader(img, img.base + info)
    header = r.u8()
    if header & 0x04:
        r.unsigned()                       # bbtFlags
    if header & 0x08:
        r.i32()                            # unwind map
    try_map = r.i32() if header & 0x10 else 0
    ip_map = r.i32()
    if header & 0x02 or not try_map:       # separated functions: none in this executable
        assert not header & 0x02, f"separated FuncInfo at {begin:#x}"
        return [], []
    # States this entry's own code can be in.
    r = Reader(img, img.base + ip_map)
    states = {r2 for _ in range(r.unsigned()) for r2 in [(r.unsigned(), r.unsigned() - 1)[1]]}
    r = Reader(img, img.base + try_map)
    conts, bare = [], []
    for _ in range(r.unsigned()):
        low, high, _catch_high, handlers = r.unsigned(), r.unsigned(), r.unsigned(), r.i32()
        mine = any(low <= s <= high for s in states)
        h = Reader(img, img.base + handlers)
        for _ in range(h.unsigned()):
            head = h.u8()
            if head & 0x01:
                h.unsigned()               # adjectives
            if head & 0x02:
                h.i32()                    # type
            if head & 0x04:
                h.unsigned()               # catch object
            funclet = img.base + h.i32()
            n = head >> 4 & 3
            assert n < 3
            addrs = [img.base + h.i32() if head & 0x08 else begin + h.unsigned() for _ in range(n)]
            if not mine:
                continue
            conts += addrs
            if not n:
                bare.append(funclet)
    return conts, bare


def fh3(img, data):
    """Catch funclets of an FH3 FuncInfo."""
    info = img.base + img.u32(data)
    magic, _max_state, _unwind, n_try, try_map = struct.unpack("<IiIII", img.read(info, 20))
    assert magic & 0xFFFFFFF0 == 0x19930520, hex(magic)
    out = []
    for i in range(n_try):
        _lo, _hi, _ch, n_catch, handlers = struct.unpack("<iiiiI", img.read(img.base + try_map + 20 * i, 20))
        for k in range(n_catch):
            out.append(img.base + img.u32(img.base + handlers + 20 * k + 12))
    return out


def seh(img, data):
    """__except blocks of a scope table."""
    return [img.base + target
            for i in range(img.u32(data))
            for target in [img.u32(data + 4 + 16 * i + 12)] if target]


def returned_addresses(img, md, begin, end):
    """Code addresses a catch funclet loads into rax to return."""
    out = []
    for ins in md.disasm(img.read(begin, end - begin), begin):
        ops = ins.operands
        if (ins.mnemonic == "lea" and ops[0].reg == X.X86_REG_RAX and ops[1].type == X.X86_OP_MEM
                and ops[1].mem.base == X.X86_REG_RIP):
            out.append(ins.address + ins.size + ops[1].mem.disp)
    return out


def resume_points(img=None):
    """[(resume va, kind, .pdata entry va)], sorted, without duplicates."""
    img = img or Image()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    entries = list(pdata(img))
    ends = {b: e for b, e, _ in entries}
    text = next(s for s in img.sections if s[0] == ".text")
    lo, hi = img.base + text[1], img.base + text[1] + text[2]
    out = set()
    for begin, _end, unwind in entries:
        h = handler_of(img, unwind)
        if not h:
            continue
        handler, data = h
        funclets = []
        if handler in FH4:
            conts, funclets = fh4(img, begin, data)
            out.update((a, "catch", begin) for a in conts)
        elif handler in FH3:
            funclets = fh3(img, data)
        elif handler in SEH:
            out.update((a, "except", begin) for a in seh(img, data))
        else:
            assert handler in PLAIN, f"unknown language handler {handler:#x} for {begin:#x}"
        for f in funclets:
            out.update((a, "catch-returned", begin) for a in returned_addresses(img, md, f, ends[f]) if lo <= a < hi)
    return sorted(out)


if __name__ == "__main__":
    points = resume_points()
    with open(ROOT / "analysis/eh_resume.tsv", "w") as f:
        f.write("resume_va\tkind\tpdata_entry\n")
        for a, kind, entry in points:
            f.write(f"{a:#x}\t{kind}\t{entry:#x}\n")
    kinds = collections.Counter(k for _, k, _ in points)
    print(f"{len({a for a, _, _ in points}):,} resume addresses: " + ", ".join(f"{n} {k}" for k, n in kinds.items()))
