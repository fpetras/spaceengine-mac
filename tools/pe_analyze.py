#!/usr/bin/env python3
"""Dependency-free static pass over SpaceEngine.exe (PE32+, MSVC, x86-64).

Recovers what a stripped MSVC binary still carries:
  * sections, imports (IAT slot -> dll!symbol)
  * function boundaries from .pdata (RUNTIME_FUNCTION), chained chunks folded
    into their primary function
  * MSVC RTTI: classes, base-class lists, vtables and their slot targets
  * string -> referencing function map, from RIP-relative LEA operands
  * inferred function names from "Class::Method" text in referenced log strings

Everything written here is derived from file bytes. Names are inferences, not
symbols: the binary ships none.
"""
import json
import re
import struct
import sys
from bisect import bisect_right
from collections import defaultdict
from pathlib import Path

exe_path, strings_tsv, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
data = Path(exe_path).read_bytes()
u16 = lambda o: struct.unpack_from("<H", data, o)[0]
u32 = lambda o: struct.unpack_from("<I", data, o)[0]
i32 = lambda o: struct.unpack_from("<i", data, o)[0]
u64 = lambda o: struct.unpack_from("<Q", data, o)[0]

# ---------------------------------------------------------------- headers
pe = u32(0x3C)
assert data[pe:pe + 4] == b"PE\0\0"
nsec = u16(pe + 6)
timestamp = u32(pe + 8)
opt = pe + 24
assert u16(opt) == 0x20B, "expected PE32+"
opt_size = u16(pe + 20)
image_base = u64(opt + 24)
entry_rva = u32(opt + 16)
dirs = [(u32(opt + 112 + i * 8), u32(opt + 116 + i * 8)) for i in range(16)]
sections = []
so = opt + opt_size
for i in range(nsec):
    o = so + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    sections.append(dict(name=name, va=va, vsize=vsize, raw=raw, rsize=rsize,
                         chars=u32(o + 36)))


def rva2off(rva):
    for s in sections:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rsize"]):
            d = rva - s["va"]
            return s["raw"] + d if d < s["rsize"] else None
    return None


def sec(name):
    return next(s for s in sections if s["name"] == name)


def in_sec(rva, s):
    return s["va"] <= rva < s["va"] + s["vsize"]


def cstr(off, limit=4096):
    end = data.index(b"\0", off, off + limit)
    return data[off:end].decode("latin-1")


text, rdata, dsec = sec(".text"), sec(".rdata"), sec(".data")

# ---------------------------------------------------------------- imports
def export_ordinals(dll_path):
    """ordinal -> exported name, read from a sibling DLL's export directory."""
    try:
        d = dll_path.read_bytes()
    except OSError:
        return {}
    p = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, p + 6)[0]
    o = p + 24
    magic = struct.unpack_from("<H", d, o)[0]
    secs = o + struct.unpack_from("<H", d, p + 20)[0]
    exp_rva = struct.unpack_from("<I", d, o + (112 if magic == 0x20B else 96))[0]

    def r2o(rva):
        for i in range(n):
            vs, va, rs, raw = struct.unpack_from("<IIII", d, secs + i * 40 + 8)
            if va <= rva < va + max(vs, rs):
                return raw + rva - va
        raise ValueError(rva)

    if not exp_rva:
        return {}
    e = r2o(exp_rva)
    base, _, nnames, _, names, ords = struct.unpack_from("<IIIIII", d, e + 16)
    out = {}
    for i in range(nnames):
        no = r2o(struct.unpack_from("<I", d, r2o(names) + i * 4)[0])
        idx = struct.unpack_from("<H", d, r2o(ords) + i * 2)[0]
        out[base + idx] = d[no:d.index(b"\0", no)].decode("latin-1")
    return out


ordinal_cache = {}
imports = {}  # iat slot rva -> "dll!name"
imp_rva, _ = dirs[1]
o = rva2off(imp_rva)
while True:
    oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", data, o)
    if not (oft or ft):
        break
    dll = cstr(rva2off(name_rva))
    t = rva2off(oft or ft)
    k = 0
    while True:
        v = u64(t + k * 8)
        if not v:
            break
        if v >> 63:
            if dll not in ordinal_cache:
                ordinal_cache[dll] = export_ordinals(Path(exe_path).parent / dll)
            o_ = v & 0xFFFF
            sym = ordinal_cache[dll].get(o_, f"ordinal#{o_}")
        else:
            sym = cstr(rva2off(v & 0x7FFFFFFF) + 2)
        imports[ft + k * 8] = f"{dll}!{sym}"
        k += 1
    o += 20

# ---------------------------------------------------------------- .pdata
pd_rva, pd_size = dirs[3]
po = rva2off(pd_rva)
chunks = []
for i in range(pd_size // 12):
    b, e, uw = struct.unpack_from("<III", data, po + i * 12)
    if b:
        chunks.append((b, e, uw))
chunks.sort()
chunk_by_begin = {c[0]: c for c in chunks}


def primary_of(c, depth=0):
    """Follow UNW_FLAG_CHAININFO to the function that owns this chunk."""
    b, e, uw = c
    uo = rva2off(uw)
    if uo is None or depth > 8:
        return b
    flags = data[uo] >> 3
    if flags & 4:
        ncodes = data[uo + 2]
        co = uo + 4 + ((ncodes + 1) & ~1) * 2
        pb, pe_, puw = struct.unpack_from("<III", data, co)
        return primary_of((pb, pe_, puw), depth + 1)
    return b


funcs = defaultdict(list)  # primary begin rva -> [(b, e)]
chunk_owner = []
for c in chunks:
    p = primary_of(c)
    funcs[p].append((c[0], c[1]))
    chunk_owner.append(p)
chunk_begins = [c[0] for c in chunks]


def func_of(rva):
    i = bisect_right(chunk_begins, rva) - 1
    if i >= 0 and chunks[i][0] <= rva < chunks[i][1]:
        return chunk_owner[i]
    return None


# ---------------------------------------------------------------- RTTI
def demangle_type(n):
    """Readable form of an MSVC type-descriptor name; plain cases only."""
    m = re.fullmatch(r"\.\?A[VUTW]4?([A-Za-z_0-9@$?]+)@@", n)
    if not m:
        return n
    body = m.group(1)
    if "?$" in body or "$" in body:
        return n[4:].rstrip("@")  # template: keep the mangled spelling
    return "::".join(reversed(body.split("@")))


td_names = {}  # type descriptor rva -> raw name
for m in re.finditer(rb"\.\?A[VU][\x21-\x7e]{1,400}?@@\0", data):
    off = m.start() - 16
    for s in sections:
        if s["raw"] <= off < s["raw"] + s["rsize"]:
            td_names[s["va"] + off - s["raw"]] = m.group()[:-1].decode()

cols = {}  # COL rva -> dict
ro = rdata["raw"]
for off in range(ro, ro + rdata["rsize"] - 24, 4):
    if u32(off) != 1:
        continue
    self_rva = rdata["va"] + off - ro
    if u32(off + 20) != self_rva:
        continue
    td = u32(off + 12)
    if td in td_names:
        cols[self_rva] = dict(offset=u32(off + 4), td=td, chd=u32(off + 16))

classes = {}  # raw name -> info
for col_rva, c in cols.items():
    name = td_names[c["td"]]
    info = classes.setdefault(name, dict(name=demangle_type(name), raw=name,
                                         bases=[], vtables=[]))
    co = rva2off(c["chd"])
    if co is not None and not info["bases"]:
        nbases, arr = u32(co + 8), u32(co + 12)
        ao = rva2off(arr)
        for k in range(1, min(nbases, 64)):
            bo = rva2off(u32(ao + k * 4))
            btd = u32(bo)
            if btd in td_names:
                info["bases"].append(demangle_type(td_names[btd]))

col_vas = {image_base + r: r for r in cols}
vt_starts = {}
for off in range(ro, ro + rdata["rsize"] - 8, 8):
    v = u64(off)
    if v in col_vas:
        vt_starts[rdata["va"] + off - ro + 8] = col_vas[v]

vtable_slots = {}  # func rva -> [(class, vt offset, slot)]
vt_set = set(vt_starts)
for vt_rva, col_rva in sorted(vt_starts.items()):
    c = cols[col_rva]
    name = td_names[c["td"]]
    slots = []
    o = rva2off(vt_rva)
    k = 0
    while k < 2048:
        if k and (vt_rva + k * 8) in vt_set:
            break
        v = u64(o + k * 8)
        r = v - image_base
        if not (0 <= r < 2 ** 32 and in_sec(r, text)):
            break
        slots.append(r)
        k += 1
    classes[name]["vtables"].append(dict(
        rva=vt_rva, va=hex(image_base + vt_rva), this_offset=c["offset"],
        slots=[hex(image_base + s) for s in slots]))
    for k, s in enumerate(slots):
        vtable_slots.setdefault(s, []).append((classes[name]["name"], c["offset"], k))

# ---------------------------------------------------------------- leaf functions
# A function that needs no unwind information (no stack frame, no calls) has
# no .pdata entry. Find those as targets of direct calls and of vtable slots
# that fall outside every .pdata chunk.
to, tsz, tva = text["raw"], text["rsize"], text["va"]
tb = data[to:to + tsz]
leaf_callers = defaultdict(int)
for i in range(len(tb) - 5):
    if tb[i] == 0xE8:
        tgt = tva + i + 5 + struct.unpack_from("<i", tb, i + 1)[0]
        if tva <= tgt < tva + tsz and func_of(tgt) is None and func_of(tva + i) is not None:
            leaf_callers[tgt] += 1
# 0xE8 also occurs inside other instructions; keep targets that look like a
# function start or are called more than once.
leaves = {t for t, n in leaf_callers.items()
          if n >= 2 or t % 16 == 0 or tb[t - tva - 1] in (0xCC, 0xC3, 0x90)}
leaves |= {s for s in vtable_slots if func_of(s) is None}
starts = sorted(set(chunk_begins) | leaves)
for a in sorted(leaves):
    nxt = starts[bisect_right(starts, a)] if a != starts[-1] else tva + tsz
    size = len(tb[a - tva:nxt - tva].rstrip(b"\xcc"))
    if size == 0:  # a target inside padding: not a function
        leaves.discard(a)
        continue
    funcs[a] = [(a, a + size)]
    chunks.append((a, a + size, 0))
order = sorted(range(len(chunks)), key=lambda i: chunks[i][0])
chunk_owner = [chunk_owner[i] if i < len(chunk_owner) else chunks[i][0] for i in order]
chunks = [chunks[i] for i in order]
chunk_begins = [c[0] for c in chunks]

# ---------------------------------------------------------------- xrefs
# A disassembler's string list (address<TAB>text) adds names inferred from log
# strings; "-" or a missing file skips that. The port does not need names.
strings = {}
for line in (Path(strings_tsv).read_text(errors="replace") if strings_tsv != "-" and Path(strings_tsv).exists() else "").splitlines():
    a, _, v = line.partition("\t")
    try:
        strings[int(a, 16) - image_base] = v
    except ValueError:
        pass

str_refs = defaultdict(set)      # string rva -> {func rva}
func_strings = defaultdict(set)  # func rva -> {string rva}
func_imports = defaultdict(set)
func_vtables = defaultdict(set)  # func rva -> {raw class name whose vtable it loads}
call_edges = defaultdict(set)    # caller -> {callee func rva}
LEA_MODRM = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D}
for i in range(len(tb) - 7):
    b0 = tb[i]
    if b0 in (0x48, 0x4C) and tb[i + 1] == 0x8D and tb[i + 2] in LEA_MODRM:
        tgt = tva + i + 7 + struct.unpack_from("<i", tb, i + 3)[0]
        if tgt in strings:
            f = func_of(tva + i)
            if f is not None:
                str_refs[tgt].add(f)
                func_strings[f].add(tgt)
        elif tgt in vt_starts:
            f = func_of(tva + i)
            if f is not None:
                func_vtables[f].add(td_names[cols[vt_starts[tgt]]["td"]])
    elif b0 == 0xFF and tb[i + 1] in (0x15, 0x25):
        tgt = tva + i + 6 + struct.unpack_from("<i", tb, i + 2)[0]
        if tgt in imports:
            f = func_of(tva + i)
            if f is not None:
                func_imports[f].add(imports[tgt])
    elif b0 == 0xE8:
        tgt = tva + i + 5 + struct.unpack_from("<i", tb, i + 1)[0]
        if tgt in funcs:
            f = func_of(tva + i)
            if f is not None and f != tgt:
                call_edges[f].add(tgt)

callers = defaultdict(set)
for a, bs in call_edges.items():
    for b in bs:
        callers[b].add(a)

# ---------------------------------------------------------------- naming
METHOD = re.compile(r"\b((?:se|cm|gl)?[A-Za-z_][A-Za-z0-9_]{2,})::(~?[A-Za-z_][A-Za-z0-9_]*)")
SKIP_NS = {"std", "ios_base", "__crt_strtox", "https", "http", "URL"}
name_votes = defaultdict(lambda: defaultdict(set))  # func -> name -> {string rva}
for srva, fs in str_refs.items():
    for m in METHOD.finditer(strings[srva]):
        if m.group(1) in SKIP_NS:
            continue
        for f in fs:
            name_votes[f][f"{m.group(1)}::{m.group(2)}"].add(srva)

name_owner = defaultdict(set)
for f, votes in name_votes.items():
    for n in votes:
        name_owner[n].add(f)

rows = []
for f in sorted(funcs):
    size = sum(e - b for b, e in funcs[f])
    name, basis = "", ""
    votes = name_votes.get(f)
    hints = ""
    if votes:
        # Most specific name first: an error-check macro inlined into dozens of
        # functions (e.g. "seFBO::Unbind") says little about any one of them.
        best = sorted(votes, key=lambda n: (len(name_owner[n]), -len(votes[n]), n))
        shared = len(name_owner[best[0]])
        if shared <= 3:
            name = best[0]
            basis = "log-string" + ("" if shared == 1 else f" (shared by {shared} functions)")
            if len(best) > 1:
                hints = ",".join(best[1:5])
        else:
            hints = ",".join(best[:5])
    if not name and f in vtable_slots:
        cls, off, k = vtable_slots[f][0]
        n = len({c for c, _, _ in vtable_slots[f]})
        name = f"{cls}::vf{k}"
        basis = "vtable-slot" + (f" (in {n} classes' vtables)" if n > 1 else "")
    if not name and f in func_vtables:
        # A function that stores a class's vtable pointer is that class's
        # constructor or destructor, or has one inlined into it. With several
        # vtables the most-derived class (the one listing the others as bases)
        # is the object being built or torn down.
        refs = func_vtables[f]
        derived = [r for r in refs
                   if all(classes[o]["name"] in classes[r]["bases"] for o in refs if o != r)]
        if len(derived) == 1:
            name = classes[derived[0]]["name"] + "::ctor_or_dtor"
            basis = "installs-vtable"
        else:
            hints = ",".join(filter(None, [hints, "vtables:" + "|".join(
                sorted(classes[r]["name"] for r in refs)[:4])]))
    if hints:
        basis = (basis + " " if basis else "") + "mentions:" + hints
    rows.append(dict(va=hex(image_base + f), size=size, name=name, basis=basis,
                     strings=len(func_strings.get(f, ())),
                     imports=sorted(func_imports.get(f, ())),
                     callers=len(callers.get(f, ())),
                     callees=len(call_edges.get(f, ())),
                     kind="leaf" if f in leaves else "pdata"))

out_dir.mkdir(parents=True, exist_ok=True)
with open(out_dir / "functions.tsv", "w") as fh:
    fh.write("va\tsize\tname\tbasis\tn_strings\tn_callers\tn_callees\timports\tkind\n")
    for r in rows:
        fh.write(f"{r['va']}\t{r['size']}\t{r['name']}\t{r['basis']}\t{r['strings']}\t"
                 f"{r['callers']}\t{r['callees']}\t{','.join(r['imports'])}\t{r['kind']}\n")
with open(out_dir / "imports.tsv", "w") as fh:
    for slot, n in sorted(imports.items()):
        fh.write(f"{hex(image_base + slot)}\t{n}\n")
(out_dir / "classes.json").write_text(json.dumps(
    sorted(classes.values(), key=lambda c: c["name"]), indent=1))
with open(out_dir / "string_xrefs.tsv", "w") as fh:
    for srva in sorted(str_refs):
        fs = ",".join(hex(image_base + f) for f in sorted(str_refs[srva]))
        fh.write(f"{hex(image_base + srva)}\t{fs}\t{strings[srva]}\n")
(out_dir / "callgraph.json").write_text(json.dumps(
    {hex(image_base + a): sorted(hex(image_base + b) for b in bs)
     for a, bs in sorted(call_edges.items())}))

summary = dict(
    image_base=hex(image_base), entry=hex(image_base + entry_rva),
    timestamp=timestamp, sections=sections,
    pdata_chunks=len(chunks) - len(leaves), functions=len(funcs), leaf_functions=len(leaves),
    imports=len(imports), import_dlls=sorted({n.split("!")[0] for n in imports.values()}),
    rtti_type_descriptors=len(td_names), rtti_classes_with_vtables=len(classes),
    vtables=len(vt_starts),
    strings=len(strings), strings_with_code_refs=len(str_refs),
    functions_named_from_log_strings=sum(1 for r in rows if r["basis"].startswith("log-string")),
    functions_named_from_vtables=sum(1 for r in rows if r["basis"].startswith("vtable-slot")),
    functions_named_from_vtable_install=sum(1 for r in rows if r["basis"].startswith("installs-vtable")),
    functions_with_only_string_hints=sum(1 for r in rows if r["basis"].startswith("mentions:")),
)
(out_dir / "summary.json").write_text(json.dumps(summary, indent=1))
print(json.dumps({k: v for k, v in summary.items() if k != "sections"}, indent=1))
