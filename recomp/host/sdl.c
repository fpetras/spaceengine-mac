// The game's SDL2 imports other than audio (see audio.c): game controllers
// and loading optional system libraries by name.
//
// The host itself uses SDL3, whose joystick and gamepad functions are the
// same in substance: these translate. Two differences matter to the game:
//
// - SDL2 numbers each opened joystick 0, 1, 2... ("instance ID") and the game
//   uses that number as an index into its own table. SDL3's IDs are
//   arbitrary, so each joystick the game opens gets the next SDL2 number.
// - SDL2 events are 56-byte unions the game reads by offset. The window layer
//   (window.c) receives every SDL3 event and hands the joystick and gamepad
//   ones to host_controller_event(), which queues them in SDL2 form.
//
// SE_VIRTUAL_GAMEPAD=1 attaches a virtual SDL3 gamepad at start, and
// SE_GAMEPAD_PRESS="<seconds>:<button>;..." presses its buttons (SDL gamepad
// button numbers), for testing without a controller.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

// --------------------------------------------------------------- SDL2 core
enum { SDL2_INIT_JOYSTICK = 0x200, SDL2_INIT_GAMECONTROLLER = 0x2000 };
static int controllers_ready;

static void attach_virtual_gamepad(void);

IMPORT(SDL_Init) {
    if ((ARG(0) & (SDL2_INIT_JOYSTICK | SDL2_INIT_GAMECONTROLLER)) && !controllers_ready) {
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            controllers_ready = 1;
            if (getenv("SE_VIRTUAL_GAMEPAD")) attach_virtual_gamepad();
            SDL_UpdateJoysticks();
        } else fprintf(stderr, "host: SDL could not start game controllers: %s\n", SDL_GetError());
    }
    RET(0);
}
IMPORT(SDL_Quit) { (void)c; }
IMPORT(SDL_GetError) { extern const char *host_audio_error(void); RET(host_audio_error()); }
// The game looks up optional Windows DLLs (per-monitor DPI functions) this
// way and works without them.
IMPORT(SDL_LoadObject) { RET(0); }
IMPORT(SDL_LoadFunction) { RET(0); }

// ----------------------------------------------------------- controllers
enum { MAX_STICKS = 16 };
static struct { SDL_JoystickID id; SDL_Joystick *joystick; SDL_Gamepad *gamepad; } opened[MAX_STICKS];
static int opened_count;
static SDL_JoystickID devices[MAX_STICKS];   // SDL2 device index -> SDL3 ID, as of the last count
static int device_count;

static int sdl2_number(SDL_JoystickID id) {
    for (int i = 0; i < opened_count; i++) if (opened[i].id == id) return i;
    return -1;
}
static int slot_for(SDL_JoystickID id) {
    int n = sdl2_number(id);
    if (n < 0 && opened_count < MAX_STICKS) { n = opened_count++; opened[n].id = id; }
    return n;
}
static SDL_JoystickID device(uint64_t index) { return index < (uint64_t)device_count ? devices[index] : 0; }

IMPORT(SDL_NumJoysticks) {
    device_count = 0;
    if (!controllers_ready) RET(0);
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n && device_count < MAX_STICKS; i++) devices[device_count++] = ids[i];
    SDL_free(ids);
    RET(device_count);
}
IMPORT(SDL_JoystickOpen) {
    SDL_JoystickID id = device(ARG(0));
    SDL_Joystick *j = id ? SDL_OpenJoystick(id) : NULL;
    int n = j ? slot_for(id) : -1;
    if (n >= 0) opened[n].joystick = j;
    RET(n >= 0 ? j : NULL);
}
IMPORT(SDL_JoystickClose) {
    SDL_Joystick *j = PTR(0);
    for (int i = 0; i < opened_count; i++) if (opened[i].joystick == j) opened[i].joystick = NULL;
    if (j) SDL_CloseJoystick(j);
}
IMPORT(SDL_JoystickInstanceID) {
    SDL_Joystick *j = PTR(0);
    for (int i = 0; i < opened_count; i++) if (j && opened[i].joystick == j) RET(i);
    RET((uint64_t)(int64_t)-1);
}
IMPORT(SDL_JoystickName) { RET(PTR(0) ? SDL_GetJoystickName(PTR(0)) : NULL); }
IMPORT(SDL_JoystickNumAxes) { RET((uint64_t)(int64_t)SDL_GetNumJoystickAxes(PTR(0))); }
IMPORT(SDL_JoystickNumHats) { RET((uint64_t)(int64_t)SDL_GetNumJoystickHats(PTR(0))); }
IMPORT(SDL_JoystickNumButtons) { RET((uint64_t)(int64_t)SDL_GetNumJoystickButtons(PTR(0))); }
IMPORT(SDL_JoystickNumBalls) { RET((uint64_t)(int64_t)SDL_GetNumJoystickBalls(PTR(0))); }
IMPORT(SDL_JoystickGetAxis) { RET((uint64_t)(int64_t)SDL_GetJoystickAxis(PTR(0), (int)ARG(1))); }
IMPORT(SDL_IsGameController) { SDL_JoystickID id = device(ARG(0)); RET(id && SDL_IsGamepad(id)); }
IMPORT(SDL_GameControllerOpen) {
    SDL_JoystickID id = device(ARG(0));
    SDL_Gamepad *g = id ? SDL_OpenGamepad(id) : NULL;
    int n = g ? slot_for(id) : -1;
    if (n >= 0) opened[n].gamepad = g;
    RET(n >= 0 ? g : NULL);
}
IMPORT(SDL_GameControllerClose) {
    SDL_Gamepad *g = PTR(0);
    for (int i = 0; i < opened_count; i++) if (opened[i].gamepad == g) opened[i].gamepad = NULL;
    if (g) SDL_CloseGamepad(g);
}
IMPORT(SDL_GameControllerName) { RET(PTR(0) ? SDL_GetGamepadName(PTR(0)) : NULL); }
// Axes and buttons up to MISC1 are numbered the same in SDL2 and SDL3.
IMPORT(SDL_GameControllerGetAxis) { RET((uint64_t)(int64_t)SDL_GetGamepadAxis(PTR(0), (SDL_GamepadAxis)(int)ARG(1))); }

// --------------------------------------------------------------- events
// SDL2's event union, as far as the game reads it: type, then (for joystick
// and controller events) which at 8, the axis/hat/button number at 12, the
// hat value or button state at 13, and the axis value at 16.
enum { QUEUE = 256 };
static uint8_t queue[QUEUE][56];
static int head, count;

static void enqueue(uint32_t type, int32_t which, uint8_t number, uint8_t state, int16_t value) {
    if (count == QUEUE) return;
    uint8_t *e = queue[(head + count++) % QUEUE];
    memset(e, 0, 56);
    memcpy(e, &type, 4);
    uint32_t now = (uint32_t)SDL_GetTicks();
    memcpy(e + 4, &now, 4);
    memcpy(e + 8, &which, 4);
    e[12] = number;
    e[13] = state;
    memcpy(e + 16, &value, 2);
}

void host_controller_event(const SDL_Event *e) {
    // SDL3's event numbers for these are SDL2's.
    switch (e->type) {
        case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
            int n = sdl2_number(e->jaxis.which);
            if (n >= 0 && opened[n].joystick) enqueue(e->type, n, e->jaxis.axis, 0, e->jaxis.value);
            break;
        }
        case SDL_EVENT_JOYSTICK_HAT_MOTION: {
            int n = sdl2_number(e->jhat.which);
            if (n >= 0 && opened[n].joystick) enqueue(e->type, n, e->jhat.hat, e->jhat.value, 0);
            break;
        }
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP: {
            int n = sdl2_number(e->jbutton.which);
            if (n >= 0 && opened[n].joystick) enqueue(e->type, n, e->jbutton.button, e->jbutton.down, 0);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            int n = sdl2_number(e->gaxis.which);
            if (n >= 0 && opened[n].gamepad) enqueue(e->type, n, e->gaxis.axis, 0, e->gaxis.value);
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            int n = sdl2_number(e->gbutton.which);
            if (n >= 0 && opened[n].gamepad) enqueue(e->type, n, e->gbutton.button, e->gbutton.down, 0);
            break;
        }
        default: break;
    }
}

static void press_virtual_buttons(void);

IMPORT(SDL_PollEvent) {
    press_virtual_buttons();
    if (!count) RET(0);
    if (ARG(0)) memcpy(PTR(0), queue[head], 56);
    if (getenv("SE_VERBOSE")) {
        uint32_t type; int32_t which; int16_t value;
        memcpy(&type, queue[head], 4); memcpy(&which, queue[head] + 8, 4); memcpy(&value, queue[head] + 16, 2);
        fprintf(stderr, "host: controller event %#x for controller %d: %u/%u/%d\n", type, which, queue[head][12], queue[head][13], value);
    }
    head = (head + 1) % QUEUE;
    count--;
    RET(1);
}

// ------------------------------------------------------ testing without one
static SDL_JoystickID virtual_id;
static SDL_Joystick *virtual_stick;

static void attach_virtual_gamepad(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Virtual test gamepad";
    virtual_id = SDL_AttachVirtualJoystick(&desc);
    if (!virtual_id) fprintf(stderr, "host: no virtual gamepad: %s\n", SDL_GetError());
    else virtual_stick = SDL_OpenJoystick(virtual_id);   // the host's own handle, to drive it
}

// SE_GAMEPAD_PRESS="<seconds>:<button>;..." holds each button down for a
// third of a second at that many seconds after the controllers started.
static void press_virtual_buttons(void) {
    static uint64_t start;
    static long done;                 // bit n: entry n pressed; bit n+32: released
    if (!virtual_stick) return;
    if (!start) start = SDL_GetTicks();
    double t = (double)(SDL_GetTicks() - start) / 1000.0;
    const char *p = getenv("SE_GAMEPAD_PRESS");
    for (int n = 0; p && *p && n < 32; n++) {
        double at = strtod(p, (char **)&p);
        int button = *p == ':' ? (int)strtol(p + 1, (char **)&p, 10) : -1;
        if (button >= 0 && t >= at && !(done >> n & 1)) {
            done |= 1L << n;
            SDL_SetJoystickVirtualButton(virtual_stick, button, true);
            fprintf(stderr, "host: virtual gamepad button %d pressed at %.1f s\n", button, t);
        }
        if (button >= 0 && t >= at + 0.33 && !(done >> (n + 32) & 1)) {
            done |= 1L << (n + 32);
            SDL_SetJoystickVirtualButton(virtual_stick, button, false);
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
}
