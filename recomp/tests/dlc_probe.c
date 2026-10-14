// Asks Steam, through the interface version the game uses, which SpaceEngine
// DLCs this account owns and which Steam counts as installed.
// While it runs, Steam shows the account as playing SpaceEngine.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_LIB "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/" \
                    "Steam Helper.app/Contents/MacOS/libsteam_api.dylib"

int main(void) {
    char path[2048];
    snprintf(path, sizeof path, "%s%s", getenv("HOME"), DEFAULT_LIB);
    void *lib = dlopen(path, RTLD_NOW);
    if (!lib) return 2;
    setenv("SteamAppId", "314650", 1);
    setenv("SteamGameId", "314650", 1);
    char message[1024] = "";
    if (((int (*)(char *))dlsym(lib, "SteamAPI_InitFlat"))(message) != 0) { fprintf(stderr, "no Steam: %s\n", message); return 1; }
    void *(*find)(int32_t, const char *) = dlsym(lib, "SteamInternal_FindOrCreateUserInterface");
    int32_t user = ((int32_t (*)(void))dlsym(lib, "SteamAPI_GetHSteamUser"))();
    void *apps = find(user, "STEAMAPPS_INTERFACE_VERSION008");
    void **a = *(void ***)apps;
    int count = ((int (*)(void *))a[10])(apps);
    for (int i = 0; i < count; i++) {
        uint32_t id = 0; bool available = false; char name[256] = "";
        ((bool (*)(void *, int, uint32_t *, bool *, char *, int))a[11])(apps, i, &id, &available, name, sizeof name);
        bool owned = ((bool (*)(void *, uint32_t))a[6])(apps, id);
        bool installed = ((bool (*)(void *, uint32_t))a[7])(apps, id);
        printf("%-8u owned %-3s installed %-3s %s\n", id, owned ? "yes" : "no", installed ? "yes" : "no", name);
    }
    ((void (*)(void))dlsym(lib, "SteamAPI_Shutdown"))();
    return 0;
}
