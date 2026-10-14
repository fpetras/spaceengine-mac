#pragma once
#include <stdint.h>

struct SDL_Window;
enum { MAX_WINDOWS = 8 };
#define HWND_BASE 0x10000ull
#define SCREEN_DC 0x1ull
#define MONITOR_HANDLE 0x30001ull

typedef struct host_window {
    int used, visible, pixel_format_set;
    struct SDL_Window *sdl;     // null until the window gets an OpenGL context
    uint64_t wndproc, user_data;
    uint32_t style, ex_style;
    int32_t x, y, width, height;
    char title[128];
} host_window_t;

// Device pixels per macOS point: 2 on a Retina display. The game works in
// pixels throughout, as it does on Windows; SDL positions and sizes windows in
// points. SE_RETINA=0 makes the game render at one pixel per point instead.
float host_scale(void);
void host_display_prepare(void);
host_window_t *window_from_handle(uint64_t hwnd);
host_window_t *window_from_sdl(uint32_t id);
void host_video_init(void);
void host_post_message(uint64_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam);
