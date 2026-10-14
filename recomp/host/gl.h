// OpenGL as the game reaches it: native functions bridged by generated
// thunks, plus the pieces macOS's OpenGL 4.1 lacks.
#pragma once
#include "host.h"

typedef struct { const char *name; void (*fn)(cpu_t *); } gl_entry_t;
extern const gl_entry_t gl_native[];   // generated, sorted by name
extern const int gl_native_count;

extern const gl_entry_t gl_compat[];   // host versions that take precedence over the native ones
extern const int gl_compat_count;

// Floating-point arguments by position, Windows x64 convention.
static inline float host_argf(cpu_t *c, int n) { return n < 4 ? c->x[n].f32[0] : ldf32(c->r[RSP] + 8 + 8 * (uint64_t)n); }
static inline double host_argd(cpu_t *c, int n) { return n < 4 ? c->x[n].f64[0] : ldf64(c->r[RSP] + 8 + 8 * (uint64_t)n); }

// The function the game gets for a name from wglGetProcAddress or an import,
// or null when there is none.
void (*gl_lookup(const char *name))(cpu_t *);

// Compiles a shader, repairing the constructs Apple's compiler rejects.
void gl_compile_shader(unsigned shader);
// Links a program, pairing stage interfaces by location where the linker pairs by name.
void gl_link_program(unsigned program);
void gl_shader_report(void);
void gl_debug_frame(void);

// Debugging aid: with SE_GL_ERRORS set, every bridged OpenGL call is followed
// by glGetError, and a failure is printed with the call's first arguments.
extern int gl_check_errors;
void gl_report_error(cpu_t *c, const char *name);

// Debugging aid: SE_GL_LOG=<first frame>[,<frames>] logs every OpenGL call the
// main thread makes in those frames, with its first arguments.
extern int gl_logging;
void gl_log_call(cpu_t *c, const char *name);
