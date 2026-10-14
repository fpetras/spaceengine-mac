#!/usr/bin/env python3
"""Static translator: x86-64 functions of SpaceEngine.exe to C.

Each original function becomes `void f_<address>(cpu_t *c)`, one C statement
group per instruction, operating on the register file in cpu_t and on memory
at the original addresses. clang then compiles that C for the host CPU.

  translate.py emit <out.c> <address>...   translate the listed functions
  translate.py program <outdir>            translate every function, split
                                           across many C files
  translate.py coverage                    report how much of the whole binary
                                           the instruction handlers cover

Instructions without a handler translate to a recomp_unimplemented() call, so
partial coverage still yields code that compiles and fails loudly at run time.
"""
import bisect
import collections
import sys
from pathlib import Path

import capstone
from capstone import x86 as X

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from pe import Image  # noqa: E402

MASK64 = (1 << 64) - 1

GPR64 = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()
REG = {}  # name -> (index, bits, shift)
for i, n in enumerate(GPR64):
    REG[n] = (i, 64, 0)
for i, (d, w, b) in enumerate(zip("eax ecx edx ebx esp ebp esi edi".split(),
                                  "ax cx dx bx sp bp si di".split(),
                                  "al cl dl bl spl bpl sil dil".split())):
    REG[d], REG[w], REG[b] = (i, 32, 0), (i, 16, 0), (i, 8, 0)
for i in range(8, 16):
    REG[f"r{i}d"], REG[f"r{i}w"], REG[f"r{i}b"] = (i, 32, 0), (i, 16, 0), (i, 8, 0)
for i, n in enumerate("ah ch dh bh".split()):
    REG[n] = (i, 8, 8)

CC = {}
for names, expr in (("o", "of"), ("no", "!of"), ("b c nae", "cf"), ("ae nb nc", "!cf"),
                    ("e z", "zf"), ("ne nz", "!zf"), ("be na", "(cf || zf)"), ("a nbe", "(!cf && !zf)"),
                    ("s", "sf"), ("ns", "!sf"), ("p pe", "pf"), ("np po", "!pf"),
                    ("l nge", "(sf != of)"), ("ge nl", "(sf == of)"),
                    ("le ng", "(zf || sf != of)"), ("g nle", "(!zf && sf == of)")):
    for n in names.split():
        CC[n] = expr


class Unsupported(Exception):
    pass


class Translator:
    def __init__(self):
        self.img = Image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.ranges = dict(self.img.function_ranges())
        # .pdata describes where each of its functions is. Leaf functions have
        # no .pdata entry, so only their entry address is known for certain;
        # the size recorded for them is an estimate and decode_flow() finds
        # their real extent by following control flow.
        self.chunks = sorted(c for r in self.ranges.values() for c in r)
        self.chunk_begins = [b for b, _ in self.chunks]
        self.leaf = set()
        for line in (ROOT / "analysis/functions.tsv").read_text().splitlines()[1:]:
            f = line.split("\t")
            if f[8].strip() == "leaf":
                self.add_leaf(int(f[0], 16), int(f[1]))
        self.imports = {}
        for line in (ROOT / "analysis/imports.tsv").read_text().splitlines():
            va, name = line.split("\t")[:2]
            self.imports[int(va, 16)] = name
        self.missing = collections.Counter()
        self.switches = {}
        self.setjmp = hle_addresses("setjmp")
        text = next(sec for sec in self.img.sections if sec[0] == ".text")
        self.text_lo = self.img.base + text[1]
        self.text_hi = self.text_lo + text[2]
        self.code_refs = set()  # addresses in .text that some instruction takes the address of
        self.pointer_leaves = self.discover_pointers()

    def discover_pointers(self):
        """Adds functions that are only ever reached through a stored pointer.

        Callbacks, and virtual functions of classes without RTTI, appear in no
        call instruction. They do appear as 8-byte values in the data
        sections. A value pointing into .text but into no .pdata function is
        the entry of a leaf function.
        """
        found = 0
        for name, va, _, raw, rsize in self.img.sections:
            if name not in (".rdata", ".data"):
                continue
            blob = self.img.data[raw:raw + rsize]
            for off in range(0, len(blob) - 7, 8):
                v = int.from_bytes(blob[off:off + 8], "little")
                if self.text_lo <= v < self.text_hi and v not in self.ranges and not self.in_pdata(v):
                    self.add_leaf(v)
                    found += 1
        return found

    LEAF_LIMIT = 0x10000  # no leaf function is anywhere near this long

    def add_leaf(self, addr, size_estimate=16):
        self.leaf.add(addr)
        self.ranges[addr] = [(addr, addr + size_estimate)]

    def in_pdata(self, a):
        i = bisect.bisect_right(self.chunk_begins, a) - 1
        return i >= 0 and self.chunks[i][0] <= a < self.chunks[i][1]

    def extent(self, addr):
        """(contains(a), end_of_run(a)) for the body of the function at addr."""
        if addr not in self.leaf:
            ranges = self.ranges[addr]
            return (lambda a: any(b <= a < e for b, e in ranges),
                    lambda a: next(e for b, e in ranges if b <= a < e))
        # A leaf's body is whatever follows its entry without running into
        # code that .pdata assigns to another function.
        limit = addr + self.LEAF_LIMIT
        i = bisect.bisect_right(self.chunk_begins, addr)
        if i < len(self.chunk_begins):
            limit = min(limit, self.chunk_begins[i])
        return (lambda a: addr <= a < limit and not self.in_pdata(a)), (lambda a: limit)

    # ----------------------------------------------------------- decoding
    def decode(self, addr):
        out = []
        for b, e in self.ranges[addr]:
            a = b
            while a < e:
                progressed = False
                for ins in self.md.disasm(self.img.read(a, e - a), a):
                    progressed = True
                    out.append(ins)
                    a = ins.address + ins.size
                if not progressed:
                    out.append(None)  # undecodable byte: data inside the range
                    a += 1
        return out

    TERMINATORS = ("ret", "repz ret", "retf", "jmp", "int3", "ud2", "hlt")

    def decode_flow(self, addr):
        """Decodes the instructions reachable from the function's entry.

        Following control flow, instead of decoding the address range from
        start to end, keeps data stored inside the function (switch tables)
        from being read as instructions. Returns (instructions sorted by
        address, branch targets, {jmp address: switch targets}).
        """
        in_fn, range_end = self.extent(addr)
        is_leaf = addr in self.leaf

        seen, leaders, switches = {}, set(), {}
        work = [addr]
        while work:
            a = work.pop()
            if a in seen or not in_fn(a):
                continue
            end = range_end(a)
            path = []
            for ins in self.md.disasm(self.img.read(a, end - a), a):
                if ins.address in seen:
                    break
                seen[ins.address] = ins
                path.append(ins)
                m, ops = ins.mnemonic, ins.operands
                if m[0] == "j" and ops and ops[0].type == X.X86_OP_IMM:
                    target = ops[0].imm
                    # In a leaf, an unconditional jump to another function's
                    # entry is a tail call; everything else stays in the body.
                    tail = is_leaf and m == "jmp" and target != addr and target in self.ranges
                    if in_fn(target) and not tail:
                        leaders.add(target)
                        work.append(target)
                elif m == "jmp" and ops and ops[0].type == X.X86_OP_REG:
                    targets = self.switch_targets(path, in_fn)
                    if targets:
                        switches[ins.address] = targets
                        leaders.update(targets)
                        work.extend(targets)
                if m in self.TERMINATORS:
                    break
            else:
                # The decoder stopped without reaching a terminator: either it
                # hit the end of this chunk and a chained chunk continues, or
                # it gave up early and resumes from where it stopped. Nothing
                # decoded at all means the bytes at `a` are not an instruction.
                if path and in_fn(path[-1].address + path[-1].size):
                    work.append(path[-1].address + path[-1].size)
        return [seen[k] for k in sorted(seen)], leaders, switches

    def switch_targets(self, path, in_fn):
        """Targets of a compiler-generated switch ending in `jmp reg`.

        The compiler's shape is: load a 32-bit table entry indexed by the case
        number, add the image base, jump. Entries are image-relative.
        """
        jmp = path[-1]
        reg = jmp.reg_name(jmp.operands[0].reg)
        for ins in reversed(path[-8:-1]):
            ops = ins.operands
            if (ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X.X86_OP_REG
                    and REG.get(ins.reg_name(ops[0].reg), (None,))[0] == REG[reg][0]
                    and ops[1].type == X.X86_OP_MEM and ops[1].size == 4 and ops[1].mem.scale == 4
                    and ops[1].mem.base and ops[1].mem.index):
                table = self.img.base + ops[1].mem.disp
                targets = []
                for i in range(4096):
                    t = self.img.base + self.img.u32(table + 4 * i)
                    if not in_fn(t):
                        break
                    targets.append(t)
                return sorted(set(targets))
        return None

    # ----------------------------------------------------------- operands
    @staticmethod
    def R(idx):
        return f"c->r[{idx}]"

    def rdreg(self, name):
        idx, bits, shift = REG[name]
        if bits == 64:
            return self.R(idx)
        if shift:
            return f"(uint8_t)({self.R(idx)} >> 8)"
        return f"(uint{bits}_t){self.R(idx)}"

    def wrreg(self, name, expr):
        idx, bits, shift = REG[name]
        r = self.R(idx)
        if bits == 64:
            return f"{r} = {expr};"
        if bits == 32:  # a 32-bit write clears the upper half
            return f"{r} = (uint32_t)({expr});"
        if shift:
            return f"{r} = ({r} & ~0xff00ull) | ((uint64_t)(uint8_t)({expr}) << 8);"
        mask = (1 << bits) - 1
        return f"{r} = ({r} & ~{mask:#x}ull) | (uint{bits}_t)({expr});"

    def ea(self, ins, op):
        m = op.mem
        if m.base == X.X86_REG_RIP:
            return f"{(ins.address + ins.size + m.disp) & MASK64:#x}ull"
        parts, narrow = [], False
        if m.segment == X.X86_REG_GS:
            parts.append("c->gs_base")
        elif m.segment == X.X86_REG_FS:
            raise Unsupported("fs segment")
        for reg, scale in ((m.base, 1), (m.index, m.scale)):
            if reg:
                name = ins.reg_name(reg)
                narrow |= REG[name][1] == 32
                term = self.rdreg(name)
                parts.append(f"(uint64_t){term} * {scale}" if scale != 1 else term)
        if m.disp or not parts:
            parts.append(f"{m.disp & MASK64:#x}ull")
        expr = "(" + " + ".join(parts) + ")"
        return f"(uint64_t)(uint32_t){expr}" if narrow else expr

    def is_xmm(self, ins, op):
        return op.type == X.X86_OP_REG and ins.reg_name(op.reg).startswith("xmm")

    def rd(self, ins, op, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            return self.rdreg(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"(uint{bits}_t){op.imm & ((1 << bits) - 1):#x}ull"
        return f"ld{bits}({self.ea(ins, op)})"

    def wr(self, ins, op, expr):
        if op.type == X.X86_OP_REG:
            return self.wrreg(ins.reg_name(op.reg), expr)
        return f"st{op.size * 8}({self.ea(ins, op)}, {expr});"

    def xr(self, ins, op):
        name = ins.reg_name(op.reg)
        if not name.startswith("xmm"):
            raise Unsupported(f"vector register {name}")
        return f"c->x[{int(name[3:])}]"

    def f64(self, ins, op):
        return f"{self.xr(ins, op)}.f64[0]" if op.type == X.X86_OP_REG else f"ldf64({self.ea(ins, op)})"

    def f32(self, ins, op):
        return f"{self.xr(ins, op)}.f32[0]" if op.type == X.X86_OP_REG else f"ldf32({self.ea(ins, op)})"

    def v128(self, ins, op):
        return self.xr(ins, op) if op.type == X.X86_OP_REG else f"ld128({self.ea(ins, op)})"

    @staticmethod
    def zsp(bits):
        return f"zf = r == 0; sf = (r >> {bits - 1}) & 1; pf = parity8(r);"

    # ------------------------------------------------------- integer ops
    def alu(self, ins, kind):
        d, s = ins.operands
        b = d.size * 8
        T = f"uint{b}_t"
        L = [f"{T} a = {self.rd(ins, d)}, b = {self.rd(ins, s, b)};"]
        if kind == "add":
            L += [f"{T} r = a + b; cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> {b - 1}) & 1;"]
        elif kind == "adc":
            L += [f"uint8_t ci = cf; {T} r = a + b + ci; cf = ci ? r <= a : r < a;"
                  f" of = ((~(a ^ b) & (a ^ r)) >> {b - 1}) & 1;"]
        elif kind in ("sub", "cmp"):
            L += [f"{T} r = a - b; cf = a < b; of = (((a ^ b) & (a ^ r)) >> {b - 1}) & 1;"]
        elif kind == "sbb":
            L += [f"uint8_t ci = cf; {T} r = a - b - ci; cf = ci ? a <= b : a < b;"
                  f" of = (((a ^ b) & (a ^ r)) >> {b - 1}) & 1;"]
        else:
            c_op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[kind]
            L += [f"{T} r = a {c_op} b; cf = 0; of = 0;"]
        L.append(self.zsp(b))
        if kind not in ("cmp", "test"):
            L.append(self.wr(ins, d, "r"))
        return " ".join(L)

    def unary(self, ins, kind):
        d = ins.operands[0]
        b = d.size * 8
        T = f"uint{b}_t"
        sign = f"(({T})1 << {b - 1})"
        L = [f"{T} a = {self.rd(ins, d)};"]
        if kind == "not":
            return L[0] + " " + self.wr(ins, d, f"({T})~a")
        if kind == "neg":
            L.append(f"{T} r = 0 - a; cf = a != 0; of = a == {sign};")
        elif kind == "inc":
            L.append(f"{T} r = a + 1; of = r == {sign};")
        else:
            L.append(f"{T} r = a - 1; of = a == {sign};")
        return " ".join(L + [self.zsp(b), self.wr(ins, d, "r")])

    def shift(self, ins, kind):
        d = ins.operands[0]
        b = d.size * 8
        T, S = f"uint{b}_t", f"int{b}_t"
        count = self.rd(ins, ins.operands[1], 8) if len(ins.operands) > 1 else "1"
        L = [f"{T} a = {self.rd(ins, d)}; uint8_t n = ({count}) & {63 if b == 64 else 31};"]
        if kind in ("rcl", "rcr"):
            # Rotate through the carry flag, one bit at a time: rare enough
            # that clarity matters more than speed.
            step = (f"uint8_t out = (a >> {b - 1}) & 1; a = ({T})((a << 1) | cf); cf = out;" if kind == "rcl" else
                    f"uint8_t out = a & 1; a = ({T})((a >> 1) | (({T})cf << {b - 1})); cf = out;")
            L.append(f"n %= {b + 1}; for (uint8_t i = 0; i < n; i++) {{ {step} }} {self.wr(ins, d, 'a')}")
            return " ".join(L)
        if kind in ("rol", "ror"):
            if b == 32 and d.type == X.X86_OP_REG:
                L.append(self.wr(ins, d, "a"))
            # A count that is a multiple of the width leaves the value alone
            # but still sets CF from it.
            rot = (f"({T})((a << n) | (a >> ({b} - n)))" if kind == "rol" else
                   f"({T})((a >> n) | (a << ({b} - n)))")
            carry = "r & 1" if kind == "rol" else f"(r >> {b - 1}) & 1"
            L.append(f"if (n) {{ n %= {b}; {T} r = n ? {rot} : a; cf = {carry}; {self.wr(ins, d, 'r')} }}")
            return " ".join(L)
        if kind in ("shl", "sal"):
            body = (f"{T} r = a << n; cf = (a >> (64 - n)) & 1;" if b == 64 else
                    f"uint64_t w = (uint64_t)a << n; {T} r = ({T})w; cf = (w >> {b}) & 1;")
            body += f" of = ((r >> {b - 1}) & 1) ^ cf;"
        elif kind == "shr":
            body = f"{T} r = ({T})((uint64_t)a >> n); cf = ((uint64_t)a >> (n - 1)) & 1; of = (a >> {b - 1}) & 1;"
        else:
            body = f"int64_t sa = ({S})a; {T} r = ({T})(sa >> n); cf = (sa >> (n - 1)) & 1; of = 0;"
        L.append(f"if (n) {{ {body} {self.zsp(b)} {self.wr(ins, d, 'r')} }}")
        if b == 32 and d.type == X.X86_OP_REG:  # a zero count still zero-extends the register
            L.append("else { " + self.wr(ins, d, "a") + " }")
        return " ".join(L)

    def imul(self, ins):
        ops = ins.operands
        b = ops[0].size * 8
        if len(ops) == 1:
            if b == 64:
                return (f"__int128 p = (__int128)(int64_t)c->r[0] * (int64_t){self.rd(ins, ops[0])};"
                        " c->r[0] = (uint64_t)p; c->r[2] = (uint64_t)(p >> 64); cf = of = p != (__int128)(int64_t)p;")
            if b == 32:
                return (f"int64_t p = (int64_t)(int32_t)c->r[0] * (int32_t){self.rd(ins, ops[0])};"
                        " c->r[0] = (uint32_t)p; c->r[2] = (uint32_t)(p >> 32); cf = of = p != (int32_t)p;")
            raise Unsupported("imul r/m8 or r/m16")
        a, bsrc = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
        S = f"int{b}_t"
        return (f"{S} r; cf = of = __builtin_mul_overflow(({S}){self.rd(ins, a)}, ({S}){self.rd(ins, bsrc, b)}, &r); "
                + self.wr(ins, ops[0], f"(uint{b}_t)r"))

    def muldiv(self, ins, kind):
        s = ins.operands[0]
        b = s.size * 8
        if b not in (32, 64):
            raise Unsupported(f"{kind} r/m{b}")
        src = self.rd(ins, s)
        if kind == "mul":
            if b == 64:
                return (f"unsigned __int128 p = (unsigned __int128)c->r[0] * {src}; c->r[0] = (uint64_t)p;"
                        " c->r[2] = (uint64_t)(p >> 64); cf = of = c->r[2] != 0;")
            return (f"uint64_t p = (uint64_t)(uint32_t)c->r[0] * {src}; c->r[0] = (uint32_t)p;"
                    " c->r[2] = p >> 32; cf = of = c->r[2] != 0;")
        signed = kind == "idiv"
        if b == 64:
            W = "__int128" if signed else "unsigned __int128"
            D = "(int64_t)" if signed else ""
            return (f"{W} n = ({W})(((unsigned __int128)c->r[2] << 64) | c->r[0]); {W} d = {D}{src};"
                    f" if (d == 0) recomp_trap(c, {ins.address:#x}ull);"
                    " c->r[0] = (uint64_t)(n / d); c->r[2] = (uint64_t)(n % d);")
        W = "int64_t" if signed else "uint64_t"
        D = "(int32_t)" if signed else ""
        return (f"{W} n = ({W})((c->r[2] << 32) | (uint32_t)c->r[0]); {W} d = {D}{src};"
                f" if (d == 0) recomp_trap(c, {ins.address:#x}ull);"
                " c->r[0] = (uint32_t)(n / d); c->r[2] = (uint32_t)(n % d);")

    # --------------------------------------------------------------- SSE
    def sse_mov_scalar(self, ins, lane):  # movsd / movss
        d, s = ins.operands
        ld, st, zero = ("ld64", "st64", "") if lane == "q" else ("ld32", "st32", " {D}.d[1] = 0;")
        if d.type == X.X86_OP_MEM:
            return f"{st}({self.ea(ins, d)}, {self.xr(ins, s)}.{lane}[0]);"
        D = self.xr(ins, d)
        if s.type == X.X86_OP_MEM:
            return f"{D}.{lane}[0] = {ld}({self.ea(ins, s)});" + zero.format(D=D) + f" {D}.q[1] = 0;"
        return f"{D}.{lane}[0] = {self.xr(ins, s)}.{lane}[0];"

    def sse_mov128(self, ins):
        d, s = ins.operands
        if d.type == X.X86_OP_MEM:
            return f"st128({self.ea(ins, d)}, {self.xr(ins, s)});"
        return f"{self.xr(ins, d)} = {self.v128(ins, s)};"

    def sse_movq_movd(self, ins, bits):
        d, s = ins.operands
        if self.is_xmm(ins, d):
            D = self.xr(ins, d)
            src = f"{self.xr(ins, s)}.q[0]" if self.is_xmm(ins, s) else self.rd(ins, s, bits)
            if bits == 32 and self.is_xmm(ins, s):
                src = f"{self.xr(ins, s)}.d[0]"
            return f"{D}.q[0] = {src}; {D}.q[1] = 0;"
        lane = "q[0]" if bits == 64 else "d[0]"
        return self.wr(ins, d, f"{self.xr(ins, s)}.{lane}")

    def sse_scalar(self, ins, c_op, prec):
        d, s = ins.operands
        src = self.f64(ins, s) if prec == 64 else self.f32(ins, s)
        D = f"{self.xr(ins, d)}.f{prec}[0]"
        if c_op == "sqrt":
            return f"{D} = x86_sqrt{prec}({src});"
        if c_op in ("<", ">"):  # min / max: the source wins on NaN and on equality
            T = "double" if prec == 64 else "float"
            return f"{T} s = {src}; {D} = ({D} {c_op} s) ? {D} : s;"
        return f"{D} = x86_{self.FP_NAME[c_op]}{prec}({D}, {src});"

    FP_NAME = {"+": "add", "-": "sub", "*": "mul", "/": "div"}

    def sse_packed(self, ins, c_op, prec):
        d, s = ins.operands
        D = self.xr(ins, d)
        lanes = range(2 if prec == 64 else 4)
        return f"xmm_t s = {self.v128(ins, s)}; " + " ".join(
            f"{D}.f{prec}[{i}] = x86_{self.FP_NAME[c_op]}{prec}({D}.f{prec}[{i}], s.f{prec}[{i}]);" for i in lanes)

    def sse_bitwise(self, ins, kind):
        d, s = ins.operands
        D = self.xr(ins, d)
        fmt = {"xor": "{D}.q[{i}] ^= s.q[{i}];", "and": "{D}.q[{i}] &= s.q[{i}];",
               "or": "{D}.q[{i}] |= s.q[{i}];", "andn": "{D}.q[{i}] = ~{D}.q[{i}] & s.q[{i}];"}[kind]
        return f"xmm_t s = {self.v128(ins, s)}; " + " ".join(fmt.format(D=D, i=i) for i in (0, 1))

    def sse_compare(self, ins, prec):
        d, s = ins.operands
        T = "double" if prec == 64 else "float"
        src = self.f64(ins, s) if prec == 64 else self.f32(ins, s)
        return (f"{T} a = {self.xr(ins, d)}.f{prec}[0], b = {src}; of = sf = 0;"
                " if (a != a || b != b) { zf = pf = cf = 1; } else { zf = a == b; cf = a < b; pf = 0; }")

    def sse_convert(self, ins):
        m = ins.mnemonic
        d, s = ins.operands
        if m in ("cvtsi2sd", "cvtsi2ss"):
            T = "double" if m.endswith("sd") else "float"
            lane = "f64" if m.endswith("sd") else "f32"
            return f"{self.xr(ins, d)}.{lane}[0] = ({T})(int{s.size * 8}_t){self.rd(ins, s)};"
        if m in ("cvttsd2si", "cvtsd2si", "cvttss2si", "cvtss2si"):
            fn = ("cvtt" if m.startswith("cvtt") else "cvt") + ("_f64" if "sd" in m else "_f32") + f"_i{d.size * 8}"
            src = self.f64(ins, s) if "sd" in m else self.f32(ins, s)
            return self.wr(ins, d, f"{fn}({src})")
        if m == "cvtsd2ss":
            return f"{self.xr(ins, d)}.f32[0] = x86_f64_to_f32({self.f64(ins, s)});"
        if m == "cvtss2sd":
            return f"{self.xr(ins, d)}.f64[0] = x86_f32_to_f64({self.f32(ins, s)});"
        D = self.xr(ins, d)
        if m == "cvtdq2pd":
            src = f"{self.xr(ins, s)}.q[0]" if self.is_xmm(ins, s) else f"ld64({self.ea(ins, s)})"
            return (f"uint64_t s = {src}; {D}.f64[0] = (double)(int32_t)s;"
                    f" {D}.f64[1] = (double)(int32_t)(s >> 32);")
        if m == "cvtps2pd":
            src = f"{self.xr(ins, s)}.q[0]" if self.is_xmm(ins, s) else f"ld64({self.ea(ins, s)})"
            return (f"xmm_t s; s.q[0] = {src}; {D}.f64[1] = x86_f32_to_f64(s.f32[1]); {D}.f64[0] = x86_f32_to_f64(s.f32[0]);")
        if m == "cvtpd2ps":
            return (f"xmm_t s = {self.v128(ins, s)}; {D}.f32[0] = x86_f64_to_f32(s.f64[0]);"
                    f" {D}.f32[1] = x86_f64_to_f32(s.f64[1]); {D}.q[1] = 0;")
        if m == "cvtdq2ps":
            return f"xmm_t s = {self.v128(ins, s)}; " + " ".join(
                f"{D}.f32[{i}] = (float)(int32_t)s.d[{i}];" for i in range(4))
        if m in ("cvttps2dq", "cvtps2dq"):
            fn = "cvtt_f32_i32" if m == "cvttps2dq" else "cvt_f32_i32"
            return f"xmm_t s = {self.v128(ins, s)}; " + " ".join(
                f"{D}.d[{i}] = {fn}(s.f32[{i}]);" for i in range(4))
        if m in ("cvttpd2dq", "cvtpd2dq"):
            fn = "cvtt_f64_i32" if m == "cvttpd2dq" else "cvt_f64_i32"
            return (f"xmm_t s = {self.v128(ins, s)}; {D}.d[0] = {fn}(s.f64[0]);"
                    f" {D}.d[1] = {fn}(s.f64[1]); {D}.q[1] = 0;")
        raise Unsupported(m)

    # ------------------------------------------- bit tests, atomics, strings
    def bittest(self, ins, kind):
        d, s = ins.operands
        b = d.size * 8
        T = f"uint{b}_t"
        change = {"bt": None, "bts": "a | m", "btr": "a & ~m", "btc": "a ^ m"}[kind]
        locked = ins.prefix[0] == 0xF0
        if d.type == X.X86_OP_REG or s.type == X.X86_OP_IMM:
            if locked and d.type == X.X86_OP_MEM:
                fetch = {"bts": "or", "btr": "and", "btc": "xor"}[kind]
                arg = "~m" if kind == "btr" else "m"
                return (f"{T} m = ({T})1 << (({self.rd(ins, s, b)}) & {b - 1});"
                        f" {T} a = __atomic_fetch_{fetch}(({T} *){self.ea(ins, d)}, ({T}){arg}, __ATOMIC_SEQ_CST);"
                        " cf = (a & m) != 0;")
            L = (f"{T} a = {self.rd(ins, d)}; {T} m = ({T})1 << (({self.rd(ins, s, b)}) & {b - 1});"
                 " cf = (a & m) != 0;")
            return L + (" " + self.wr(ins, d, f"({T})({change})") if change else "")
        # Memory operand with a register index: the index selects a bit
        # anywhere relative to the address, not only inside one operand.
        L = (f"int64_t i = (int64_t)(int{b}_t){self.rd(ins, s)};"
             f" uint64_t p = {self.ea(ins, d)} + (uint64_t)((i >> {b.bit_length() - 1}) * {b // 8});"
             f" {T} m = ({T})1 << (i & {b - 1});")
        if locked:
            fetch = {"bts": "or", "btr": "and", "btc": "xor"}[kind]
            arg = "~m" if kind == "btr" else "m"
            return L + f" {T} a = __atomic_fetch_{fetch}(({T} *)p, ({T}){arg}, __ATOMIC_SEQ_CST); cf = (a & m) != 0;"
        L += f" {T} a = ld{b}(p); cf = (a & m) != 0;"
        return L + (f" st{b}(p, ({T})({change}));" if change else "")

    def atomic(self, ins):
        """Instructions that update memory atomically: lock-prefixed ones,
        cmpxchg, xadd, and xchg with a memory operand."""
        m = ins.mnemonic.replace("lock ", "")
        ops = ins.operands
        if m in ("bts", "btr", "btc"):
            return self.bittest(ins, m)
        mem = next((o for o in ops if o.type == X.X86_OP_MEM), None)
        if mem is None:
            raise Unsupported(f"{m} without a memory operand")
        b = mem.size * 8
        T = f"uint{b}_t"
        p = f"({T} *){self.ea(ins, mem)}"
        sign = f"(({T})1 << {b - 1})"
        seq = "__ATOMIC_SEQ_CST"
        if m == "xchg":
            reg = next(o for o in ops if o.type == X.X86_OP_REG)
            return f"{T} v = __atomic_exchange_n({p}, {self.rd(ins, reg)}, {seq}); " + self.wr(ins, reg, "v")
        if m == "cmpxchg":
            acc = {8: "al", 16: "ax", 32: "eax", 64: "rax"}[b]
            return (f"{T} a = {self.rdreg(acc)}; {T} e = a;"
                    f" uint8_t ok = __atomic_compare_exchange_n({p}, &e, {self.rd(ins, ops[1])}, 0, {seq}, {seq});"
                    f" {T} r = a - e; cf = a < e; of = (((a ^ e) & (a ^ r)) >> {b - 1}) & 1; {self.zsp(b)}"
                    f" if (!ok) {{ {self.wrreg(acc, 'e')} }}")
        if m in ("inc", "dec", "add", "sub", "xadd"):
            operand = "1" if m in ("inc", "dec") else self.rd(ins, ops[1], b)
            if m in ("dec", "sub"):
                L = (f"{T} b = {operand}; {T} a = __atomic_fetch_sub({p}, b, {seq}); {T} r = a - b;"
                     + (f" of = a == {sign};" if m == "dec" else
                        f" cf = a < b; of = (((a ^ b) & (a ^ r)) >> {b - 1}) & 1;"))
            else:
                L = (f"{T} b = {operand}; {T} a = __atomic_fetch_add({p}, b, {seq}); {T} r = a + b;"
                     + (f" of = r == {sign};" if m == "inc" else
                        f" cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> {b - 1}) & 1;"))
            L += " " + self.zsp(b)
            return L + (" " + self.wr(ins, ops[1], "a") if m == "xadd" else "")
        if m in ("or", "and", "xor"):
            c_op = {"or": "|", "and": "&", "xor": "^"}[m]
            return (f"{T} b = {self.rd(ins, ops[1], b)}; {T} a = __atomic_fetch_{m}({p}, b, {seq});"
                    f" {T} r = a {c_op} b; cf = 0; of = 0; {self.zsp(b)}")
        raise Unsupported(f"lock {m}")

    def string_op(self, ins):
        parts = ins.mnemonic.split()
        op = parts[-1]
        if op[:4] in ("scas", "cmps") and len(op) == 5 and op[4] in "bwdq" and len(parts) == 2:
            # Compare until the count runs out or the condition fails. Flags
            # come from the last comparison; a zero count leaves them alone.
            b = {"b": 8, "w": 16, "d": 32, "q": 64}[op[4]]
            n, T = b // 8, f"uint{b}_t"
            stop = "a != b" if parts[0] in ("repe", "repz") else "a == b"
            if op.startswith("scas"):
                load = f"{T} a = ({T})c->r[0], b = ld{b}(c->r[7]); c->r[7] += {n};"
            else:
                load = f"{T} a = ld{b}(c->r[6]), b = ld{b}(c->r[7]); c->r[6] += {n}; c->r[7] += {n};"
            return (f"while (c->r[1]) {{ c->r[1]--; {load} {T} r = a - b; cf = a < b;"
                    f" of = (((a ^ b) & (a ^ r)) >> {b - 1}) & 1; {self.zsp(b)} if ({stop}) break; }}")
        if parts[0] in ("repe", "repne", "repz", "repnz") or op[:4] not in ("movs", "stos"):
            raise Unsupported(ins.mnemonic)
        b = {"b": 8, "w": 16, "d": 32, "q": 64}[op[4]]
        n = b // 8
        if op.startswith("movs"):
            step = f"st{b}(c->r[7], ld{b}(c->r[6])); c->r[6] += {n}; c->r[7] += {n};"
        else:
            step = f"st{b}(c->r[7], (uint{b}_t)c->r[0]); c->r[7] += {n};"
        return f"while (c->r[1]) {{ {step} c->r[1]--; }}" if parts[0] == "rep" else step

    # ------------------------------------------------------ more SSE forms
    LANES = {"b": ("b", 16, 8), "w": ("w", 8, 16), "d": ("d", 4, 32), "q": ("q", 2, 64)}
    PRED = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "(a != a || b != b)",
            "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "(a == a && b == b)"}

    def sse_misc(self, ins):
        m = ins.mnemonic
        ops = ins.operands
        if not ops or not self.is_xmm(ins, ops[0]):
            if m in ("pmovmskb", "movmskps", "movmskpd") and len(ops) == 2:
                lane, count, bits = {"pmovmskb": ("b", 16, 8), "movmskps": ("d", 4, 32), "movmskpd": ("q", 2, 64)}[m]
                S = self.xr(ins, ops[1])
                expr = " | ".join(f"(uint32_t)(({S}.{lane}[{i}] >> {bits - 1}) & 1) << {i}" for i in range(count))
                return self.wr(ins, ops[0], f"(uint64_t)({expr})")
            if m in ("movlps", "movlpd", "movhps", "movhpd") and len(ops) == 2:
                return f"st64({self.ea(ins, ops[0])}, {self.xr(ins, ops[1])}.q[{0 if m[3] == 'l' else 1}]);"
            if m in ("movntdq", "movntps", "movntpd") and len(ops) == 2:
                return self.sse_mov128(ins)
            return None
        D = self.xr(ins, ops[0])
        S = self.v128(ins, ops[1]) if len(ops) > 1 and ops[1].type != X.X86_OP_IMM else None
        imm = ops[-1].imm & 0xFF if ops[-1].type == X.X86_OP_IMM else None
        pre = f"xmm_t a = {D}, s = {S}; (void)a; " if S else f"xmm_t a = {D}; "

        if m in ("lddqu", "movntdqa"):
            return self.sse_mov128(ins)
        if m == "shufps":
            return pre + (f"{D}.d[0] = a.d[{imm & 3}]; {D}.d[1] = a.d[{imm >> 2 & 3}];"
                          f" {D}.d[2] = s.d[{imm >> 4 & 3}]; {D}.d[3] = s.d[{imm >> 6 & 3}];")
        if m == "shufpd":
            return pre + f"{D}.q[0] = a.q[{imm & 1}]; {D}.q[1] = s.q[{imm >> 1 & 1}];"
        if m == "pshufd":
            return pre + " ".join(f"{D}.d[{i}] = s.d[{imm >> 2 * i & 3}];" for i in range(4))
        if m in ("pshuflw", "pshufhw"):
            base, keep = (0, 1) if m == "pshuflw" else (4, 0)
            return pre + " ".join(f"{D}.w[{base + i}] = s.w[{base + (imm >> 2 * i & 3)}];" for i in range(4)) \
                + f" {D}.q[{keep}] = s.q[{keep}];"
        if m in ("packuswb", "packsswb"):
            lo, hi, T = (0, 255, "uint8_t") if m == "packuswb" else (-128, 127, "int8_t")
            clamp = lambda v: f"(uint8_t)({T})((int16_t){v} < {lo} ? {lo} : (int16_t){v} > {hi} ? {hi} : (int16_t){v})"
            return pre + " ".join(f"{D}.b[{i}] = {clamp(f'a.w[{i}]')}; {D}.b[{8 + i}] = {clamp(f's.w[{i}]')};"
                                  for i in range(8))
        if m in ("unpcklps", "punpckldq"):
            return pre + f"{D}.d[0] = a.d[0]; {D}.d[1] = s.d[0]; {D}.d[2] = a.d[1]; {D}.d[3] = s.d[1];"
        if m in ("unpckhps", "punpckhdq"):
            return pre + f"{D}.d[0] = a.d[2]; {D}.d[1] = s.d[2]; {D}.d[2] = a.d[3]; {D}.d[3] = s.d[3];"
        if m in ("unpcklpd", "punpcklqdq", "movlhps"):
            return pre + f"{D}.q[1] = s.q[0];"
        if m in ("unpckhpd", "punpckhqdq"):
            return pre + f"{D}.q[0] = a.q[1]; {D}.q[1] = s.q[1];"
        if m == "movhlps":
            return pre + f"{D}.q[0] = s.q[1];"
        if m in ("punpcklbw", "punpckhbw", "punpcklwd", "punpckhwd"):
            lane, count, _ = self.LANES[m[7]]
            base = 0 if m[6] == "l" else count // 2
            return pre + " ".join(f"{D}.{lane}[{2 * i}] = a.{lane}[{base + i}]; {D}.{lane}[{2 * i + 1}] = s.{lane}[{base + i}];"
                                  for i in range(count // 2))
        if m in ("movlps", "movlpd"):
            return f"{D}.q[0] = ld64({self.ea(ins, ops[1])});"
        if m in ("movhps", "movhpd"):
            return f"{D}.q[1] = ld64({self.ea(ins, ops[1])});"
        if m in ("sqrtpd", "sqrtps"):
            lane, fn, n = ("f64", "sqrt64", 2) if m == "sqrtpd" else ("f32", "sqrt32", 4)
            return pre + " ".join(f"{D}.{lane}[{i}] = x86_{fn}(s.{lane}[{i}]);" for i in range(n))
        if m in ("minps", "maxps", "minpd", "maxpd"):
            lane, n = ("f64", 2) if m.endswith("pd") else ("f32", 4)
            c_op = "<" if m.startswith("min") else ">"
            return pre + " ".join(f"{D}.{lane}[{i}] = (a.{lane}[{i}] {c_op} s.{lane}[{i}]) ? a.{lane}[{i}] : s.{lane}[{i}];"
                                  for i in range(n))
        if m in ("psrldq", "pslldq"):
            n = min(imm, 16)
            src = (lambda i: i + n) if m == "psrldq" else (lambda i: i - n)
            return pre + " ".join(f"{D}.b[{i}] = " + (f"a.b[{src(i)}];" if 0 <= src(i) < 16 else "0;") for i in range(16))
        if len(m) == 5 and m[:4] in ("padd", "psub") and m[4] in self.LANES:
            lane, count, bits = self.LANES[m[4]]
            c_op = "+" if m[:4] == "padd" else "-"
            return pre + " ".join(f"{D}.{lane}[{i}] = (uint{bits}_t)(a.{lane}[{i}] {c_op} s.{lane}[{i}]);" for i in range(count))
        if len(m) == 7 and m[:6] in ("pcmpeq", "pcmpgt") and m[6] in "bwd":
            lane, count, bits = self.LANES[m[6]]
            cmp = (lambda i: f"a.{lane}[{i}] == s.{lane}[{i}]") if m[:6] == "pcmpeq" else \
                  (lambda i: f"(int{bits}_t)a.{lane}[{i}] > (int{bits}_t)s.{lane}[{i}]")
            return pre + " ".join(f"{D}.{lane}[{i}] = ({cmp(i)}) ? (uint{bits}_t)-1 : 0;" for i in range(count))
        if len(m) == 5 and m[:4] in ("psll", "psrl", "psra") and m[4] in "wdq":
            lane, count, bits = self.LANES[m[4]]
            n = f"{imm}u" if imm is not None else "s.q[0]"
            head = (f"xmm_t a = {D}; " if imm is not None else pre) + f"uint64_t n = {n}; "
            if m[:4] == "psra":
                return head + " ".join(f"{D}.{lane}[{i}] = (uint{bits}_t)((int{bits}_t)a.{lane}[{i}] >> (n >= {bits} ? {bits - 1} : n));"
                                       for i in range(count))
            c_op = "<<" if m[:4] == "psll" else ">>"
            return head + " ".join(f"{D}.{lane}[{i}] = n >= {bits} ? 0 : (uint{bits}_t)(a.{lane}[{i}] {c_op} n);"
                                   for i in range(count))
        if m.startswith("cmp") and m[-2:] in ("ps", "pd", "ss", "sd") and m[3:-2] in self.PRED:
            wide = m[-1] == "d"
            lane, ilane, T, ones = ("f64", "q", "double", "~0ull") if wide else ("f32", "d", "float", "~0u")
            n = 1 if m[-2] == "s" else (2 if wide else 4)
            if n == 1:
                src = self.f64(ins, ops[1]) if wide else self.f32(ins, ops[1])
                return f"{T} a = {D}.{lane}[0], b = {src}; {D}.{ilane}[0] = ({self.PRED[m[3:-2]]}) ? {ones} : 0;"
            return pre + " ".join(f"{{ {T} a = {D}.{lane}[{i}], b = s.{lane}[{i}]; {D}.{ilane}[{i}] = ({self.PRED[m[3:-2]]}) ? {ones} : 0; }}"
                                  for i in range(n))
        if m in ("roundsd", "roundss"):
            wide = m == "roundsd"
            fn = {0: "nearbyint", 1: "floor", 2: "ceil", 3: "trunc"}[imm & 3 if not imm & 4 else 0] + ("" if wide else "f")
            src = self.f64(ins, ops[1]) if wide else self.f32(ins, ops[1])
            return f"{D}.{'f64' if wide else 'f32'}[0] = {fn}({src});"
        return None

    # ------------------------------------------------------ one instruction
    def insn(self, ins, in_fn, known):
        m = ins.mnemonic
        ops = ins.operands
        a = ins.address
        nxt = a + ins.size
        any_xmm = any(self.is_xmm(ins, o) for o in ops)

        if ins.prefix[0] == 0xF0 or m in ("cmpxchg", "xadd") or (
                m == "xchg" and any(o.type == X.X86_OP_MEM for o in ops)):
            return self.atomic(ins)
        if m in ("bt", "bts", "btr", "btc"):
            return self.bittest(ins, m)
        if m in ("bsf", "bsr"):
            b = ops[0].size * 8
            scan = (f"__builtin_ctzll(s)" if m == "bsf" else f"63 - __builtin_clzll(s)")
            # With a zero source the destination keeps its value. Whether a
            # 32-bit destination still gets its upper half cleared differs
            # between processors; this follows the reference emulator.
            keep = f" else {{ {self.wr(ins, ops[0], self.rd(ins, ops[0]))} }}" if b == 32 else ""
            return (f"uint64_t s = {self.rd(ins, ops[1])}; zf = s == 0;"
                    f" if (s) {{ {self.wr(ins, ops[0], f'(uint{b}_t)({scan})')} }}{keep}")
        if m == "bswap":
            b = ops[0].size * 8
            return self.wr(ins, ops[0], f"__builtin_bswap{b}({self.rd(ins, ops[0])})")
        last = m.split()[-1]
        if (last[:4] in ("movs", "stos") and len(last) == 5 and last[4] in "bwdq" and not any_xmm) or m.startswith("rep"):
            return self.string_op(ins)
        if m == "cpuid":
            return "recomp_cpuid(c);"
        if m == "xgetbv":
            return "c->r[0] = 0; c->r[2] = 0;"
        if m == "rdtsc":
            return "uint64_t v = recomp_rdtsc(); c->r[0] = (uint32_t)v; c->r[2] = v >> 32;"
        if m == "fnstcw":   # the x87 control word: default precision and rounding, all exceptions masked
            return f"st16({self.ea(ins, ops[0])}, 0x027f);"
        if m in ("fldcw", "fnclex", "emms"):
            return ""
        if m == "stmxcsr":
            return f"st32({self.ea(ins, ops[0])}, 0x1f80u);"
        if m == "ldmxcsr":
            return f"recomp_ldmxcsr(c, ld32({self.ea(ins, ops[0])}));"
        if m == "repz ret":
            return "c->r[4] += 8; return;"
        if m in ("nop", "endbr64", "pause", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "prefetchw"):
            return ""
        if m in ("int3", "ud2", "hlt"):
            return f"recomp_trap(c, {a:#x}ull);"
        if m in ("mov", "movabs"):
            if any(o.type == X.X86_OP_REG and ins.reg_name(o.reg) not in REG for o in ops):
                raise Unsupported("mov with segment or control register")
            return self.wr(ins, ops[0], self.rd(ins, ops[1], ops[0].size * 8))
        if m == "lea":
            if ops[1].mem.base == X.X86_REG_RIP and self.text_lo <= nxt + ops[1].mem.disp < self.text_hi:
                self.code_refs.add(nxt + ops[1].mem.disp)
            return self.wr(ins, ops[0], self.ea(ins, ops[1]))
        if m == "movzx":
            return self.wr(ins, ops[0], self.rd(ins, ops[1]))
        if m in ("movsx", "movsxd"):
            return self.wr(ins, ops[0], f"(uint64_t)(int64_t)(int{ops[1].size * 8}_t){self.rd(ins, ops[1])}")
        if m in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test") and ins.prefix[0] != 0xF0:
            return self.alu(ins, m)
        if m in ("inc", "dec", "neg", "not") and ins.prefix[0] != 0xF0:
            return self.unary(ins, m)
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return self.shift(ins, m)
        if m == "imul":
            return self.imul(ins)
        if m in ("mul", "div", "idiv"):
            return self.muldiv(ins, m)
        if m == "cdqe":
            return "c->r[0] = (uint64_t)(int64_t)(int32_t)c->r[0];"
        if m == "cwde":
            return "c->r[0] = (uint32_t)(int32_t)(int16_t)c->r[0];"
        if m == "cdq":
            return "c->r[2] = (uint32_t)((int32_t)c->r[0] >> 31);"
        if m == "cqo":
            return "c->r[2] = (uint64_t)((int64_t)c->r[0] >> 63);"
        if m == "xchg" and ins.prefix[0] != 0xF0 and all(o.type == X.X86_OP_REG for o in ops):
            b = ops[0].size * 8
            return (f"uint{b}_t t = {self.rd(ins, ops[0])}; {self.wr(ins, ops[0], self.rd(ins, ops[1]))} "
                    + self.wr(ins, ops[1], "t"))
        if m in ("push", "pop") and ops[0].size != 8:
            raise Unsupported(f"{m} with a 16-bit operand")
        if m == "push":
            return f"uint64_t v = {self.rd(ins, ops[0], 64)}; c->r[4] -= 8; st64(c->r[4], v);"
        if m == "pop":
            return f"uint64_t v = ld64(c->r[4]); c->r[4] += 8; {self.wr(ins, ops[0], 'v')}"
        if m == "leave":
            return "c->r[4] = c->r[5]; c->r[5] = ld64(c->r[4]); c->r[4] += 8;"
        if m.startswith("set") and m[3:] in CC:
            return self.wr(ins, ops[0], f"(uint8_t)({CC[m[3:]]})")
        if m.startswith("cmov") and m[4:] in CC:
            return self.wr(ins, ops[0], f"{CC[m[4:]]} ? {self.rd(ins, ops[1])} : {self.rd(ins, ops[0])}")

        # ---- control flow
        if m == "ret":
            extra = ops[0].imm if ops else 0
            return f"c->r[4] += {8 + extra}; return;"
        if m == "call":
            push = f"c->r[4] -= 8; st64(c->r[4], {nxt:#x}ull);"
            if ops[0].type == X.X86_OP_IMM:
                known.add(ops[0].imm)
                if ops[0].imm in self.setjmp:
                    # The game's setjmp records its registers; the native one
                    # records this C frame, so a later longjmp can come back
                    # here with the game's registers already restored.
                    return (f"{push} if (_setjmp(*recomp_jmp_slot(c)) == 0) f_{ops[0].imm:x}(c);")
                return f"{push} f_{ops[0].imm:x}(c);"
            if ops[0].type == X.X86_OP_MEM and ops[0].mem.base == X.X86_REG_RIP:
                slot = nxt + ops[0].mem.disp
                if slot in self.imports:
                    return f"{push} recomp_call_import(c, {slot:#x}ull); /* {self.imports[slot]} */"
            return f"uint64_t t = {self.rd(ins, ops[0], 64)}; {push} recomp_call(c, t);"
        if m == "jmp":
            if ops[0].type == X.X86_OP_IMM:
                if in_fn(ops[0].imm):
                    return f"goto L_{ops[0].imm:x};"
                known.add(ops[0].imm)
                return f"f_{ops[0].imm:x}(c); return;"  # tail call
            if ops[0].type == X.X86_OP_MEM and ops[0].mem.base == X.X86_REG_RIP:
                slot = nxt + ops[0].mem.disp
                if slot in self.imports:
                    return f"recomp_call_import(c, {slot:#x}ull); return; /* {self.imports[slot]} */"
            if a in self.switches:
                cases = " ".join(f"case {t:#x}ull: goto L_{t:x};" for t in self.switches[a])
                return (f"switch ({self.rd(ins, ops[0], 64)}) {{ {cases} "
                        f"default: recomp_trap(c, {a:#x}ull); }}")
            # Anything else is a tail call through a register, a vtable slot
            # or a global function pointer.
            return f"uint64_t t = {self.rd(ins, ops[0], 64)}; recomp_call(c, t); return;"
        if m in ("jrcxz", "jecxz"):
            test = "c->r[1] == 0" if m == "jrcxz" else "(uint32_t)c->r[1] == 0"
            return f"if ({test}) goto L_{ops[0].imm:x};" if in_fn(ops[0].imm) else f"if ({test}) {{ f_{ops[0].imm:x}(c); return; }}"
        if m[0] == "j" and m[1:] in CC:
            if not in_fn(ops[0].imm):
                known.add(ops[0].imm)
                return f"if ({CC[m[1:]]}) {{ f_{ops[0].imm:x}(c); return; }}"
            return f"if ({CC[m[1:]]}) goto L_{ops[0].imm:x};"

        # ---- SSE
        if m == "movsd" and any_xmm:
            return self.sse_mov_scalar(ins, "q")
        if m == "movss":
            return self.sse_mov_scalar(ins, "d")
        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu"):
            return self.sse_mov128(ins)
        if m == "movq":
            return self.sse_movq_movd(ins, 64)
        if m == "movd":
            return self.sse_movq_movd(ins, 32)
        scalar = {"add": "+", "sub": "-", "mul": "*", "div": "/", "sqrt": "sqrt", "min": "<", "max": ">"}
        if m[-2:] in ("sd", "ss") and m[:-2] in scalar:
            return self.sse_scalar(ins, scalar[m[:-2]], 64 if m[-2:] == "sd" else 32)
        if m[-2:] in ("pd", "ps") and m[:-2] in ("add", "sub", "mul", "div"):
            return self.sse_packed(ins, scalar[m[:-2]], 64 if m[-2:] == "pd" else 32)
        bitwise = {"xorps": "xor", "xorpd": "xor", "pxor": "xor", "andps": "and", "andpd": "and", "pand": "and",
                   "orps": "or", "orpd": "or", "por": "or", "andnps": "andn", "andnpd": "andn", "pandn": "andn"}
        if m in bitwise:
            return self.sse_bitwise(ins, bitwise[m])
        if m in ("comisd", "ucomisd"):
            return self.sse_compare(ins, 64)
        if m in ("comiss", "ucomiss"):
            return self.sse_compare(ins, 32)
        if m.startswith("cvt"):
            return self.sse_convert(ins)
        text = self.sse_misc(ins)
        if text is not None:
            return text
        raise Unsupported(m)

    # ------------------------------------------------------- one function
    def function(self, addr):
        """Returns (C text, callee addresses, count of untranslated instructions)."""
        insns, leaders, self.switches = self.decode_flow(addr)
        starts = {i.address for i in insns}
        leaders &= starts

        def in_fn(a):
            return a in starts
        known, bad = set(), 0
        out = [f"void f_{addr:x}(cpu_t *c) {{",
               "    uint8_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0;",
               "    (void)cf; (void)zf; (void)sf; (void)of; (void)pf;"]
        prev_end = None
        for i in insns:
            if prev_end is not None and i.address != prev_end:
                out.append(f"    recomp_trap(c, {prev_end:#x}ull); /* not reached: the code before this does not fall through */")
            if i.address in leaders:
                out.append(f"L_{i.address:x}:;")
            text = f"{i.mnemonic} {i.op_str}".strip().replace("*/", "* /")
            try:
                body = self.insn(i, in_fn, known)
            except (Unsupported, KeyError, IndexError) as e:
                bad += 1
                self.missing[i.mnemonic if isinstance(e, Unsupported) and str(e) == i.mnemonic else f"{i.mnemonic}: {e}"] += 1
                body = f'recomp_unimplemented(c, {i.address:#x}ull, "{i.mnemonic}");'
            out.append(f"    {{ {body} }} /* {i.address:x} {text} */" if body else f"    /* {i.address:x} {text} */")
            prev_end = i.address + i.size
        out.append(f"    recomp_trap(c, {prev_end or addr:#x}ull); /* ran off the end */")
        out.append("}")
        return "\n".join(out), known, bad, len(insns)


def emit(path, addrs):
    t = Translator()
    bodies, callees = [], set()
    for a in addrs:
        text, known, bad, n = t.function(a)
        bodies.append(text)
        callees |= known
        print(f"f_{a:x}: {n} instructions" + (f", {bad} untranslated" if bad else ""))
    protos = "\n".join(f"void f_{a:x}(cpu_t *c);" for a in sorted(set(addrs) | callees))
    table = "\n".join(f"    {{{a:#x}ull, f_{a:x}}}," for a in sorted(addrs))
    Path(path).write_text(
        "// Generated by recomp/translate.py from SpaceEngine.exe. Do not edit.\n"
        '#include "x86.h"\n\n' + protos + "\n\n" + "\n\n".join(bodies) +
        "\n\nconst struct recomp_entry { uint64_t va; recomp_fn fn; } recomp_table[] = {\n" + table +
        f"\n}};\nconst unsigned recomp_table_count = {len(addrs)};\n")
    external = sorted(callees - set(addrs))
    print(f"wrote {path}: {len(addrs)} functions; calls {len(external)} not in this unit: "
          + " ".join(f"{a:#x}" for a in external))
    if t.missing:
        print("untranslated:", dict(t.missing))


def hle_addresses(tag=None):
    """Addresses listed in hle.txt: with tag None, the functions replaced by
    host code in runtime/hle.c; with a tag, the ones marked with that word."""
    out = set()
    for line in (ROOT / "recomp/hle.txt").read_text().splitlines():
        words = line.split("#")[0].split()
        if words and (words[1] if len(words) > 1 else None) == tag:
            out.add(int(words[0], 16))
    return out


def program(outdir, per_file=6000):
    """Translates every function into outdir: part_NNN.c files of about
    per_file instructions each, functions.h with all prototypes, and table.c
    mapping original addresses to translated functions."""
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.c"):
        old.unlink()
    t = Translator()
    hle = hle_addresses()
    # Functions host code runs around: translated as f_<address>_original;
    # the host's f_<address> calls it. table.c has a weak f_<address> that
    # only calls through, for builds without a host (the test drivers).
    wrapped = hle_addresses("wrap")
    text_lo, text_hi = t.text_lo, t.text_hi

    work = sorted(t.ranges)
    queued = set(work)
    done, stubs, discovered = {}, [], 0
    total = untranslated = 0
    while work:
        a = work.pop()
        if a in hle:
            continue
        text, known, bad, n = t.function(a)
        if n == 0:
            stubs.append(a)
            continue
        if a in wrapped:
            text = text.replace(f"void f_{a:x}(cpu_t *c) {{", f"void f_{a:x}_original(cpu_t *c) {{", 1)
        done[a] = text
        total += n
        untranslated += bad
        # Code whose address is taken (a callback handed to another function)
        # is a function too, unless it lies inside one .pdata already covers.
        for target in t.code_refs:
            if target not in queued and target not in hle and not t.in_pdata(target):
                queued.add(target)
                t.add_leaf(target)
                work.append(target)
                discovered += 1
        t.code_refs.clear()
        for target in known:
            if target not in queued and target not in hle:
                queued.add(target)
                if text_lo <= target < text_hi:
                    # A call or tail call to an address nothing listed as a
                    # function: one the earlier analysis missed.
                    t.add_leaf(target)
                    work.append(target)
                    discovered += 1
                else:
                    stubs.append(target)

    names = sorted(set(done) | set(stubs) | hle)
    (out / "functions.h").write_text(
        "// Generated by recomp/translate.py. Do not edit.\n#pragma once\n#include \"x86.h\"\n\n"
        + "\n".join(f"void f_{a:x}(cpu_t *c);" for a in names) + "\n"
        + "".join(f"void f_{a:x}_original(cpu_t *c);\n" for a in sorted(wrapped)))
    part, lines, count = 0, [], 0

    def flush():
        nonlocal part, lines, count
        if lines:
            (out / f"part_{part:03d}.c").write_text('#include "functions.h"\n\n' + "\n\n".join(lines) + "\n")
            part += 1
            lines, count = [], 0

    for a in sorted(done):
        lines.append(done[a])
        count += done[a].count("\n")
        if count >= per_file:
            flush()
    flush()
    (out / "table.c").write_text(
        '#include "functions.h"\n\n'
        + "\n".join(f"void f_{a:x}(cpu_t *c) {{ recomp_trap(c, {a:#x}ull); }}" for a in sorted(stubs))
        + "\n" + "\n".join(f"__attribute__((weak)) void f_{a:x}(cpu_t *c) {{ f_{a:x}_original(c); }}" for a in sorted(wrapped))
        + "\n\nconst struct recomp_entry { uint64_t va; recomp_fn fn; } recomp_table[] = {\n"
        + "\n".join(f"    {{{a:#x}ull, f_{a:x}}}," for a in names)
        + f"\n}};\nconst unsigned recomp_table_count = {len(names)};\n")
    size = sum(f.stat().st_size for f in out.glob("*.c"))
    print(f"{len(done):,} functions translated ({t.pointer_leaves} found through pointers in data, "
          f"{discovered} more through calls and address-taking), "
          f"{len(stubs)} stubs for targets with no code, {len(hle)} replaced by host code")
    print(f"{total:,} instructions, {untranslated:,} untranslated ({100 * untranslated / total:.2f}%)")
    print(f"{part} source files, {size / 1e6:.0f} MB of C in {out}")


def coverage():
    t = Translator()
    total = bad = clean = 0
    for a in sorted(t.ranges):
        _, _, b, n = t.function(a)
        total += n
        bad += b
        clean += b == 0
    print(f"{len(t.ranges):,} functions, {total:,} instructions")
    print(f"instructions translated: {total - bad:,} ({100 * (total - bad) / total:.2f}%)")
    print(f"functions fully translated: {clean:,} ({100 * clean / len(t.ranges):.1f}%)")
    listing = ROOT / "analysis/untranslated.tsv"
    listing.write_text("".join(f"{n}\t{k}\n" for k, n in t.missing.most_common()))
    avx = sum(n for k, n in t.missing.items() if k.startswith("v"))
    print(f"untranslated: {avx:,} AVX, {bad - avx:,} other; full list in {listing.relative_to(ROOT)}")
    for k, n in [(k, n) for k, n in t.missing.most_common() if not k.startswith("v")][:30]:
        print(f"  {n:7,}  {k}")


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "emit":
        emit(sys.argv[2], [int(a, 16) for a in sys.argv[3:]])
    elif len(sys.argv) == 2 and sys.argv[1] == "coverage":
        coverage()
    elif len(sys.argv) == 3 and sys.argv[1] == "program":
        program(sys.argv[2])
    else:
        sys.exit(__doc__)
