// The FFmpeg bridge: generated thunks (gen/ff_thunks.c) and hand-written ones (ffmpeg.c).
#pragma once
#include "host.h"

typedef struct { const char *library, *name; void **native; void (*thunk)(cpu_t *); } ff_entry_t;
extern const ff_entry_t ff_functions[];   // generated; hand-written entries have no thunk
extern const int ff_function_count;

// LoadLibrary / GetProcAddress for the game's four FFmpeg DLLs. 0 when the
// name is not one of them, or the libraries are not built.
uint64_t ffmpeg_load_library(const char *name);
uint64_t ffmpeg_get_proc(uint64_t module, const char *name);

// AVERROR(e) is -e, and errno values differ between Windows' C runtime and
// macOS (EAGAIN: 11 there, 35 here). FFmpeg's own codes (AVERROR_EOF...) agree.
int ff_error_to_windows(int code);
int ff_error_from_windows(int code);
