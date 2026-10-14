#!/usr/bin/env python3
"""Just enough of pkg-config for FFmpeg's configure script.

Answers --exists, --modversion, --cflags and --libs (with "name >= version"
requirements and Requires: chains) from the .pc files on PKG_CONFIG_PATH. The
Mac has no pkg-config and this avoids installing one for a single build.
"""
import os
import re
import sys


def find(name):
    for d in os.environ.get("PKG_CONFIG_PATH", "").split(":"):
        p = os.path.join(d, name + ".pc")
        if d and os.path.exists(p):
            return p
    return None


def load(path):
    vars_, fields = {}, {}
    for line in open(path):
        line = line.split("#", 1)[0].rstrip()
        m = re.match(r"^([A-Za-z0-9_.]+)\s*=\s*(.*)$", line)
        if m:
            vars_[m.group(1)] = m.group(2)
            continue
        m = re.match(r"^([A-Za-z0-9_.]+)\s*:\s*(.*)$", line)
        if m:
            fields[m.group(1).lower()] = m.group(2)

    def expand(s):
        for _ in range(10):
            s = re.sub(r"\$\{([^}]+)\}", lambda m: vars_.get(m.group(1), ""), s)
        return s
    return {k: expand(v) for k, v in fields.items()}


def version_key(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-]", v)]


def requirements(text):
    """[(name, op, version)] from 'a >= 1.0 b, c'."""
    tokens = text.replace(",", " ").split()
    out, i = [], 0
    while i < len(tokens):
        if i + 2 < len(tokens) and tokens[i + 1] in ("<", "<=", "=", ">=", ">", "!="):
            out.append((tokens[i], tokens[i + 1], tokens[i + 2]))
            i += 3
        else:
            out.append((tokens[i], None, None))
            i += 1
    return out


def satisfied(have, op, want):
    if not op:
        return True
    a, b = version_key(have), version_key(want)
    return {"<": a < b, "<=": a <= b, "=": a == b, ">=": a >= b, ">": a > b, "!=": a != b}[op]


def collect(name, field, seen):
    path = find(name)
    if not path or name in seen:
        return []
    seen.add(name)
    pc = load(path)
    out = pc.get(field, "").split()
    for dep, _, _ in requirements(pc.get("requires", "") + " " + pc.get("requires.private", "") * 0):
        out += collect(dep, field, seen)
    return out


def main(argv):
    flags = [a for a in argv if a.startswith("--")]
    reqs = requirements(" ".join(a for a in argv if not a.startswith("--")))
    if "--version" in flags:
        print("0.29.2")
        return 0
    for name, op, want in reqs:
        path = find(name)
        if not path or not satisfied(load(path).get("version", "0"), op, want):
            if "--print-errors" in flags:
                print(f"Package {name} was not found or is too old", file=sys.stderr)
            return 1
    out = []
    for name, _, _ in reqs:
        if "--modversion" in flags:
            out.append(load(find(name)).get("version", ""))
        if "--cflags" in flags:
            out += collect(name, "cflags", set())
        if "--libs" in flags:
            out += collect(name, "libs", set())
    if out:
        print(" ".join(dict.fromkeys(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
