#!/usr/bin/env python3
"""Differential test: translated functions against the original machine code.

Every case is run twice: by a driver built from the translated functions
(build/driver for 13 of them, build/driver_full for the whole program) and by
the same function of SpaceEngine.exe inside a CPU emulator. Results must
agree bit for bit. The cases exercise the game's time and orbit routines.

The emulator substitutes the host's sin, cos and floor for the CRT's, so both
sides share one math library; without that, the two libraries' sin and cos
differ in the last bit for about 5% of arguments. A second pass uses the
executable's own math routines and reports how far apart the results are.
"""
import math
import os
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT.parent / "tools"))
from emu import Emu  # noqa: E402

FROM_DAYS, TO_DAYS, DATE_TO_TIME = 0x140440660, 0x140440790, 0x1401E3BE0
SERIES_EVAL, INIT_TIME_CONSTANTS, EPOCH_J2000 = 0x1401EBA10, 0x1401E3210, 0x140855480
ORBITS = {  # vtable, Init, GetPosition, GetVelocity
    "rect": (0x1406895B0, 0x1401EECC0, 0x1401EEA40, 0x1401EE8D0),
    "polar": (0x1406895D8, 0x1401EF260, 0x1401EEFA0, 0x1401EEE30),
}

rng = random.Random(20261013)


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def hexd(x):
    return f"{bits(x):016x}"


def random_double():
    kind = rng.randrange(6)
    if kind == 0:
        return rng.uniform(-3e6, 3e6)            # Julian-date sized
    if kind == 1:
        return rng.uniform(-1, 1)
    if kind == 2:
        return rng.choice([-1, 1]) * 2.0 ** rng.uniform(-90, 46)
    if kind == 3:
        return float(rng.randrange(-10**7, 10**7))
    if kind == 4:
        return rng.choice([0.0, -0.0, 0.5, -0.5, 65536.0, -65536.0, 2451545.0, 2.0**-80, -(2.0**-80)])
    return rng.uniform(-1.4e14, 1.4e14)


def random_tables():
    """Three coordinates, each a list of series, each a list of (A, B, C)."""
    return [[[(rng.uniform(-30, 30) * 10 ** rng.uniform(-8, 0), rng.uniform(0, 2 * math.pi),
               rng.uniform(0, 80000) * rng.choice([0, 1, 1, 1]))
              for _ in range(rng.randrange(0, 25))]
             for _ in range(rng.randrange(0, 7))]
            for _ in range(3)]


def tables_text(tables):
    out = [" ".join(str(len(coord)) for coord in tables)]
    for coord in tables:
        for series in coord:
            out.append(str(len(series)))
            out += [hexd(v) for term in series for v in term]
    return " ".join(out)


class Original:
    """The executable's functions, called in the emulator."""

    def __init__(self, host_math):
        self.e = Emu(host_math=host_math)
        self.e.call(INIT_TIME_CONSTANTS)
        self.mark = self.e.brk

    def reset(self):
        self.e.brk = self.mark

    def from_days(self, x):
        out = self.e.alloc(16)
        self.e.call(FROM_DAYS, out, x)
        return self.e.qwords(out, 2)

    def to_days(self, hi, lo):
        return (self.e.call(TO_DAYS, self.e.alloc(16, struct.pack("<QQ", hi, lo)))[1],)

    def date(self, year, month, day, hour, minute, second):
        date = self.e.alloc(16, struct.pack("<iBBBBd", year, month, day, hour, minute, second))
        out = self.e.alloc(16)
        self.e.call(DATE_TO_TIME, date, out)
        return self.e.qwords(out, 2)

    def put_series(self, terms):
        data = b"".join(struct.pack("<3d", *t) for t in terms)
        return self.e.alloc(max(len(data), 8), data), len(terms)

    def series(self, T, terms):
        ptr, n = self.put_series(terms)
        return (self.e.call(SERIES_EVAL, self.e.alloc(16, struct.pack("<Qi", ptr, n)), T)[1],)

    def orbit(self, kind, tables=None):
        vtable = ORBITS[kind][0]
        obj = self.e.alloc(0xA0, struct.pack("<Q", vtable) + b"\xee" * 0x98)
        if tables is not None:
            for c, coord in enumerate(tables):
                arr = b"".join(struct.pack("<Qi4x", *self.put_series(s)) for s in coord)
                self.e.write(obj + 0x78 + 8 * c, struct.pack("<Q", self.e.alloc(max(len(arr), 8), arr)))
                self.e.write(obj + 0x90 + 4 * c, struct.pack("<i", len(coord)))
        return obj

    def orbit_init(self, kind):
        obj = self.orbit(kind)
        self.e.call(ORBITS[kind][1], obj)
        return self.e.qwords(obj + 8, 4) + tuple(bits(v) for v in self.e.doubles(obj + 0x28, 9))

    def orbit_eval(self, kind, what, hi, lo, tables):
        obj = self.orbit(kind, tables)
        out = self.e.alloc(24)
        time = self.e.alloc(16, struct.pack("<QQ", hi, lo))
        self.e.call(ORBITS[kind][2 if what == "pos" else 3], obj, out, time)
        return self.e.doubles(out, 3)


def cases():
    """(group, driver command line, callable on Original returning a tuple)."""
    for _ in range(3000):
        x = random_double()
        yield "seTime::FromDays", f"from_days {hexd(x)}", lambda o, x=x: o.from_days(x)
    for _ in range(3000):
        hi, lo = rng.getrandbits(64), rng.getrandbits(64)
        k = rng.randrange(8)
        if k == 0:
            lo = 0
        elif k == 1:
            hi = rng.choice([0, 2**64 - 1, 2**63, 2**63 - 1])
        elif k == 2:
            hi, lo = Original.from_days(shared, random_double())
        yield "seTime::ToDays", f"to_days {hi:016x} {lo:016x}", lambda o, hi=hi, lo=lo: o.to_days(hi, lo)
    edge = [(1582, 10, d) for d in range(1, 32)] + [(1582, m, 15) for m in range(1, 13)] + \
           [(y, m, d) for y in (1581, 1583, 0, -1, 1, 100, 400, -400, 2000) for m in (1, 2, 3, 12) for d in (1, 28)]
    for i in range(2500):
        y, m, d = edge[i] if i < len(edge) else (rng.randrange(-6000, 10000), rng.randrange(1, 13), rng.randrange(1, 32))
        hh, mm, ss = rng.randrange(24), rng.randrange(60), rng.choice([0.0, rng.uniform(0, 60)])
        yield ("DateToTime", f"date {y} {m} {d} {hh} {mm} {hexd(ss)}",
               lambda o, a=(y, m, d, hh, mm, ss): o.date(*a))
    for _ in range(600):
        T = rng.uniform(-8, 8)
        terms = random_tables()[0]
        terms = terms[0] if terms else []
        line = f"series {hexd(T)} {len(terms)} " + " ".join(hexd(v) for t in terms for v in t)
        yield "seVSOP87Series::Evaluate", line, lambda o, T=T, terms=terms: o.series(T, terms)
    for kind, name in (("rect", "seOrbitVSOP87Rect"), ("polar", "seOrbitVSOP87Polar")):
        yield f"{name}::Init", f"{kind}_init", lambda o, kind=kind: o.orbit_init(kind)
        for what, method in (("pos", "GetPosition"), ("vel", "GetVelocity")):
            for _ in range(250):
                hi, lo = Original.from_days(shared, rng.uniform(990000, 3900000))
                lo ^= rng.getrandbits(40)
                tables = random_tables()
                yield (f"{name}::{method}", f"{kind}_{what} {hi:016x} {lo:016x} {tables_text(tables)}",
                       lambda o, a=(kind, what, hi, lo, tables): o.orbit_eval(*a))


def as_bits(values):
    return tuple(v if isinstance(v, int) else bits(v) for v in values)


shared = Original(host_math=True)
crt = Original(host_math=False)
all_cases = list(cases())

# SE_DRIVER selects another implementation that speaks the same protocol, such
# as the machine-translated build in ../recomp. It is given the executable's
# path as its argument.
driver_path = os.environ.get("SE_DRIVER", str(ROOT / "build/driver"))
driver = subprocess.run([driver_path, str(shared.e.img.path)], input="\n".join(c[1] for c in all_cases) + "\n",
                        capture_output=True, text=True, check=True)
replies = [tuple(int(w, 16) for w in line.split()) for line in driver.stdout.splitlines()]
assert len(replies) == len(all_cases), "driver reply count"

assert shared.e.qwords(EPOCH_J2000, 2) == (2451545 << 16, 0), "kEpochJ2000 differs from the original global"

stats = {}
failures = []
for (group, line, run), mine in zip(all_cases, replies):
    s = stats.setdefault(group, dict(n=0, exact=0, crt_exact=0, crt_worst=0.0))
    s["n"] += 1
    shared.reset()
    theirs = as_bits(run(shared))
    if theirs == mine:
        s["exact"] += 1
    elif len(failures) < 5:
        failures.append((group, line[:150], theirs, mine))
    crt.reset()
    crt_bits = as_bits(run(crt))
    if crt_bits == mine:
        s["crt_exact"] += 1
    else:
        for a, b in zip(crt_bits, mine):
            fa, fb = (struct.unpack("<d", struct.pack("<Q", v))[0] for v in (a, b))
            if fa != fb and math.isfinite(fa) and math.isfinite(fb):
                s["crt_worst"] = max(s["crt_worst"], abs(fa - fb) / max(abs(fa), abs(fb)))

print(f"{'function':34s} {'cases':>6s} {'bit-exact':>10s}   with the exe's own sin/cos")
for group, s in stats.items():
    crt_note = "bit-exact" if s["crt_exact"] == s["n"] else \
        f"{s['crt_exact']}/{s['n']} exact, worst relative difference {s['crt_worst']:.1e}"
    print(f"{group:34s} {s['n']:6d} {s['exact']:6d}/{s['n']:<6d} {crt_note}")
for f in failures:
    print("MISMATCH", *f, sep="\n  ")
total = sum(s["n"] for s in stats.values())
exact = sum(s["exact"] for s in stats.values())
print(f"\n{exact}/{total} cases bit-exact")
sys.exit(0 if exact == total else 1)
