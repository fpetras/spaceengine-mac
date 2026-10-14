#include "runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern const struct recomp_entry { uint64_t va; recomp_fn fn; } recomp_table[];
extern const unsigned recomp_table_count;

static const char *reserved_start;
static size_t reserved_size;

void recomp_set_reserved_range(const void *start, size_t size) {
    reserved_start = start;
    reserved_size = size;
}

void recomp_unimplemented(cpu_t *c, uint64_t address, const char *what) {
    (void)c;
    fprintf(stderr, "recomp: untranslated instruction '%s' at %#llx\n", what, (unsigned long long)address);
    abort();
}

void recomp_trap(cpu_t *c, uint64_t address) {
    (void)c;
    fprintf(stderr, "recomp: trap at %#llx\n", (unsigned long long)address);
    abort();
}

recomp_fn recomp_lookup(uint64_t target);

int (*recomp_foreign_call_hook)(cpu_t *c, uint64_t target);

void recomp_call(cpu_t *c, uint64_t target) {
    recomp_fn fn = recomp_lookup(target);
    if (!fn && recomp_foreign_call_hook && recomp_foreign_call_hook(c, target)) return;
    if (!fn) {
        fprintf(stderr, "recomp: indirect call to untranslated address %#llx\n", (unsigned long long)target);
        abort();
    }
    fn(c);
}

void (*recomp_import_hook)(cpu_t *c, uint64_t slot);

void recomp_call_import(cpu_t *c, uint64_t slot) {
    if (!recomp_import_hook) {
        fprintf(stderr, "recomp: import slot %#llx called, but no host is attached\n", (unsigned long long)slot);
        abort();
    }
    recomp_import_hook(c, slot);
}

recomp_fn recomp_lookup(uint64_t target) {
    unsigned lo = 0, hi = recomp_table_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (recomp_table[mid].va < target) lo = mid + 1; else hi = mid;
    }
    return lo < recomp_table_count && recomp_table[lo].va == target ? recomp_table[lo].fn : 0;
}

void recomp_cpuid(cpu_t *c) {
    uint32_t leaf = (uint32_t)c->r[RAX];
    uint32_t a = 0, b = 0, cx = 0, d = 0;
    if (leaf == 0) {
        a = 1;                                       // highest basic leaf
        b = 0x756e6547; d = 0x49656e69; cx = 0x6c65746e;  // "GenuineIntel"
    } else if (leaf == 1) {
        a = 0x000006f0;                              // family 6
        d = 1u << 0 | 1u << 4 | 1u << 8 | 1u << 15 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26;
        // FPU, TSC, CMPXCHG8B, CMOV, MMX, FXSR, SSE, SSE2. No SSE3 and up, no AVX.
    }
    c->r[RAX] = a; c->r[RBX] = b; c->r[RCX] = cx; c->r[RDX] = d;
}

uint64_t recomp_rdtsc(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void recomp_ldmxcsr(cpu_t *c, uint32_t value) {
    (void)c;
    static uint32_t reported;
    uint32_t odd = value & ~0x3fu & ~0x1f80u;        // anything beyond "all exceptions masked"
    if (odd && odd != reported) {
        reported = odd;
        fprintf(stderr, "recomp: floating-point mode %#x requested and not applied\n", value);
    }
}

int recomp_map_image(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { perror(path); return -1; }
    uint8_t *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { perror("mmap file"); return -1; }

    uint32_t pe; memcpy(&pe, file + 0x3c, 4);
    const uint8_t *opt = file + pe + 24;
    uint16_t nsec, optsize; memcpy(&nsec, file + pe + 6, 2); memcpy(&optsize, file + pe + 20, 2);
    uint64_t base; memcpy(&base, opt + 24, 8);
    uint32_t image_size, header_size; memcpy(&image_size, opt + 56, 4); memcpy(&header_size, opt + 60, 4);

    // A program that reserved the image's range at link time (reserve.s) maps
    // over its own reservation with MAP_FIXED. Any other process, such as the
    // Python test harness, only asks for the address and fails if it is taken:
    // MAP_FIXED there would silently replace whatever lives in the range.
    void *want = (void *)base;
    int inside = reserved_start && (uint64_t)reserved_start <= base &&
                 base + image_size <= (uint64_t)reserved_start + reserved_size;
    void *got = mmap(want, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (inside ? MAP_FIXED : 0), -1, 0);
    if (got != want) {
        if (got != MAP_FAILED) munmap(got, image_size);
        fprintf(stderr, "recomp: could not map the image at %p (got %p)\n", want, got);
        return -1;
    }
    memcpy(got, file, header_size);
    const uint8_t *sec = opt + optsize;
    for (unsigned i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize, va, rsize, raw;
        memcpy(&vsize, sec + 8, 4); memcpy(&va, sec + 12, 4); memcpy(&rsize, sec + 16, 4); memcpy(&raw, sec + 20, 4);
        memcpy((uint8_t *)got + va, file + raw, rsize < vsize ? rsize : vsize);
    }
    munmap(file, (size_t)st.st_size);
    return 0;
}

enum { STACK_SIZE = 8 << 20 };
static _Thread_local uint8_t *stack;

uint8_t *recomp_stack_low(void) {
    if (!stack) stack = malloc(STACK_SIZE);
    return stack;
}

void recomp_invoke(cpu_t *c, recomp_fn fn) {
    uint64_t rsp = ((uint64_t)recomp_stack_low() + STACK_SIZE - 0x100) & ~15ull;
    rsp -= 0x20;             // shadow space the callee may write to
    rsp -= 8;                // return address slot, as a call instruction leaves it
    st64(rsp, 0);
    c->r[RSP] = rsp;
    fn(c);
}

// ------------------------------------------------------- setjmp / longjmp
enum { JMP_SLOTS = 32 };
static _Thread_local struct { uint64_t guest; jmp_buf native; } jmp_slots[JMP_SLOTS];
static _Thread_local unsigned jmp_next;

jmp_buf *recomp_jmp_slot(cpu_t *c) {
    uint64_t guest = c->r[RCX];
    for (unsigned i = 0; i < JMP_SLOTS; i++)
        if (jmp_slots[i].guest == guest) return &jmp_slots[i].native;
    unsigned i = jmp_next++ % JMP_SLOTS;
    jmp_slots[i].guest = guest;
    return &jmp_slots[i].native;
}

// The game's jmp_buf, as its _setjmp fills it: frame, RBX, RSP, RBP, RSI,
// RDI, R12-R15, RIP, MXCSR, x87 control word, then XMM6-XMM15.
void recomp_longjmp(cpu_t *c) {
    uint64_t buf = c->r[RCX], value = (uint32_t)c->r[RDX] ? (uint32_t)c->r[RDX] : 1;
    for (unsigned i = 0; i < JMP_SLOTS; i++)
        if (jmp_slots[i].guest == buf) {
            c->r[RBX] = ld64(buf + 0x08); c->r[RSP] = ld64(buf + 0x10); c->r[RBP] = ld64(buf + 0x18);
            c->r[RSI] = ld64(buf + 0x20); c->r[RDI] = ld64(buf + 0x28);
            c->r[R12] = ld64(buf + 0x30); c->r[R13] = ld64(buf + 0x38);
            c->r[R14] = ld64(buf + 0x40); c->r[R15] = ld64(buf + 0x48);
            for (int x = 6; x < 16; x++) c->x[x] = ld128(buf + 0x60 + 16 * (uint64_t)(x - 6));
            c->r[RAX] = value;
            _longjmp(jmp_slots[i].native, 1);
        }
    fprintf(stderr, "recomp: longjmp to a buffer no setjmp filled (%#llx)\n", (unsigned long long)buf);
    abort();
}
