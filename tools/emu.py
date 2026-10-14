"""Run individual functions of SpaceEngine.exe in a CPU emulator.

The image is mapped at its preferred base with no OS underneath: there is no
loader, no imports, no heap and no CRT startup. That is enough for pure
computational functions, which is what the differential tests need. A call
that reaches an import or unmapped memory raises instead of doing anything.
"""
import math
import struct
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn import x86_const as x

sys.path.insert(0, str(Path(__file__).parent))
from pe import Image  # noqa: E402

STACK_TOP = 0x7FF0_0000_0000
STACK_SIZE = 0x10_0000
HEAP = 0x2_0000_0000
HEAP_SIZE = 0x100_0000
STOP = 0x1_0000_0000  # return address of the outermost call

GPR = [x.UC_X86_REG_RCX, x.UC_X86_REG_RDX, x.UC_X86_REG_R8, x.UC_X86_REG_R9]
XMM = [x.UC_X86_REG_XMM0, x.UC_X86_REG_XMM1, x.UC_X86_REG_XMM2, x.UC_X86_REG_XMM3]

# CRT math routines, replaced by the host's so both sides of a differential
# test use one libm. The originals stay available with host_math=False.
HOST_MATH = {
    0x1405EA080: math.cos,
    0x1405ED8D0: math.sin,
    0x1405EB1F0: math.floor,
}


class Emu:
    def __init__(self, host_math=True):
        self.img = Image()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        size = (self.img.size + 0xFFF) & ~0xFFF
        self.uc.mem_map(self.img.base, size)
        self.uc.mem_write(self.img.base, self.img.data[:self.img.header_size])
        for _, va, _, raw, rsize in self.img.sections:
            self.uc.mem_write(self.img.base + va, self.img.data[raw:raw + rsize])
        self.uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE)
        self.uc.mem_map(HEAP, HEAP_SIZE)
        self.uc.mem_map(STOP, 0x1000)
        self.brk = HEAP
        if host_math:
            for addr, fn in HOST_MATH.items():
                self.uc.hook_add(UC_HOOK_CODE, self._host_call, fn, addr, addr)

    def _host_call(self, uc, address, size, fn):
        arg = struct.unpack("<d", uc.reg_read(x.UC_X86_REG_XMM0).to_bytes(16, "little")[:8])[0]
        uc.reg_write(x.UC_X86_REG_XMM0, int.from_bytes(struct.pack("<d", float(fn(arg))), "little"))
        rsp = uc.reg_read(x.UC_X86_REG_RSP)
        uc.reg_write(x.UC_X86_REG_RIP, struct.unpack("<Q", uc.mem_read(rsp, 8))[0])
        uc.reg_write(x.UC_X86_REG_RSP, rsp + 8)

    # ------------------------------------------------------------- memory
    def alloc(self, n, data=None):
        addr = self.brk
        self.brk += (n + 15) & ~15
        if data is not None:
            self.uc.mem_write(addr, bytes(data))
        return addr

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def read(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def doubles(self, addr, n):
        return struct.unpack(f"<{n}d", self.read(addr, 8 * n))

    def qwords(self, addr, n):
        return struct.unpack(f"<{n}Q", self.read(addr, 8 * n))

    # --------------------------------------------------------------- call
    def call(self, addr, *args, max_insns=50_000_000):
        """Win64 call: ints go in rcx/rdx/r8/r9, floats in xmm0-3, by position.

        Returns (rax, xmm0 as double).
        """
        assert len(args) <= 4, "stack arguments are not set up"
        for i, a in enumerate(args):
            if isinstance(a, float):
                self.uc.reg_write(XMM[i], int.from_bytes(struct.pack("<d", a), "little"))
            else:
                self.uc.reg_write(GPR[i], a & 0xFFFFFFFFFFFFFFFF)
        rsp = STACK_TOP - 0x1000
        rsp -= 0x28  # 32 bytes of shadow space, and rsp = 8 mod 16 at entry
        self.uc.mem_write(rsp, struct.pack("<Q", STOP))
        self.uc.reg_write(x.UC_X86_REG_RSP, rsp)
        try:
            self.uc.emu_start(addr, STOP, count=max_insns)
        except UcError as e:
            rip = self.uc.reg_read(x.UC_X86_REG_RIP)
            raise RuntimeError(f"emulation of {addr:#x} stopped at {rip:#x}: {e}") from None
        rip = self.uc.reg_read(x.UC_X86_REG_RIP)
        if rip != STOP:
            raise RuntimeError(f"{addr:#x} did not return within {max_insns} instructions (rip={rip:#x})")
        xmm0 = self.uc.reg_read(x.UC_X86_REG_XMM0).to_bytes(16, "little")
        return self.uc.reg_read(x.UC_X86_REG_RAX), struct.unpack("<d", xmm0[:8])[0]
