"""Minimal PE32+ image reader shared by the disassembly and emulation tools."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_game import find  # noqa: E402

DEFAULT_EXE = find()   # SpaceEngine.exe, wherever Steam put it (find_game.py)


class Image:
    def __init__(self, path=DEFAULT_EXE):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.size = struct.unpack_from("<I", d, opt + 56)[0]
        self.header_size = struct.unpack_from("<I", d, opt + 60)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + i * 8) for i in range(16)]
        so = opt + struct.unpack_from("<H", d, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            o = so + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode()
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, va, vsize, raw, rsize))

    def off(self, va):
        rva = va - self.base
        for _, sva, vsize, raw, rsize in self.sections:
            if sva <= rva < sva + max(vsize, rsize):
                return raw + rva - sva if rva - sva < rsize else None
        return None

    def read(self, va, n):
        """Bytes at va as loaded: zero-filled past a section's raw data."""
        o = self.off(va)
        if o is None:
            return b"\0" * n
        return self.data[o:o + n]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def u64(self, va):
        return struct.unpack("<Q", self.read(va, 8))[0]

    def f64(self, va):
        return struct.unpack("<d", self.read(va, 8))[0]

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]

    def functions(self):
        """(begin_va, end_va) for every .pdata chunk, sorted."""
        rva, size = self.dirs[3]
        o = self.off(self.base + rva)
        out = []
        for i in range(size // 12):
            b, e, _ = struct.unpack_from("<III", self.data, o + i * 12)
            if b:
                out.append((self.base + b, self.base + e))
        return sorted(out)

    def function_ranges(self):
        """primary_va -> [(begin_va, end_va)], with chained .pdata chunks
        (UNW_FLAG_CHAININFO) folded into the function that owns them."""
        rva, size = self.dirs[3]
        o = self.off(self.base + rva)
        d = self.data

        def primary(b, uw, depth=0):
            uo = self.off(self.base + uw)
            if uo is None or depth > 8 or not (d[uo] >> 3) & 4:
                return b
            co = uo + 4 + ((d[uo + 2] + 1) & ~1) * 2
            pb, _, puw = struct.unpack_from("<III", d, co)
            return primary(pb, puw, depth + 1)

        out = {}
        for i in range(size // 12):
            b, e, uw = struct.unpack_from("<III", d, o + i * 12)
            if b:
                out.setdefault(self.base + primary(b, uw), []).append((self.base + b, self.base + e))
        return {k: sorted(v) for k, v in out.items()}
