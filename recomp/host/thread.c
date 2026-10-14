// The per-thread block Windows code reaches through the gs segment, and the
// image's static thread-local storage.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

_Thread_local host_thread_t host_thread;
static atomic_uint next_thread_id = 1000;

enum { TEB_SIZE = 0x2000, TEB_STACK_BASE = 0x08, TEB_STACK_LIMIT = 0x10, TEB_SELF = 0x30, TEB_TLS_ARRAY = 0x58,
       TEB_PEB = 0x60 };

// The process environment block every thread block points to. The C runtime
// reads a few fields directly: the process parameters' flags, the image base
// and the Windows version. Everything else is zero.
static uint8_t *process_block(void) {
    static uint8_t *peb;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    extern void host_make_peb(uint8_t **out);
    (void)once;
    if (!peb) host_make_peb(&peb);
    return peb;
}
void host_make_peb(uint8_t **out) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!*out) {
        uint8_t *peb = calloc(1, 0x1000), *parameters = calloc(1, 0x1000);
        st64((uint64_t)peb + 0x10, 0x140000000ull);          // ImageBaseAddress
        st64((uint64_t)peb + 0x20, (uint64_t)parameters);    // ProcessParameters
        st32((uint64_t)peb + 0x118, 10);                     // OSMajorVersion
        st32((uint64_t)peb + 0x11c, 0);                      // OSMinorVersion
        st16((uint64_t)peb + 0x120, 19045);                  // OSBuildNumber
        st32((uint64_t)peb + 0x124, 2);                      // OSPlatformId: NT
        *out = peb;
    }
    pthread_mutex_unlock(&lock);
}

uint32_t host_new_thread_id(void) { return atomic_fetch_add(&next_thread_id, 4); }

void host_thread_attach(cpu_t *c) {
    const uint64_t base = 0x140000000ull;
    uint8_t *teb = calloc(1, TEB_SIZE);
    host_thread.teb = teb;
    if (!host_thread.id) host_thread.id = host_new_thread_id();   // a created thread already has the ID its creator was told
    uint8_t *stack = recomp_stack_low();
    st64((uint64_t)teb + TEB_STACK_BASE, (uint64_t)stack + RECOMP_STACK_SIZE);
    st64((uint64_t)teb + TEB_STACK_LIMIT, (uint64_t)stack);
    st64((uint64_t)teb + TEB_SELF, (uint64_t)teb);
    st64((uint64_t)teb + TEB_PEB, (uint64_t)process_block());

    // Static TLS: the PE's TLS directory names a template; every thread gets
    // a private copy, found through slot `*index` of the thread's TLS array.
    uint32_t pe = ld32(base + 0x3c);
    uint32_t tls_rva = ld32(base + pe + 24 + 112 + 9 * 8);
    uint64_t *slots = calloc(64, sizeof *slots);
    st64((uint64_t)teb + TEB_TLS_ARRAY, (uint64_t)slots);
    if (tls_rva) {
        uint64_t start = ld64(base + tls_rva), end = ld64(base + tls_rva + 8), index = ld64(base + tls_rva + 16);
        uint32_t zero_fill = ld32(base + tls_rva + 32);
        uint8_t *block = calloc(1, (size_t)(end - start) + zero_fill + 64);
        memcpy(block, (void *)start, (size_t)(end - start));
        st32(index, 0);
        slots[0] = (uint64_t)block;
    }
    c->gs_base = (uint64_t)teb;
}
