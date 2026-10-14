#!/usr/bin/env python3
"""Prints the path of SpaceEngine.exe: the first line of
~/Library/Application Support/SpaceEngine-native/game-path.txt, or the places
Steam puts the game on a Mac (the same search as recomp/host/main.c)."""
import sys
from pathlib import Path

HOME = Path.home()
PLACES = [
    "Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_314650/depot_314651",
    "Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/common/SpaceEngine",
    "Steam/steamapps/content/app_314650/depot_314651",
    "Steam/steamapps/common/SpaceEngine",
]


def find():
    override = HOME / "Library/Application Support/SpaceEngine-native/game-path.txt"
    if override.exists():
        line = override.read_text().splitlines()[0].strip() if override.read_text().strip() else ""
        if line and Path(line).exists():
            return Path(line)
    for place in PLACES:
        exe = HOME / "Library/Application Support" / place / "system/SpaceEngine.exe"
        if exe.exists():
            return exe
    return None


if __name__ == "__main__":
    exe = find()
    if not exe:
        sys.exit("SpaceEngine.exe not found; put its path on the first line of "
                 "~/Library/Application Support/SpaceEngine-native/game-path.txt")
    print(exe)
