// Host-side services for the translated code.
#pragma once
#include "x86.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maps the PE file at its preferred image base so that every address the
// translated code uses is the address the original used. Returns 0 on success.
int recomp_map_image(const char *path);

// Address range the program set aside at link time for the image (reserve.s).
// Call before recomp_map_image; without it the mapping is best-effort.
void recomp_set_reserved_range(const void *start, size_t size);
extern char recomp_reserved_range[];
#define RECOMP_RESERVED_SIZE 0x12000000ul

// The translated function for an original address, or null.
recomp_fn recomp_lookup(uint64_t target);
// Set by the host to receive calls through the import table.
extern void (*recomp_import_hook)(cpu_t *c, uint64_t slot);
// Set by the host to handle calls to addresses that are not translated code:
// methods of host objects handed to the game. Returns nonzero if it took the
// call, having popped the return address as a translated function would.
extern int (*recomp_foreign_call_hook)(cpu_t *c, uint64_t target);
// Lowest address of this thread's stack for translated code.
uint8_t *recomp_stack_low(void);
#define RECOMP_STACK_SIZE (8u << 20)

// Runs a translated function on a private stack, following the Win64 calling
// convention. Arguments must already be in c's registers.
void recomp_invoke(cpu_t *c, recomp_fn fn);

#ifdef __cplusplus
}
#endif
