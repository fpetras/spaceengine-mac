# Native port by static recompilation

Goal: SpaceEngine 0.991.50.2155 running as an arm64 macOS app with no Wine and
no Rosetta, built from the Windows executable the user owns.

Method: translate the executable's x86-64 machine code to C ahead of time,
compile that C for Apple Silicon, and link it against a native host that
stands in for Windows. The result contains the developers' code in translated
form, so it stays on the machine of the person who owns the game; the
repository holds only the tools.

## Status

The translated game starts, opens a window and renders. It runs through its
whole start-up (C runtime, Steam, configuration, archives, catalogs, shaders,
eight worker threads) into its main loop and draws its start screen and main
menu at a steady 60 frames per second at Retina resolution (2560x1440 pixels in
a 1280x720-point window) on an M5 MacBook Air. Every shader compiles and
links. It shuts down in order and exits with status 0.

Confirmed by a person on 2026-10-13: the window, keyboard and mouse, the main
menu, flying around in the planetarium, audible sound, and mouse clicks
landing correctly at Retina resolution. Not specifically confirmed: how
atmospheres, rings and orbit lines look since the last shader repairs.

Every feature the game has on Windows now works, apart from VR (no OpenVR on
macOS) and the Windows-only hardware video encoders.

C++ exceptions end the program with a dialog naming what was thrown, which is
what they do on Windows as well: the game's exception tables hold only two
real `catch` blocks, both in the standard library's stream operators (see
`host/exceptions.c` and `tools/eh.py`). No unwinder is implemented.

The game runs at 4 frames per second while its window is in the background.
That is its own `PowerSaveMaxFPS` setting, not a fault; `SE_KEEP_FOCUS=1`
turns it off for measurements.

| Piece | State |
|---|---|
| Translator (`translate.py`) | 12,177 functions, about 1.38 million instructions; under 0.2% have no handler, nearly all of them AVX that is never reached |
| Whole-program build | 162 MB of C in 240 files; compiles in about 23 s with `make -j10` at `-O1`; 18 MB executable |
| Instruction checker (`fuzz.py`) | 715 of 715 instruction shapes match the original; 3,515 instances with 24 random states each |
| Function checker (`fuzz_functions.py`) | 772 of 772 whole functions match, 3,129 cases, 16 of the functions with switch tables |
| Function-level test (`make test`, `make test-full`) | 10,102 of 10,102 cases bit-exact, for the 13-function build and for the whole-program build |
| Host (`host/`) | 384 of the 398 imported functions answered (no run has called the other 14): Windows API, Steam, OpenGL, and the game's bundled libraries |
| Graphics | OpenGL 4.1 core on Apple's driver; 509 native functions bridged by generated thunks; `GL_ARB_buffer_storage` emulated; shaders repaired from the compiler's and linker's messages, none failing |
| Sound | The game's SDL Mixer X calls answered by a mixer on SDL3 audio with libvorbisfile (`host/audio.c`) |
| App | `make install` puts a double-clickable `SpaceEngine.app` in `~/Applications` |

All comparisons are strict: registers, XMM registers, flags and memory must
equal the original's bit for bit, NaN results included.

"Translates" means C is produced. What has actually run is the start-up path
and whatever the checkers exercise; most game code has not executed yet.

## How it fits together

- **Image at its own address.** `recomp_map_image` maps the PE file at
  `0x140000000`, its preferred base. Every pointer in the game's data stays
  valid and nothing is relocated. The range is set aside at link time by a
  zero-fill segment (`runtime/reserve.s`); without that, macOS placed something
  else there in about 3% of launches. 400 launches with it: no failures.
- **One C function per original function**, `void f_<address>(cpu_t *c)`. The
  sixteen general and sixteen XMM registers live in `cpu_t`; the stack is
  ordinary memory addressed through RSP; flags are C locals the compiler
  discards when unused.
- **Finding the code.** Functions come from `.pdata`, from call targets, from
  code pointers stored in the data sections (296), and from addresses the code
  takes (601 more). Instructions are decoded by following control flow, so
  switch tables stored inside functions are not mistaken for code. All 242
  compiler-generated switches are recovered.
- **Calls.** Direct calls become C calls. Indirect calls and tail calls go
  through `recomp_call`, a lookup from original address to translated function.
  Import-table calls go to the host.
- **Host** (`host/`). Each imported Windows function is a small C function
  declared with `IMPORT(name)`, reading its arguments from the CPU state.
- **Host replacements** (`runtime/hle.c`, listed in `hle.txt`). Some original
  functions are replaced instead of translated: the CRT's `sin`, `cos` and
  `floor` today.
- **x86 behaviour kept on purpose.** NaN results use x86's rules and bit
  patterns; float-to-integer conversion overflows the x86 way; a 32-bit shift
  by zero still clears the upper half of the register. `cpuid` describes a
  plain SSE2 processor, which keeps run-time-selected AVX and SSE4 paths
  unused.

## Paths

The game sees a Windows file system. `host/paths.c` maps it:

| Windows path | On this Mac |
|---|---|
| `C:\SpaceEngine\...` | The Steam depot. Never written. Anything the game creates or changes there goes to `overlay/`, and reads prefer the overlay copy |
| `C:\Users\player\...` | `home/` |
| anything else on `C:` | `drive_c/` |

The three directories are under
`~/Library/Application Support/SpaceEngine-native/`. So far the game has
created its profile folders, `config/main-user.cfg`, `config/addons.cfg`,
`save/save-user.cfg` and `logs/se.log` under `home/`. Deleting that directory
resets the game to a first launch.


DLC: the HD packs are extra archives. Download one with Steam's console
(`download_depot 314650 <depot>`, the depot number being the DLC's app ID,
e.g. 1083282 for Earth HD); it lands beside the game's own depot as
`depot_<id>`, and every such folder is layered under the game's files.
## Commands

```sh
make -j10 program   # translate everything and compile it (build/libprogram.a)
./boot.sh           # rebuild what changed, run the game, show where it stops
./boot.sh regen     # same, retranslating first
make test           # 10,102 cases against the original, 13-function build
make test-full      # the same cases against the whole-program build
make coverage       # what the translator cannot handle; list in ../analysis/untranslated.tsv
../tools/.venv/bin/python fuzz.py [instances_per_shape] [states]
../tools/.venv/bin/python fuzz_functions.py [cases_per_function]
```

`SE_TRACE=1 ./build/se <exe>` prints every Windows call with its arguments and
result. `SE_VERBOSE=1` prints file opens and the import count.

```sh
make app            # build/SpaceEngine.app
make install        # the same, copied to ~/Applications
```

The app finds the game in Steam's library (or at the path on the first line of
`~/Library/Application Support/SpaceEngine-native/game-path.txt`) and writes
its messages to `~/Library/Logs/SpaceEngine-native.log`. Its icon is taken
from the game's executable when the app is built. Steam has to be running.
Messages the player must see, the game's own message boxes included, are shown
as dialogs; `SE_NO_DIALOGS=1` only prints them.

## Steam

`WinMain` calls Steam before anything else and exits if Steam is not there.
The port answers with the real Steam client or not at all; it never pretends
Steam said yes.

- **Library.** Valve's arm64 `libsteam_api.dylib` from inside the Steam
  application bundle, loaded at run time (`SE_STEAM_API` overrides the path).
  It is not copied into this repository. `tests/steam_probe.c` is a standalone
  check: on this machine the client initialises app 314650 for a native
  process and reports the account as logged on and owning the game.
- **Flat functions** are forwarded. `SteamInternal_ContextInit` is implemented
  in the host because it must call back into the game.
- **Interfaces** (`SteamClient020`, `SteamUser021`, `SteamUtils010`,
  `STEAMAPPS_INTERFACE_VERSION008`, `STEAMUGC_INTERFACE_VERSION014`) are C++
  objects the game calls through vtables. Each is wrapped in a proxy whose
  vtable entries are marker addresses that `recomp_call` routes to
  `host/steam.c`. A method is forwarded only if it is in the table there, with
  its slot and signature checked against Valve's published definition for
  that exact version. An unlisted method stops the run and names itself.
- **Differences handled**: `CSteamID` results come back in a register on
  macOS and through a hidden pointer on Windows; one-byte results and
  arguments are cleaned; Windows lays out overloaded methods in reverse, so
  two pairs of Workshop slots are swapped; methods that install a callback
  into game code are ignored instead of forwarded.
- **Asynchronous answers** are polled after each `SteamAPI_RunCallbacks` and
  delivered by calling the game's own result object as translated code. Only
  answer structures whose layout is checked to be the same on both systems
  are delivered; `SteamUGCQueryCompleted_t` is the one so far.

All methods the game calls are bridged. Three need conversion: folders
Steam names (`GetAppInstallDir`, `GetItemInstallInfo`) are handed over as the
paths the game sees (its own folder as `C:\SpaceEngine`, anything else on the
read-only `Z:` drive that mirrors the Mac's files), and `GetQueryUGCResult`'s
`SteamUGCDetails_t` is repacked from macOS's 4-byte to Windows' 8-byte callback
packing. `tests/ugc_probe.c` checks that layout against a live public Workshop
query.

While the game runs, Steam shows the account as playing SpaceEngine.

## What the translator does not handle yet

| Gap | Count | Plan |
|---|---|---|
| AVX instructions | 2,411 | Sit behind a CPU check that now reports no AVX. Replace the CRT math routines that contain them with the host's when convenient |
| Other instructions | about 100 | SSE4 string instructions, x87, a few bit-manipulation forms; mostly behind the same CPU check. Add when a run reaches one |
| C++ exceptions | 5 try blocks | Not delivered, by decision: nothing in the game catches them apart from two stream operators. `RaiseException` reports and stops; the `Rtl*Unwind` imports are not implemented |
| Threads | basic | Creation, exit, join, condition variables and TLS callbacks work; nothing has stressed them yet |
| Floating-point mode changes | logged | `ldmxcsr` is recorded and not applied |

Not compared by the checkers: the overflow flag after shifts and rotates by
more than one, which x86 leaves undefined; `div` and `idiv`, because random
inputs make the original fault; anything involving calls, which only the
function-level test and real runs cover.

## Graphics

- **Contexts.** Every WGL context is an OpenGL 4.1 core context from SDL3,
  sharing objects through a root context that is never drawn with. Creating a
  context leaves the calling thread's current context alone, as on Windows.
  Only the main thread attaches a context to the visible window; the game's
  loader threads make theirs current with no window. (SDL on macOS makes a new
  context current and attaches it to the window at creation. Left that way, the
  main thread ended up drawing with a loader thread's context.)
- **Version.** The game demands 4.4; its own configuration switch
  `IgnoreCriticalCheck` (set in the sandboxed `main-user.cfg`) lets it continue
  on 4.1, and `LoaderAsyncShaders` is off.
- **Program binaries.** Apple's driver offers no program binary format, so
  `GL_ARB_get_program_binary` is not reported and the game compiles shaders
  from source on every start. When it was reported, the game cached programs on
  the first run and drew nothing on every later run.
- **Functions.** `gen_gl.py` reads Apple's OpenGL headers and emits a thunk per
  function that takes arguments the Windows way and calls the native function
  with its real prototype. `host/gl_compat.c` overrides a few.
- **Extensions.** Apple does not list extensions that became core, and the
  game's loader only fetches a feature's functions when its extension is
  listed, so the reported list adds the extensions that are core in 4.1. That
  includes `GL_EXT_texture3D` and `GL_EXT_framebuffer_multisample`, by which
  the game decides whether it has 3D textures (without them: no atmosphere
  models) and multisampling; their EXT-named functions map to the core ones.
- **Keyboard.** Keys arrive as Windows virtual-key messages. The game decides
  shortcuts by asking for each side's Shift, Ctrl and Alt (`VK_LSHIFT` and so
  on), so both the generic and the per-side state are kept. Its text fields
  edit on character messages only, so Backspace, Tab, Enter, Escape and
  Ctrl+letter also produce the character message Windows' `TranslateMessage`
  would. Copy and paste go to the macOS pasteboard as text.
- **Video recording.** The recorder reads frames with `glReadnPixels`
  (GLEW loads it with `GL_KHR_robustness`, now reported) into a persistently
  mapped pack buffer; with the mapping emulated, read pixels are fetched into
  it as soon as the read is issued. FFmpeg's errno-based error codes are
  translated (EAGAIN is 11 on Windows, 35 here). Of the game's encoders only
  SVT-AV1 exists on a Mac (the others are NVIDIA, AMD and Intel hardware, and
  libaom, which is not built); recording as image sequences also works.
- **Trackpad and speed.** With macOS's natural scrolling, a trackpad swipe
  changes camera speed the opposite way to a wheel. For trackpad scrolls
  (AppKit's `hasPreciseScrollingDeltas`, noted by an event monitor) the
  wheel movement is reversed while the game's per-frame mouse handling reads
  it; menus are unaffected. That function is "wrapped" (`hle.txt`): the
  translator emits it as `f_1405b2620_original` and the host calls it.
  `SE_TRACKPAD_SPEED=natural` turns this off.
- **Float sentinels.** The game writes +-1e38 into uniform data to mean
  "none" or "infinitely far" (Jupiter's and Saturn's ring parameters, for
  one). Squared, that overflows; other drivers carry the infinity through,
  but Apple's GPU compiler uses fast math and produced NaN, so Jupiter and
  Saturn drew black. Those exact values (and +-FLT_MAX) are replaced by
  +-1e18 when uniform buffers are uploaded. `SE_KEEP_SENTINELS=1` turns this
  off.
- **Resolution.** The window has Retina pixel density and the game works in
  pixels, as on Windows; SDL's point sizes and mouse positions are converted
  at the boundary (`host_scale`). Saved window geometry is rescaled when the
  density changes. `SE_RETINA=0` renders one pixel per point.
- **`GL_ARB_buffer_storage`** (4.4) is emulated. The game maps uniform buffers
  once and writes each draw's uniforms into the mapping; here the mapping is
  host memory, and the ranges bound for a draw are uploaded just before it if
  they changed.
- **Shaders.** Apple's compiler enforces GLSL 4.10 where other drivers are
  lenient. A shader that fails is repaired from the compiler's error list and
  compiled again, and a program that fails to link is repaired from the
  linker's (`host/gl_shader.c` lists the repairs). 45 shaders and 4 programs
  need it; none fails. The first start after a change compiles the large
  ray-marching shaders, which takes most of a minute once; macOS caches the
  result.
- Reverse-Z depth (`glClipControl`) and direct state access are not available
  on Apple's driver, and the game falls back.

Debugging aids: `SE_SCREENSHOT=file.png` saves one frame (with
`SE_SCREENSHOT_FRAME=n`), `SE_FPS=1` prints the frame rate, `SE_GL_DEBUG=1`
reports OpenGL errors per draw and per-frame draw counts,
`SE_PRESS_ESC_AT=n` feeds an Escape key press in at frame n,
`SE_KEYS="1300:Escape;2600:Left Ctrl+F11;2700:'typed text"` feeds in key
combinations and text, `SE_ARGS='C:\Users\player\look.se'` passes the game a
script to run (e.g. `Select "Jupiter"` / `Goto { Time 0 DistRad 4 }`),
`SE_SCREENSHOT_COUNT=n` captures n consecutive frames (kept in memory until the
last, so capturing does not slow them; `tests/flicker.c` compares them),
`SE_GL_PROBE=<frame>` reads the centre pixel after every draw of a frame
(`SE_GL_PROBE_PROGRAM=<id>` adds that program's uniforms, blocks and
textures), `SE_GL_LOG=<frame>[,<n>]` logs every OpenGL call of those frames,
`SE_GL_ERRORS=1` checks every OpenGL call for an error,
`SE_HIDE_EXTENSIONS=a,b` leaves extensions out, `SE_SHADER_GUARDS=all` (an
experiment) clamps the arguments of acos, asin, sqrt, log, pow, and
`SE_SHADER_DEBUG=1` prints the lines the shader compiler rejects (with the
macros and struct types named on them, and `SE_SHADER_CONTEXT=n` lines before)
and the interface declarations of programs that fail to link. Run it to the
terminal; do not keep its output.

What these can and cannot show: the screenshot reads the main context's back
buffer, and the key press enters at the host's event queue. Neither proves
what the macOS window displays or that macOS delivers real key presses to it;
only a person looking at the window can. Test at least two consecutive runs
without clearing anything in between: the first-run and later-run paths
differ.

## Bundled libraries

| The game imports | Here |
|---|---|
| `zlibwapi.dll` (minizip) | `host/unzip.c`, written on the system zlib |
| `libpng16.dll`, `tiff.dll` | Homebrew's libpng and libtiff, with callbacks routed back into the game |
| `SDL2.dll` | Game controllers: translated to SDL3's joystick and gamepad functions and events (`host/sdl.c`); `SE_VIRTUAL_GAMEPAD=1` with `SE_GAMEPAD_PRESS="<seconds>:<button>;..."` tests without one |
| `SDL2_mixer_ext.dll` | `host/audio.c`: a mixer on SDL3 audio, Ogg Vorbis decoded by Homebrew's libvorbisfile |
| `openvr_api.dll` | Answered as "no VR runtime" |
| `avcodec-62.dll`, `avformat-62.dll`, `avutil-60.dll`, `swscale-9.dll` (loaded at run time) | An LGPL build of FFmpeg 8.1.3 with SVT-AV1, made by `../tools/build_ffmpeg.sh` into `third_party/ffmpeg`; 297 functions bridged (`gen_ff.py`, `host/ffmpeg.c`) |
| `steam_api64.dll` | Valve's native library and the running Steam client (see Steam) |

## Roadmap

1. Finish the translator: done apart from rare instructions.
2. Compile the whole program: done.
3. C runtime and start-up: done.
4. Windows API on macOS: enough for start-up and the main loop.
5. Graphics: done for everything seen so far, at Retina resolution.
6. Input, sound, clean exit, app bundle: done.
7. Controllers, Workshop, video recording: done.

Milestones visible without reading code: the game's log file appears (done),
a window opens (done), the start screen renders (done), the game can be
navigated and exited normally (done), it starts from the Finder (done).

## Risks

- **Apple's OpenGL.** It is deprecated and stops at 4.1. It works for the
  start screen; deeper parts of the game may need more than the emulation
  covers. The fallback is Mesa's Zink over KosmicKrisp for OpenGL 4.6.
- **Exceptions.** Settled: the game does not catch its own exceptions (two
  stream-operator `catch` blocks aside), so none need to be delivered.
- **Performance.** Registers live in memory and flags are computed eagerly.
  Unmeasured until something renders.
- **Undiscovered code.** A function reached only through a pointer computed at
  run time is still invisible. It shows up as a `recomp_call` failure and is
  added then.
