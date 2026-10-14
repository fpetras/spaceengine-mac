// KERNEL32: process, memory, time, locks and text conversion. Enough for the
// C runtime's start-up; files, threads and the rest are added as the game
// reaches them.
#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include "host.h"
#include "ffmpeg.h"

enum { ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122, ERROR_MOD_NOT_FOUND = 126,
       ERROR_PROC_NOT_FOUND = 127, ERROR_INVALID_HANDLE = 6 };

// ------------------------------------------------------------ process info
IMPORT(GetCurrentProcess) { RET(H_PROCESS); }
IMPORT(GetCurrentProcessId) { RET((uint32_t)getpid()); }
IMPORT(GetCurrentThreadId) { RET(host_thread.id); }
IMPORT(GetLastError) { RET(host_thread.last_error); }
IMPORT(SetLastError) { host_thread.last_error = (uint32_t)ARG(0); }
IMPORT(IsDebuggerPresent) { RET(0); }
IMPORT(SetUnhandledExceptionFilter) { RET(0); }
IMPORT(UnhandledExceptionFilter) { RET(0); }
IMPORT(ExitProcess) {
    extern void gl_shader_report(void);
    if (getenv("SE_VERBOSE")) gl_shader_report();
    fflush(NULL);
    exit((int)ARG(0));
}
IMPORT(TerminateProcess) { fflush(NULL); _exit((int)ARG(1)); }
IMPORT(Sleep) { usleep((useconds_t)ARG(0) * 1000); }
IMPORT(SwitchToThread) { sched_yield(); RET(1); }
// The only modules that exist are the game itself and a stand-in for
// ntdll.dll, which the game asks for RtlGetVersion.
#define NTDLL_MODULE 0x7ffe00000000ull
static void rtl_get_version(cpu_t *c) {
    // RTL_OSVERSIONINFOW: size, major, minor, build, platform, service pack text.
    // The host presents itself as Windows 10 22H2, the environment it emulates.
    uint64_t info = c->r[RCX];
    st32(info + 4, 10); st32(info + 8, 0); st32(info + 12, 19045); st32(info + 16, 2);
    st16(info + 20, 0);
    c->r[RAX] = 0;
}
IMPORT(GetModuleHandleW) {
    if (!ARG(0)) RET(0x140000000ull);
    char *name = u16_to_utf8(PTR(0), -1);
    int ntdll = !strcasecmp(name, "ntdll.dll") || !strcasecmp(name, "ntdll");
    free(name);
    if (ntdll) RET(NTDLL_MODULE);
    SET_ERROR(ERROR_MOD_NOT_FOUND); RET(0);
}
IMPORT(GetModuleHandleExW) {
    if (!ARG(1) || (ARG(0) & 4)) { st64(ARG(2), 0x140000000ull); RET(1); }  // null name, or lookup by address
    st64(ARG(2), 0); SET_ERROR(ERROR_MOD_NOT_FOUND); RET(0);
}
IMPORT(GetModuleFileNameW) {
    size_t need = utf8_to_u16(GUEST_EXE, PTR(1), ARG(2));
    RET(need - 1 < ARG(2) ? need - 1 : ARG(2));
}
IMPORT(GetModuleFileNameA) {
    size_t n = strlen(GUEST_EXE);
    if (n >= ARG(2)) { SET_ERROR(ERROR_INSUFFICIENT_BUFFER); RET(ARG(2)); }
    memcpy(PTR(1), GUEST_EXE, n + 1);
    RET(n);
}
// The game takes a script to run (a Windows path) or -mode=... after its own
// name. SE_ARGS supplies them.
static const char *command_line(void) {
    static char line[2048];
    if (!line[0]) {
        const char *extra = getenv("SE_ARGS");
        snprintf(line, sizeof line, "\"%s\"%s%s", GUEST_EXE, extra ? " " : "", extra ? extra : "");
    }
    return line;
}
IMPORT(GetCommandLineA) { RET(command_line()); }
IMPORT(GetCommandLineW) {
    static uint16_t line[2048];
    if (!line[0]) utf8_to_u16(command_line(), line, 2048);
    RET(line);
}
IMPORT(GetStartupInfoW) { memset(PTR(0), 0, 104); st32(ARG(0), 104); }
IMPORT(IsProcessorFeaturePresent) {
    uint32_t f = (uint32_t)ARG(0);
    RET(f == 6 || f == 10 || f == 3 || f == 8 || f == 12);  // MMX, SSE, SSE2, RDTSC, NX. Not fast-fail (23).
}

// ----------------------------------------------------------------- memory
// The process heap. Every block has 64 readable bytes in front of it and 64
// zero bytes behind it, as a Windows heap block has its header and padding.
// Programs get away with reading slightly outside a block on Windows, and
// this game does so at least once; macOS places large blocks at the very
// start of a mapping, where the same read would fault.
enum { HEAP_FRONT = 64, HEAP_BACK = 64 };
static void *heap_alloc(size_t size, int zero) {
    uint8_t *base = zero ? calloc(1, size + HEAP_FRONT + HEAP_BACK) : malloc(size + HEAP_FRONT + HEAP_BACK);
    if (!base) return NULL;
    if (!zero) { memset(base, 0, HEAP_FRONT); memset(base + HEAP_FRONT + size, 0, HEAP_BACK); }
    memcpy(base + HEAP_FRONT - 8, &size, 8);
    return base + HEAP_FRONT;
}
static size_t heap_size(const void *p) { size_t n; memcpy(&n, (const uint8_t *)p - 8, 8); return n; }
IMPORT(GetProcessHeap) { RET(H_HEAP); }
IMPORT(HeapAlloc) { RET(heap_alloc(ARG(2), (ARG(1) & 8) != 0)); }
IMPORT(HeapFree) { if (ARG(2)) free((uint8_t *)PTR(2) - HEAP_FRONT); RET(1); }
IMPORT(HeapSize) { RET(heap_size(PTR(2))); }
IMPORT(HeapReAlloc) {
    if (!ARG(2)) RET(heap_alloc(ARG(3), (ARG(1) & 8) != 0));
    size_t before = heap_size(PTR(2)), want = ARG(3);
    uint8_t *base = realloc((uint8_t *)PTR(2) - HEAP_FRONT, want + HEAP_FRONT + HEAP_BACK);
    if (!base) RET(0);
    memcpy(base + HEAP_FRONT - 8, &want, 8);
    if ((ARG(1) & 8) && want > before) memset(base + HEAP_FRONT + before, 0, want - before);
    memset(base + HEAP_FRONT + want, 0, HEAP_BACK);
    RET(base + HEAP_FRONT);
}
IMPORT(HeapCompact) { RET(0); }
IMPORT(VirtualProtect) { if (ARG(3)) st32(ARG(3), 4); RET(1); }  // the image is mapped read-write
IMPORT(EncodePointer) { RET(ARG(0)); }
IMPORT(DecodePointer) { RET(ARG(0)); }
IMPORT(InitializeSListHead) { memset(PTR(0), 0, 16); }
IMPORT(LocalAlloc) { RET((ARG(0) & 0x40) ? calloc(1, ARG(1) ? ARG(1) : 1) : malloc(ARG(1) ? ARG(1) : 1)); }
IMPORT(LocalFree) { free(PTR(0)); RET(0); }
// Global memory, used for clipboard data: a handle is the block's address.
IMPORT(GlobalAlloc) { RET(calloc(1, ARG(1) ? ARG(1) : 1)); }
IMPORT(GlobalLock) { RET(ARG(0)); }
IMPORT(GlobalUnlock) { RET(1); }
IMPORT(GlobalFree) { free(PTR(0)); RET(0); }

// ------------------------------------------------------------------- time
static uint64_t filetime_now(void) {  // 100 ns units since 1601-01-01
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
IMPORT(GetSystemTimeAsFileTime) { st64(ARG(0), filetime_now()); }
IMPORT(QueryPerformanceCounter) { st64(ARG(0), clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 100); RET(1); }
IMPORT(QueryPerformanceFrequency) { st64(ARG(0), 10000000); RET(1); }
IMPORT(GetTickCount) { RET((uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000)); }
IMPORT(GetTickCount64) { RET(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000); }

// ------------------------------------------------- thread and fiber slots
static int fls_used = 1, tls_used = 1;  // slot 0 stays unused so 0 is never a valid index
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;
IMPORT(FlsAlloc) {
    pthread_mutex_lock(&slot_lock);
    uint32_t i = fls_used < HOST_FLS_SLOTS ? (uint32_t)fls_used++ : 0xffffffffu;
    pthread_mutex_unlock(&slot_lock);
    RET(i);
}
IMPORT(FlsFree) { RET(1); }
IMPORT(FlsGetValue) { SET_ERROR(0); RET(ARG(0) < HOST_FLS_SLOTS ? host_thread.fls[ARG(0)] : 0); }
IMPORT(FlsSetValue) { if (ARG(0) >= HOST_FLS_SLOTS) RET(0); host_thread.fls[ARG(0)] = PTR(1); RET(1); }
IMPORT(TlsAlloc) {
    pthread_mutex_lock(&slot_lock);
    uint32_t i = tls_used < HOST_TLS_SLOTS ? (uint32_t)tls_used++ : 0xffffffffu;
    pthread_mutex_unlock(&slot_lock);
    RET(i);
}
IMPORT(TlsFree) { RET(1); }
IMPORT(TlsGetValue) { SET_ERROR(0); RET(ARG(0) < HOST_TLS_SLOTS ? host_thread.tls[ARG(0)] : 0); }
IMPORT(TlsSetValue) { if (ARG(0) >= HOST_TLS_SLOTS) RET(0); host_thread.tls[ARG(0)] = PTR(1); RET(1); }

// ------------------------------------------------------------------ locks
// A CRITICAL_SECTION is 40 bytes the game owns. Its first field holds a
// pointer to the recursive mutex that does the work.
static void cs_init(uint64_t cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, &attr);
    memset((void *)cs, 0, 40);
    st64(cs, (uint64_t)m);
}
IMPORT(InitializeCriticalSection) { cs_init(ARG(0)); }
IMPORT(InitializeCriticalSectionAndSpinCount) { cs_init(ARG(0)); RET(1); }
IMPORT(InitializeCriticalSectionEx) { cs_init(ARG(0)); RET(1); }
// The game's exit-time destructors enter sections it has already deleted,
// which Windows lets through; so does this.
IMPORT(EnterCriticalSection) { pthread_mutex_t *m = (pthread_mutex_t *)ld64(ARG(0)); if (m) pthread_mutex_lock(m); }
IMPORT(LeaveCriticalSection) { pthread_mutex_t *m = (pthread_mutex_t *)ld64(ARG(0)); if (m) pthread_mutex_unlock(m); }
IMPORT(DeleteCriticalSection) {
    pthread_mutex_t *m = (pthread_mutex_t *)ld64(ARG(0));
    if (m) { pthread_mutex_destroy(m); free(m); st64(ARG(0), 0); }
}
// An SRWLOCK is one zero-initialised pointer with no init call, which is
// exactly what os_unfair_lock is.
IMPORT(AcquireSRWLockExclusive) { os_unfair_lock_lock((os_unfair_lock *)PTR(0)); }
IMPORT(ReleaseSRWLockExclusive) { os_unfair_lock_unlock((os_unfair_lock *)PTR(0)); }
IMPORT(TryAcquireSRWLockExclusive) { RET(os_unfair_lock_trylock((os_unfair_lock *)PTR(0))); }

// ------------------------------------------------------- dynamic loading
// Nothing can be loaded by name yet. The C runtime probes optional system
// DLLs this way and copes with "not found".
// The only libraries there are: the game's FFmpeg DLLs (ffmpeg.c).
static uint64_t load_library(const char *name) {
    uint64_t module = name ? ffmpeg_load_library(name) : 0;
    if (!module) host_thread.last_error = ERROR_MOD_NOT_FOUND;
    return module;
}
IMPORT(LoadLibraryExW) { char *n = u16_to_utf8(PTR(0), -1); uint64_t m = load_library(n); free(n); RET(m); }
IMPORT(LoadLibraryW) { char *n = u16_to_utf8(PTR(0), -1); uint64_t m = load_library(n); free(n); RET(m); }
IMPORT(LoadLibraryA) { RET(load_library(PTR(0))); }
IMPORT(FreeLibrary) { RET(1); }
IMPORT(GetProcAddress) {
    if (ARG(0) == NTDLL_MODULE && ARG(1) > 0xffff && !strcmp(PTR(1), "RtlGetVersion")) RET(host_export(rtl_get_version));
    uint64_t ff = ARG(1) > 0xffff ? ffmpeg_get_proc(ARG(0), PTR(1)) : 0;
    if (ff) RET(ff);
    SET_ERROR(ERROR_PROC_NOT_FOUND); RET(0);
}
IMPORT(RtlPcToFileHeader) {
    uint64_t base = ARG(0) >= 0x140000000ull && ARG(0) < 0x141a93000ull ? 0x140000000ull : 0;
    st64(ARG(1), base);
    RET(base);
}

// ------------------------------------------------------- standard handles
IMPORT(GetStdHandle) {
    uint32_t which = (uint32_t)ARG(0);
    RET(which == (uint32_t)-10 ? H_STDIN : which == (uint32_t)-11 ? H_STDOUT : which == (uint32_t)-12 ? H_STDERR : INVALID_HANDLE);
}
IMPORT(SetStdHandle) { RET(1); }
IMPORT(GetFileType) { RET(ARG(0) == H_STDIN || ARG(0) == H_STDOUT || ARG(0) == H_STDERR ? 2 : 1); }  // character device, else disk
IMPORT(GetConsoleMode) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }  // not a console: the runtime then uses WriteFile
IMPORT(GetConsoleOutputCP) { RET(65001); }

// --------------------------------------------------------- text and locale
enum { CP_ACP = 0, CP_UTF8 = 65001 };
IMPORT(GetACP) { RET(1252); }
IMPORT(GetOEMCP) { RET(437); }
IMPORT(IsValidCodePage) { RET(ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == CP_UTF8 || ARG(0) == 20127); }
IMPORT(GetCPInfo) {
    uint8_t *info = PTR(1);
    memset(info, 0, 20);
    st32((uint64_t)info, ARG(0) == CP_UTF8 ? 4 : 1);  // MaxCharSize
    info[4] = '?';                                    // DefaultChar
    RET(1);
}
IMPORT(GetUserDefaultLCID) { RET(0x0409); }
IMPORT(IsValidLocale) { RET(1); }
// Code pages other than UTF-8 are treated as Latin-1, which matches Windows
// 1252 everywhere except 0x80-0x9f.
IMPORT(MultiByteToWideChar) {
    uint32_t cp = (uint32_t)ARG(0);
    const unsigned char *src = PTR(2);
    long n = (int32_t)ARG(3);
    uint16_t *dst = PTR(4);
    long cap = (int32_t)ARG(5);
    size_t len = n < 0 ? strlen((const char *)src) + 1 : (size_t)n;
    if (cp == CP_UTF8) {
        char *tmp = malloc(len + 1);
        memcpy(tmp, src, len);
        tmp[len] = 0;
        size_t units = utf8_to_u16(tmp, NULL, 0) - 1 + (n < 0 ? 1 : 0);
        if (n >= 0) { size_t real = strlen(tmp); if (real < len) units += len - real; }
        if (cap > 0) {
            if ((size_t)cap < units) { free(tmp); SET_ERROR(ERROR_INSUFFICIENT_BUFFER); RET(0); }
            uint16_t *wide = malloc((units + 2) * 2);
            utf8_to_u16(tmp, wide, units + 2);
            memcpy(dst, wide, units * 2);
            free(wide);
        }
        free(tmp);
        RET(units);
    }
    if (cap == 0) RET(len);
    if ((size_t)cap < len) { SET_ERROR(ERROR_INSUFFICIENT_BUFFER); RET(0); }
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    RET(len);
}
IMPORT(WideCharToMultiByte) {
    uint32_t cp = (uint32_t)ARG(0);
    const uint16_t *src = PTR(2);
    long n = (int32_t)ARG(3);
    char *dst = PTR(4);
    long cap = (int32_t)ARG(5);
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    if (ARG(7)) st32(ARG(7), 0);  // lpUsedDefaultChar
    if (cp == CP_UTF8) {
        char *utf8 = u16_to_utf8(src, (long)len);
        size_t bytes = 0;
        // u16_to_utf8 stops writing at nothing; embedded terminators are kept by length.
        for (size_t i = 0; i < len; i++) {
            uint32_t u = src[i];
            bytes += u < 0x80 ? 1 : u < 0x800 ? 2 : (u >= 0xd800 && u < 0xdc00) ? 4 : (u >= 0xdc00 && u < 0xe000) ? 0 : 3;
        }
        if (cap > 0) {
            if ((size_t)cap < bytes) { free(utf8); SET_ERROR(ERROR_INSUFFICIENT_BUFFER); RET(0); }
            memcpy(dst, utf8, bytes);
        }
        free(utf8);
        RET(bytes);
    }
    if (cap == 0) RET(len);
    if ((size_t)cap < len) { SET_ERROR(ERROR_INSUFFICIENT_BUFFER); RET(0); }
    for (size_t i = 0; i < len; i++) dst[i] = src[i] < 0x100 ? (char)src[i] : '?';
    RET(len);
}
IMPORT(GetStringTypeW) {
    // CT_CTYPE1 only: upper, lower, digit, space, punct, cntrl, blank, hex, alpha.
    const uint16_t *src = PTR(1);
    long n = (int32_t)ARG(2);
    uint16_t *out = PTR(3);
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    for (size_t i = 0; i < len; i++) {
        wint_t ch = src[i];
        uint16_t t = 0;
        if (iswupper(ch)) t |= 0x001;
        if (iswlower(ch)) t |= 0x002;
        if (iswdigit(ch)) t |= 0x004;
        if (iswspace(ch)) t |= 0x008;
        if (iswpunct(ch)) t |= 0x010;
        if (iswcntrl(ch)) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040;
        if (iswxdigit(ch)) t |= 0x080;
        if (iswalpha(ch)) t |= 0x100;
        out[i] = t;
    }
    RET(1);
}
static uint64_t map_string(uint32_t flags, const uint16_t *src, long n, uint16_t *dst, long cap) {
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    if (cap == 0) return len;
    if ((size_t)cap < len) { SET_ERROR(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (size_t i = 0; i < len; i++)
        dst[i] = (flags & 0x200) ? (uint16_t)towupper(src[i]) : (flags & 0x100) ? (uint16_t)towlower(src[i]) : src[i];
    return len;
}
IMPORT(LCMapStringW) { RET(map_string((uint32_t)ARG(1), PTR(2), (int32_t)ARG(3), PTR(4), (int32_t)ARG(5))); }
IMPORT(LCMapStringEx) { RET(map_string((uint32_t)ARG(1), PTR(2), (int32_t)ARG(3), PTR(4), (int32_t)ARG(5))); }
