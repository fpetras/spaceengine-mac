// Asks the running Steam client, through Valve's own library, whether it will
// initialise SpaceEngine (app 314650) for a native macOS process, and whether
// it hands out the interface versions the game requests.
//
// While this runs, Steam shows the account as playing SpaceEngine.
//
// usage: steam_probe [path to libsteam_api.dylib]
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define APP_ID 314650u
#define DEFAULT_LIB "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/" \
                    "Steam Helper.app/Contents/MacOS/libsteam_api.dylib"

static void *lib;
static void *sym(const char *name) {
    void *p = dlsym(lib, name);
    if (!p) { fprintf(stderr, "missing export %s\n", name); exit(2); }
    return p;
}

int main(int argc, char **argv) {
    char path[2048];
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else snprintf(path, sizeof path, "%s%s", getenv("HOME"), DEFAULT_LIB);
    lib = dlopen(path, RTLD_NOW);
    if (!lib) { fprintf(stderr, "cannot load %s: %s\n", path, dlerror()); return 2; }

    // Without an app id in the environment the library asks Steam to relaunch
    // the title, which Steam cannot do for a title with no Mac build.
    setenv("SteamAppId", "314650", 1);
    setenv("SteamGameId", "314650", 1);

    bool (*is_running)(void) = sym("SteamAPI_IsSteamRunning");
    bool (*restart)(uint32_t) = sym("SteamAPI_RestartAppIfNecessary");
    int (*init)(char *) = sym("SteamAPI_InitFlat");
    void (*shutdown_api)(void) = sym("SteamAPI_Shutdown");
    int32_t (*get_user)(void) = sym("SteamAPI_GetHSteamUser");
    void *(*find_interface)(int32_t, const char *) = sym("SteamInternal_FindOrCreateUserInterface");
    void *(*apps_v009)(void) = sym("SteamAPI_SteamApps_v009");
    bool (*is_subscribed)(void *) = sym("SteamAPI_ISteamApps_BIsSubscribed");
    bool (*is_subscribed_app)(void *, uint32_t) = sym("SteamAPI_ISteamApps_BIsSubscribedApp");
    void *(*user_v023)(void) = sym("SteamAPI_SteamUser_v023");
    bool (*logged_on)(void *) = sym("SteamAPI_ISteamUser_BLoggedOn");

    printf("Steam client running:          %s\n", is_running() ? "yes" : "no");
    printf("RestartAppIfNecessary(%u): %s\n", APP_ID, restart(APP_ID) ? "wants a relaunch through Steam" : "no relaunch needed");

    char message[1024] = "";
    int result = init(message);
    static const char *names[] = {"OK", "FailedGeneric", "NoSteamClient", "VersionMismatch"};
    printf("SteamAPI_Init:                 %s%s%s\n", result >= 0 && result < 4 ? names[result] : "?",
           message[0] ? " - " : "", message);
    if (result != 0) return 1;

    printf("logged on:                     %s\n", logged_on(user_v023()) ? "yes" : "no");
    printf("subscribed to the running app: %s\n", is_subscribed(apps_v009()) ? "yes" : "no");
    printf("subscribed to app %u:      %s\n", APP_ID, is_subscribed_app(apps_v009(), APP_ID) ? "yes" : "no");

    // The versions the game itself asks for.
    static const char *wanted[] = {"SteamUser021", "SteamFriends017", "SteamUtils010",
                                   "STEAMAPPS_INTERFACE_VERSION008", "STEAMUGC_INTERFACE_VERSION014"};
    for (unsigned i = 0; i < sizeof wanted / sizeof wanted[0]; i++)
        printf("interface %-31s %s\n", wanted[i], find_interface(get_user(), wanted[i]) ? "available" : "NOT available");

    shutdown_api();
    return 0;
}
