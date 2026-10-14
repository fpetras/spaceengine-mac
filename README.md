# SpaceEngine on Apple Silicon

The Steam release of SpaceEngine (0.991, build 50.2155), running natively on an
Apple Silicon Mac. No Wine, no CrossOver, no Rosetta, no virtual machine: the
game's Windows executable is translated ahead of time into C, compiled for
arm64, and linked against a small layer that stands in for Windows.

It runs on an M5 MacBook Air (32 GB, macOS 27) at Retina resolution, with
sound, Steam, controllers and video recording.

This repository contains no part of the game. Everything specific to the
executable is derived from your own copy when you build, and the translated
code is only ever produced on your machine. See [Boundaries](#boundaries).

## Why

Wine and CrossOver can't run SpaceEngine: the game needs more OpenGL than
macOS provides. Boot Camp is gone on Apple Silicon, and Rosetta, which only
helps with Intel Mac programs anyway, is being wound down. A native arm64
build is the only kind that lasts.

## What works

| Area | State | Notes |
|---|---|---|
| Rendering | Works | OpenGL 4.1 on Apple's Metal-backed driver. Every shader compiles and links after automatic repairs. 60 fps in the menu at 2560×1440; about 30–35 fps close to Saturn in a 2940×1782 window. |
| Planets | Works | Atmospheres, rings and ring shadows; Jupiter and Saturn needed a fix (finding 9). |
| Steam | Works | Valve's own macOS library: sign-in, ownership and DLC checks, Workshop. Ownership checks run normally. |
| HD packs (DLC) | Works | Downloaded with Steam's console, read from beside the game. |
| Sound | Works | Music and interface sounds. |
| Keyboard, mouse, trackpad | Works | Modifier shortcuts, text fields, copy and paste. Trackpad swipes change speed the natural way, and holding a key no longer opens the accent picker. |
| Controllers | Works | Through SDL3. Tested with a virtual gamepad. |
| Video recording | Works | SVT-AV1 through an LGPL FFmpeg 8 build. The game's other encoders are NVIDIA, AMD and Intel hardware. |
| App | Works | A double-clickable `SpaceEngine.app` with the game's icon. |
| VR | No | There is no OpenVR runtime for macOS. |

Confirmed by a person, not only by tests: window, keyboard, mouse, sound, the
main menu, flying around the planetarium, and mouse input at Retina
resolution. Under investigation: an occasional flicker reported with orbit
lines shown.

## Using it

You need the Windows version of SpaceEngine from Steam on the Mac. Steam won't
install it there normally, so download it from Steam's console
(`steam://open/console`):

```text
download_depot 314650 314651
```

The HD packs are separate downloads into the same place, for example
`download_depot 314650 1083282` for Earth HD. The port finds the game and any
`depot_<id>` folders next to it by itself.

Build and install (needs an Apple Silicon Mac, Xcode's command-line tools and
Homebrew's `sdl3`, `libpng`, `libtiff`, `libvorbis` and `svt-av1`):

```sh
python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt
cd recomp
make -j10 program      # read your SpaceEngine.exe, translate it and compile the result
make install           # build SpaceEngine.app into ~/Applications
```

The first build reads the executable's function and import tables into
`analysis/` and compiles an LGPL FFmpeg from source (`tools/build_ffmpeg.sh`).
It expects exactly version 0.991, build 50.2155; other builds need the few
addresses in `recomp/hle.txt` and `recomp/host/` checked.
Steam has to be running. Messages go to `~/Library/Logs/SpaceEngine-native.log`,
and the game's own files (settings, saves, logs, screenshots, videos) live in
`~/Library/Application Support/SpaceEngine-native/`.

## How it works

1. **Translate.** `recomp/translate.py` follows the control flow of each
   function in the executable and writes it as C: about 12,200 functions and
   1.38 million x86-64 instructions become 1.6 million lines of C. Registers
   live in a structure, flags are computed as needed, and the original stack
   layout is kept, return addresses included.
2. **Check.** The translation is compared against the original running in a
   CPU emulator: every instruction form (715 of 715 match), whole functions
   with random inputs (772 of 772), and 10,102 cases of the game's time and
   orbit routines. All are bit-exact, NaN results included.
3. **Stand in for Windows.** `recomp/host/` answers the 384 Windows functions
   the game calls: windows and input on SDL3, files with the Steam folder kept
   read-only, threads, OpenGL contexts, Steam through Valve's macOS library,
   and the game's bundled libraries (PNG, TIFF, ZIP, audio, FFmpeg).
4. **Repair at run time.** Shaders that Apple's compiler rejects are fixed
   from its own error messages, and missing OpenGL features are emulated.
   The game's code itself is never edited.

About 12,000 lines are written by hand; the rest is generated. The technical
detail, measurements and debugging aids are in [`recomp/README.md`](recomp/README.md).

## What macOS does differently

These are the places where a Windows driver and Apple's driver disagree. Each
one broke something until it was handled, and each is worth knowing for an
official Mac version.

1. **OpenGL stops at 4.1.** The game asks for 4.4. It runs on 4.1 once its
   own `IgnoreCriticalCheck` setting is on, apart from persistently mapped
   buffers (4.4, now emulated) and `glReadnPixels` (4.5, used by the video
   recorder).
2. **Extensions that became core aren't listed.** The game decides features
   by extension name, so it believed it had no 3D textures (and therefore no
   atmosphere models) and no multisampling.
3. **Shader binaries are "supported" with zero formats.** Trusting the
   extension gives a working first launch and a black screen on every later
   one.
4. **The shader compiler is strict GLSL 4.10.** 45 shaders and 4 programs
   needed changes: undefined names in `#if`, qualifier order, 4.20 features,
   1.20 texture functions, variables named like types, stage interfaces
   matched by name instead of location, and two attributes at one location.
5. **Only the main thread may use the window's drawable.** Loader threads'
   contexts must stay detached from it.
6. **Retina.** The game works in pixels, macOS in points; converting at the
   boundary is enough.
7. **Keyboards.** The game checks left and right modifier keys separately, and
   its text fields edit only through character messages. Mac laptops have no
   keypad, and Home, End, Page Up and Down and F-keys need fn.
8. **Video encoders.** NVENC, AMF and QSV don't exist; SVT-AV1 works, and
   Apple's VideoToolbox encoders would be the natural hardware equivalent.
9. **Apple's GPU compiler uses fast math.** The game marks "no rings here"
   with ±1e38. Squared, that overflows; Windows drivers carry the infinity
   through harmlessly, Apple's turns it into NaN, and Jupiter and Saturn drew
   black.

Steam, SDL and LGPL FFmpeg need nothing special on macOS.

## Repository

| Folder | What it holds |
|---|---|
| `recomp/` | The port: translator, checkers, the Windows stand-in (`host/`), runtime, app bundle. Its README has the details. |
| `tools/` | Reading the executable (functions, imports, exception tables, FFmpeg names), a disassembler, a CPU emulator, the FFmpeg build. |
| `docs/` | The report for SpaceEngine's developers. |
| `analysis/` | Made by the build from your executable; not in the repository. |

## Boundaries

- The repository holds only the tools: the translator, the stand-in for
  Windows and the checks. It contains none of the game's code, data or
  derived tables, and none should be added. What the build produces
  (`analysis/`, `recomp/gen/`, `recomp/build/`, the app) contains the game's
  code in translated form: it stays on the machine of the person who owns
  the game and is never shared.
- You need your own copy of SpaceEngine from Steam. Steam's ownership checks
  are never bypassed; Steam has to be running, and the account has to own
  the game and any DLC used.
- The game's encrypted `.pak` archives are never extracted or decrypted
  outside the game, and shader text is only ever looked at as the lines the
  compiler rejects.
- The Steam folder is read-only to the port. Everything the game writes goes
  to its own folder in Application Support.
- SpaceEngine is Cosmographic's. This is an unofficial project, not
  endorsed by them; check with them before distributing it.

A summary for SpaceEngine's developers, with the findings above and no code
of theirs, is in [`docs/mac-port-report.html`](docs/mac-port-report.html).
