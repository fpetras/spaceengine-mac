// Original functions that are replaced by host implementations instead of
// being translated. Each keeps the translated-code convention: arguments and
// results in cpu_t, and the return address popped on the way out.
#include "x86.h"

void f_1405ea080(cpu_t *c) { c->x[0].f64[0] = cos(c->x[0].f64[0]);   c->r[RSP] += 8; }  // CRT cos
void f_1405ed8d0(cpu_t *c) { c->x[0].f64[0] = sin(c->x[0].f64[0]);   c->r[RSP] += 8; }  // CRT sin
void f_1405eb1f0(cpu_t *c) { c->x[0].f64[0] = floor(c->x[0].f64[0]); c->r[RSP] += 8; }  // CRT floor

void f_1405f3990(cpu_t *c) { recomp_longjmp(c); }   // CRT longjmp

