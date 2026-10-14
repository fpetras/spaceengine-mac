// The native side of the port: what the translated program gets instead of
// Windows. Each Windows function the game imports is written here as a host
// function that reads its arguments from the translated CPU state.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

// Defines the host implementation of one imported Windows function and
// registers it under its import name. Arguments follow the Win64 convention:
// ARG(0..3) are RCX, RDX, R8, R9, later ones are on the stack. The result
// goes in RAX; the dispatcher pops the return address afterwards.
#define IMPORT(name)                                                              \
    static void imp_##name(cpu_t *c);                                             \
    __attribute__((constructor)) static void reg_##name(void) { host_register(#name, imp_##name); } \
    static void imp_##name(cpu_t *c)

#define ARG(n) host_arg(c, n)
#define PTR(n) ((void *)host_arg(c, n))
#define RET(v) do { c->r[RAX] = (uint64_t)(v); return; } while (0)

static inline uint64_t host_arg(cpu_t *c, int n) {
    static const int reg[4] = {RCX, RDX, R8, R9};
    return n < 4 ? c->r[reg[n]] : ld64(c->r[RSP] + 8 + 8 * (uint64_t)n);
}

void host_register(const char *name, void (*fn)(cpu_t *));
void host_init(void);
// A message the player has to see: printed, and shown as a dialog when on the
// main thread. host_fatal then ends the program with that exit status.
void host_alert(const char *title, const char *text);
_Noreturn void host_fatal(int status, const char *format, ...) __attribute__((format(printf, 2, 3)));

// ------------------------------------------------------------- per thread
enum { HOST_FLS_SLOTS = 128, HOST_TLS_SLOTS = 64 };
typedef struct host_thread {
    uint32_t last_error;
    uint32_t id;
    void *fls[HOST_FLS_SLOTS];
    void *tls[HOST_TLS_SLOTS];
    uint8_t *teb;  // the block translated code reaches through gs:
} host_thread_t;
extern _Thread_local host_thread_t host_thread;

// Gives the calling thread its thread block and thread-local storage and
// points c->gs_base at it.
void host_thread_attach(cpu_t *c);
#define SET_ERROR(e) (host_thread.last_error = (e))
// Runs the image's TLS callbacks on the calling thread: reason 1 at process
// start, 2 when a thread starts.
void host_run_tls_callbacks(cpu_t *c, uint32_t reason);
void host_thread_handle_closed(uint64_t handle);
uint32_t host_new_thread_id(void);

// Calls translated code from inside a host function, for callbacks. Up to
// four integer arguments; returns RAX. Only valid on a thread that is
// currently inside a call from translated code.
uint64_t host_call_guest(cpu_t *c, uint64_t target, int argc, const uint64_t *argv);

// The translated CPU state of the calling thread, for native library
// callbacks that must call back into the game. Set whenever translated code
// calls into the host.
extern _Thread_local cpu_t *host_cpu;

// Makes a host function callable from translated code through a function
// pointer. The returned value is not real code: recomp_call recognises it.
uint64_t host_export(void (*fn)(cpu_t *));
// Implemented in steam.c: handles calls on bridged Steam interface objects.
int steam_foreign_call(cpu_t *c, uint64_t target);

void host_write_png(const char *path, const uint8_t *rgba, int width, int height);

// ---------------------------------------------------------------- strings
// Windows text is UTF-16; the host works in UTF-8.
size_t u16_len(const uint16_t *s);
char *u16_to_utf8(const uint16_t *s, long count);       // count < 0: up to the terminator. malloc'd.
size_t utf8_to_u16(const char *s, uint16_t *out, size_t capacity);  // returns units incl. terminator

// ------------------------------------------------------------------ paths
// Windows paths the game sees, and where they live on this machine.
#define GUEST_ROOT "C:\\SpaceEngine"
#define GUEST_EXE GUEST_ROOT "\\system\\SpaceEngine.exe"
#define GUEST_HOME "C:\\Users\\player"
extern char host_game_root[1024];   // the depot directory holding system/, data/, config/

// ---------------------------------------------------------------- handles
enum { H_STDIN = 0x10, H_STDOUT = 0x14, H_STDERR = 0x18, H_HEAP = 0x1000, H_PROCESS = (uint64_t)-1 };
#define INVALID_HANDLE ((uint64_t)-1)
