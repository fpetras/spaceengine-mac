// Shader compilation on Apple's GLSL 4.10 compiler.
//
// The game's shaders are written for drivers that accept a little more than
// GLSL 4.10 strictly allows. When a shader fails to compile, the compiler's
// own error list drives a repair of exactly the lines it names, and the
// shader is compiled again. A shader that compiles is never modified.
//
//   error from the compiler                    repair of that line
//   ---------------------------------------    ------------------------------------------
//   bad #if / #elif directive                  a name that no #define above mentions
//                                              becomes 0, which is what C preprocessors
//                                              and other vendors' compilers assume
//   interpolation qualifier must precede       swap it with the `in` / `out` before it
//   storage qualifiers
//   initializer not allowed (on a const)       at file scope `const T x = e;` becomes
//                                              `#define x (T(e))`; inside a function the
//                                              `const` is dropped
//   syntax error at a lone `};` or `;`         drop the semicolon (allowed from GLSL 4.20)
//   syntax error on a line declaring a         rename that variable throughout its scope;
//   variable named like a const turned         the macro from the repair above would
//   into a #define                             otherwise replace its name
//   syntax error at `.` after a variable       rename the variable throughout its function;
//   named like a struct type                   Apple's parser reads the name as the type
//   call of undeclared texture2D etc.          the GLSL 1.30 name: texture, textureLod...
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "host.h"

static int is_ident(int ch) { return isalnum(ch) || ch == '_'; }
static const char *skip_space(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

typedef struct { char **line; int count; } text_t;

static text_t split(const char *source) {
    text_t t = {NULL, 0};
    int cap = 0;
    for (const char *p = source; ; ) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (t.count == cap) t.line = realloc(t.line, sizeof *t.line * (size_t)(cap = cap ? cap * 2 : 256));
        t.line[t.count] = malloc(n + 1);
        memcpy(t.line[t.count], p, n);
        t.line[t.count++][n] = 0;
        if (!end) break;
        p = end + 1;
    }
    return t;
}
static char *join(const text_t *t) {
    size_t bytes = 1;
    for (int i = 0; i < t->count; i++) bytes += strlen(t->line[i]) + 1;
    char *out = malloc(bytes), *p = out;
    for (int i = 0; i < t->count; i++) { size_t n = strlen(t->line[i]); memcpy(p, t->line[i], n); p += n; *p++ = '\n'; }
    *p = 0;
    return out;
}
static void set_line(text_t *t, int i, char *replacement) { free(t->line[i]); t->line[i] = replacement; }

// Is `name` the subject of a #define on any line before `limit`?
static int defined_above(const text_t *t, int limit, const char *name, size_t len) {
    for (int i = 0; i < limit; i++) {
        const char *p = skip_space(t->line[i]);
        if (*p != '#') continue;
        p = skip_space(p + 1);
        if (strncmp(p, "define", 6) != 0 || is_ident(p[6])) continue;
        p = skip_space(p + 6);
        if (strncmp(p, name, len) == 0 && !is_ident(p[len])) return 1;
    }
    return 0;
}

static int fix_directive(text_t *t, int n) {
    const char *src = t->line[n], *p = skip_space(src);
    if (*p != '#') return 0;
    p = skip_space(p + 1);
    if (strncmp(p, "if", 2) == 0 && !is_ident(p[2])) p += 2;
    else if (strncmp(p, "elif", 4) == 0 && !is_ident(p[4])) p += 4;
    else return 0;
    char *out = malloc(strlen(src) + 8), *o = out;
    memcpy(o, src, (size_t)(p - src)); o += p - src;
    int changed = 0, after_defined = 0;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') break;                    // a comment ends the expression
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (is_ident((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            if (len == 7 && !strncmp(start, "defined", 7)) { after_defined = 1; memcpy(o, start, len); o += len; continue; }
            if (!after_defined && !defined_above(t, n, start, len)) { *o++ = '0'; changed = 1; }
            else { memcpy(o, start, len); o += len; }
            after_defined = 0;
            continue;
        }
        if (isdigit((unsigned char)*p)) {                          // a number, including any suffix letters
            while (is_ident((unsigned char)*p) || *p == '.') *o++ = *p++;
            continue;
        }
        if (*p != ' ' && *p != '\t' && *p != '(') after_defined = 0;
        *o++ = *p++;
    }
    *o = 0;
    if (!changed) { free(out); return 0; }
    set_line(t, n, out);
    return 1;
}

static int fix_qualifier_order(text_t *t, int n) {
    static const char *const interpolation[] = {"noperspective", "flat", "smooth"};
    char *line = t->line[n];
    for (char *p = line; *p; p++) {
        if (p != line && is_ident((unsigned char)p[-1])) continue;
        size_t storage = !strncmp(p, "out", 3) && !is_ident((unsigned char)p[3]) ? 3 :
                         !strncmp(p, "in", 2) && !is_ident((unsigned char)p[2]) ? 2 : 0;
        if (!storage) continue;
        char *q = (char *)skip_space(p + storage);
        for (int k = 0; k < 3; k++) {
            size_t len = strlen(interpolation[k]);
            if (q == p + storage || strncmp(q, interpolation[k], len) != 0 || is_ident((unsigned char)q[len])) continue;
            char *out = malloc(strlen(line) + 2);
            int head = (int)(p - line);
            snprintf(out, strlen(line) + 2, "%.*s%s %.*s%s", head, line, interpolation[k], (int)storage, p, q + len);
            set_line(t, n, out);
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------ scopes
// Walks the text from a position, keeping the brace depth relative to where
// it started. Of the branches of a conditional only the first is counted,
// so braces opened in both arms of an #if / #else are not counted twice.
typedef struct { const text_t *t; int line, col, depth, skipping, nested, in_comment; } walk_t;

static walk_t walk_from(const text_t *t, int line, int col) { return (walk_t){t, line, col, 0, 0, 0, 0}; }

// Advances to the next brace and returns it ('{' or '}') with line/col on it
// and depth updated, or 0 at the end of the text.
static int walk_brace(walk_t *w) {
    for (; w->line < w->t->count; w->line++, w->col = 0) {
        const char *s = w->t->line[w->line];
        const char *d = skip_space(s);
        if (*d == '#' && !w->in_comment) {
            d = skip_space(d + 1);
            int opens = !strncmp(d, "if", 2), closes = !strncmp(d, "endif", 5);
            int other = !strncmp(d, "else", 4) || !strncmp(d, "elif", 4);
            if (w->skipping) {
                if (opens) w->nested++;
                else if (closes) { if (w->nested) w->nested--; else w->skipping = 0; }
            } else if (other) { w->skipping = 1; w->nested = 0; }
            continue;
        }
        if (w->skipping) continue;
        for (int i = w->col; s[i]; i++) {
            if (w->in_comment) { if (s[i] == '*' && s[i + 1] == '/') { w->in_comment = 0; i++; } continue; }
            if (s[i] == '/' && s[i + 1] == '/') break;
            if (s[i] == '/' && s[i + 1] == '*') { w->in_comment = 1; i++; continue; }
            if (s[i] == '{' || s[i] == '}') {
                w->depth += s[i] == '{' ? 1 : -1;
                w->col = i;
                return s[i];
            }
        }
    }
    return 0;
}

// Renames the variable `name` between two positions. Left alone: a field
// after a dot, and the name used as a type or a call (`name x`, `name(`).
static int rename_variable(text_t *t, int l0, int c0, int l1, int c1, const char *name, size_t len) {
    int changed = 0;
    for (int n = l0; n <= l1 && n < t->count; n++) {
        const char *s = t->line[n];
        if (*skip_space(s) == '#') continue;
        size_t from = n == l0 ? (size_t)c0 : 0, to = n == l1 ? (size_t)c1 : strlen(s);
        char *out = malloc(strlen(s) * 2 + 8), *o = out;
        for (size_t i = 0; s[i]; ) {
            if (!is_ident((unsigned char)s[i]) || (i && is_ident((unsigned char)s[i - 1]))) { *o++ = s[i++]; continue; }
            size_t k = i;
            while (is_ident((unsigned char)s[k])) k++;
            const char *next = skip_space(s + k);
            int is_variable = k - i == len && !strncmp(s + i, name, len) && i >= from && i < to &&
                              !(i && s[i - 1] == '.') && !is_ident((unsigned char)*next) && *next != '(';
            memcpy(o, s + i, k - i); o += k - i;
            if (is_variable) { memcpy(o, "_se", 3); o += 3; changed++; }
            i = k;
        }
        *o = 0;
        set_line(t, n, out);
    }
    return changed;
}

#define CONST_MARK "//se-const"
// Is `name` a macro made by fix_const_initializer on a line before `limit`?
static int const_macro_above(const text_t *t, int limit, const char *name, size_t len) {
    for (int i = 0; i < limit; i++) {
        const char *p = t->line[i];
        if (strncmp(p, "#define ", 8) == 0 && strncmp(p + 8, name, len) == 0 && p[8 + len] == ' ' && strstr(p, CONST_MARK)) return 1;
    }
    return 0;
}
static int struct_above(const text_t *t, int limit, const char *name, size_t len) {
    for (int i = 0; i < limit; i++) {
        const char *p = skip_space(t->line[i]);
        if (strncmp(p, "struct", 6) != 0 || is_ident((unsigned char)p[6])) continue;
        p = skip_space(p + 6);
        if (strncmp(p, name, len) == 0 && !is_ident((unsigned char)p[len])) return 1;
    }
    return 0;
}

// A variable declared on line n (`type name`) whose name a const repair made a macro.
static int fix_shadowed_macro(text_t *t, int n) {
    const char *s = t->line[n];
    for (size_t i = 0; s[i]; ) {
        if (!is_ident((unsigned char)s[i]) || isdigit((unsigned char)s[i])) { i++; continue; }
        size_t k = i;
        while (is_ident((unsigned char)s[k])) k++;
        size_t back = i;
        while (back && (s[back - 1] == ' ' || s[back - 1] == '\t')) back--;
        int declared = back < i && back && is_ident((unsigned char)s[back - 1]);
        if (!declared || !const_macro_above(t, n, s + i, k - i)) { i = k; continue; }
        // A parameter's scope is the function body that follows; a local's ends with its block.
        int parens = 0;
        for (size_t j = 0; j < i; j++) parens += s[j] == '(' ? 1 : s[j] == ')' ? -1 : 0;
        walk_t w = walk_from(t, n, (int)k);
        int brace, opened = 0;
        while ((brace = walk_brace(&w))) {
            opened |= w.depth > 0;
            if (parens > 0 ? opened && w.depth == 0 : w.depth < 0) break;
            w.col++;
        }
        if (!brace) return 0;
        char *name = strndup(s + i, k - i);
        int changed = rename_variable(t, n, (int)i, w.line, w.col, name, k - i);
        free(name);
        return changed > 0;
    }
    return 0;
}

// A variable used on line n (`name.field`) whose name is also a struct type.
static int fix_variable_named_like_type(text_t *t, int n) {
    const char *s = t->line[n];
    for (size_t i = 0; s[i]; ) {
        if (!is_ident((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || (i && (is_ident((unsigned char)s[i - 1]) || s[i - 1] == '.'))) { i++; continue; }
        size_t k = i;
        while (is_ident((unsigned char)s[k])) k++;
        if (*skip_space(s + k) != '.' || !struct_above(t, n, s + i, k - i)) { i = k; continue; }
        // The enclosing function: the last block opened at file scope before this line.
        walk_t w = walk_from(t, 0, 0), open = w;
        int brace, found = 0, header = 0, last_close = 0;
        while ((brace = walk_brace(&w)) && w.line <= n) {
            if (brace == '{' && w.depth == 1) { open = w; found = 1; header = last_close; }
            if (brace == '}' && w.depth == 0) last_close = w.line + 1;
            w.col++;
        }
        if (!found) return 0;
        w = open; w.col++;
        while ((brace = walk_brace(&w)) && w.depth > 0) w.col++;
        if (!brace || w.line < n) return 0;
        char *name = strndup(s + i, k - i);
        int changed = rename_variable(t, header, 0, w.line, w.col, name, k - i);
        free(name);
        return changed > 0;
    }
    return 0;
}

static int fix_legacy_texture_call(text_t *t, int n) {
    static const char *const names[][2] = {
        {"texture2DLod", "textureLod"}, {"texture2DProj", "textureProj"}, {"texture2D", "texture"},
        {"texture3DLod", "textureLod"}, {"texture3D", "texture"}, {"textureCubeLod", "textureLod"},
        {"textureCube", "texture"}, {"texture1D", "texture"},
    };
    const char *s = t->line[n];
    char *out = malloc(strlen(s) + 1), *o = out;
    int changed = 0;
    for (size_t i = 0; s[i]; ) {
        size_t k = i;
        while (is_ident((unsigned char)s[k])) k++;
        if (k == i || (i && is_ident((unsigned char)s[i - 1]))) { *o++ = s[i++]; continue; }
        const char *put = s + i;
        size_t len = k - i;
        for (size_t a = 0; a < sizeof names / sizeof names[0]; a++)
            if (len == strlen(names[a][0]) && !strncmp(s + i, names[a][0], len)) { put = names[a][1]; len = strlen(put); changed = 1; break; }
        memcpy(o, put, len); o += len;
        i = k;
    }
    *o = 0;
    if (changed) set_line(t, n, out); else free(out);
    return changed;
}

// Inside a function a const with a run-time initializer (allowed from GLSL
// 4.20) is an ordinary variable that happens never to be assigned again.
static int drop_const(text_t *t, int n) {
    const char *s = t->line[n];
    char *out = malloc(strlen(s) + 1), *o = out;
    int changed = 0;
    for (size_t i = 0; s[i]; ) {
        if (!strncmp(s + i, "const", 5) && !is_ident((unsigned char)s[i + 5]) && !(i && is_ident((unsigned char)s[i - 1]))) {
            i += 5; changed = 1;
            if (s[i] == ' ' || s[i] == '\t') i++;
            continue;
        }
        *o++ = s[i++];
    }
    *o = 0;
    if (changed) set_line(t, n, out); else free(out);
    return changed;
}

static int fix_const_initializer(text_t *t, int n) {
    const char *p = skip_space(t->line[n]);
    walk_t w = walk_from(t, 0, 0);
    int depth = 0;
    while (walk_brace(&w) && w.line < n) { depth = w.depth; w.col++; }
    if (depth > 0 || strncmp(p, "const", 5) != 0 || is_ident((unsigned char)p[5])) return drop_const(t, n);
    const char *type = skip_space(p + 5), *q = type;
    while (is_ident((unsigned char)*q)) q++;
    const char *name = skip_space(q), *r = name;
    while (is_ident((unsigned char)*r)) r++;
    const char *eq = skip_space(r);
    const char *semi = strrchr(eq, ';');
    if (q == type || r == name || *eq != '=' || !semi) return 0;
    char *out = malloc(strlen(t->line[n]) * 2 + 32);
    sprintf(out, "#define %.*s (%.*s(%.*s)) " CONST_MARK, (int)(r - name), name, (int)(q - type), type, (int)(semi - eq - 1), eq + 1);
    set_line(t, n, out);
    return 1;
}

static int fix_stray_semicolon(text_t *t, int n) {
    const char *p = skip_space(t->line[n]);
    const char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (end - p == 2 && p[0] == '}' && p[1] == ';') { set_line(t, n, strdup("}")); return 1; }
    if (end - p == 1 && p[0] == ';') { set_line(t, n, strdup("")); return 1; }
    return 0;
}

static int repairs_total, shaders_repaired, shaders_failed, programs_relinked;

void gl_shader_report(void) {
    fprintf(stderr, "host: %d shaders needed repair (%d lines changed); %d still do not compile; %d programs relinked\n",
            shaders_repaired, repairs_total, shaders_failed, programs_relinked);
}

// Experiment switch: SE_SHADER_GUARDS=<list> puts guards around built-in
// functions whose results are undefined outside their domain, in every shader,
// right after the #version line. Each guard keeps the argument in range.
static void add_guards(GLuint shader) {
    const char *which = getenv("SE_SHADER_GUARDS");
    if (!which) return;
    static const struct { const char *name, *define; } guards[] = {
        {"acos", "#define acos(x) acos(clamp((x), -1.0, 1.0))"},
        {"asin", "#define asin(x) asin(clamp((x), -1.0, 1.0))"},
        {"sqrt", "#define sqrt(x) sqrt(max((x), 0.0))"},
        {"log", "#define log(x) log(max((x), 1e-37))"},
        {"log2", "#define log2(x) log2(max((x), 1e-37))"},
        {"pow", "#define pow(x, y) pow(max((x), 0.0), (y))"},
        {"inversesqrt", "#define inversesqrt(x) inversesqrt(max((x), 1e-37))"},
    };
    GLint length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    char *source = calloc(1, (size_t)length + 2);
    glGetShaderSource(shader, length + 1, NULL, source);
    text_t t = split(source);
    char block[1024] = "";
    for (size_t g = 0; g < sizeof guards / sizeof guards[0]; g++) {
        const char *at = strstr(which, guards[g].name);
        size_t n = strlen(guards[g].name);
        if (!strcmp(which, "all") || (at && (at == which || at[-1] == ',') && (at[n] == 0 || at[n] == ','))) {
            strlcat(block, guards[g].define, sizeof block);
            strlcat(block, "\n", sizeof block);
        }
    }
    int version = 0;
    while (version < t.count && strncmp(skip_space(t.line[version]), "#version", 8)) version++;
    if (version < t.count && block[0]) {
        size_t len = strlen(t.line[version]) + strlen(block) + 2;
        char *line = malloc(len);
        snprintf(line, len, "%s\n%s", t.line[version], block);
        line[strlen(line) - 1] = 0;
        set_line(&t, version, line);
        char *text = join(&t);
        const GLchar *ptr = text;
        glShaderSource(shader, 1, &ptr, NULL);
        free(text);
    }
    for (int i = 0; i < t.count; i++) free(t.line[i]);
    free(t.line); free(source);
}

void gl_compile_shader(GLuint shader) {
    add_guards(shader);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    int edits = 0;
    for (int pass = 0; pass < 16 && !ok; pass++) {
        GLint length = 0, log_length = 0;
        glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        char *source = calloc(1, (size_t)length + 2), *log = calloc(1, (size_t)log_length + 2);
        glGetShaderSource(shader, length + 1, NULL, source);
        glGetShaderInfoLog(shader, log_length + 1, NULL, log);
        text_t t = split(source);
        int fixed = 0;
        for (char *e = log; e && *e; e = strchr(e, '\n') ? strchr(e, '\n') + 1 : NULL) {
            int line = 0;
            if (sscanf(e, "ERROR: 0:%d:", &line) != 1 || line < 1 || line > t.count) continue;
            size_t len = strcspn(e, "\n");
            char *message = strndup(e, len);
            if (strstr(message, "preprocessor directive")) fixed += fix_directive(&t, line - 1);
            else if (strstr(message, "must precede storage qualifiers")) fixed += fix_qualifier_order(&t, line - 1);
            else if (strstr(message, "Initializer not allowed")) fixed += fix_const_initializer(&t, line - 1);
            else if (strstr(message, "';' : syntax error")) fixed += fix_stray_semicolon(&t, line - 1);
            else if (strstr(message, "'.' : syntax error")) fixed += fix_variable_named_like_type(&t, line - 1);
            else if (strstr(message, "syntax error")) fixed += fix_shadowed_macro(&t, line - 1);
            else if (strstr(message, "undeclared identifier 'texture")) fixed += fix_legacy_texture_call(&t, line - 1);
            free(message);
        }
        if (fixed) {
            char *repaired = join(&t);
            const GLchar *text = repaired;
            glShaderSource(shader, 1, &text, NULL);
            glCompileShader(shader);
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            free(repaired);
            edits += fixed;
        }
        for (int i = 0; i < t.count; i++) free(t.line[i]);
        free(t.line); free(source); free(log);
        if (!fixed) break;
    }
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (edits) { repairs_total += edits; shaders_repaired++; }
    if (!ok) shaders_failed++;
    pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------- linking
// The game's stages name their interface variables differently on each side
// (a vertex `out vec4 outColor` feeding a geometry `in vec4 inColor[]`) and
// pair them by `layout(location = N)`. Apple's linker pairs by name only.
// When a program fails to link, every located interface variable is renamed
// after its location, the same on both sides, and the program linked again.
// Two vertex attributes declared at one location are made one in the same pass.
//
// One source file holds all stages, selected by #ifdef. Whether a line is in
// the active part is asked of the compiler: a copy with #error placed on that
// line fails with that message exactly when the line is compiled.
static int line_is_active(GLenum type, const text_t *t, int n) {
    char *saved = t->line[n];
    t->line[n] = "#error SEPROBE";
    char *probe = join(t);
    t->line[n] = saved;
    GLuint scratch = glCreateShader(type);
    const GLchar *text = probe;
    glShaderSource(scratch, 1, &text, NULL);
    glCompileShader(scratch);
    char log[4096] = "";
    glGetShaderInfoLog(scratch, sizeof log, NULL, log);
    glDeleteShader(scratch);
    free(probe);
    return strstr(log, "SEPROBE") != NULL;
}

// `layout(location = N) ... in|out ... type name[...];` -> direction, N and the name.
static int parse_interface(const char *line, int *is_out, int *location, const char **name, size_t *len) {
    const char *p = skip_space(line);
    if (strncmp(p, "layout", 6) != 0) return 0;
    p = skip_space(p + 6);
    if (*p != '(') return 0;
    p = skip_space(p + 1);
    if (strncmp(p, "location", 8) != 0) return 0;
    p = skip_space(p + 8);
    if (*p != '=') return 0;
    *location = atoi(p + 1);
    if (!(p = strchr(p, ')'))) return 0;
    const char *end = strchr(p, ';');
    if (!end) return 0;
    int direction = -1;
    const char *last = NULL;
    size_t last_len = 0;
    for (p++; p < end; ) {
        if (*p == '[') break;
        if (!is_ident((unsigned char)*p)) { p++; continue; }
        const char *q = p;
        while (is_ident((unsigned char)*q)) q++;
        if (q - p == 2 && !strncmp(p, "in", 2) && direction < 0) direction = 0;
        else if (q - p == 3 && !strncmp(p, "out", 3) && direction < 0) direction = 1;
        last = p; last_len = (size_t)(q - p);
        p = q;
    }
    if (direction < 0 || !last) return 0;
    *is_out = direction; *name = last; *len = last_len;
    return 1;
}

static int rename_everywhere(text_t *t, const char *name, size_t len, const char *to) {
    int changed = 0;
    for (int n = 0; n < t->count; n++) {
        const char *s = t->line[n];
        char *out = malloc(strlen(s) * 3 + 16), *o = out;
        int hit = 0;
        for (size_t i = 0; s[i]; ) {
            size_t k = i;
            while (is_ident((unsigned char)s[k])) k++;
            if (k == i || (i && is_ident((unsigned char)s[i - 1]))) { *o++ = s[i++]; continue; }
            if (k - i == len && !strncmp(s + i, name, len) && !(i && s[i - 1] == '.')) { o += sprintf(o, "%s", to); hit = 1; }
            else { memcpy(o, s + i, k - i); o += k - i; }
            i = k;
        }
        *o = 0;
        if (hit) { set_line(t, n, out); changed++; } else free(out);
    }
    return changed;
}


// Two active vertex attributes at one location. Drivers that allow it feed
// both from the same vertex data, so the second becomes a view of the first.
static int components(const char *type, size_t len) {
    if (len == 5 && !strncmp(type, "float", 5)) return 1;
    if (len == 4 && !strncmp(type, "vec", 3) && type[3] >= '2' && type[3] <= '4') return type[3] - '0';
    return 0;
}
static const char *type_before(const char *line, const char *name, size_t *len) {
    const char *end = name;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
    const char *start = end;
    while (start > line && is_ident((unsigned char)start[-1])) start--;
    *len = (size_t)(end - start);
    return start;
}
static int fix_aliased_attribute(GLenum type, text_t *t, const char *log) {
    const char *e = strstr(log, "Active attribute aliasing. Slot ");
    if (!e) return 0;
    int slot = atoi(e + 32);
    const char *quoted = strchr(e, '\'');
    if (!quoted) return 0;
    size_t wanted_len = strcspn(quoted + 1, "'");
    int mine = -1, other = -1;
    for (int n = 0; n < t->count; n++) {
        int is_out, location;
        const char *name;
        size_t len;
        if (!parse_interface(t->line[n], &is_out, &location, &name, &len) || is_out || location != slot) continue;
        if (!line_is_active(type, t, n)) continue;
        if (len == wanted_len && !strncmp(name, quoted + 1, len)) mine = n; else if (other < 0) other = n;
    }
    if (mine < 0 || other < 0) return 0;
    int is_out, location;
    const char *a, *b;
    size_t a_len, b_len, a_type_len, b_type_len;
    parse_interface(t->line[mine], &is_out, &location, &a, &a_len);
    parse_interface(t->line[other], &is_out, &location, &b, &b_len);
    const char *a_type = type_before(t->line[mine], a, &a_type_len), *b_type = type_before(t->line[other], b, &b_type_len);
    int want = components(a_type, a_type_len), have = components(b_type, b_type_len);
    if (!want || want > have) return 0;
    char *out = malloc(a_len + b_len + 32);
    sprintf(out, "#define %.*s (%.*s%s%.*s)", (int)a_len, a, (int)b_len, b, want < have ? "." : "", want < have ? want : 0, "xyzw");
    set_line(t, mine, out);
    return 1;
}

void gl_link_program(GLuint program) {
    glLinkProgram(program);
    GLint linked = 0, count = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked) return;
    GLuint shaders[8];
    glGetAttachedShaders(program, 8, &count, shaders);
    char log[2048] = "";
    glGetProgramInfoLog(program, sizeof log, NULL, log);
    int has_geometry = 0, renamed = 0;
    for (GLint i = 0; i < count; i++) {
        GLint type = 0;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        has_geometry |= type == GL_GEOMETRY_SHADER;
    }
    for (GLint i = 0; i < count; i++) {
        GLint type = 0, length = 0;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        if (type != GL_VERTEX_SHADER && type != GL_GEOMETRY_SHADER && type != GL_FRAGMENT_SHADER) continue;
        glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
        char *source = calloc(1, (size_t)length + 2);
        glGetShaderSource(shaders[i], length + 1, NULL, source);
        text_t t = split(source);
        int changed = type == GL_VERTEX_SHADER ? fix_aliased_attribute(GL_VERTEX_SHADER, &t, log) : 0;
        // Which stage writes the inputs of this one, and what this one's outputs are called.
        const char *in_from = type == GL_FRAGMENT_SHADER && has_geometry ? "gs" : "vs";
        const char *out_as = type == GL_GEOMETRY_SHADER ? "gs" : "vs";
        for (int n = 0; n < t.count; n++) {
            int is_out, location;
            const char *name;
            size_t len;
            if (!parse_interface(t.line[n], &is_out, &location, &name, &len)) continue;
            if (type == GL_VERTEX_SHADER && !is_out) continue;     // attributes
            if (type == GL_FRAGMENT_SHADER && is_out) continue;    // colour outputs
            if (len > 6 && !strncmp(name, "se_", 3)) continue;     // already renamed
            if (!line_is_active((GLenum)type, &t, n)) continue;
            char to[32], *from = strndup(name, len);
            snprintf(to, sizeof to, "se_%s_%d", is_out ? out_as : in_from, location);
            changed += rename_everywhere(&t, from, len, to);
            free(from);
        }
        if (changed) {
            char *text = join(&t);
            const GLchar *ptr = text;
            glShaderSource(shaders[i], 1, &ptr, NULL);
            glCompileShader(shaders[i]);
            free(text);
            renamed++;
        }
        for (int n = 0; n < t.count; n++) free(t.line[n]);
        free(t.line); free(source);
    }
    if (!renamed) return;
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked) programs_relinked++;
}
