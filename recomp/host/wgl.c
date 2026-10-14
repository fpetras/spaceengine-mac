// The OpenGL context functions (WGL) and the GDI calls around them.
//
// A window becomes a real macOS window when it first gets an OpenGL context.
// Every context is an OpenGL 4.1 core context, the newest macOS offers, and
// all contexts share objects, which is what the game sets up with
// wglShareLists anyway.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <SDL3/SDL.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "host.h"
#include "window.h"

enum { MAX_CONTEXTS = 16 };
#define CONTEXT_BASE 0x40000ull
static struct { SDL_GLContext context; } contexts[MAX_CONTEXTS];
static _Thread_local uint64_t current_dc;

// How contexts relate to windows here, and why.
//
// On Windows, creating a context changes nothing about which context a thread
// is using, and a context has no window until it is made current with one.
// SDL on macOS does both at creation: the new context becomes current on the
// calling thread and is attached to the window. Left like that, the game's
// main thread ends up drawing with the last loader context it created, and
// every context is attached to the one visible window, which then shows
// nothing.
//
// So: all contexts are created against a hidden helper window and share with
// one root context that is never used for drawing; creation restores whatever
// the calling thread had current; only the main thread ever attaches a
// context to a visible window; other threads (the game's loader threads, which
// compile shaders and upload textures) make their context current without any
// window at all.
static SDL_Window *helper_window;
static SDL_GLContext root_context;

static host_window_t *window_of_dc(uint64_t hdc) { return window_from_handle(hdc & ~1ull); }

static void set_attributes(int share) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, share);
}

// The CGL context inside an SDL context, for making it current on a thread
// without involving a window.
static CGLContextObj cgl_of(SDL_GLContext context) {
    return ((CGLContextObj (*)(id, SEL))objc_msgSend)((id)context, sel_registerName("CGLContextObj"));
}

static uint64_t create_context(uint64_t hdc) {
    host_window_t *w = window_of_dc(hdc);
    if (!w) return 0;
    if (!pthread_main_np()) {
        fprintf(stderr, "host: an OpenGL context was requested off the main thread, which macOS does not allow here\n");
        return 0;
    }
    host_video_init();
    if (!root_context) {
        set_attributes(0);
        helper_window = SDL_CreateWindow("", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        root_context = helper_window ? SDL_GL_CreateContext(helper_window) : NULL;
        if (!root_context) { fprintf(stderr, "host: could not create an OpenGL context: %s\n", SDL_GetError()); return 0; }
        SDL_GL_MakeCurrent(NULL, NULL);
    }
    if (!w->sdl) {
        set_attributes(0);
        float scale = host_scale();
        w->sdl = SDL_CreateWindow(w->title, (int)((float)w->width / scale), (int)((float)w->height / scale),
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (w->visible ? 0 : SDL_WINDOW_HIDDEN) |
                                  (scale > 1.0f ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0));
        if (!w->sdl) { fprintf(stderr, "host: could not create the window: %s\n", SDL_GetError()); return 0; }
        SDL_StartTextInput(w->sdl);
    }
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    SDL_GL_MakeCurrent(helper_window, root_context);
    set_attributes(1);
    SDL_GLContext context = SDL_GL_CreateContext(helper_window);
    if (previous_context) SDL_GL_MakeCurrent(previous_window, previous_context);
    else SDL_GL_MakeCurrent(NULL, NULL);
    if (!context) { fprintf(stderr, "host: could not create an OpenGL context: %s\n", SDL_GetError()); return 0; }
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (!contexts[i].context) {
            contexts[i].context = context;
            if (getenv("SE_VERBOSE"))
                fprintf(stderr, "host: OpenGL context %d created, %s the root context\n", i,
                        CGLGetShareGroup(cgl_of(context)) == CGLGetShareGroup(cgl_of(root_context)) ? "sharing with" : "NOT sharing with");
            return CONTEXT_BASE + (uint64_t)i;
        }
    return 0;
}

IMPORT(wglCreateContext) { RET(create_context(ARG(0))); }
static void wgl_create_context_attribs(cpu_t *c) { c->r[RAX] = create_context(c->r[RCX]); }
IMPORT(wglMakeCurrent) {
    uint64_t i = ARG(1) - CONTEXT_BASE;
    int main_thread = pthread_main_np();
    if (!ARG(1)) {
        if (main_thread) SDL_GL_MakeCurrent(NULL, NULL); else CGLSetCurrentContext(NULL);
        current_dc = 0;
        RET(1);
    }
    if (i >= MAX_CONTEXTS || !contexts[i].context) RET(0);
    current_dc = ARG(0);
    if (!main_thread) RET(CGLSetCurrentContext(cgl_of(contexts[i].context)) == kCGLNoError);
    host_window_t *w = window_of_dc(ARG(0));
    int ok = SDL_GL_MakeCurrent(w && w->sdl ? w->sdl : helper_window, contexts[i].context);
    if (ok && getenv("SE_VERBOSE"))
        fprintf(stderr, "host: OpenGL context %d current on the main thread: %s, %s\n", (int)i, glGetString(GL_VERSION), glGetString(GL_RENDERER));
    RET(ok);
}
IMPORT(wglDeleteContext) {
    uint64_t i = ARG(0) - CONTEXT_BASE;
    if (i < MAX_CONTEXTS && contexts[i].context && pthread_main_np()) {
        SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
        SDL_GL_DestroyContext(contexts[i].context);
        if (previous_context && previous_context != contexts[i].context) SDL_GL_MakeCurrent(previous_window, previous_context);
        contexts[i].context = NULL;
    }
    RET(1);   // a context deleted from another thread is left for process exit
}
IMPORT(wglGetCurrentDC) { RET(current_dc); }
IMPORT(wglShareLists) { RET(1); }
// Debugging aid: SE_SCREENSHOT=<file.png> saves the frame about to be shown
// once the frame counter reaches SE_SCREENSHOT_FRAME (default 300). This reads
// the context's back buffer; it does not prove the window displays it.
// SE_SCREENSHOT_COUNT=n saves n consecutive frames, as <path>-0.png, -1...
static void maybe_screenshot(SDL_Window *window) {
    static long frame;
    static int done;
    const char *path = getenv("SE_SCREENSHOT");
    if (!path || done) return;
    const char *at = getenv("SE_SCREENSHOT_FRAME"), *count_text = getenv("SE_SCREENSHOT_COUNT");
    long first = at ? atol(at) : 300, count = count_text ? atol(count_text) : 1;
    if (++frame < first) return;
    if (frame >= first + count - 1) done = 1;
    char numbered[1024];
    if (count > 1) {
        snprintf(numbered, sizeof numbered, "%.*s-%ld.png", (int)(strlen(path) > 4 && !strcmp(path + strlen(path) - 4, ".png") ? strlen(path) - 4 : strlen(path)), path, frame - first);
        path = numbered;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    uint8_t *pixels = malloc((size_t)w * (size_t)h * 4);
    GLint read_fbo = 0, pack_buffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    // A series is kept in memory and written after its last frame, so that
    // capturing does not slow the frames being captured.
    static struct { char path[1024]; uint8_t *pixels; } kept[256];
    static int kept_count;
    if (count > 1 && kept_count < 256) {
        snprintf(kept[kept_count].path, sizeof kept[0].path, "%s", path);
        kept[kept_count++].pixels = pixels;
        if (!done) return;
        for (int i = 0; i < kept_count; i++) { host_write_png(kept[i].path, kept[i].pixels, w, h); free(kept[i].pixels); }
        fprintf(stderr, "host: frames %ld to %ld saved as %s (%dx%d)\n", first, frame, path, w, h);
        return;
    }
    host_write_png(path, pixels, w, h);
    free(pixels);
    fprintf(stderr, "host: frame %ld saved to %s (%dx%d)\n", frame, path, w, h);
}
// Debugging aid: SE_KEYS="<frame>:<key>[+<key>...];..." feeds key presses into
// the event queue, e.g. "1300:Escape;2600:Left Ctrl+F11". Keys are named as SDL
// names them. All keys of an entry go down in order, two frames apart, and
// come up in reverse six frames after the last. An entry starting with an
// apostrophe is typed text instead: "2500:'abc". Like SE_PRESS_ESC_AT it tests
// everything from the host's event translation onwards.
static void push_key(SDL_Window *window, SDL_Scancode code, int down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(window);
    e.key.scancode = code;
    e.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    e.key.down = down != 0;
    SDL_PushEvent(&e);
}
static void maybe_press_keys(SDL_Window *window) {
    enum { MAX = 64 };
    typedef struct { long frame; SDL_Scancode code; int down; const char *text; } step_t;
    static step_t plan[MAX];
    static int count = -1;
    static long frame;
    if (count < 0) {
        count = 0;
        const char *spec = getenv("SE_KEYS");
        char *copy = spec ? strdup(spec) : NULL, *outer = NULL;
        for (char *entry = copy ? strtok_r(copy, ";", &outer) : NULL; entry; entry = strtok_r(NULL, ";", &outer)) {
            char *colon = strchr(entry, ':'), *inner = NULL;
            if (!colon) continue;
            long at = atol(entry);
            if (colon[1] == '\'') {
                if (count < MAX) plan[count++] = (step_t){at, SDL_SCANCODE_UNKNOWN, 0, strdup(colon + 2)};
                continue;
            }
            SDL_Scancode codes[8];
            int n = 0;
            for (char *name = strtok_r(colon + 1, "+", &inner); name && n < 8; name = strtok_r(NULL, "+", &inner)) {
                SDL_Scancode code = SDL_GetScancodeFromName(name);
                if (code == SDL_SCANCODE_UNKNOWN) fprintf(stderr, "host: SE_KEYS: no key is called \"%s\"\n", name);
                else codes[n++] = code;
            }
            for (int i = 0; i < n && count < MAX; i++) plan[count++] = (step_t){at + 2 * i, codes[i], 1, NULL};
            for (int i = n - 1; i >= 0 && count < MAX; i--) plan[count++] = (step_t){at + 2 * n + 6 + 2 * (n - 1 - i), codes[i], 0, NULL};
        }
        free(copy);
    }
    frame++;
    for (int i = 0; i < count; i++)
        if (plan[i].frame == frame && plan[i].text) {
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = SDL_GetWindowID(window);
            e.text.text = plan[i].text;
            SDL_PushEvent(&e);
            fprintf(stderr, "host: \"%s\" typed at frame %ld\n", plan[i].text, frame);
        } else if (plan[i].frame == frame) {
            push_key(window, plan[i].code, plan[i].down);
            if (plan[i].down) fprintf(stderr, "host: %s pressed at frame %ld\n", SDL_GetScancodeName(plan[i].code), frame);
        }
}

// Debugging aid: SE_PRESS_ESC_AT=<frame> feeds an Escape key press into the
// event queue at that frame, as if it had come from the keyboard. It tests
// everything from the host's event translation onwards, not macOS delivering
// keys to the window.
static void maybe_press_escape(SDL_Window *window) {
    static long frame, at = -1;
    if (at == -1) { const char *v = getenv("SE_PRESS_ESC_AT"); at = v ? atol(v) : 0; }
    if (!at) return;
    frame++;
    if (frame != at && frame != at + 6) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = frame == at ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(window);
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    e.key.key = SDLK_ESCAPE;
    e.key.down = frame == at;
    SDL_PushEvent(&e);
    if (frame == at) fprintf(stderr, "host: Escape pressed at frame %ld\n", frame);
}
// SE_FPS=1 prints the frame rate every five seconds.
static void count_frame(void) {
    static uint64_t since, frames;
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SE_FPS") != NULL;
    if (!enabled) return;
    uint64_t now = SDL_GetTicks();
    if (!since) since = now;
    frames++;
    if (now - since >= 5000) {
        fprintf(stderr, "host: %.1f frames per second\n", (double)frames * 1000.0 / (double)(now - since));
        since = now; frames = 0;
    }
}
IMPORT(wglSwapLayerBuffers) {
    host_window_t *w = window_of_dc(ARG(0));
    if (w && w->sdl && pthread_main_np()) { maybe_press_escape(w->sdl); maybe_press_keys(w->sdl); gl_debug_frame(); maybe_screenshot(w->sdl); SDL_GL_SwapWindow(w->sdl); count_frame(); }
    RET(1);
}

// ----------------------------------------------- WGL extension functions
static void wgl_extensions_string(cpu_t *c) {
    c->r[RAX] = (uint64_t)"WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_create_context "
                          "WGL_ARB_create_context_profile WGL_ARB_pixel_format WGL_EXT_swap_control";
}
static void wgl_choose_pixel_format(cpu_t *c) {   // (dc, int attribs, float attribs, max, formats, count)
    if (host_arg(c, 3) && host_arg(c, 4)) st32(host_arg(c, 4), 1);
    if (host_arg(c, 5)) st32(host_arg(c, 5), 1);
    c->r[RAX] = 1;
}
static void wgl_pixel_format_attribs(cpu_t *c) {  // (dc, format, layer, n, attributes, values)
    uint32_t n = (uint32_t)host_arg(c, 3);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t attribute = ld32(host_arg(c, 4) + 4 * i), value = 0;
        switch (attribute) {
            case 0x2000: value = 1; break;        // NUMBER_PIXEL_FORMATS
            case 0x2001: case 0x2010: case 0x2011: value = 1; break;  // DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLE_BUFFER
            case 0x2003: value = 0x2027; break;   // ACCELERATION: full
            case 0x2013: value = 0x202b; break;   // PIXEL_TYPE: RGBA
            case 0x2014: value = 32; break;       // COLOR_BITS
            case 0x2015: case 0x2017: case 0x2019: case 0x201b: value = 8; break;  // red, green, blue, alpha
            case 0x2022: value = 24; break;       // DEPTH_BITS
            case 0x2023: value = 8; break;        // STENCIL_BITS
            default: value = 0;                   // multisampling and the rest: not offered
        }
        st32(host_arg(c, 5) + 4 * i, value);
    }
    c->r[RAX] = 1;
}
static void wgl_swap_interval(cpu_t *c) { c->r[RAX] = SDL_GL_SetSwapInterval((int)(int32_t)c->r[RCX]); }
static void wgl_get_swap_interval(cpu_t *c) { int v = 0; SDL_GL_GetSwapInterval(&v); c->r[RAX] = (uint32_t)v; }

static const gl_entry_t wgl_functions[] = {
    {"wglCreateContextAttribsARB", wgl_create_context_attribs}, {"wglGetExtensionsStringARB", wgl_extensions_string},
    {"wglGetExtensionsStringEXT", wgl_extensions_string}, {"wglChoosePixelFormatARB", wgl_choose_pixel_format},
    {"wglGetPixelFormatAttribivARB", wgl_pixel_format_attribs}, {"wglSwapIntervalEXT", wgl_swap_interval},
    {"wglGetSwapIntervalEXT", wgl_get_swap_interval},
};

// EXT-named entry points of extensions that became core unchanged.
static const char *const gl_aliases[][2] = {
    {"glTexImage3DEXT", "glTexImage3D"}, {"glTexSubImage3DEXT", "glTexSubImage3D"},
    {"glRenderbufferStorageMultisampleEXT", "glRenderbufferStorageMultisample"},
    {"glBlitFramebufferEXT", "glBlitFramebuffer"},
};

void (*gl_lookup(const char *name))(cpu_t *) {
    for (size_t i = 0; i < sizeof gl_aliases / sizeof gl_aliases[0]; i++)
        if (!strcmp(gl_aliases[i][0], name)) { name = gl_aliases[i][1]; break; }
    for (size_t i = 0; i < sizeof wgl_functions / sizeof wgl_functions[0]; i++)
        if (!strcmp(wgl_functions[i].name, name)) return wgl_functions[i].fn;
    for (int i = 0; i < gl_compat_count; i++)
        if (!strcmp(gl_compat[i].name, name)) return gl_compat[i].fn;
    int lo = 0, hi = gl_native_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2, cmp = strcmp(gl_native[mid].name, name);
        if (cmp == 0) return gl_native[mid].fn;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static int lookups, found;
IMPORT(wglGetProcAddress) {
    void (*fn)(cpu_t *) = gl_lookup(PTR(0));
    lookups++; found += fn != NULL;
    RET(fn ? host_export(fn) : 0);
}
void gl_report_lookups(void) { fprintf(stderr, "host: %d OpenGL names requested, %d provided\n", lookups, found); }

// The game's direct imports from OPENGL32.dll are the same native functions.
__attribute__((constructor)) static void register_gl_imports(void) {
    for (int i = 0; i < gl_native_count; i++) host_register(gl_native[i].name, gl_lookup(gl_native[i].name));
    for (int i = 0; i < gl_compat_count; i++)   // names that exist only as host versions
        if (gl_lookup(gl_compat[i].name) == gl_compat[i].fn) host_register(gl_compat[i].name, gl_compat[i].fn);
}

// -------------------------------------------------------------------- GDI
// Pixel formats: one is offered, a 32-bit double-buffered RGBA format with a
// 24-bit depth and 8-bit stencil buffer.
static void describe(uint64_t pfd) {
    memset((void *)pfd, 0, 40);
    st16(pfd, 40); st16(pfd + 2, 1); st32(pfd + 4, 0x25);  // draw to window, support OpenGL, double buffer
    uint8_t *p = (uint8_t *)pfd;
    p[9] = 32; p[10] = 8; p[12] = 8; p[14] = 8; p[16] = 8; p[23] = 24; p[24] = 8;
}
IMPORT(ChoosePixelFormat) { RET(1); }
IMPORT(DescribePixelFormat) { if (ARG(3)) describe(ARG(3)); RET(1); }
// A window's pixel format can be set once. Windows refuses a second attempt,
// and the game relies on that refusal when it re-selects a format before
// creating its real context.
IMPORT(SetPixelFormat) {
    host_window_t *w = window_of_dc(ARG(0));
    if (!w) RET(0);
    if (w->pixel_format_set) { SET_ERROR(2000); RET(0); }   // ERROR_INVALID_PIXEL_FORMAT
    w->pixel_format_set = 1;
    RET(1);
}
IMPORT(GetDeviceCaps) {
    switch ((int32_t)ARG(1)) {
        case 88: case 90: RET(96);   // LOGPIXELSX, LOGPIXELSY
        case 12: RET(32);            // BITSPIXEL
        case 116: RET(60);           // VREFRESH
        default: RET(0);
    }
}
// Drawing with GDI (the splash screen) goes nowhere. Objects are handed out
// so that the calls succeed; a DIB section gets real memory to draw into.
IMPORT(CreateSolidBrush) { RET(0x50001); }
IMPORT(CreatePen) { RET(0x50002); }
IMPORT(CreateFontIndirectW) { RET(0x50003); }
IMPORT(CreateBitmap) { RET(0x50004); }
IMPORT(GetStockObject) { RET(0x50005); }
IMPORT(CreateCompatibleDC) { RET(0x50007); }
IMPORT(CreateDIBSection) {
    int32_t width = (int32_t)ld32(ARG(1) + 4), height = (int32_t)ld32(ARG(1) + 8);
    uint32_t bits = ld16(ARG(1) + 14);
    size_t bytes = (size_t)((width * (int32_t)bits + 31) / 32 * 4) * (size_t)(height < 0 ? -height : height);
    if (ARG(3)) st64(ARG(3), (uint64_t)calloc(1, bytes ? bytes : 4));
    RET(0x50006);
}
IMPORT(SelectObject) { RET(0x50005); }
IMPORT(DeleteObject) { RET(1); }
IMPORT(DeleteDC) { RET(1); }
IMPORT(BitBlt) { RET(1); }
IMPORT(StretchBlt) { RET(1); }
IMPORT(SetStretchBltMode) { RET(1); }
IMPORT(SetBkMode) { RET(1); }
IMPORT(SetTextColor) { RET(0); }
IMPORT(Rectangle) { RET(1); }
IMPORT(DrawTextW) { RET(16); }
IMPORT(BeginPaint) { memset(PTR(1), 0, 72); RET(ARG(0) | 1); }
IMPORT(EndPaint) { RET(1); }
