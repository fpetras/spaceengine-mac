// What the game sees of OpenGL beyond the plain native functions.
//
// macOS offers an OpenGL 4.1 core context. In a core context it does not list
// the ARB extensions whose features became part of core OpenGL, while Windows
// drivers do, and the game's loader (GLEW) only fetches a feature's functions
// when its extension is listed. So the extension list reported here is the
// native one plus every extension that is core in 4.1 and that the native
// library therefore fully provides under the standard names.
//
// The version string is reported as it is. The game's own configuration
// switch, IgnoreCriticalCheck, is what lets it start on 4.1.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "host.h"

static const char *const core_in_41[] = {
    "GL_ARB_buffer_storage",    // not core in 4.1: emulated below
    "GL_ARB_framebuffer_object", "GL_ARB_vertex_array_object", "GL_ARB_map_buffer_range",
    "GL_ARB_uniform_buffer_object", "GL_ARB_draw_instanced", "GL_ARB_instanced_arrays", "GL_ARB_sync",
    "GL_ARB_texture_multisample", "GL_ARB_draw_elements_base_vertex", "GL_ARB_provoking_vertex",
    "GL_ARB_copy_buffer", "GL_ARB_sampler_objects", "GL_ARB_timer_query", "GL_ARB_occlusion_query2",
    "GL_ARB_vertex_type_2_10_10_10_rev", "GL_ARB_separate_shader_objects",
    "GL_ARB_viewport_array", "GL_ARB_vertex_attrib_64bit", "GL_ARB_transform_feedback2",
    "GL_ARB_transform_feedback3", "GL_ARB_tessellation_shader", "GL_ARB_shader_subroutine",
    "GL_ARB_texture_float", "GL_ARB_half_float_vertex", "GL_ARB_half_float_pixel", "GL_ARB_color_buffer_float",
    "GL_ARB_depth_buffer_float", "GL_ARB_framebuffer_sRGB", "GL_ARB_texture_rg", "GL_ARB_texture_compression_rgtc",
    "GL_ARB_texture_rgb10_a2ui", "GL_ARB_texture_swizzle", "GL_ARB_shader_bit_encoding",
    "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather", "GL_ARB_texture_query_lod", "GL_ARB_sample_shading",
    "GL_ARB_texture_buffer_object", "GL_ARB_texture_buffer_object_rgb32", "GL_ARB_seamless_cube_map",
    "GL_ARB_fragment_coord_conventions", "GL_ARB_vertex_array_bgra", "GL_ARB_depth_clamp",
    "GL_ARB_shader_precision", "GL_ARB_texture_rectangle", "GL_ARB_pixel_buffer_object",
    "GL_ARB_texture_non_power_of_two", "GL_ARB_point_sprite", "GL_ARB_draw_buffers", "GL_ARB_occlusion_query",
    "GL_ARB_multisample", "GL_ARB_texture_compression", "GL_ARB_texture_cube_map", "GL_ARB_depth_texture",
    "GL_ARB_shadow", "GL_ARB_texture_border_clamp", "GL_ARB_texture_mirrored_repeat", "GL_ARB_multitexture",
    "GL_ARB_shading_language_100", "GL_EXT_texture_array", "GL_EXT_texture_integer", "GL_EXT_packed_depth_stencil",
    "GL_EXT_texture_sRGB", "GL_EXT_framebuffer_sRGB", "GL_EXT_texture_compression_s3tc",
    // The game decides "3D textures" and "Multisampling" by these names. Without
    // the first it has no atmosphere models. GLEW also wants the EXT-named entry
    // points, which wgl.c maps to the core functions.
    "GL_EXT_texture3D", "GL_EXT_framebuffer_multisample", "GL_EXT_framebuffer_blit",
    "GL_KHR_robustness",        // its five functions are provided below
};

enum { MAX_EXTENSIONS = 512 };
static const char *extensions[MAX_EXTENSIONS];
static int extension_count;
static char *extension_string;

static void build_list(void) {
    if (extension_count) return;
    GLint native = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &native);
    // Program binaries are nominally part of 4.1, but Apple's driver offers no
    // binary format: a program can be neither saved nor restored. The game
    // caches compiled programs when this extension is listed and then draws
    // with programs that failed to load, so it is listed only if it works.
    GLint binary_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binary_formats);
    for (GLint i = 0; i < native && extension_count < MAX_EXTENSIONS; i++) {
        const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (binary_formats == 0 && !strcmp(name, "GL_ARB_get_program_binary")) continue;
        extensions[extension_count++] = name;
    }
    if (binary_formats > 0) extensions[extension_count++] = "GL_ARB_get_program_binary";
    for (size_t k = 0; k < sizeof core_in_41 / sizeof core_in_41[0] && extension_count < MAX_EXTENSIONS; k++) {
        int have = 0;
        for (int i = 0; i < extension_count; i++) have |= !strcmp(extensions[i], core_in_41[k]);
        if (!have) extensions[extension_count++] = core_in_41[k];
    }
    // Debugging aid: SE_HIDE_EXTENSIONS="name,name" leaves extensions out, to
    // see what the game does without them.
    const char *hide = getenv("SE_HIDE_EXTENSIONS");
    for (int i = 0; hide && i < extension_count; i++) {
        const char *at = strstr(hide, extensions[i]);
        size_t n = strlen(extensions[i]);
        if (at && (at == hide || at[-1] == ',') && (at[n] == 0 || at[n] == ',')) extensions[i--] = extensions[--extension_count];
    }
    size_t bytes = 1;
    for (int i = 0; i < extension_count; i++) bytes += strlen(extensions[i]) + 1;
    extension_string = calloc(1, bytes);
    for (int i = 0; i < extension_count; i++) { strcat(extension_string, extensions[i]); strcat(extension_string, " "); }
    if (getenv("SE_VERBOSE")) fprintf(stderr, "host: %d OpenGL extensions reported (%d native)\n", extension_count, native);
}

static void compat_glGetString(cpu_t *c) {
    GLenum name = (GLenum)host_arg(c, 0);
    if (name == GL_EXTENSIONS) { build_list(); c->r[RAX] = (uint64_t)extension_string; return; }
    c->r[RAX] = (uint64_t)glGetString(name);
}
static void compat_glGetStringi(cpu_t *c) {
    GLenum name = (GLenum)host_arg(c, 0);
    GLuint index = (GLuint)host_arg(c, 1);
    if (name == GL_EXTENSIONS) { build_list(); c->r[RAX] = index < (GLuint)extension_count ? (uint64_t)extensions[index] : 0; return; }
    c->r[RAX] = (uint64_t)glGetStringi(name, index);
}
static void compat_glGetIntegerv(cpu_t *c) {
    GLenum name = (GLenum)host_arg(c, 0);
    if (name == GL_NUM_EXTENSIONS) { build_list(); st32(host_arg(c, 1), (uint32_t)extension_count); return; }
    glGetIntegerv(name, (GLint *)host_arg(c, 1));
}

// ------------------------------------------- GL_ARB_buffer_storage, emulated
// OpenGL 4.4 lets a buffer stay mapped while it is drawn from ("persistent
// mapping"). The game's uniform-buffer manager is built on that: it maps each
// buffer once, and for every draw binds a range and writes that draw's
// uniforms into the mapped memory. OpenGL 4.1 has no such mapping.
//
// Here a persistently mapped buffer is ordinary memory (the "shadow"). The
// game writes there. Immediately before each draw, every uniform range bound
// from such a buffer is compared with what was last uploaded and, if it
// changed, uploaded with glBufferSubData.
#define STORAGE_MAP_PERSISTENT 0x0040
#define STORAGE_DYNAMIC 0x0100
enum { MAX_SHADOWS = 256, MAX_UBO_BINDINGS = 96 };
typedef struct { GLuint id; GLsizeiptr size; uint8_t *shadow, *uploaded; } shadow_t;
static shadow_t shadows[MAX_SHADOWS];
static pthread_mutex_t shadow_lock = PTHREAD_MUTEX_INITIALIZER;
// What this thread's context has bound: generic uniform-buffer binding, and
// the indexed ones that come from a shadowed buffer.
static _Thread_local GLuint uniform_binding;
static _Thread_local struct { shadow_t *buffer; GLintptr offset; GLsizeiptr size; } indexed[MAX_UBO_BINDINGS];
static _Thread_local int indexed_high;

static GLenum binding_query(GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
        case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
        case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
        case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
        case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
        case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER;
        case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER;
        default: return target;
    }
}
static GLuint bound_buffer(GLenum target) {
    GLint id = 0;
    glGetIntegerv(binding_query(target), &id);
    return (GLuint)id;
}
static shadow_t *shadow_of(GLuint id) {
    if (!id) return NULL;
    for (int i = 0; i < MAX_SHADOWS; i++) if (shadows[i].id == id) return &shadows[i];
    return NULL;
}

static void compat_glBufferStorage(cpu_t *c) {   // (target, size, data, flags)
    if (gl_logging) gl_log_call(c, "glBufferStorage");
    GLenum target = (GLenum)host_arg(c, 0);
    GLsizeiptr size = (GLsizeiptr)host_arg(c, 1);
    const void *data = (const void *)host_arg(c, 2);
    GLbitfield flags = (GLbitfield)host_arg(c, 3);
    glBufferData(target, size, data, (flags & (STORAGE_MAP_PERSISTENT | STORAGE_DYNAMIC)) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    if (!(flags & STORAGE_MAP_PERSISTENT)) return;
    GLuint id = bound_buffer(target);
    pthread_mutex_lock(&shadow_lock);
    shadow_t *s = shadow_of(id);
    for (int i = 0; i < MAX_SHADOWS && !s; i++) if (!shadows[i].id) s = &shadows[i];
    if (!s) { fprintf(stderr, "host: too many persistently mapped buffers\n"); exit(74); }
    free(s->shadow); free(s->uploaded);
    s->id = id; s->size = size;
    s->shadow = calloc(1, (size_t)size + 16);
    s->uploaded = calloc(1, (size_t)size + 16);
    if (data) { memcpy(s->shadow, data, (size_t)size); memcpy(s->uploaded, data, (size_t)size); }
    pthread_mutex_unlock(&shadow_lock);
}
// The loader the game uses only counts GL_ARB_buffer_storage as present if
// this variant, which names the buffer instead of using the bound one, loads
// too. It goes through the copy-write binding, which is restored.
static void compat_glNamedBufferStorageEXT(cpu_t *c) {  // (buffer, size, data, flags)
    GLuint previous = bound_buffer(GL_COPY_WRITE_BUFFER);
    glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)host_arg(c, 0));
    c->r[RCX] = GL_COPY_WRITE_BUFFER;
    compat_glBufferStorage(c);
    glBindBuffer(GL_COPY_WRITE_BUFFER, previous);
}
static void compat_glMapBufferRange(cpu_t *c) {  // (target, offset, length, access)
    if (gl_logging) gl_log_call(c, "glMapBufferRange");
    GLenum target = (GLenum)host_arg(c, 0);
    GLintptr offset = (GLintptr)host_arg(c, 1);
    GLbitfield access = (GLbitfield)host_arg(c, 3);
    shadow_t *s = shadow_of(bound_buffer(target));
    if (s && (access & STORAGE_MAP_PERSISTENT)) { c->r[RAX] = (uint64_t)(s->shadow + offset); return; }
    c->r[RAX] = (uint64_t)glMapBufferRange(target, offset, (GLsizeiptr)host_arg(c, 2), access & ~(GLbitfield)0xc0);
}
static void compat_glUnmapBuffer(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glUnmapBuffer");
    GLenum target = (GLenum)host_arg(c, 0);
    GLint mapped = 0;
    glGetBufferParameteriv(target, GL_BUFFER_MAPPED, &mapped);
    c->r[RAX] = mapped ? (uint8_t)glUnmapBuffer(target) : 1;   // a shadowed buffer was never really mapped
}
static void compat_glDeleteBuffers(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glDeleteBuffers");
    GLsizei n = (GLsizei)host_arg(c, 0);
    const GLuint *ids = (const GLuint *)host_arg(c, 1);
    pthread_mutex_lock(&shadow_lock);
    for (GLsizei i = 0; i < n; i++) {
        shadow_t *s = shadow_of(ids[i]);
        if (!s) continue;
        for (int k = 0; k < MAX_UBO_BINDINGS; k++) if (indexed[k].buffer == s) indexed[k].buffer = NULL;
        free(s->shadow); free(s->uploaded);
        memset(s, 0, sizeof *s);
    }
    pthread_mutex_unlock(&shadow_lock);
    glDeleteBuffers(n, ids);
}
static void note_indexed(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if (target != GL_UNIFORM_BUFFER) return;
    uniform_binding = buffer;
    if (index >= MAX_UBO_BINDINGS) return;
    shadow_t *s = shadow_of(buffer);
    indexed[index].buffer = s;
    indexed[index].offset = offset;
    indexed[index].size = s && size < 0 ? s->size : size;
    if (s && (int)index >= indexed_high) indexed_high = (int)index + 1;
}
static void compat_glBindBufferRange(cpu_t *c) { // (target, index, buffer, offset, size)
    if (gl_logging) gl_log_call(c, "glBindBufferRange");
    GLenum target = (GLenum)host_arg(c, 0);
    GLuint index = (GLuint)host_arg(c, 1), buffer = (GLuint)host_arg(c, 2);
    GLintptr offset = (GLintptr)host_arg(c, 3);
    GLsizeiptr size = (GLsizeiptr)host_arg(c, 4);
    glBindBufferRange(target, index, buffer, offset, size);
    note_indexed(target, index, buffer, offset, size);
}
static void compat_glBindBufferBase(cpu_t *c) {  // (target, index, buffer)
    if (gl_logging) gl_log_call(c, "glBindBufferBase");
    GLenum target = (GLenum)host_arg(c, 0);
    GLuint index = (GLuint)host_arg(c, 1), buffer = (GLuint)host_arg(c, 2);
    glBindBufferBase(target, index, buffer);
    note_indexed(target, index, buffer, 0, -1);
}
static void compat_glBindBuffer(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glBindBuffer");
    GLenum target = (GLenum)host_arg(c, 0);
    GLuint buffer = (GLuint)host_arg(c, 1);
    if (target == GL_UNIFORM_BUFFER) uniform_binding = buffer;
    glBindBuffer(target, buffer);
}
// The other direction: pixels read into a persistently mapped pack buffer
// land in the driver's copy, but the game reads the mapping, which is the
// shadow. So they are fetched into it as soon as the read is issued (the
// video recorder reads every frame this way).
static void read_into_shadow(GLintptr offset, GLsizeiptr bytes) {
    pthread_mutex_lock(&shadow_lock);
    shadow_t *s = shadow_of(bound_buffer(GL_PIXEL_PACK_BUFFER));
    if (s && s->shadow && offset >= 0 && bytes > 0 && offset < s->size) {
        if (bytes > s->size - offset) bytes = s->size - offset;
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, offset, bytes, s->shadow + offset);
        memcpy(s->uploaded + offset, s->shadow + offset, (size_t)bytes);
    }
    pthread_mutex_unlock(&shadow_lock);
}

// The game marks "none" or "infinitely far" in uniform data with +-1e38
// (Jupiter's and Saturn's ring parameters, for one). Squared, that overflows.
// Other drivers carry the infinity through harmlessly; Apple's GPU compiler
// uses fast math, which turns it into NaN, and those planets drew black. The
// exact sentinel values (and +-FLT_MAX) become +-1e18, still far beyond any
// distance in use but safe to square. Ordinary data never has those bit
// patterns. SE_KEEP_SENTINELS=1 leaves them alone.
static void tame_sentinels(uint8_t *data, GLsizeiptr size) {
    static int keep = -1;
    if (keep < 0) keep = getenv("SE_KEEP_SENTINELS") != NULL;
    if (keep) return;
    uint32_t *u = (uint32_t *)data;
    for (GLsizeiptr q = 0; q < size / 4; q++) {
        uint32_t magnitude = u[q] & 0x7fffffffu;
        if (magnitude == 0x7e967699u || magnitude == 0x7f7fffffu) {   // 1e38f, FLT_MAX
            float replacement = (u[q] >> 31) ? -1e18f : 1e18f;
            memcpy(&u[q], &replacement, 4);
        }
    }
}

// Brings the driver's copy of every bound shadow range up to date.
static void upload_shadows(void) {
    int touched = 0;
    for (int i = 0; i < indexed_high; i++) {
        shadow_t *s = indexed[i].buffer;
        if (!s || !s->shadow) continue;
        GLintptr offset = indexed[i].offset;
        GLsizeiptr size = indexed[i].size;
        if (offset < 0 || size <= 0 || offset + size > s->size) continue;
        if (!memcmp(s->shadow + offset, s->uploaded + offset, (size_t)size)) continue;
        tame_sentinels(s->shadow + offset, size);
        glBindBuffer(GL_UNIFORM_BUFFER, s->id);
        glBufferSubData(GL_UNIFORM_BUFFER, offset, size, s->shadow + offset);
        memcpy(s->uploaded + offset, s->shadow + offset, (size_t)size);
        touched = 1;
    }
    if (touched) glBindBuffer(GL_UNIFORM_BUFFER, uniform_binding);
}
// Debugging aid: SE_GL_DEBUG=1 checks for an OpenGL error after every draw on
// the main thread and reports each distinct situation once, and
// gl_debug_frame() summarises each of the first frames.
static int gl_debug = -1;

int gl_check_errors;
int gl_logging;
void gl_log_call(cpu_t *c, const char *name) {
    if (!pthread_main_np()) return;
    fprintf(stderr, "gl-log: %s(%#llx, %#llx, %#llx, %#llx, %#llx)\n", name,
            (unsigned long long)host_arg(c, 0), (unsigned long long)host_arg(c, 1), (unsigned long long)host_arg(c, 2),
            (unsigned long long)host_arg(c, 3), (unsigned long long)host_arg(c, 4));
}
__attribute__((constructor)) static void read_gl_check_errors(void) { gl_check_errors = getenv("SE_GL_ERRORS") != NULL; }
void gl_report_error(cpu_t *c, const char *name) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    static int reported;
    if (reported++ > 200) return;
    fprintf(stderr, "gl-error: %s(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) -> %#x\n", name,
            (unsigned long long)host_arg(c, 0), (unsigned long long)host_arg(c, 1), (unsigned long long)host_arg(c, 2),
            (unsigned long long)host_arg(c, 3), (unsigned long long)host_arg(c, 4), (unsigned long long)host_arg(c, 5), e);
}
static long draws_this_frame, errors_this_frame;

// Debugging aid: SE_GL_PROBE=<frame> reads the pixel at the centre of the
// viewport after every draw of that frame (on the main thread) and prints it
// with the program and framebuffer. SE_GL_PROBE_PROGRAM=<id> additionally lists
// that program's uniforms and the textures bound for it.
static shadow_t *shadow_of(GLuint id);
static long probe_frame_now, probe_frame = -1, probe_draw;
static void probe_details(GLuint program) {
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; i++) {
        char name[128]; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, sizeof name, NULL, &size, &type, name);
        GLint loc = glGetUniformLocation(program, name);
        if (loc < 0) continue;   // in a uniform block
        float v[16] = {0}; GLint iv[4] = {0};
        if (type == GL_FLOAT || type == GL_FLOAT_VEC2 || type == GL_FLOAT_VEC3 || type == GL_FLOAT_VEC4) {
            glGetUniformfv(program, loc, v);
            fprintf(stderr, "gl-probe:   uniform %s = %g %g %g %g\n", name, v[0], v[1], v[2], v[3]);
        } else {
            glGetUniformiv(program, loc, iv);
            fprintf(stderr, "gl-probe:   uniform %s (type %#x) = %d\n", name, type, iv[0]);
            int unit = iv[0];
            if (unit >= 0 && unit < 32 && (type == GL_SAMPLER_2D || type == GL_SAMPLER_3D || type == GL_SAMPLER_CUBE ||
                                            type == GL_SAMPLER_2D_ARRAY || type == GL_SAMPLER_2D_SHADOW || type == GL_SAMPLER_1D)) {
                GLint active = 0;
                glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
                glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
                GLenum targets[] = {GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_1D};
                GLenum bindings[] = {GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_3D, GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_BINDING_1D};
                for (int k = 0; k < 5; k++) {
                    GLint tex = 0, w = 0, h = 0, d = 0, fmt = 0;
                    glGetIntegerv(bindings[k], &tex);
                    if (!tex) continue;
                    GLenum level_target = targets[k] == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : targets[k];
                    glGetTexLevelParameteriv(level_target, 0, GL_TEXTURE_WIDTH, &w);
                    glGetTexLevelParameteriv(level_target, 0, GL_TEXTURE_HEIGHT, &h);
                    glGetTexLevelParameteriv(level_target, 0, GL_TEXTURE_DEPTH, &d);
                    glGetTexLevelParameteriv(level_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                    fprintf(stderr, "gl-probe:     unit %d target %#x: texture %d, %dx%dx%d, format %#x\n", unit, targets[k], tex, w, h, d, fmt);
                    // Floating-point textures: how many texels are NaN or infinite?
                    if ((fmt == GL_RGBA32F || fmt == GL_RGBA16F || fmt == GL_RGB32F || fmt == GL_RGB16F || fmt == GL_R32F ||
                         fmt == GL_R16F || fmt == GL_RG32F || fmt == GL_RG16F) && targets[k] != GL_TEXTURE_CUBE_MAP &&
                        (size_t)w * (size_t)h * (size_t)(d ? d : 1) <= (size_t)4 << 20) {
                        size_t n = (size_t)w * (size_t)h * (size_t)(d ? d : 1) * 4;
                        float *texels = malloc(n * sizeof *texels);
                        glGetTexImage(targets[k], 0, GL_RGBA, GL_FLOAT, texels);
                        size_t bad = 0, first = (size_t)-1;
                        float lo = 1e30f, hi = -1e30f;
                        for (size_t q = 0; q < n; q++) {
                            if (texels[q] != texels[q] || texels[q] - texels[q] != 0) { bad++; if (first == (size_t)-1) first = q; }
                            else { if (texels[q] < lo) lo = texels[q]; if (texels[q] > hi) hi = texels[q]; }
                        }
                        fprintf(stderr, "gl-probe:       %zu of %zu values NaN/inf (first at texel %zu); finite range %g .. %g\n",
                                bad, n, first == (size_t)-1 ? 0 : first / 4, lo, hi);
                        free(texels);
                    }
                }
                glActiveTexture((GLenum)active);
            }
        }
    }
    GLint blocks = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    for (GLint b = 0; b < blocks; b++) {
        char name[128]; GLint binding = 0, size = 0;
        glGetActiveUniformBlockName(program, (GLuint)b, sizeof name, NULL, name);
        glGetActiveUniformBlockiv(program, (GLuint)b, GL_UNIFORM_BLOCK_BINDING, &binding);
        glGetActiveUniformBlockiv(program, (GLuint)b, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        GLint buffer = 0; GLint64 offset = 0, bsize = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)binding, &buffer);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)binding, &offset);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, (GLuint)binding, &bsize);
        fprintf(stderr, "gl-probe:   block %s: binding %d, %d bytes; buffer %d at %lld, %lld bytes\n", name, binding, size, buffer, (long long)offset, (long long)bsize);
        if (buffer && size > 0 && size <= 65536) {
            float *gpu = calloc(1, (size_t)size);
            GLint previous = 0;
            glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &previous);
            glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)buffer);
            glGetBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)offset, size, gpu);
            glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)previous);
            shadow_t *sh = shadow_of((GLuint)buffer);
            int nans = 0, differ = 0;
            for (int k = 0; k < size / 4; k++) {
                if (gpu[k] != gpu[k] || gpu[k] - gpu[k] != 0) { if (nans++ < 8) fprintf(stderr, "gl-probe:     float %d (byte %d) = %g\n", k, k * 4, gpu[k]); }
                if (sh && memcmp(&gpu[k], sh->shadow + offset + 4 * k, 4)) differ++;
            }
            fprintf(stderr, "gl-probe:     %d of %d floats are NaN or infinite; %d differ from the shadow copy%s\n", nans, size / 4, differ, sh ? "" : " (no shadow)");
            for (int k = 0; k < size / 4 && k < 360; k += 4)
                fprintf(stderr, "gl-probe:     [%4d] %g %g %g %g\n", k * 4, gpu[k], gpu[k + 1], gpu[k + 2], gpu[k + 3]);
            free(gpu);
        }
    }
    while (glGetError() != GL_NO_ERROR) {}
}
static void probe_after_draw(const char *what) {
    if (probe_frame < 0) { const char *v = getenv("SE_GL_PROBE"); probe_frame = v ? atol(v) : 0; }
    if (!probe_frame || probe_frame_now != probe_frame || !pthread_main_np()) return;
    GLint program = 0, fbo = 0, viewport[4] = {0}, read = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_VIEWPORT, viewport);
    float px[4] = {-1, -1, -1, -1};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)fbo);
    glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    while (glGetError() != GL_NO_ERROR) {}
    fprintf(stderr, "gl-probe: draw %ld %s program %d fbo %d viewport %dx%d centre %.4g %.4g %.4g %.4g\n",
            ++probe_draw, what, program, fbo, viewport[2], viewport[3], px[0], px[1], px[2], px[3]);
    const char *detail = getenv("SE_GL_PROBE_PROGRAM");
    if (detail && atoi(detail) == program) probe_details((GLuint)program);
}

static void after_draw(const char *what) {
    probe_after_draw(what);
    if (gl_debug < 0) gl_debug = getenv("SE_GL_DEBUG") != NULL;
    if (!gl_debug || !pthread_main_np()) return;
    draws_this_frame++;
    GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    errors_this_frame++;
    static unsigned seen[64];
    static int seen_count;
    GLint program = 0, fbo = 0, vao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    unsigned key = error * 1000003u + (unsigned)program * 31u + (unsigned)fbo;
    for (int i = 0; i < seen_count; i++) if (seen[i] == key) return;
    if (seen_count < 64) seen[seen_count++] = key;
    GLint linked = 0, valid = 0;
    if (program) { glGetProgramiv((GLuint)program, GL_LINK_STATUS, &linked); glValidateProgram((GLuint)program); glGetProgramiv((GLuint)program, GL_VALIDATE_STATUS, &valid); }
    fprintf(stderr, "gl-debug: %s -> error %#x; program %d (linked %d, valid %d), vertex array %d, framebuffer %d (status %#x)\n",
            what, error, program, linked, valid, vao, fbo, glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
    while (glGetError() != GL_NO_ERROR) {}
}
void gl_debug_frame(void) {
    static long frame;
    probe_frame_now++;
    static long log_first = -1, log_count = 1;
    if (log_first < 0) {
        const char *v = getenv("SE_GL_LOG");
        log_first = v ? atol(v) : 0;
        if (v && strchr(v, ',')) log_count = atol(strchr(v, ',') + 1);
    }
    gl_logging = log_first && probe_frame_now >= log_first && probe_frame_now < log_first + log_count;
    if (gl_logging) fprintf(stderr, "gl-log: ---- frame %ld\n", probe_frame_now);
    if (gl_debug < 0) gl_debug = getenv("SE_GL_DEBUG") != NULL;
    if (!gl_debug) return;
    frame++;
    if (frame <= 3 || frame % 300 == 0) {
        GLint fbo = 0, viewport[4] = {0};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        fprintf(stderr, "gl-debug: frame %ld: %ld draws, %ld with errors; at swap framebuffer %d, viewport %dx%d\n",
                frame, draws_this_frame, errors_this_frame, fbo, viewport[2], viewport[3]);
    }
    draws_this_frame = errors_this_frame = 0;
}
static void compat_glUseProgram(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glUseProgram");
    GLuint program = (GLuint)host_arg(c, 0);
    glUseProgram(program);
    if (gl_debug < 0) gl_debug = getenv("SE_GL_DEBUG") != NULL;
    if (!gl_debug) return;
    GLenum error = glGetError();
    static int reported;
    if (error != GL_NO_ERROR && reported < 12) {
        GLint linked = -1;
        if (glIsProgram(program)) glGetProgramiv(program, GL_LINK_STATUS, &linked);
        reported++;
        fprintf(stderr, "gl-debug: glUseProgram(%u) on %s thread -> error %#x; is a program: %d, linked: %d\n",
                program, pthread_main_np() ? "the main" : "a loader", error, glIsProgram(program), linked);
    }
}
// Debugging aid: with SE_SHADER_DEBUG set, a program that fails to link is
// reported with the linker's messages and the interface declarations (in,
// out, layout) of each stage, and nothing else of the shaders' text.
static void report_link_failure(GLuint program) {
    GLint linked = 0, log_length = 0, count = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked) return;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    char *log = calloc(1, (size_t)log_length + 2);
    glGetProgramInfoLog(program, log_length + 1, NULL, log);
    GLuint shaders[8];
    glGetAttachedShaders(program, 8, &count, shaders);
    fprintf(stderr, "shader-debug: link failed:\n%s", log);
    for (GLint i = 0; i < count; i++) {
        GLint type = 0, length = 0;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
        char *source = calloc(1, (size_t)length + 2);
        glGetShaderSource(shaders[i], length + 1, NULL, source);
        const char *stage = type == GL_VERTEX_SHADER ? "vertex" : type == GL_GEOMETRY_SHADER ? "geometry" :
                            type == GL_FRAGMENT_SHADER ? "fragment" : "other";
        int n = 1;
        for (const char *p = source; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL, n++) {
            const char *q = p + strspn(p, " \t");
            size_t len = strcspn(q, "\r\n");
            static const char *const starts[] = {"layout", "in ", "out ", "flat ", "smooth ", "noperspective ", "centroid ", "#if", "#el", "#endif"};
            for (size_t k = 0; k < sizeof starts / sizeof starts[0]; k++)
                if (!strncmp(q, starts[k], strlen(starts[k]))) { fprintf(stderr, "    %s %d: %.*s\n", stage, n, (int)len, q); break; }
        }
        free(source);
    }
    free(log);
}
static void compat_glLinkProgram(cpu_t *c) {
    GLuint program = (GLuint)host_arg(c, 0);
    gl_link_program(program);
    if (getenv("SE_SHADER_DEBUG")) report_link_failure(program);
    if (gl_debug < 0) gl_debug = getenv("SE_GL_DEBUG") != NULL;
    if (!gl_debug) return;
    static int total, failed;
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    total++; failed += !linked;
    if (total % 50 == 0) fprintf(stderr, "gl-debug: %d programs linked so far, %d failed\n", total, failed);
}
static void compat_glProgramBinary(cpu_t *c) {  // (program, format, binary, length)
    GLuint program = (GLuint)host_arg(c, 0);
    glProgramBinary(program, (GLenum)host_arg(c, 1), (const void *)host_arg(c, 2), (GLsizei)host_arg(c, 3));
    if (gl_debug < 0) gl_debug = getenv("SE_GL_DEBUG") != NULL;
    if (!gl_debug) return;
    static int total, failed;
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    total++; failed += !linked;
    if (total % 50 == 0 || total == 1) fprintf(stderr, "gl-debug: %d cached program binaries loaded so far, %d rejected\n", total, failed);
}
static void compat_glDrawArrays(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glDrawArrays");
    upload_shadows();
    glDrawArrays((GLenum)host_arg(c, 0), (GLint)host_arg(c, 1), (GLsizei)host_arg(c, 2));
    after_draw("glDrawArrays");
}
static void compat_glDrawElements(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glDrawElements");
    upload_shadows();
    glDrawElements((GLenum)host_arg(c, 0), (GLsizei)host_arg(c, 1), (GLenum)host_arg(c, 2), (const GLvoid *)host_arg(c, 3));
    after_draw("glDrawElements");
}
static void compat_glDrawArraysInstanced(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glDrawArraysInstanced");
    upload_shadows();
    glDrawArraysInstanced((GLenum)host_arg(c, 0), (GLint)host_arg(c, 1), (GLsizei)host_arg(c, 2), (GLsizei)host_arg(c, 3));
}
static void compat_glDrawElementsInstanced(cpu_t *c) {
    if (gl_logging) gl_log_call(c, "glDrawElementsInstanced");
    upload_shadows();
    glDrawElementsInstanced((GLenum)host_arg(c, 0), (GLsizei)host_arg(c, 1), (GLenum)host_arg(c, 2),
                            (const GLvoid *)host_arg(c, 3), (GLsizei)host_arg(c, 4));
}

// ---------------------------------------------------------------- shaders
// Debugging aid: with SE_SHADER_DEBUG set, a shader that fails to compile is
// reported with each line the compiler names, and nothing else of its text.
static void compat_glCompileShader(cpu_t *c) {
    GLuint shader = (GLuint)host_arg(c, 0);
    gl_compile_shader(shader);   // compiles, repairing what Apple's compiler rejects (gl_shader.c)
    if (!getenv("SE_SHADER_DEBUG")) return;
    GLint ok = 0, length = 0, log_length = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    char *source = calloc(1, (size_t)length + 2), *log = calloc(1, (size_t)log_length + 2);
    glGetShaderSource(shader, length + 1, NULL, source);
    glGetShaderInfoLog(shader, log_length + 1, NULL, log);
    static pthread_mutex_t out = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&out);
    for (char *e = log; e && *e; e = strchr(e, '\n') ? strchr(e, '\n') + 1 : NULL) {
        int line = 0;
        if (sscanf(e, "ERROR: 0:%d:", &line) != 1) continue;
        const char *p = source;
        for (int n = 1; n < line && p; n++) p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL;
        fprintf(stderr, "shader-debug: %.*s\n    line %d: %.*s\n", (int)strcspn(e, "\n"), e, line,
                p ? (int)strcspn(p, "\r\n") : 0, p ? p : "");
        // Names on the rejected line that are macros there, with their definitions.
        for (const char *id = p; id && *id && *id != '\n' && *id != '\r'; ) {
            if (!isalpha((unsigned char)*id) && *id != '_') { id++; continue; }
            size_t n = 1;
            while (isalnum((unsigned char)id[n]) || id[n] == '_') n++;
            for (const char *d = source; d && d < p; d = strchr(d, '\n') ? strchr(d, '\n') + 1 : NULL) {
                const char *h = d + strspn(d, " \t");
                if (strncmp(h, "struct", 6) == 0 && (h[6] == ' ' || h[6] == '\t')) {
                    const char *tag = h + 6 + strspn(h + 6, " \t");
                    if (strncmp(tag, id, n) == 0 && !isalnum((unsigned char)tag[n]) && tag[n] != '_')
                        fprintf(stderr, "    type: %.*s\n", (int)strcspn(d, "\r\n"), d);
                }
                if (*h != '#') continue;
                h += 1 + strspn(h + 1, " \t");
                if (strncmp(h, "define", 6) != 0) continue;
                h += 6 + strspn(h + 6, " \t");
                if (strncmp(h, id, n) == 0 && !isalnum((unsigned char)h[n]) && h[n] != '_')
                    fprintf(stderr, "    macro: %.*s\n", (int)strcspn(d, "\r\n"), d);
            }
            id += n;
        }
        // A bare syntax error usually refers to a construct that began a few lines earlier.
        if (strstr(e, ": syntax error: syntax error") && strncmp(e, "ERROR: 0:", 9) == 0) {
            const char *q = source, *wanted = getenv("SE_SHADER_CONTEXT");
            int before = wanted ? atoi(wanted) : 7;
            for (int n = 1; n < line - before && q; n++) q = strchr(q, '\n') ? strchr(q, '\n') + 1 : NULL;
            for (int n = line > before ? line - before : 1; n < line && q; n++) {
                fprintf(stderr, "    context %d: %.*s\n", n, (int)strcspn(q, "\r\n"), q);
                q = strchr(q, '\n') ? strchr(q, '\n') + 1 : NULL;
            }
        }
    }
    pthread_mutex_unlock(&out);
    free(source); free(log);
}

// GL_KHR_robustness, reported so that GLEW loads glReadnPixels, which the
// video recorder calls unconditionally. Its functions are the 4.1 ones with a
// destination size, and a reset status that is always "no reset": Apple's
// driver reports none.
static void read_into_shadow(GLintptr offset, GLsizeiptr bytes);
static void compat_glReadnPixels(cpu_t *c) {   // (x, y, width, height, format, type, bufSize, data)
    glReadPixels((GLint)host_arg(c, 0), (GLint)host_arg(c, 1), (GLsizei)host_arg(c, 2), (GLsizei)host_arg(c, 3),
                 (GLenum)host_arg(c, 4), (GLenum)host_arg(c, 5), (void *)host_arg(c, 7));
    read_into_shadow((GLintptr)host_arg(c, 7), (GLsizeiptr)(GLsizei)host_arg(c, 6));
}
static void compat_glGetGraphicsResetStatus(cpu_t *c) { c->r[RAX] = 0; }   // GL_NO_ERROR
static void compat_glGetnUniformfv(cpu_t *c) { glGetUniformfv((GLuint)host_arg(c, 0), (GLint)host_arg(c, 1), (GLfloat *)host_arg(c, 3)); }
static void compat_glGetnUniformiv(cpu_t *c) { glGetUniformiv((GLuint)host_arg(c, 0), (GLint)host_arg(c, 1), (GLint *)host_arg(c, 3)); }
static void compat_glGetnUniformuiv(cpu_t *c) { glGetUniformuiv((GLuint)host_arg(c, 0), (GLint)host_arg(c, 1), (GLuint *)host_arg(c, 3)); }

const gl_entry_t gl_compat[] = {
    {"glBindBuffer", compat_glBindBuffer}, {"glBindBufferBase", compat_glBindBufferBase},
    {"glBindBufferRange", compat_glBindBufferRange}, {"glBufferStorage", compat_glBufferStorage},
    {"glCompileShader", compat_glCompileShader},
    {"glDeleteBuffers", compat_glDeleteBuffers}, {"glDrawArrays", compat_glDrawArrays},
    {"glDrawArraysInstanced", compat_glDrawArraysInstanced}, {"glDrawElements", compat_glDrawElements},
    {"glDrawElementsInstanced", compat_glDrawElementsInstanced},
    {"glGetIntegerv", compat_glGetIntegerv}, {"glGetString", compat_glGetString}, {"glGetStringi", compat_glGetStringi},
    {"glLinkProgram", compat_glLinkProgram},
    {"glMapBufferRange", compat_glMapBufferRange}, {"glNamedBufferStorageEXT", compat_glNamedBufferStorageEXT},
    {"glProgramBinary", compat_glProgramBinary}, {"glReadnPixels", compat_glReadnPixels},
    {"glGetGraphicsResetStatus", compat_glGetGraphicsResetStatus}, {"glGetnUniformfv", compat_glGetnUniformfv},
    {"glGetnUniformiv", compat_glGetnUniformiv}, {"glGetnUniformuiv", compat_glGetnUniformuiv},
    {"glUnmapBuffer", compat_glUnmapBuffer}, {"glUseProgram", compat_glUseProgram},
};
const int gl_compat_count = sizeof gl_compat / sizeof gl_compat[0];
