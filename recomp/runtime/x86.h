// CPU state and primitive operations used by the translated code.
//
// Every translated function has the signature `void f_<address>(cpu_t *c)` and
// keeps the original's view of the machine: sixteen general registers, sixteen
// XMM registers, and a stack in ordinary memory addressed through RSP.
// Pointers are the original's own values, because the image is mapped at its
// preferred base.
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

typedef union {
    uint64_t q[2];
    uint32_t d[4];
    uint16_t w[8];
    uint8_t b[16];
    double f64[2];
    float f32[4];
} xmm_t;

typedef struct cpu {
    uint64_t r[16];
    xmm_t x[16];
    uint64_t gs_base;
} cpu_t;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

typedef void (*recomp_fn)(cpu_t *);

// ------------------------------------------------------------------ memory
#define RECOMP_LOAD(name, type) \
    static inline type name(uint64_t a) { type v; memcpy(&v, (const void *)a, sizeof v); return v; }
#define RECOMP_STORE(name, type) \
    static inline void name(uint64_t a, type v) { memcpy((void *)a, &v, sizeof v); }
RECOMP_LOAD(ld8, uint8_t) RECOMP_LOAD(ld16, uint16_t) RECOMP_LOAD(ld32, uint32_t) RECOMP_LOAD(ld64, uint64_t)
RECOMP_LOAD(ldf32, float) RECOMP_LOAD(ldf64, double) RECOMP_LOAD(ld128, xmm_t)
RECOMP_STORE(st8, uint8_t) RECOMP_STORE(st16, uint16_t) RECOMP_STORE(st32, uint32_t) RECOMP_STORE(st64, uint64_t)
RECOMP_STORE(st128, xmm_t)

static inline uint8_t parity8(uint64_t v) { return !__builtin_parity((uint8_t)v); }

// ------------------------------------------------- float to integer, as x86
// Out-of-range and NaN inputs give the "integer indefinite" value. A C cast
// is undefined there and ARM saturates instead.
static inline uint64_t cvtt_f64_i64(double v) {
    return (v >= -0x1p63 && v < 0x1p63) ? (uint64_t)(int64_t)v : 0x8000000000000000ull;
}
static inline uint32_t cvtt_f64_i32(double v) {
    return (v > -2147483649.0 && v < 2147483648.0) ? (uint32_t)(int32_t)v : 0x80000000u;
}
static inline uint64_t cvtt_f32_i64(float v) {
    return (v >= -0x1p63f && v < 0x1p63f) ? (uint64_t)(int64_t)v : 0x8000000000000000ull;
}
static inline uint32_t cvtt_f32_i32(float v) {
    return (v >= -0x1p31f && v < 0x1p31f) ? (uint32_t)(int32_t)v : 0x80000000u;
}
// Round to nearest even, the MXCSR default, which the game does not change.
static inline uint64_t cvt_f64_i64(double v) { return cvtt_f64_i64(nearbyint(v)); }
static inline uint32_t cvt_f64_i32(double v) { return cvtt_f64_i32(nearbyint(v)); }
static inline uint64_t cvt_f32_i64(float v) { return cvtt_f32_i64(nearbyintf(v)); }
static inline uint32_t cvt_f32_i32(float v) { return cvtt_f32_i32(nearbyintf(v)); }

// ------------------------------------------------------- NaN results, as x86
// When an SSE operation yields a NaN, x86 returns the first operand if it is
// a NaN, otherwise the second, in either case made quiet; with no NaN operand
// it returns one fixed NaN with the sign bit set. ARM picks operands by other
// rules and its fixed NaN has the sign bit clear. The difference is visible
// in memory, so the NaN case is redone the x86 way. It costs one predictable
// branch per operation.
static inline double x86_nan64(double a, double b) {
    uint64_t u = 0xfff8000000000000ull;
    if (a != a) { memcpy(&u, &a, 8); u |= 0x0008000000000000ull; }
    else if (b != b) { memcpy(&u, &b, 8); u |= 0x0008000000000000ull; }
    double r; memcpy(&r, &u, 8); return r;
}
static inline float x86_nan32(float a, float b) {
    uint32_t u = 0xffc00000u;
    if (a != a) { memcpy(&u, &a, 4); u |= 0x00400000u; }
    else if (b != b) { memcpy(&u, &b, 4); u |= 0x00400000u; }
    float r; memcpy(&r, &u, 4); return r;
}
#define X86_FP_OP(name, type, nan, expr) \
    static inline type name(type a, type b) { type r = expr; return __builtin_expect(r != r, 0) ? nan(a, b) : r; }
X86_FP_OP(x86_add64, double, x86_nan64, a + b) X86_FP_OP(x86_sub64, double, x86_nan64, a - b)
X86_FP_OP(x86_mul64, double, x86_nan64, a * b) X86_FP_OP(x86_div64, double, x86_nan64, a / b)
X86_FP_OP(x86_add32, float, x86_nan32, a + b)  X86_FP_OP(x86_sub32, float, x86_nan32, a - b)
X86_FP_OP(x86_mul32, float, x86_nan32, a * b)  X86_FP_OP(x86_div32, float, x86_nan32, a / b)
static inline double x86_sqrt64(double s) { double r = sqrt(s); return __builtin_expect(r != r, 0) ? x86_nan64(s, s) : r; }
static inline float x86_sqrt32(float s) { float r = sqrtf(s); return __builtin_expect(r != r, 0) ? x86_nan32(s, s) : r; }
// Changing precision keeps the sign and the top of the payload, and quiets.
static inline float x86_f64_to_f32(double v) {
    if (__builtin_expect(v != v, 0)) {
        uint64_t u; memcpy(&u, &v, 8);
        uint32_t w = (uint32_t)(u >> 63) << 31 | 0x7fc00000u | (uint32_t)((u >> 29) & 0x003fffffu);
        float r; memcpy(&r, &w, 4); return r;
    }
    return (float)v;
}
static inline double x86_f32_to_f64(float v) {
    if (__builtin_expect(v != v, 0)) {
        uint32_t w; memcpy(&w, &v, 4);
        uint64_t u = (uint64_t)(w >> 31) << 63 | 0x7ff8000000000000ull | (uint64_t)(w & 0x003fffffu) << 29;
        double r; memcpy(&r, &u, 8); return r;
    }
    return (double)v;
}

// ------------------------------------------------------------------ runtime
void recomp_unimplemented(cpu_t *c, uint64_t address, const char *what) __attribute__((noreturn));
void recomp_trap(cpu_t *c, uint64_t address) __attribute__((noreturn));
// Calls the translated function for an original code address.
void recomp_call(cpu_t *c, uint64_t target);
// Calls whatever an import-table slot is bound to.
void recomp_call_import(cpu_t *c, uint64_t slot);
// setjmp and longjmp. The game's jmp_buf holds its registers; the native
// stack position that goes with it is kept here, found by the jmp_buf's
// address (in RCX at the call to setjmp).
jmp_buf *recomp_jmp_slot(cpu_t *c);
void recomp_longjmp(cpu_t *c) __attribute__((noreturn));
// Processor identification as the translated code sees it: a plain SSE2
// machine, so run-time-selected AVX and SSE4 code paths are never taken.
void recomp_cpuid(cpu_t *c);
uint64_t recomp_rdtsc(void);
// The translated code runs with the host's default floating-point mode. A
// request for anything else (flush-to-zero, another rounding mode) is logged.
void recomp_ldmxcsr(cpu_t *c, uint32_t value);
