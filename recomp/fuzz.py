#!/usr/bin/env python3
"""Instruction-level differential test of the translator.

For every distinct instruction shape in the binary (mnemonic plus operand
kinds and sizes) a few real instances are translated to C, compiled into a
dylib, and executed with random machine states. The same instruction bytes
run in the CPU emulator from the same state. Registers, XMM registers, flags
and the touched memory must come out identical.

Not covered here: control flow (call, jmp, ret, conditional jumps), which the
function-level tests exercise, and instructions the translator does not
handle yet, which `translate.py coverage` lists.

usage: fuzz.py [instances_per_shape] [states_per_instance]
"""
import collections
import ctypes
import random
import struct
import subprocess
import sys
from pathlib import Path

from capstone import x86 as X

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
from translate import REG, Translator, Unsupported  # noqa: E402

PER_SHAPE = int(sys.argv[1]) if len(sys.argv) > 1 else 2
STATES = int(sys.argv[2]) if len(sys.argv) > 2 else 12
SCRATCH, SCRATCH_SIZE = 0x1_5000_0000, 0x10_0000  # free in this process: above the image, below the dyld shared cache
STACK = SCRATCH + 0x4000
SKIP = {"call", "jmp", "ret", "retf", "int3", "ud2", "hlt", "syscall", "div", "idiv", "leave",
        # Answered by the runtime, not translated one-to-one:
        "cpuid", "rdtsc", "xgetbv", "ldmxcsr", "stmxcsr", "fnstcw", "fldcw", "fnclex"}
# Flags the processor leaves undefined; AF is never modelled.
UNDEFINED = {"bt": "osp", "bts": "osp", "btr": "osp", "btc": "osp", "bsf": "cosp", "bsr": "cosp",
             "imul": "zsp", "mul": "zsp", "shl": "o", "sal": "o", "shr": "o", "sar": "o",
             "rol": "o", "ror": "o", "rcl": "o", "rcr": "o", "and": "", "or": "", "xor": "", "test": ""}
rng = random.Random(7)

# ------------------------------------------------------------ collect shapes
t = Translator()


def shape(ins):
    parts = [ins.mnemonic]
    for op in ins.operands:
        if op.type == X.X86_OP_REG:
            name = ins.reg_name(op.reg)
            kind = "x" if name.startswith("xmm") else "h" if name in ("ah", "bh", "ch", "dh") else "r"
            parts.append(f"{kind}{op.size * 8}")
        elif op.type == X.X86_OP_IMM:
            parts.append("i")
        else:
            parts.append(("rip" if op.mem.base == X.X86_REG_RIP else "m") + str(op.size * 8))
    return " ".join(parts)


def testable(ins):
    m = ins.mnemonic
    if m in SKIP or (m[0] == "j") or m.startswith(("rep", "v")) or m[:4] in ("movs", "stos") and len(m) == 5:
        return False
    mems = [op for op in ins.operands if op.type == X.X86_OP_MEM]
    if len(mems) > 1:
        return False
    for op in mems:
        mem = op.mem
        if mem.segment in (X.X86_REG_GS, X.X86_REG_FS):
            return False
        if mem.base != X.X86_REG_RIP:
            if not mem.base or mem.base == mem.index:
                return False
            if any(reg and REG.get(ins.reg_name(reg), (0, 0))[1] != 64 for reg in (mem.base, mem.index)):
                return False
    return True


instances = collections.defaultdict(list)
for addr in sorted(t.ranges):
    for ins in t.decode(addr):
        if ins is not None and testable(ins):
            key = shape(ins)
            if len(instances[key]) < PER_SHAPE:
                instances[key].append(ins)

tests, skipped = [], collections.Counter()
src = ['#include "x86.h"',
       "const struct recomp_entry { uint64_t va; recomp_fn fn; } recomp_table[1] = {{0, 0}};",
       "const unsigned recomp_table_count = 0;"]
for key, group in sorted(instances.items()):
    for ins in group:
        try:
            body = t.insn(ins, lambda a: False, set())
        except (Unsupported, KeyError, IndexError):
            skipped[ins.mnemonic] += 1
            continue
        if "recomp_trap" in body or "return" in body:
            continue
        src.append(f"void t_{len(tests)}(cpu_t *c, uint8_t *fl) {{ uint8_t cf = fl[0], zf = fl[1], sf = fl[2], "
                   f"of = fl[3], pf = fl[4]; {{ {body} }} fl[0] = cf; fl[1] = zf; fl[2] = sf; fl[3] = of; fl[4] = pf; }}"
                   f" /* {ins.address:x} {ins.mnemonic} {ins.op_str} */".replace("*/ */", "*/"))
        tests.append((key, ins))
(HERE / "gen").mkdir(exist_ok=True)
(HERE / "build").mkdir(exist_ok=True)
(HERE / "gen/fuzz.c").write_text("\n".join(src) + "\n")
subprocess.run(["cc", "-std=c11", "-O1", "-ffp-contract=off", "-fno-strict-aliasing", "-fwrapv", "-w",
                "-dynamiclib", f"-I{HERE / 'runtime'}", str(HERE / "gen/fuzz.c"), str(HERE / "runtime/runtime.c"),
                "-o", str(HERE / "build/libfuzz.dylib")], check=True)

# ------------------------------------------------------------- both machines
lib = ctypes.CDLL(str(HERE / "build/libfuzz.dylib"))
if lib.recomp_map_image(str(t.img.path).encode()) != 0:
    # This process has no link-time reservation, so the range is occasionally
    # taken. Address-space randomisation differs per launch: start again.
    import os
    tries = int(os.environ.get("SE_FUZZ_TRY", "0"))
    if tries >= 5:
        sys.exit("could not map the image in this process")
    os.environ["SE_FUZZ_TRY"] = str(tries + 1)
    os.execv(sys.executable, [sys.executable] + sys.argv)
libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
if libc.mmap(SCRATCH, SCRATCH_SIZE, 3, 0x1002, -1, 0) != SCRATCH:  # PROT_READ|WRITE, MAP_PRIVATE|MAP_ANON
    sys.exit("could not map the scratch region in this process")

from emu import Emu  # noqa: E402  (after the fixed mappings, so they get their addresses)
from unicorn import UcError  # noqa: E402
from unicorn import x86_const as U  # noqa: E402

emu = Emu(host_math=False)
uc = emu.uc
uc.mem_map(SCRATCH, SCRATCH_SIZE)
UC_GPR = [getattr(U, "UC_X86_REG_" + n.upper()) for n in
          "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()]
UC_XMM = [getattr(U, f"UC_X86_REG_XMM{i}") for i in range(16)]


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint64 * 16), ("x", ctypes.c_uint8 * 256), ("gs_base", ctypes.c_uint64)]


INTERESTING = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
               0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF]
FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, -2.5, 3.0e9, -3.0e9, 1e19, -1e19, 1e300, 1e-300, 5e-324,
          float("inf"), float("-inf"), 2451545.0, 0.1]


def random_gpr():
    k = rng.randrange(4)
    if k == 0:
        return rng.choice(INTERESTING)
    if k == 1:
        return rng.getrandbits(rng.choice([8, 16, 32]))
    return rng.getrandbits(64)


def random_xmm():
    k = rng.randrange(4)
    if k == 0:
        return rng.getrandbits(128)
    if k == 1:  # two doubles
        return int.from_bytes(struct.pack("<2d", rng.choice(FLOATS), rng.uniform(-1e6, 1e6)), "little")
    if k == 2:  # four floats
        return int.from_bytes(struct.pack("<4f", *(rng.choice([0.0, -0.0, 1.5, -7.25, 3e9, 1e-30, rng.uniform(-100, 100)])
                                                   for _ in range(4))), "little")
    return int.from_bytes(struct.pack("<2d", rng.uniform(-1e3, 1e3), rng.choice(FLOATS)), "little")


def is_nan_lanes(a, b):
    """True when a and b differ only in lanes where both hold a NaN."""
    for fmt, width in (("<2d", 8), ("<4f", 4)):
        ok = True
        for i in range(0, 16, width):
            pa, pb = a[i:i + width], b[i:i + width]
            if pa != pb:
                fa, fb = struct.unpack(fmt[0] + fmt[2], pa)[0], struct.unpack(fmt[0] + fmt[2], pb)[0]
                ok &= fa != fa and fb != fb
        if ok:
            return True
    return False


def run(index, key, ins):
    """Returns None on agreement, 'nan' for a NaN-encoding difference, or a description."""
    fn = getattr(lib, f"t_{index}")
    mem = next((op.mem for op in ins.operands if op.type == X.X86_OP_MEM), None)
    undefined = UNDEFINED.get(ins.mnemonic.replace("lock ", ""), "")
    verdict = None
    for _ in range(STATES):
        regs = [random_gpr() for _ in range(16)]
        xmms = [random_xmm() for _ in range(16)]
        flags = [rng.randrange(2) for _ in range(5)]
        regs[4] = STACK
        windows = [STACK - 32]
        if mem is not None:
            if mem.base == X.X86_REG_RIP:
                target = ins.address + ins.size + mem.disp
                if not t.img.base + 16 <= target < t.img.base + t.img.size - 64:
                    return "skip"  # points outside the image: not a real instruction
            else:
                target = SCRATCH + 0x8000 + 16 * rng.randrange(64)
                offset = mem.disp
                if mem.index:
                    idx = REG[ins.reg_name(mem.index)][0]
                    regs[idx] = rng.randrange(64)
                    offset += regs[idx] * mem.scale
                regs[REG[ins.reg_name(mem.base)][0]] = (target - offset) & (2**64 - 1)
            windows.append(target - 16)
        # Scratch windows start from random bytes. A window inside the image
        # (a RIP-relative operand) keeps the image's own bytes: overwriting
        # them would corrupt constants and, in .text, the instructions that
        # later tests execute. Whatever the instruction writes there is undone
        # after the comparison.
        in_image = [t.img.base <= w < t.img.base + t.img.size for w in windows]
        original = [bytes(uc.mem_read(w, 64)) if inside else None for w, inside in zip(windows, in_image)]
        for w, inside in zip(windows, in_image):
            if not inside:
                data = rng.randbytes(64)
                uc.mem_write(w, data)
                ctypes.memmove(w, data, 64)

        for reg, v in zip(UC_GPR, regs):
            uc.reg_write(reg, v)
        for reg, v in zip(UC_XMM, xmms):
            uc.reg_write(reg, v)
        cf, zf, sf, of, pf = flags
        uc.reg_write(U.UC_X86_REG_EFLAGS, 2 | cf | pf << 2 | zf << 6 | sf << 7 | of << 11)
        uc.reg_write(U.UC_X86_REG_MXCSR, 0x1F80)  # default rounding, all exceptions masked
        try:
            uc.emu_start(ins.address, ins.address + ins.size, count=1)
        except UcError:
            for w, before in zip(windows, original):
                if before is not None:
                    uc.mem_write(w, before)
            continue  # the original faults on this state (for example a misaligned operand)
        want_regs = [uc.reg_read(r) for r in UC_GPR]
        want_xmm = [uc.reg_read(r).to_bytes(16, "little") for r in UC_XMM]
        ef = uc.reg_read(U.UC_X86_REG_EFLAGS)
        want_flags = [ef & 1, ef >> 6 & 1, ef >> 7 & 1, ef >> 11 & 1, ef >> 2 & 1]
        want_mem = [bytes(uc.mem_read(w, 64)) for w in windows]

        cpu = Cpu()
        for i, v in enumerate(regs):
            cpu.r[i] = v
        ctypes.memmove(cpu.x, b"".join(v.to_bytes(16, "little") for v in xmms), 256)
        fl = (ctypes.c_uint8 * 5)(*flags)
        fn(ctypes.byref(cpu), fl)
        got_xmm = [bytes(cpu.x[16 * i:16 * i + 16]) for i in range(16)]
        got_mem = [ctypes.string_at(w, 64) for w in windows]
        for w, data, before in zip(windows, want_mem, original):  # leave both memories identical for later tests
            if before is not None:
                uc.mem_write(w, before)
                data = before
            ctypes.memmove(w, data, 64)

        for i in range(16):
            if cpu.r[i] != want_regs[i]:
                return f"{GPR_NAMES[i]} = {cpu.r[i]:#x}, original {want_regs[i]:#x}"
        for name, i in zip("czsop", range(5)):
            if name not in undefined and fl[i] != want_flags[i]:
                return f"flag {name}f = {fl[i]}, original {want_flags[i]}"
        for i in range(16):
            if got_xmm[i] != want_xmm[i]:
                return f"xmm{i} = {got_xmm[i].hex()}, original {want_xmm[i].hex()}"
        if got_mem != want_mem:
            return "memory contents differ"
    return verdict


GPR_NAMES = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()
ok = nan = 0
failed = collections.OrderedDict()
shapes_ok, shapes_all = set(), set()
for index, (key, ins) in enumerate(tests):
    shapes_all.add(key)
    result = run(index, key, ins)
    if result == "skip":
        continue
    if result is None or result == "nan":
        ok += 1
        nan += result == "nan"
    else:
        failed.setdefault(key, (ins, result))
shapes_ok = shapes_all - set(failed)

print(f"instruction shapes in the binary (testable kinds): {len(instances):,}")
print(f"shapes with a translation: {len(shapes_all):,}; instances run: {len(tests):,} x {STATES} states")
print(f"shapes that match the original: {len(shapes_ok):,}; shapes with a mismatch: {len(failed):,}")
print(f"instances that differ only in how a NaN result is encoded: {nan}")
by_mnemonic = collections.Counter(k.split()[0] for k in failed)
if failed:
    print("mismatches by mnemonic:", dict(by_mnemonic.most_common()))
    for key, (ins, why) in list(failed.items())[:25]:
        print(f"  {key:34s} {ins.address:x} {ins.mnemonic} {ins.op_str}: {why}")
if skipped:
    print(f"untranslated mnemonics skipped: {len(skipped)} (see translate.py coverage)")
sys.exit(1 if failed else 0)
