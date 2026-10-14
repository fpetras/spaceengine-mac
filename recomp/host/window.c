// USER32 windowing on SDL3: displays, window classes, windows, and the
// message queue. Keyboard, mouse and window events from macOS are turned into
// the Windows messages the game's window procedure expects.
//
// Only a window that gets an OpenGL context becomes a real macOS window (see
// wgl.c). Others, such as a GDI-drawn splash screen, exist as bookkeeping
// only and are never shown.
#include <SDL3/SDL.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "paths.h"
#include "window.h"

enum { WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
       WM_KILLFOCUS = 0x08, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1c,
       WM_NCCREATE = 0x81, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
       WM_SYSKEYUP = 0x105, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
       WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
       WM_MOUSEWHEEL = 0x20a };

// ---------------------------------------------------------------- classes
static struct { char name[96]; uint64_t wndproc; } classes[16];
static int class_count;

static uint64_t class_proc(const uint16_t *name) {
    char *utf8 = (uint64_t)name > 0xffff ? u16_to_utf8(name, -1) : NULL;
    uint64_t proc = 0;
    for (int i = 0; i < class_count; i++)
        if (utf8 ? !strcmp(classes[i].name, utf8) : (uint64_t)name == 0xc000u + (uint64_t)i) proc = classes[i].wndproc;
    free(utf8);
    return proc;
}
static uint64_t register_class(const uint16_t *name, uint64_t wndproc) {
    if (class_count == 16) return 0;
    char *utf8 = u16_to_utf8(name, -1);
    snprintf(classes[class_count].name, sizeof classes[0].name, "%s", utf8);
    free(utf8);
    classes[class_count].wndproc = wndproc;
    return 0xc000u + (uint64_t)class_count++;   // the class atom
}
// WNDCLASSW and WNDCLASSEXW keep the window procedure at offset 8 and the
// class name at offset 64; the extended form only adds a size in front, where
// the plain form has padding, and a small icon at the end.
IMPORT(RegisterClassW) { RET(register_class((const uint16_t *)ld64(ARG(0) + 64), ld64(ARG(0) + 8))); }
IMPORT(RegisterClassExW) { RET(register_class((const uint16_t *)ld64(ARG(0) + 64), ld64(ARG(0) + 8))); }
IMPORT(UnregisterClassW) { RET(1); }

// ---------------------------------------------------------------- windows
static host_window_t windows[MAX_WINDOWS];

host_window_t *window_from_handle(uint64_t hwnd) {
    uint64_t i = (hwnd - HWND_BASE) / 0x10;
    return hwnd >= HWND_BASE && i < MAX_WINDOWS && windows[i].used ? &windows[i] : NULL;
}
static uint64_t handle_of(host_window_t *w) { return HWND_BASE + (uint64_t)(w - windows) * 0x10; }

host_window_t *window_from_sdl(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].sdl && SDL_GetWindowID(windows[i].sdl) == id) return &windows[i];
    return NULL;
}

void host_alert(const char *title, const char *text) {
    fprintf(stderr, "\n[%s]\n%s\n\n", title, text);
    fflush(stderr);
    if (pthread_main_np() && !getenv("SE_NO_DIALOGS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, NULL);
}
void host_fatal(int status, const char *format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    host_alert("SpaceEngine cannot continue", text);
    fflush(NULL);
    exit(status);
}

float host_scale(void) {
    static float scale;
    if (scale == 0.0f) {
        const char *retina = getenv("SE_RETINA");
        host_video_init();
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        scale = (retina && retina[0] == '0') || !mode || mode->pixel_density <= 0.0f ? 1.0f : mode->pixel_density;
    }
    return scale;
}
static int to_points(int pixels) { return (int)((float)pixels / host_scale() + 0.5f); }
static int to_pixels(float points) { return (int)(points * host_scale() + 0.5f); }

// macOS opens its accent picker when a letter key is held while a window
// takes text input, which the game's window always does; in a game, held keys
// are movement. Press-and-hold is turned off for this process only, through
// the argument domain of the app's defaults (as `-ApplePressAndHoldEnabled NO`
// on the command line would), before AppKit starts. Nothing is written to disk.
typedef id (*send_id)(id, SEL);
typedef id (*send_id_id)(id, SEL, id);
typedef id (*send_id_bool)(id, SEL, BOOL);
typedef void (*send_void_id)(id, SEL, id);
typedef void (*send_void_id_id)(id, SEL, id, id);
typedef id (*send_id_cstr)(id, SEL, const char *);
typedef BOOL (*send_bool_id)(id, SEL, id);
static void disable_press_and_hold(void) {
    Class defaults_class = objc_getClass("NSUserDefaults"), dict_class = objc_getClass("NSMutableDictionary");
    Class number_class = objc_getClass("NSNumber"), string_class = objc_getClass("NSString");
    if (!defaults_class || !dict_class || !number_class || !string_class) return;
    id defaults = ((send_id)objc_msgSend)((id)defaults_class, sel_registerName("standardUserDefaults"));
    id domain = ((send_id_cstr)objc_msgSend)((id)string_class, sel_registerName("stringWithUTF8String:"), "NSArgumentDomain");
    id key = ((send_id_cstr)objc_msgSend)((id)string_class, sel_registerName("stringWithUTF8String:"), "ApplePressAndHoldEnabled");
    id current = ((send_id_id)objc_msgSend)(defaults, sel_registerName("volatileDomainForName:"), domain);
    id updated = current ? ((send_id_id)objc_msgSend)((id)dict_class, sel_registerName("dictionaryWithDictionary:"), current)
                         : ((send_id)objc_msgSend)((id)dict_class, sel_registerName("dictionary"));
    id no = ((send_id_bool)objc_msgSend)((id)number_class, sel_registerName("numberWithBool:"), NO);
    ((send_void_id_id)objc_msgSend)(updated, sel_registerName("setObject:forKey:"), no, key);
    ((send_void_id)objc_msgSend)(defaults, sel_registerName("removeVolatileDomainForName:"), domain);
    ((send_void_id_id)objc_msgSend)(defaults, sel_registerName("setVolatileDomain:forName:"), updated, domain);
    if (getenv("SE_VERBOSE"))
        fprintf(stderr, "host: press-and-hold for this process: %s\n",
                ((send_bool_id)objc_msgSend)(defaults, sel_registerName("boolForKey:"), key) ? "on" : "off");
}

// Was the last scroll from a trackpad (or another touch surface)? AppKit says
// so on each scroll event (hasPreciseScrollingDeltas); SDL does not pass it
// on, so a local event monitor notes it before SDL sees the event.
static volatile int scroll_from_trackpad;
static void watch_scroll_source(void) {
    Class event_class = objc_getClass("NSEvent");
    if (!event_class) return;
    typedef id (*send_monitor)(id, SEL, unsigned long long, id);
    id (^handler)(id) = ^id(id event) {
        scroll_from_trackpad = ((BOOL (*)(id, SEL))objc_msgSend)(event, sel_registerName("hasPreciseScrollingDeltas"));
        return event;
    };
    const unsigned long long scroll_wheel_mask = 1ull << 22;   // NSEventMaskScrollWheel
    ((send_monitor)objc_msgSend)((id)event_class, sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"),
                                 scroll_wheel_mask, (id)handler);
}

// The game's per-frame mouse handling (0x1405b2620, wrapped through hle.txt)
// turns the wheel movement it has collected into camera speed and zoom; menus
// get the wheel elsewhere. With macOS's natural scrolling a trackpad swipe
// then works opposite to what it does on a wheel, so for trackpad scrolls the
// collected movement is reversed while this function looks at it, and put
// back if it was left unused. SE_TRACKPAD_SPEED=natural keeps it as macOS
// delivers it.
void f_1405b2620_original(cpu_t *c);
void f_1405b2620(cpu_t *c) {
    static int keep = -1;
    if (keep < 0) {
        const char *v = getenv("SE_TRACKPAD_SPEED");
        keep = v && !strcmp(v, "natural");
        if (getenv("SE_VERBOSE")) fprintf(stderr, "host: trackpad scrolling %s for camera speed\n", keep ? "as macOS delivers it" : "reversed");
    }
    float *wheel[2] = {(float *)(c->r[RCX] + 0x3a2c), (float *)(c->r[RCX] + 0x3a2c + 0x78)};
    int flip = scroll_from_trackpad && !keep;
    if (flip) for (int i = 0; i < 2; i++) *wheel[i] = -*wheel[i];
    f_1405b2620_original(c);
    if (flip) for (int i = 0; i < 2; i++) if (*wheel[i] != 0.0f) *wheel[i] = -*wheel[i];
}

void host_video_init(void) {
    static int done;
    if (done) return;
    done = 1;
    disable_press_and_hold();
    watch_scroll_source();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "host: SDL could not start video: %s\n", SDL_GetError());
        exit(72);
    }
}

// The game saves its window position and size in pixels. When the pixel
// density differs from the one they were saved under (first Retina run, a
// different display, SE_RETINA), the saved numbers are rescaled so the window
// comes back the same size on screen. Called before the game reads its
// settings.
void host_display_prepare(void) {
    static const char *const keys[] = {"WindowSizeX", "WindowSizeY", "WindowPosX", "WindowPosY"};
    float now = host_scale(), saved = 1.0f;
    char *cfg = host_path("C:/Users/player/Documents/Cosmographic/SpaceEngine/config/main-user.cfg", 0);
    char *mark = host_path("C:/Users/player/display-scale", 1);
    FILE *f = fopen(mark, "r");
    int recorded = f != NULL;
    if (f) { if (fscanf(f, "%f", &saved) != 1 || saved <= 0.0f) saved = 1.0f; fclose(f); }
    if (saved != now && (f = fopen(cfg, "rb"))) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);
        char *in = malloc((size_t)size + 1), *out = malloc((size_t)size + 256), *o = out;
        size = (long)fread(in, 1, (size_t)size, f);
        in[size] = 0;
        fclose(f);
        // Each setting is "<tab>Name<spaces>value<tabs>// comment" on its own line.
        for (char *line = in; *line; ) {
            char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
            const char *name = line + strspn(line, " \t");
            int done = 0;
            for (int k = 0; k < 4 && !done; k++) {
                size_t n = strlen(keys[k]);
                const char *value = name + n + strspn(name + n, " \t");
                if (strncmp(name, keys[k], n) != 0 || value == name + n) continue;
                char *rest;
                long v = strtol(value, &rest, 10);
                if (rest == value) continue;
                o += sprintf(o, "%.*s%ld", (int)(value - line), line, lroundf((float)v * now / saved));
                memcpy(o, rest, len - (size_t)(rest - line));
                o += len - (size_t)(rest - line);
                done = 1;
            }
            if (!done) { memcpy(o, line, len); o += len; }
            line += len;
        }
        if ((f = fopen(cfg, "wb"))) { fwrite(out, 1, (size_t)(o - out), f); fclose(f); }
        if (getenv("SE_VERBOSE")) fprintf(stderr, "host: saved window geometry rescaled from %gx to %gx\n", saved, now);
        free(in); free(out);
    }
    if ((saved != now || !recorded) && (f = fopen(mark, "w"))) { fprintf(f, "%g\n", now); fclose(f); }
    free(cfg); free(mark);
}

static uint64_t send(cpu_t *c, host_window_t *w, uint32_t msg, uint64_t wparam, uint64_t lparam) {
    if (!w || !w->wndproc) return 0;
    uint64_t args[4] = {handle_of(w), msg, wparam, lparam};
    return host_call_guest(c, w->wndproc, 4, args);
}

IMPORT(CreateWindowExW) {
    host_window_t *w = NULL;
    for (int i = 0; i < MAX_WINDOWS && !w; i++) if (!windows[i].used) w = &windows[i];
    if (!w) RET(0);
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->wndproc = class_proc(PTR(1));
    w->ex_style = (uint32_t)ARG(0);
    w->style = (uint32_t)ARG(3);
    w->x = (int32_t)ARG(4); w->y = (int32_t)ARG(5);
    w->width = (int32_t)ARG(6); w->height = (int32_t)ARG(7);
    if (w->x == (int32_t)0x80000000) w->x = 100;         // CW_USEDEFAULT
    if (w->y == (int32_t)0x80000000) w->y = 100;
    if (w->width == (int32_t)0x80000000 || w->width <= 0) w->width = 1280;
    if (w->height == (int32_t)0x80000000 || w->height <= 0) w->height = 720;
    char *title = ARG(2) ? u16_to_utf8(PTR(2), -1) : strdup("");
    snprintf(w->title, sizeof w->title, "%s", title);
    free(title);
    if (getenv("SE_VERBOSE")) fprintf(stderr, "host: window \"%s\" %dx%d created\n", w->title, w->width, w->height);

    // CREATESTRUCTW, as far as window procedures commonly read it.
    uint64_t cs[10] = {ARG(11), ARG(10), ARG(9), ARG(8),
                       (uint64_t)(uint32_t)w->height | (uint64_t)(uint32_t)w->width << 32,
                       (uint64_t)(uint32_t)w->y | (uint64_t)(uint32_t)w->x << 32,
                       w->style, ARG(2), ARG(1), w->ex_style};
    uint64_t hwnd = handle_of(w);
    send(c, w, WM_NCCREATE, 0, (uint64_t)cs);
    if ((int64_t)send(c, w, WM_CREATE, 0, (uint64_t)cs) == -1) { w->used = 0; RET(0); }
    RET(hwnd);
}
IMPORT(DestroyWindow) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    send(c, w, WM_DESTROY, 0, 0);
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->used = 0;
    RET(1);
}
IMPORT(DefWindowProcW) {
    host_window_t *w = window_from_handle(ARG(0));
    if (w && (uint32_t)ARG(1) == WM_CLOSE) {
        uint64_t hwnd = ARG(0);
        send(c, w, WM_DESTROY, 0, 0);
        if (w->sdl) SDL_DestroyWindow(w->sdl);
        w->used = 0;
        (void)hwnd;
    }
    RET(0);
}
// A window hears about its size when it is shown, not while it is still
// hidden: the game takes any size message as the user's choice and saves it.
static void sync_geometry(host_window_t *w);
void host_post_message(uint64_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam);
static void set_visible(host_window_t *w, int visible) {
    int was = w->visible;
    w->visible = visible;
    if (!w->sdl) return;
    if (!visible) { SDL_HideWindow(w->sdl); return; }
    SDL_ShowWindow(w->sdl);
    if (!was) {
        sync_geometry(w);
        host_post_message(handle_of(w), WM_SIZE, 0, ((uint64_t)(uint16_t)w->height << 16) | (uint16_t)w->width);
        // SE_KEEP_FOCUS (for measuring and testing) also treats the window as
        // focused from the start, in case macOS does not bring it forward.
        if (getenv("SE_KEEP_FOCUS")) {
            host_post_message(handle_of(w), WM_ACTIVATEAPP, 1, 0);
            host_post_message(handle_of(w), WM_ACTIVATE, 1, 0);
            host_post_message(handle_of(w), WM_SETFOCUS, 0, 0);
        }
    }
}
IMPORT(ShowWindow) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    int was = w->visible;
    set_visible(w, ARG(1) != 0);
    RET(was);
}
IMPORT(SetWindowPos) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    uint32_t flags = (uint32_t)ARG(6);
    if (!(flags & 2)) { w->x = (int32_t)ARG(2); w->y = (int32_t)ARG(3); if (w->sdl) SDL_SetWindowPosition(w->sdl, to_points(w->x), to_points(w->y)); }
    if (!(flags & 1)) { w->width = (int32_t)ARG(4); w->height = (int32_t)ARG(5); if (w->sdl) SDL_SetWindowSize(w->sdl, to_points(w->width), to_points(w->height)); }
    if (flags & 0x40) set_visible(w, 1);
    if (flags & 0x80) set_visible(w, 0);
    RET(1);
}
static void put_rect(uint64_t out, int l, int t, int r, int b) { st32(out, (uint32_t)l); st32(out + 4, (uint32_t)t); st32(out + 8, (uint32_t)r); st32(out + 12, (uint32_t)b); }
// The size the game is told is the real window's, once there is one: macOS
// may have placed or sized it differently from what was asked.
static void sync_geometry(host_window_t *w) {
    if (!w->sdl) return;
    int x = 0, y = 0;
    SDL_GetWindowSizeInPixels(w->sdl, &w->width, &w->height);
    SDL_GetWindowPosition(w->sdl, &x, &y);
    w->x = to_pixels((float)x); w->y = to_pixels((float)y);
}
IMPORT(GetClientRect) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    sync_geometry(w);
    put_rect(ARG(1), 0, 0, w->width, w->height);
    RET(1);
}
IMPORT(GetWindowRect) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    sync_geometry(w);
    put_rect(ARG(1), w->x, w->y, w->x + w->width, w->y + w->height);
    RET(1);
}
// Windows here have no border or title bar of their own: the client area is the window.
IMPORT(AdjustWindowRectExForDpi) { RET(1); }
IMPORT(ClientToScreen) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    st32(ARG(1), ld32(ARG(1)) + (uint32_t)w->x); st32(ARG(1) + 4, ld32(ARG(1) + 4) + (uint32_t)w->y);
    RET(1);
}
IMPORT(ScreenToClient) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    st32(ARG(1), ld32(ARG(1)) - (uint32_t)w->x); st32(ARG(1) + 4, ld32(ARG(1) + 4) - (uint32_t)w->y);
    RET(1);
}
static uint64_t *long_slot(host_window_t *w, int32_t index) {
    switch (index) {
        case -21: return &w->user_data;     // GWLP_USERDATA
        case -4: return &w->wndproc;        // GWLP_WNDPROC
        default: return NULL;
    }
}
static uint64_t window_long(cpu_t *c, int set) {
    host_window_t *w = window_from_handle(ARG(0));
    int32_t index = (int32_t)ARG(1);
    if (!w) return 0;
    uint64_t old, *slot = long_slot(w, index);
    if (slot) { old = *slot; if (set) *slot = ARG(2); }
    else if (index == -16) { old = w->style; if (set) w->style = (uint32_t)ARG(2); }      // GWL_STYLE
    else if (index == -20) { old = w->ex_style; if (set) w->ex_style = (uint32_t)ARG(2); } // GWL_EXSTYLE
    else old = 0;
    return old;
}
IMPORT(GetWindowLongPtrW) { RET(window_long(c, 0)); }
IMPORT(SetWindowLongPtrW) { RET(window_long(c, 1)); }
IMPORT(GetWindowLongW) { RET((uint32_t)window_long(c, 0)); }
IMPORT(SetWindowLongW) { RET((uint32_t)window_long(c, 1)); }
IMPORT(SetWindowTextW) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    char *title = u16_to_utf8(PTR(1), -1);
    snprintf(w->title, sizeof w->title, "%s", title);
    if (w->sdl) SDL_SetWindowTitle(w->sdl, title);
    free(title);
    RET(1);
}
IMPORT(SetForegroundWindow) { host_window_t *w = window_from_handle(ARG(0)); if (w && w->sdl) SDL_RaiseWindow(w->sdl); RET(1); }
IMPORT(BringWindowToTop) { host_window_t *w = window_from_handle(ARG(0)); if (w && w->sdl) SDL_RaiseWindow(w->sdl); RET(1); }
IMPORT(GetWindowThreadProcessId) { if (ARG(1)) st32(ARG(1), 1); RET(host_thread.id); }
IMPORT(GetWindowPlacement) {
    host_window_t *w = window_from_handle(ARG(0));
    if (!w) RET(0);
    st32(ARG(1) + 8, 1);                                   // SW_SHOWNORMAL
    put_rect(ARG(1) + 28, w->x, w->y, w->x + w->width, w->y + w->height);
    RET(1);
}
IMPORT(InvalidateRect) { RET(1); }
IMPORT(GetDC) { RET(ARG(0) ? ARG(0) | 1 : SCREEN_DC); }    // a window's device context is its handle, tagged
IMPORT(ReleaseDC) { RET(1); }
IMPORT(LoadIconW) { RET(0x20001); }
IMPORT(LoadCursorW) { RET(0x20002); }
IMPORT(SetCursor) { RET(0x20002); }
IMPORT(DestroyCursor) { RET(1); }
IMPORT(CreateIconIndirect) { RET(0x20003); }
IMPORT(EnableNonClientDpiScaling) { RET(1); }

// --------------------------------------------------------------- displays
static void display_size(int *w, int *h, float *hz) {
    host_video_init();
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    *w = mode ? to_pixels((float)mode->w) : 1920; *h = mode ? to_pixels((float)mode->h) : 1080;
    if (hz) *hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0f;
}
IMPORT(GetSystemMetrics) {
    int w, h;
    display_size(&w, &h, NULL);
    switch ((int32_t)ARG(0)) {
        case 0: case 16: case 78: RET(w);     // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
        case 1: case 17: case 79: RET(h);
        case 80: RET(1);                      // SM_CMONITORS
        default: RET(0);
    }
}
IMPORT(EnumDisplayMonitors) {
    // One monitor. The callback is the game's: (monitor, dc, rect, data).
    int w, h;
    display_size(&w, &h, NULL);
    int32_t *rect = malloc(16);
    rect[0] = 0; rect[1] = 0; rect[2] = w; rect[3] = h;
    uint64_t args[4] = {MONITOR_HANDLE, 0, (uint64_t)rect, ARG(3)};
    host_call_guest(c, ARG(2), 4, args);
    free(rect);
    RET(1);
}
IMPORT(MonitorFromWindow) { RET(MONITOR_HANDLE); }
IMPORT(GetMonitorInfoA) {
    int w, h;
    display_size(&w, &h, NULL);
    uint64_t p = ARG(1);
    uint32_t size = ld32(p);
    put_rect(p + 4, 0, 0, w, h);          // rcMonitor
    put_rect(p + 20, 0, 0, w, h);         // rcWork
    st32(p + 36, 1);                      // MONITORINFOF_PRIMARY
    if (size >= 72) snprintf((char *)(p + 40), 32, "\\\\.\\DISPLAY1");
    RET(1);
}
IMPORT(EnumDisplaySettingsA) {
    // DEVMODEA. Mode -1 is the current mode; 0.. enumerates. One mode is offered: the desktop's.
    int32_t index = (int32_t)ARG(1);
    if (index > 0) RET(0);
    int w, h; float hz;
    display_size(&w, &h, &hz);
    uint64_t p = ARG(2);
    st16(p + 36, 156);                    // dmSize
    st32(p + 40, 0x1c0000 | 0x400000);    // dmFields: bits per pixel, width, height, frequency
    st32(p + 104, 32); st32(p + 108, (uint32_t)w); st32(p + 112, (uint32_t)h); st32(p + 120, (uint32_t)(hz + 0.5f));
    RET(1);
}
IMPORT(ChangeDisplaySettingsExA) { RET(0); }   // DISP_CHANGE_SUCCESSFUL: the desktop mode is never changed

// ----------------------------------------------------------- message queue
typedef struct { uint64_t hwnd; uint32_t message; uint64_t wparam, lparam; } message_t;
enum { QUEUE = 512 };
static message_t queue[QUEUE];
static int queue_head, queue_count;
static int32_t mouse_x, mouse_y;
static uint8_t key_down[256];
void host_controller_event(const SDL_Event *e);   // sdl.c

void host_post_message(uint64_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
    if (queue_count == QUEUE) return;
    queue[(queue_head + queue_count++) % QUEUE] = (message_t){hwnd, message, wparam, lparam};
}

// Windows virtual-key code for an SDL scancode, or 0.
static uint32_t virtual_key(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (uint32_t)(s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (uint32_t)(s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (uint32_t)(s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (uint32_t)(s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_0: return '0';           case SDL_SCANCODE_RETURN: return 0x0d;
        case SDL_SCANCODE_ESCAPE: return 0x1b;     case SDL_SCANCODE_BACKSPACE: return 0x08;
        case SDL_SCANCODE_TAB: return 0x09;        case SDL_SCANCODE_SPACE: return 0x20;
        case SDL_SCANCODE_MINUS: return 0xbd;      case SDL_SCANCODE_EQUALS: return 0xbb;
        case SDL_SCANCODE_LEFTBRACKET: return 0xdb; case SDL_SCANCODE_RIGHTBRACKET: return 0xdd;
        case SDL_SCANCODE_BACKSLASH: return 0xdc;  case SDL_SCANCODE_SEMICOLON: return 0xba;
        case SDL_SCANCODE_APOSTROPHE: return 0xde; case SDL_SCANCODE_GRAVE: return 0xc0;
        case SDL_SCANCODE_COMMA: return 0xbc;      case SDL_SCANCODE_PERIOD: return 0xbe;
        case SDL_SCANCODE_SLASH: return 0xbf;      case SDL_SCANCODE_CAPSLOCK: return 0x14;
        case SDL_SCANCODE_PRINTSCREEN: return 0x2c; case SDL_SCANCODE_SCROLLLOCK: return 0x91;
        case SDL_SCANCODE_PAUSE: return 0x13;      case SDL_SCANCODE_INSERT: return 0x2d;
        case SDL_SCANCODE_HOME: return 0x24;       case SDL_SCANCODE_PAGEUP: return 0x21;
        case SDL_SCANCODE_DELETE: return 0x2e;     case SDL_SCANCODE_END: return 0x23;
        case SDL_SCANCODE_PAGEDOWN: return 0x22;   case SDL_SCANCODE_RIGHT: return 0x27;
        case SDL_SCANCODE_LEFT: return 0x25;       case SDL_SCANCODE_DOWN: return 0x28;
        case SDL_SCANCODE_UP: return 0x26;         case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
        case SDL_SCANCODE_KP_DIVIDE: return 0x6f;  case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
        case SDL_SCANCODE_KP_MINUS: return 0x6d;   case SDL_SCANCODE_KP_PLUS: return 0x6b;
        case SDL_SCANCODE_KP_ENTER: return 0x0d;   case SDL_SCANCODE_KP_0: return 0x60;
        case SDL_SCANCODE_KP_PERIOD: return 0x6e;  case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
        case SDL_SCANCODE_LGUI: return 0x5b;       case SDL_SCANCODE_RGUI: return 0x5c;
        default: return 0;
    }
}

// Shift, Ctrl and Alt also have a code per side, and the game asks for those
// (GetKeyState(VK_LSHIFT) and so on) to decide which shortcut a key press is.
static uint32_t side_key(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_LSHIFT: return 0xa0;     case SDL_SCANCODE_RSHIFT: return 0xa1;
        case SDL_SCANCODE_LCTRL: return 0xa2;      case SDL_SCANCODE_RCTRL: return 0xa3;
        case SDL_SCANCODE_LALT: return 0xa4;       case SDL_SCANCODE_RALT: return 0xa5;
        default: return 0;
    }
}

// The character message Windows' TranslateMessage adds for keys that are not
// text: Backspace, Tab, Enter, Escape, and Ctrl with a letter. The game's text
// fields edit on these. Printable characters come from text-input events.
static uint32_t control_character(uint32_t vk) {
    if (vk == 0x08 || vk == 0x09 || vk == 0x0d || vk == 0x1b) return vk;
    if (vk >= 'A' && vk <= 'Z' && key_down[0x11] && !key_down[0x12]) return vk - 0x40;
    return 0;
}

static uint64_t mouse_buttons_and_modifiers(void) {
    return (key_down[1] ? 1u : 0) | (key_down[2] ? 2u : 0) | (key_down[4] ? 0x10u : 0) |
           (key_down[0x10] ? 4u : 0) | (key_down[0x11] ? 8u : 0);
}
static uint64_t point(int32_t x, int32_t y) { return (uint64_t)(uint16_t)x | (uint64_t)(uint16_t)y << 16; }

static void pump(void) {
    host_video_init();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        host_window_t *w = NULL;
        switch (e.type) {
            case SDL_EVENT_QUIT:
                for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].used && windows[i].sdl) host_post_message(handle_of(&windows[i]), WM_CLOSE, 0, 0);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if ((w = window_from_sdl(e.window.windowID))) host_post_message(handle_of(w), WM_CLOSE, 0, 0);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if ((w = window_from_sdl(e.window.windowID))) {
                    w->width = e.window.data1; w->height = e.window.data2;
                    if (w->visible) host_post_message(handle_of(w), WM_SIZE, 0, point(w->width, w->height));
                }
                break;
            case SDL_EVENT_WINDOW_MOVED:
                if ((w = window_from_sdl(e.window.windowID))) { w->x = to_pixels((float)e.window.data1); w->y = to_pixels((float)e.window.data2); }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if ((w = window_from_sdl(e.window.windowID))) {
                    host_post_message(handle_of(w), WM_ACTIVATEAPP, 1, 0);
                    host_post_message(handle_of(w), WM_ACTIVATE, 1, 0);
                    host_post_message(handle_of(w), WM_SETFOCUS, 0, 0);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // The game drops to PowerSaveMaxFPS (4) in the background. SE_KEEP_FOCUS
                // keeps it running flat out there, for measuring.
                if (getenv("SE_KEEP_FOCUS")) break;
                if ((w = window_from_sdl(e.window.windowID))) {
                    memset(key_down, 0, sizeof key_down);
                    host_post_message(handle_of(w), WM_KILLFOCUS, 0, 0);
                    host_post_message(handle_of(w), WM_ACTIVATE, 0, 0);
                    host_post_message(handle_of(w), WM_ACTIVATEAPP, 0, 0);
                }
                break;
            case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                uint32_t vk = virtual_key(e.key.scancode);
                if (!vk || !(w = window_from_sdl(e.key.windowID))) break;
                int down = e.type == SDL_EVENT_KEY_DOWN, alt = key_down[0x12] && vk != 0x12;
                uint64_t lparam = 1 | (uint64_t)(e.key.scancode & 0xff) << 16 | (e.key.repeat ? 1ull << 30 : 0) |
                                  (down ? 0 : 3ull << 30) | (alt ? 1ull << 29 : 0);
                uint32_t side = side_key(e.key.scancode);
                if (side) {
                    key_down[side] = (uint8_t)down;
                    key_down[vk] = key_down[side & ~1u] || key_down[side | 1u];
                } else key_down[vk] = (uint8_t)down;
                host_post_message(handle_of(w), down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP), vk, lparam);
                if (down && !alt && control_character(vk)) host_post_message(handle_of(w), WM_CHAR, control_character(vk), lparam);
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
                if ((w = window_from_sdl(e.text.windowID))) {
                    uint16_t wide[64];
                    size_t n = utf8_to_u16(e.text.text, wide, 64) - 1;
                    for (size_t i = 0; i < n && i < 63; i++) host_post_message(handle_of(w), WM_CHAR, wide[i], 1);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if ((w = window_from_sdl(e.motion.windowID))) {
                    mouse_x = to_pixels(e.motion.x); mouse_y = to_pixels(e.motion.y);
                    host_post_message(handle_of(w), WM_MOUSEMOVE, mouse_buttons_and_modifiers(), point(mouse_x, mouse_y));
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (!(w = window_from_sdl(e.button.windowID))) break;
                int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                uint32_t vk = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
                if (!vk) break;
                key_down[vk] = (uint8_t)down;
                uint32_t msg = vk == 1 ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP) : vk == 2 ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
                mouse_x = to_pixels(e.button.x); mouse_y = to_pixels(e.button.y);
                host_post_message(handle_of(w), msg, mouse_buttons_and_modifiers(), point(mouse_x, mouse_y));
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if ((w = window_from_sdl(e.wheel.windowID)))
                    host_post_message(handle_of(w), WM_MOUSEWHEEL, mouse_buttons_and_modifiers() | (uint64_t)(uint16_t)(int16_t)(e.wheel.y * 120) << 16,
                                      point(w->x + mouse_x, w->y + mouse_y));
                break;
            default: host_controller_event(&e); break;
        }
    }
}

// MSG: window, message, wParam, lParam, time, cursor position.
static int take(uint64_t out, int remove) {
    if (!queue_count) return 0;
    message_t m = queue[queue_head];
    if (remove) { queue_head = (queue_head + 1) % QUEUE; queue_count--; }
    memset((void *)out, 0, 48);
    st64(out, m.hwnd); st32(out + 8, m.message); st64(out + 16, m.wparam); st64(out + 24, m.lparam);
    st32(out + 32, (uint32_t)SDL_GetTicks()); st32(out + 36, (uint32_t)mouse_x); st32(out + 40, (uint32_t)mouse_y);
    return 1;
}
IMPORT(PeekMessageW) { pump(); RET(take(ARG(0), (ARG(4) & 1) != 0)); }
IMPORT(WaitMessage) { pump(); if (!queue_count) SDL_WaitEventTimeout(NULL, 50); RET(1); }
IMPORT(TranslateMessage) { RET(0); }      // character messages come straight from text-input events
IMPORT(DispatchMessageW) {
    host_window_t *w = window_from_handle(ld64(ARG(0)));
    RET(send(c, w, ld32(ARG(0) + 8), ld64(ARG(0) + 16), ld64(ARG(0) + 24)));
}
IMPORT(SendMessageW) { RET(send(c, window_from_handle(ARG(0)), (uint32_t)ARG(1), ARG(2), ARG(3))); }
IMPORT(PostQuitMessage) { host_post_message(0, WM_QUIT, ARG(0), 0); }

// ------------------------------------------------------------------ input
IMPORT(GetKeyState) { uint32_t vk = (uint32_t)ARG(0) & 0xff; RET(key_down[vk] ? 0xffff8000u : 0); }
IMPORT(GetCursorPos) {
    host_video_init();
    float x, y;
    SDL_GetGlobalMouseState(&x, &y);
    st32(ARG(0), (uint32_t)to_pixels(x)); st32(ARG(0) + 4, (uint32_t)to_pixels(y));
    RET(1);
}
IMPORT(SetCursorPos) { SDL_WarpMouseGlobal((float)(int32_t)ARG(0) / host_scale(), (float)(int32_t)ARG(1) / host_scale()); RET(1); }

// -------------------------------------------------------------- clipboard
// Text only, which is all the game uses: CF_UNICODETEXT to and from the macOS
// pasteboard. The handles are plain memory (GlobalAlloc in kernel32.c).
enum { CF_UNICODETEXT = 13 };
IMPORT(OpenClipboard) { RET(1); }
IMPORT(CloseClipboard) { RET(1); }
IMPORT(EmptyClipboard) { RET(1); }
IMPORT(IsClipboardFormatAvailable) { host_video_init(); RET(ARG(0) == CF_UNICODETEXT && SDL_HasClipboardText()); }
IMPORT(GetClipboardData) {
    static uint16_t *held;      // stays valid until the next request, as the system's copy does on Windows
    if (ARG(0) != CF_UNICODETEXT) RET(0);
    host_video_init();
    char *text = SDL_GetClipboardText();
    if (!text) RET(0);
    size_t cap = strlen(text) + 1;
    free(held);
    held = calloc(cap, sizeof *held);
    utf8_to_u16(text, held, cap);
    SDL_free(text);
    RET(held);
}
IMPORT(SetClipboardData) {
    if (ARG(0) != CF_UNICODETEXT || !ARG(1)) RET(0);
    host_video_init();
    char *text = u16_to_utf8(PTR(1), -1);
    int ok = SDL_SetClipboardText(text);
    free(text);
    RET(ok ? ARG(1) : 0);
}
