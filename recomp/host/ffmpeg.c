// FFmpeg for the game's video recorder.
//
// The game loads avcodec-62.dll, avformat-62.dll, avutil-60.dll and
// swscale-9.dll with LoadLibraryA and looks up 297 functions; it records only
// if all of them are found and the library reports an LGPL licence. Here the
// DLLs are an LGPL build of FFmpeg 8.1 made by tools/build_ffmpeg.sh, loaded
// with dlopen. FFmpeg's structures are laid out the same on both sides (same
// headers, no `long` in them), so pointers pass through unchanged and most
// functions are generated thunks (gen_ff.py). Done by hand here:
//
//   file names        Windows paths become the host paths files.c would use
//   av_log callback   FFmpeg calls the game's function; on a thread the game
//                     does not know, the message is printed instead. A va_list
//                     on Apple's arm64 is a pointer to 8-byte slots, as on
//                     Windows x64, so it is passed on as it is
//   interrupt callback (avio_open2) the same, through a host function
//   avio_printf       its variable arguments are collected the same way
//   others taking a callback: not used by the game; they stop with a message
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ffmpeg.h"
#include "paths.h"

#ifndef FFMPEG_LIB_DIR
#error FFMPEG_LIB_DIR must name the directory with the FFmpeg dylibs
#endif

static const struct { const char *dll, *library, *dylib; } libraries[] = {
    {"avcodec-62.dll", "avcodec", "libavcodec.62.dylib"}, {"avformat-62.dll", "avformat", "libavformat.62.dylib"},
    {"avutil-60.dll", "avutil", "libavutil.60.dylib"}, {"swscale-9.dll", "swscale", "libswscale.9.dylib"},
};
enum { LIBRARY_COUNT = 4 };
#define MODULE_MARK 0x7e5a000000000000ull
static void *handles[LIBRARY_COUNT];
static int resolved;

static void *hand_symbol(const char *name);

// Resolves every native function once all four libraries are open.
static int resolve_all(void) {
    if (resolved) return resolved > 0;
    for (int i = 0; i < ff_function_count; i++) {
        int lib = -1;
        for (int k = 0; k < LIBRARY_COUNT; k++) if (!strcmp(libraries[k].library, ff_functions[i].library)) lib = k;
        void *p = lib >= 0 ? dlsym(handles[lib], ff_functions[i].name) : NULL;
        if (!p) { fprintf(stderr, "host: FFmpeg has no %s\n", ff_functions[i].name); resolved = -1; return 0; }
        if (ff_functions[i].native) *ff_functions[i].native = p;
        else if (!hand_symbol(ff_functions[i].name)) { resolved = -1; return 0; }
    }
    resolved = 1;
    return 1;
}

uint64_t ffmpeg_load_library(const char *name) {
    const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    for (int k = 0; k < LIBRARY_COUNT; k++) {
        if (strcasecmp(base, libraries[k].dll)) continue;
        if (!handles[k]) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", FFMPEG_LIB_DIR, libraries[k].dylib);
            handles[k] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!handles[k]) { fprintf(stderr, "host: %s: %s\n", libraries[k].dll, dlerror()); return 0; }
        }
        return MODULE_MARK | (uint64_t)k;
    }
    return 0;
}

static void (*hand_thunk(const char *name))(cpu_t *);

uint64_t ffmpeg_get_proc(uint64_t module, const char *name) {
    if ((module & ~0xffull) != MODULE_MARK) return 0;
    int k = (int)(module & 0xff);
    if (k >= LIBRARY_COUNT || !handles[k]) return 0;
    for (int i = 0; i < LIBRARY_COUNT; i++) if (!handles[i]) return 0;   // the game opens all four first
    if (!resolve_all()) return 0;
    for (int i = 0; i < ff_function_count; i++)
        if (!strcmp(ff_functions[i].name, name) && !strcmp(ff_functions[i].library, libraries[k].library))
            return host_export(ff_functions[i].thunk ? ff_functions[i].thunk : hand_thunk(name));
    return 0;
}

// ------------------------------------------------------------- errors
// macOS errno -> the Windows C runtime's, for the codes that differ. 1 to 34
// agree except 11 (EDEADLK here, EAGAIN there).
static const int errno_pairs[][2] = {
    {35, 11}, {11, 36}, {78, 40}, {60, 138}, {45, 129}, {61, 107}, {36, 112}, {63, 38}, {66, 41},
    {62, 114}, {84, 132}, {92, 42}, {89, 105}, {57, 126}, {54, 108}, {48, 100}, {51, 118}, {65, 110},
    {100, 134}, {55, 119}, {40, 115}, {38, 128}, {37, 103}, {39, 109}, {53, 106}, {49, 101},
};
int ff_error_to_windows(int code) {
    if (code >= 0 || code < -200) return code;
    for (size_t i = 0; i < sizeof errno_pairs / sizeof errno_pairs[0]; i++)
        if (-code == errno_pairs[i][0]) return -errno_pairs[i][1];
    return code;
}
int ff_error_from_windows(int code) {
    if (code >= 0 || code < -200) return code;
    for (size_t i = 0; i < sizeof errno_pairs / sizeof errno_pairs[0]; i++)
        if (-code == errno_pairs[i][1]) return -errno_pairs[i][0];
    return code;
}

// ------------------------------------------------------------- by hand
static __typeof__(av_log_set_callback) *p_av_log_set_callback;
static __typeof__(av_log_default_callback) *p_av_log_default_callback;
static __typeof__(avformat_alloc_output_context2) *p_avformat_alloc_output_context2;
static __typeof__(avio_open) *p_avio_open;
static __typeof__(avio_open2) *p_avio_open2;
static __typeof__(avformat_open_input) *p_avformat_open_input;
static __typeof__(avio_check) *p_avio_check;
static __typeof__(avio_vprintf) *p_avio_vprintf;

// A Windows path ("C:\..." or "C:/...") becomes the host path; anything else
// (a URL, a bare format name) is left alone. *keep receives the copy to free.
static const char *host_url(const char *url, int for_write, char **keep) {
    *keep = NULL;
    if (url && ((url[0] >= 'A' && url[0] <= 'Z') || (url[0] >= 'a' && url[0] <= 'z')) && url[1] == ':' &&
        (url[2] == '\\' || url[2] == '/'))
        return *keep = host_path(url, for_write);
    return url;
}

static uint64_t game_log_callback;
static void log_to_game(void *avcl, int level, const char *fmt, va_list vl) {
    if (!host_cpu || !game_log_callback) { p_av_log_default_callback(avcl, level, fmt, vl); return; }
    uint64_t args[4] = {(uint64_t)avcl, (uint64_t)(int64_t)level, (uint64_t)fmt, (uint64_t)vl};
    host_call_guest(host_cpu, game_log_callback, 4, args);
}
static void t_av_log_set_callback(cpu_t *c) {
    // The game may hand back FFmpeg's own default callback, which it got from us.
    game_log_callback = ARG(0) == host_export(hand_thunk("av_log_default_callback")) ? 0 : ARG(0);
    p_av_log_set_callback(game_log_callback ? log_to_game : p_av_log_default_callback);
}

static void t_avformat_alloc_output_context2(cpu_t *c) {
    char *keep;
    const char *file = host_url(PTR(3), 1, &keep);
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avformat_alloc_output_context2(PTR(0), PTR(1), PTR(2), file));
    free(keep);
}
static void t_avio_open(cpu_t *c) {
    char *keep;
    const char *url = host_url(PTR(1), (int)ARG(2) & AVIO_FLAG_WRITE, &keep);
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avio_open(PTR(0), url, (int)ARG(2)));
    free(keep);
}

// avio_open2's interrupt callback is the game's code; FFmpeg calls this instead.
typedef struct { uint64_t callback, opaque; } guest_interrupt_t;
static int interrupt_from_game(void *opaque) {
    guest_interrupt_t *g = opaque;
    if (!host_cpu) return 0;
    return (int)host_call_guest(host_cpu, g->callback, 1, &g->opaque);
}
static void t_avio_open2(cpu_t *c) {
    char *keep;
    const char *url = host_url(PTR(1), (int)ARG(2) & AVIO_FLAG_WRITE, &keep);
    const AVIOInterruptCB *cb = PTR(3);
    AVIOInterruptCB native = {0};
    if (cb && cb->callback) {
        guest_interrupt_t *g = malloc(sizeof *g);   // lives as long as the AVIOContext may; small, kept
        g->callback = (uint64_t)cb->callback; g->opaque = (uint64_t)cb->opaque;
        native.callback = interrupt_from_game; native.opaque = g;
    }
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avio_open2(PTR(0), url, (int)ARG(2), cb ? &native : NULL, (AVDictionary **)ARG(4)));
    free(keep);
}
static void t_avformat_open_input(cpu_t *c) {
    char *keep;
    const char *url = host_url(PTR(1), 0, &keep);
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avformat_open_input(PTR(0), url, PTR(2), PTR(3)));
    free(keep);
}
static void t_avio_check(cpu_t *c) {
    char *keep;
    const char *url = host_url(PTR(0), (int)ARG(1) & AVIO_FLAG_WRITE, &keep);
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avio_check(url, (int)ARG(1)));
    free(keep);
}
// avio_printf(s, fmt, ...): the arguments after fmt, as 8-byte slots, are a va_list here.
static void t_avio_printf(cpu_t *c) {
    uint64_t slots[16];
    for (int i = 0; i < 16; i++) slots[i] = host_arg(c, 2 + i);
    c->r[RAX] = (uint64_t)(int64_t)ff_error_to_windows(p_avio_vprintf(PTR(0), PTR(1), (va_list)(void *)slots));
}
static void t_unsupported(cpu_t *c) {
    (void)c;
    host_fatal(70, "The game called an FFmpeg function that takes a callback (%s), which is not bridged.", "avcodec_default_execute or avio_alloc_context");
}

static const struct { const char *name; void **native; void (*thunk)(cpu_t *); } hand[] = {
    {"av_log_set_callback", (void **)&p_av_log_set_callback, t_av_log_set_callback},
    {"avformat_alloc_output_context2", (void **)&p_avformat_alloc_output_context2, t_avformat_alloc_output_context2},
    {"avio_open", (void **)&p_avio_open, t_avio_open}, {"avio_open2", (void **)&p_avio_open2, t_avio_open2},
    {"avformat_open_input", (void **)&p_avformat_open_input, t_avformat_open_input},
    {"avio_check", (void **)&p_avio_check, t_avio_check}, {"avio_printf", NULL, t_avio_printf},
    {"avcodec_default_execute", NULL, t_unsupported}, {"avcodec_default_execute2", NULL, t_unsupported},
    {"avio_alloc_context", NULL, t_unsupported},
};

static void *hand_symbol(const char *name) {
    // Natives the hand-written thunks need besides their own.
    if (!p_av_log_default_callback) p_av_log_default_callback = dlsym(handles[2], "av_log_default_callback");
    if (!p_avio_vprintf) p_avio_vprintf = dlsym(handles[1], "avio_vprintf");
    for (size_t i = 0; i < sizeof hand / sizeof hand[0]; i++) {
        if (strcmp(hand[i].name, name)) continue;
        if (!hand[i].native) return (void *)1;
        for (int k = 0; k < LIBRARY_COUNT && !*hand[i].native; k++) *hand[i].native = dlsym(handles[k], name);
        return *hand[i].native;
    }
    return NULL;
}
static void (*hand_thunk(const char *name))(cpu_t *) {
    for (size_t i = 0; i < sizeof hand / sizeof hand[0]; i++) if (!strcmp(hand[i].name, name)) return hand[i].thunk;
    for (int i = 0; i < ff_function_count; i++) if (!strcmp(ff_functions[i].name, name)) return ff_functions[i].thunk;
    return NULL;
}
