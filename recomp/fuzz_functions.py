#!/usr/bin/env python3
"""Function-level differential test of the translator.

Takes every function that makes no calls and stays inside its own body,
translates it, and runs it whole: once as translated code in this process and
once as original machine code in the CPU emulator, from the same registers and
memory. All registers, all XMM registers and the memory it could reach must
come out identical. This exercises what the instruction checker cannot:
branches, loops, switch tables, string operations and stack handling.

Arguments are random: small integers, wide integers, or pointers to buffers
of random data seeded with further pointers. A case where the original faults
or runs away is dropped; the function was handed arguments it cannot use.

usage: fuzz_functions.py [cases_per_function]
"""
import collections
import ctypes
import os
import random
import struct
import subprocess
import sys
from pathlib import Path

from capstone import x86 as X

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
from translate import Translator  # noqa: E402

CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 8
# One small scratch area holds everything a function under test can reach
# besides the image: its stack and four argument buffers. All of it is
# rewritten on both sides before each case and compared after.
SCRATCH, SCRATCH_SIZE = 0x1_5000_0000, 0x2_0000
STACK = SCRATCH + 0x8000          # rsp at entry; the stack grows down from here
BUFFERS = [SCRATCH + 0x10000 + i * 0x1000 for i in range(4)]
BUF_SIZE = 0x1000
STOP = SCRATCH + SCRATCH_SIZE - 0x1000  # return address of the function under test; never executed
MAX_INSNS = 200_000
rng = random.Random(11)

# ------------------------------------------------------------- candidates
t = Translator()
candidates = []
for addr in sorted(t.ranges):
    insns, leaders, switches = t.decode_flow(addr)
    ranges = t.ranges[addr]
    ok = bool(insns)
    for i in insns:
        m, ops = i.mnemonic, i.operands
        if m in ("call", "syscall", "int", "retf", "int3", "ud2", "hlt", "div", "idiv",
                 # answered by the runtime rather than translated one-to-one
                 "cpuid", "rdtsc", "xgetbv", "ldmxcsr", "stmxcsr") or m.startswith("v"):
            ok = False
        elif m[0] == "j" and ops:
            if ops[0].type == X.X86_OP_IMM:
                ok &= any(b <= ops[0].imm < e for b, e in ranges)
            else:
                ok &= i.address in switches
        if any(o.type == X.X86_OP_MEM and o.mem.segment in (X.X86_REG_GS, X.X86_REG_FS) for o in ops):
            ok = False
        if not ok:
            break
    if ok:
        candidates.append((addr, bool(switches)))

src = ['#include "x86.h"',
       "const struct recomp_entry { uint64_t va; recomp_fn fn; } recomp_table[1] = {{0, 0}};",
       "const unsigned recomp_table_count = 0;"]
usable = []
for addr, has_switch in candidates:
    text, known, bad, n = t.function(addr)
    if bad == 0 and not known:
        src.append(text)
        usable.append((addr, has_switch, n))
(HERE / "gen").mkdir(exist_ok=True)
(HERE / "build").mkdir(exist_ok=True)
(HERE / "gen/fuzz_functions.c").write_text("\n".join(src) + "\n")
subprocess.run(["cc", "-std=c11", "-O1", "-ffp-contract=off", "-fno-strict-aliasing", "-fwrapv", "-w",
                "-dynamiclib", f"-I{HERE / 'runtime'}", str(HERE / "gen/fuzz_functions.c"),
                str(HERE / "runtime/runtime.c"), "-o", str(HERE / "build/libfuzzfn.dylib")], check=True)

# ---------------------------------------------------------- both machines
lib = ctypes.CDLL(str(HERE / "build/libfuzzfn.dylib"))
if lib.recomp_map_image(str(t.img.path).encode()) != 0:
    tries = int(os.environ.get("SE_FUZZ_TRY", "0"))
    if tries >= 5:
        sys.exit("could not map the image in this process")
    os.environ["SE_FUZZ_TRY"] = str(tries + 1)
    os.execv(sys.executable, [sys.executable] + sys.argv)
libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
if libc.mmap(SCRATCH, SCRATCH_SIZE, 3, 0x1002, -1, 0) != SCRATCH:
    sys.exit("could not map the scratch region in this process")

from unicorn import UC_ARCH_X86, UC_HOOK_MEM_WRITE, UC_MODE_64, Uc, UcError  # noqa: E402
from unicorn import x86_const as U  # noqa: E402

# The emulator gets exactly the two regions this process also has at the same
# addresses: the image and the scratch area. Then every access the original
# completes is to memory the translated code can reach too. With any extra
# mapping on the emulator side, a stray pointer can succeed there and crash
# here.
uc = Uc(UC_ARCH_X86, UC_MODE_64)
uc.mem_map(t.img.base, (t.img.size + 0xFFF) & ~0xFFF)
uc.mem_write(t.img.base, t.img.data[:t.img.header_size])
for _name, _va, _vsize, _raw, _rsize in t.img.sections:
    uc.mem_write(t.img.base + _va, t.img.data[_raw:_raw + _rsize])
uc.mem_map(SCRATCH, SCRATCH_SIZE)

# Writes the original makes outside the scratch area, which means to the
# image's globals. Recorded so they can be compared, and copied across when a
# case is abandoned partway.
image_writes = []
uc.hook_add(UC_HOOK_MEM_WRITE,
            lambda _uc, _access, address, size, _value, _user:
            image_writes.append((address, size))
            if t.img.base <= address and address + size <= t.img.base + t.img.size else None)
GPR_NAMES = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()
UC_GPR = [getattr(U, "UC_X86_REG_" + n.upper()) for n in GPR_NAMES]
UC_XMM = [getattr(U, f"UC_X86_REG_XMM{i}") for i in range(16)]

class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint64 * 16), ("x", ctypes.c_uint8 * 256), ("gs_base", ctypes.c_uint64)]


def random_pointer():
    return rng.choice(BUFFERS) + 8 * rng.randrange(BUF_SIZE // 16)


def random_value():
    k = rng.randrange(8)
    if k < 3:
        return random_pointer()
    if k < 6:
        return rng.randrange(0, 64) if rng.random() < 0.7 else rng.getrandbits(16)
    return rng.getrandbits(64)


def random_buffer():
    out = bytearray(rng.randbytes(BUF_SIZE))
    for off in range(0, BUF_SIZE, 8):
        k = rng.randrange(4)
        if k == 0:
            out[off:off + 8] = struct.pack("<Q", random_pointer())
        elif k == 1:
            out[off:off + 8] = struct.pack("<Q", rng.randrange(0, 32))
    return bytes(out)


def nan_equal(a, b):
    for fmt, width in (("d", 8), ("f", 4)):
        if all(a[i:i + width] == b[i:i + width] or
               (lambda x, y: x != x and y != y)(struct.unpack("<" + fmt, a[i:i + width])[0],
                                                struct.unpack("<" + fmt, b[i:i + width])[0])
               for i in range(0, 16, width)):
            return True
    return False


def run_case(addr):
    """None: agreement. 'skip': the original cannot run this state. Otherwise a description."""
    regs = [random_value() for _ in range(16)]
    regs[4] = STACK
    xmms = [int.from_bytes(struct.pack("<2d", rng.uniform(-1e4, 1e4), rng.uniform(-1, 1)), "little")
            for _ in range(16)]
    scratch = bytearray(SCRATCH_SIZE)
    lo = STACK - SCRATCH - 0x2000
    scratch[lo:lo + 0x3000] = rng.randbytes(0x3000)
    for off in range(lo, lo + 0x3000, 8):  # stack arguments are plausible values too
        if rng.randrange(3) == 0:
            scratch[off:off + 8] = struct.pack("<Q", random_value())
    scratch[STACK - SCRATCH:STACK - SCRATCH + 8] = struct.pack("<Q", STOP)
    for b in BUFFERS:
        scratch[b - SCRATCH:b - SCRATCH + BUF_SIZE] = random_buffer()
    scratch = bytes(scratch)
    uc.mem_write(SCRATCH, scratch)
    ctypes.memmove(SCRATCH, scratch, SCRATCH_SIZE)
    image_writes.clear()

    for reg, v in zip(UC_GPR, regs):
        uc.reg_write(reg, v)
    for reg, v in zip(UC_XMM, xmms):
        uc.reg_write(reg, v)
    uc.reg_write(U.UC_X86_REG_EFLAGS, 2)
    uc.reg_write(U.UC_X86_REG_MXCSR, 0x1F80)  # default rounding, all exceptions masked
    try:
        uc.emu_start(addr, STOP, count=MAX_INSNS)
    except UcError:
        resync()
        return "skip"
    if uc.reg_read(U.UC_X86_REG_RIP) != STOP:
        resync()
        return "skip"
    want_regs = [uc.reg_read(r) for r in UC_GPR]
    want_xmm = [uc.reg_read(r).to_bytes(16, "little") for r in UC_XMM]

    cpu = Cpu()
    for i, v in enumerate(regs):
        cpu.r[i] = v
    ctypes.memmove(cpu.x, b"".join(v.to_bytes(16, "little") for v in xmms), 256)
    # If the translated code crashes this process, this file names the function.
    (HERE / "build/last_function.txt").write_text(
        f"{addr:#x}\n" + " ".join(f"{n}={v:#x}" for n, v in zip(GPR_NAMES, regs)) + "\n"
        + "after: " + " ".join(f"{n}={v:#x}" for n, v in zip(GPR_NAMES, want_regs)) + "\n")
    getattr(lib, f"f_{addr:x}")(ctypes.byref(cpu))

    verdict = None
    for i in range(16):
        if cpu.r[i] != want_regs[i]:
            verdict = f"{GPR_NAMES[i]} = {cpu.r[i]:#x}, original {want_regs[i]:#x}"
            break
    if verdict is None:
        for i in range(16):
            got = bytes(cpu.x[16 * i:16 * i + 16])
            if got != want_xmm[i]:
                verdict = f"xmm{i} = {got.hex()}, original {want_xmm[i].hex()}"
                break
    if verdict is None and ctypes.string_at(SCRATCH, SCRATCH_SIZE) != bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)):
        verdict = "stack or buffer contents differ"
    if verdict is None:
        for address, size in image_writes:
            if ctypes.string_at(address, size) != bytes(uc.mem_read(address, size)):
                verdict = f"global at {address:#x} differs"
                break
    if verdict is not None:
        resync()
    return verdict


def resync():
    """Copy the original's writes to globals into this process, after a case that did not complete here."""
    for address, size in image_writes:
        data = bytes(uc.mem_read(address, size))
        ctypes.memmove(address, data, size)


tested = cases = with_switch = insn_total = 0
failed = collections.OrderedDict()
for addr, has_switch, n in usable:
    ran = 0
    for _ in range(CASES):
        result = run_case(addr)
        if result == "skip":
            continue
        ran += 1
        if result is not None:
            failed[addr] = result
            break
    if ran:
        tested += 1
        cases += ran
        with_switch += has_switch
        insn_total += n

print(f"call-free functions: {len(usable):,} of {len(t.ranges):,}")
print(f"functions the original could run with random arguments: {tested:,} ({with_switch} with a switch table), "
      f"{insn_total:,} instructions, {cases:,} cases")
print(f"functions matching the original in every case: {tested - len(failed):,}; mismatching: {len(failed)}")
for addr, why in list(failed.items())[:25]:
    print(f"  f_{addr:x}: {why}")
sys.exit(1 if failed else 0)
