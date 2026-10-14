#!/usr/bin/env python3
"""Writes the game's own icon as a macOS .icns, for the app bundle.

Reads the first icon group in the executable's resources, takes its largest
image and has macOS's own tools scale it to the sizes an .icns holds.

usage: icon.py <SpaceEngine.exe> <out.icns>
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe import Image  # noqa: E402

RT_ICON, RT_GROUP_ICON = 3, 14


def entries(img, root, offset):
    """(id, is_directory, target offset) for each entry of a resource directory."""
    named, ids = struct.unpack("<HH", img.read(root + offset + 12, 4))
    for i in range(named + ids):
        name, target = struct.unpack("<II", img.read(root + offset + 16 + 8 * i, 8))
        yield name, bool(target & 0x80000000), target & 0x7FFFFFFF


def data(img, root, offset):
    """The bytes of the first language's data under a name directory."""
    _, is_dir, target = next(entries(img, root, offset))
    assert not is_dir
    rva, size = struct.unpack("<II", img.read(root + target, 8))
    return img.read(img.base + rva, size)


def main(exe, out):
    img = Image(exe)
    root = img.base + img.dirs[2][0]
    types = {t: target for t, _, target in entries(img, root, 0)}
    if RT_GROUP_ICON not in types:
        sys.exit("the executable has no icon")
    group = data(img, root, next(entries(img, root, types[RT_GROUP_ICON]))[2])
    icons = {i: target for i, _, target in entries(img, root, types[RT_ICON])}
    count = struct.unpack_from("<H", group, 4)[0]
    best = None
    for i in range(count):
        w, h, _colors, _res, _planes, bits, size, ident = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        key = ((w or 256) * (h or 256), bits, size)
        if best is None or key > best[0]:
            best = (key, w, h, bits, ident)
    _, w, h, bits, ident = best
    image = data(img, root, icons[ident])
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        if image[:8] == b"\x89PNG\r\n\x1a\n":
            source = tmp / "icon.png"
            source.write_bytes(image)
        else:  # a bitmap: wrap it as a one-image .ico, which sips reads
            source = tmp / "icon.ico"
            source.write_bytes(struct.pack("<HHH", 0, 1, 1)
                               + struct.pack("<BBBBHHII", w, h, 0, 0, 1, bits, len(image), 22) + image)
        iconset = tmp / "icon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-s", "format", "png", "-z", str(size * scale), str(size * scale),
                                str(source), "--out", str(iconset / name)], check=True, capture_output=True)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", out], check=True)
    print(f"{out}: from the {w or 256}x{h or 256} {bits}-bit icon")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
