#!/usr/bin/env python3
"""Lists the FFmpeg functions the game looks up by name.

The game loads its FFmpeg DLLs at run time and asks for each function by
name, so the names are strings in the executable. This finds every string in
it that one of the built FFmpeg libraries exports, and says which library
exports it.

usage: ffmpeg_names.py <SpaceEngine.exe> <ffmpeg lib dir> <out.tsv>
"""
import re
import subprocess
import sys
from pathlib import Path

LIBRARIES = {"avcodec": "libavcodec.62.dylib", "avformat": "libavformat.62.dylib",
             "avutil": "libavutil.60.dylib", "swscale": "libswscale.9.dylib"}


def exports(dylib):
    out = subprocess.run(["nm", "-gU", str(dylib)], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1].lstrip("_") for line in out.splitlines() if " T " in line}


def main(exe, lib_dir, out):
    owner = {}
    for library, dylib in LIBRARIES.items():
        for name in exports(Path(lib_dir) / dylib):
            owner.setdefault(name, library)
    data = Path(exe).read_bytes()
    found = sorted({m.group(1).decode() for m in re.finditer(rb"(?<=\0)([A-Za-z_][A-Za-z0-9_]{2,})(?=\0)", data)
                    if m.group(1).decode() in owner})
    Path(out).write_text("library\tname\n" + "".join(f"{owner[n]}\t{n}\n" for n in found))
    print(f"{out}: {len(found)} FFmpeg functions")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
