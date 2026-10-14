// Binds the game's import table to host functions and dispatches calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "import_slots.h"   // generated from analysis/imports.tsv

enum { MAX_REGISTERED = 2048 };
static struct { const char *name; void (*fn)(cpu_t *); } registered[MAX_REGISTERED];
static int registered_count;
static void (*bound[sizeof import_slots / sizeof import_slots[0]])(cpu_t *);
static int trace;

void host_register(const char *name, void (*fn)(cpu_t *)) {
    if (registered_count == MAX_REGISTERED) { fprintf(stderr, "host: too many imports\n"); abort(); }
    registered[registered_count].name = name;
    registered[registered_count++].fn = fn;
}

static int slot_index(uint64_t slot) {
    int lo = 0, hi = (int)(sizeof import_slots / sizeof import_slots[0]);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (import_slots[mid].slot < slot) lo = mid + 1; else hi = mid;
    }
    return lo < (int)(sizeof import_slots / sizeof import_slots[0]) && import_slots[lo].slot == slot ? lo : -1;
}

_Thread_local cpu_t *host_cpu;

static void dispatch(cpu_t *c, uint64_t slot) {
    host_cpu = c;
    int i = slot_index(slot);
    uint64_t return_address = ld64(c->r[RSP]);
    if (i < 0 || !bound[i]) {
        fprintf(stderr, "host: %s!%s is not implemented yet (called from %#llx)\n",
                i < 0 ? "?" : import_slots[i].dll, i < 0 ? "?" : import_slots[i].name,
                (unsigned long long)return_address - 6);
        exit(70);
    }
    if (trace)
        fprintf(stderr, "  %s(%#llx, %#llx, %#llx, %#llx)", import_slots[i].name,
                (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX],
                (unsigned long long)c->r[R8], (unsigned long long)c->r[R9]);
    bound[i](c);
    if (trace) fprintf(stderr, " = %#llx\n", (unsigned long long)c->r[RAX]);
    c->r[RSP] += 8;  // the return address a call instruction pushed
}

// ------------------------------------------- host functions as code pointers
// 0x7e58 in the top 16 bits can never be a real code address.
#define EXPORT_MARK 0x7e58000000000000ull
enum { MAX_EXPORTED = 2048 };
static void (*exported[MAX_EXPORTED])(cpu_t *);
static int exported_count;

uint64_t host_export(void (*fn)(cpu_t *)) {
    for (int i = 0; i < exported_count; i++) if (exported[i] == fn) return EXPORT_MARK | (uint64_t)i;
    if (exported_count == MAX_EXPORTED) { fprintf(stderr, "host: too many exported functions\n"); abort(); }
    exported[exported_count] = fn;
    return EXPORT_MARK | (uint64_t)exported_count++;
}

static int foreign_call(cpu_t *c, uint64_t target) {
    host_cpu = c;
    if ((target & 0xffff000000000000ull) == EXPORT_MARK && (target & 0xffff) < (uint64_t)exported_count) {
        exported[target & 0xffff](c);
        c->r[RSP] += 8;
        return 1;
    }
    return steam_foreign_call(c, target);
}

void host_init(void) {
    recomp_foreign_call_hook = foreign_call;
    int n = (int)(sizeof import_slots / sizeof import_slots[0]), have = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < registered_count; k++)
            if (strcmp(import_slots[i].name, registered[k].name) == 0) { bound[i] = registered[k].fn; have++; break; }
    trace = getenv("SE_TRACE") != NULL;
    if (getenv("SE_VERBOSE")) fprintf(stderr, "host: %d of %d imports implemented\n", have, n);
    recomp_import_hook = dispatch;
}

uint64_t host_call_guest(cpu_t *c, uint64_t target, int argc, const uint64_t *argv) {
    static const int reg[4] = {RCX, RDX, R8, R9};
    uint64_t saved_rsp = c->r[RSP];
    for (int i = 0; i < argc && i < 4; i++) c->r[reg[i]] = argv[i];
    // Below the caller's frame: its red-zone-free stack, shadow space for the
    // callee, then the return address slot a call instruction would leave.
    uint64_t rsp = ((saved_rsp - 0x200) & ~15ull) - 0x20 - 8;
    st64(rsp, 0);
    c->r[RSP] = rsp;
    recomp_call(c, target);
    c->r[RSP] = saved_rsp;
    return c->r[RAX];
}
