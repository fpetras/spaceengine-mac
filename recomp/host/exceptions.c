// C++ exceptions thrown by the game.
//
// MSVC implements `throw` as RaiseException with a fixed code and a pointer
// to a description of the thrown object. On Windows the unwinder then looks
// for a `catch` in the functions on the stack.
//
// This game has next to none. Its exception tables (read by tools/eh.py)
// hold five try blocks in all: three are `noexcept` functions whose handler
// calls std::terminate, and two are the standard library's stream operators,
// which catch whatever a stream buffer throws and set the stream's bad bit.
// Everything else it throws is uncaught on Windows too and ends the program
// there. So a throw is reported here with the type and message of what was
// thrown, and the program ends; no unwinder is implemented. What differs
// from Windows is only an exception thrown inside one of those two stream
// operations, which the game would survive there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define CXX_EXCEPTION 0xe06d7363u

// ".?AVlength_error@std@@" -> "std::length_error"
static void readable(const char *decorated, char *out, size_t cap) {
    const char *p = strncmp(decorated, ".?A", 3) == 0 && decorated[3] ? decorated + 4 : decorated;
    char parts[8][96];
    int n = 0;
    while (*p && *p != '@' && n < 8) {
        size_t len = strcspn(p, "@");
        snprintf(parts[n++], sizeof parts[0], "%.*s", (int)len, p);
        p += len + (p[len] == '@');
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        strlcat(out, parts[i], cap);
        if (i) strlcat(out, "::", cap);
    }
    if (!n) snprintf(out, cap, "%s", decorated);
}

// The thrown object's type (most derived first in the compiler's list) and,
// for a std::exception, its message.
static void describe(cpu_t *c, uint64_t object, uint64_t throw_info, uint64_t base, char *out, size_t cap) {
    if (!throw_info) { snprintf(out, cap, "a rethrown C++ exception"); return; }
    uint64_t types = base + ld32(throw_info + 12);
    uint32_t count = ld32(types);
    int is_std_exception = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && i < 8; i++) {
        uint64_t type = base + ld32(base + ld32(types + 4 + 4 * i) + 4);
        const char *name = (const char *)(type + 16);
        if (i == 0) readable(name, out, cap);
        if (strstr(name, "exception@std")) is_std_exception = 1;
    }
    if (is_std_exception && object) {
        // std::exception::what() is the second virtual function.
        uint64_t what = host_call_guest(c, ld64(ld64(object) + 8), 1, &object);
        if (what) { strlcat(out, ": ", cap); strlcat(out, (const char *)what, cap); }
    }
}

IMPORT(RaiseException) {
    uint32_t code = (uint32_t)ARG(0);
    uint64_t args = ARG(3);
    char what[512];
    if (code == CXX_EXCEPTION && ARG(2) >= 3 && args)
        describe(c, ld64(args + 8), ld64(args + 16), ARG(2) >= 4 ? ld64(args + 24) : 0x140000000ull, what, sizeof what);
    else snprintf(what, sizeof what, "exception %#x", code);
    host_fatal(75, "The game stopped with an error it does not handle:\n\n%s", what);
}
