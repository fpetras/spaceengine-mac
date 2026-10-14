# SpaceEngine native Mac port

Goal: SpaceEngine 0.991.50.2155 running as an arm64 macOS app on an M5 MacBook
Air, with no Wine and no Rosetta. Method: static recompilation of the Windows
executable (x86-64 machine code to C to arm64) plus a native runtime in place
of Windows. Status, design and roadmap: `recomp/README.md`.

## Layout

- `recomp/` is the port: `translate.py` (translator), `fuzz.py` and
  `fuzz_functions.py` (checkers), `host/` (the stand-in for Windows),
  `runtime/`, `tests/` (including `difftest.py`, the function-level test).
- `tools/` reads the executable (`pe.py`, `pe_analyze.py`, `eh.py`,
  `ffmpeg_names.py`, `find_game.py`), and holds `disasm.py` and `emu.py`, a
  unicorn-based emulator that runs original functions for comparison.
- `analysis/` (not in git) is made by the build from the player's
  executable: function list, imports, FFmpeg names.

The game executable is found by `tools/find_game.py`.

## Working rules

- Every new instruction handler in `translate.py` must pass `recomp/fuzz.py`
  before it is trusted. Run `make test` in `recomp/` after translator or
  runtime changes.
- Use `tools/.venv/bin/python`. The system Python lacks unicorn and capstone.
- Read instructions with `tools/disasm.py <address>` when semantics matter.
- Record measured numbers in the READMEs, and keep "translates" (C was
  produced) separate from "verified" (ran and matched the original).

## Boundaries

- Nothing derived from the game goes into the repository: no code, data,
  decompiler output, tables about the executable or translated output. The
  build derives what it needs from the player's own copy.
- Do not extract or decrypt the game's `.pak` archives; the game does that
  itself at run time. Look at shader text only as the lines the compiler
  rejects. Do not bypass Steam ownership checks.
