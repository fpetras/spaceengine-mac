// Checks the macOS layout of SteamUGCDetails_t that host/steam.c converts:
// asks the running Steam client, through the same STEAMUGC_INTERFACE_VERSION014
// object the game uses, for the top-rated SpaceEngine Workshop item and prints
// its fields. Readable text and a plausible Steam ID mean the offsets are right.
// Also prints what GetAppInstallDir and GetItemInstallInfo return natively.
//
// It reads only public Workshop listings. While it runs, Steam shows the
// account as playing SpaceEngine.
//
// usage: ugc_probe [path to libsteam_api.dylib]
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_LIB "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/" \
                    "Steam Helper.app/Contents/MacOS/libsteam_api.dylib"

#pragma pack(push, 4)
typedef struct {
    uint64_t id; int32_t result, file_type; uint32_t creator_app, consumer_app;
    char title[129], description[8000]; uint64_t owner;
    uint32_t created, updated, added_to_user_list; int32_t visibility;
    bool banned, accepted_for_use, tags_truncated; char tags[1025];
    uint64_t file, preview_file; char file_name[260]; int32_t file_size, preview_file_size;
    char url[256]; uint32_t votes_up, votes_down; float score; uint32_t num_children;
} details_t;
#pragma pack(pop)

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
    if (!(lib = dlopen(path, RTLD_NOW))) { fprintf(stderr, "cannot load %s: %s\n", path, dlerror()); return 2; }
    setenv("SteamAppId", "314650", 1);
    setenv("SteamGameId", "314650", 1);
    int (*init)(char *) = sym("SteamAPI_InitFlat");
    char message[1024] = "";
    if (init(message) != 0) { fprintf(stderr, "Steam did not initialise: %s\n", message); return 1; }
    void (*run_callbacks)(void) = sym("SteamAPI_RunCallbacks");
    int32_t (*get_user)(void) = sym("SteamAPI_GetHSteamUser");
    void *(*find)(int32_t, const char *) = sym("SteamInternal_FindOrCreateUserInterface");
    void *ugc = find(get_user(), "STEAMUGC_INTERFACE_VERSION014");
    void *utils = find(get_user(), "SteamUtils010");
    void *apps = find(get_user(), "STEAMAPPS_INTERFACE_VERSION008");
    void **u = *(void ***)ugc, **t = *(void ***)utils, **a = *(void ***)apps;

    char dir[4096] = "";
    uint32_t n = ((uint32_t (*)(void *, uint32_t, char *, uint32_t))a[18])(apps, 314650, dir, sizeof dir);
    printf("GetAppInstallDir(314650) = %u \"%s\"\n", n, dir);

    // Slot 1 natively is CreateQueryAllUGCRequest(query, matching, creator, consumer, page).
    uint64_t query = ((uint64_t (*)(void *, int, int, uint32_t, uint32_t, uint32_t))u[1])(ugc, 0, 0, 314650, 314650, 1);
    uint64_t call = ((uint64_t (*)(void *, uint64_t))u[4])(ugc, query);
    bool failed = false;
    int waited = 0;
    while (!((bool (*)(void *, uint64_t, bool *))t[11])(utils, call, &failed) && waited++ < 300) { run_callbacks(); usleep(50000); }
    printf("query %s after %.1f s%s\n", waited > 300 ? "timed out" : "answered", waited * 0.05, failed ? " (failed)" : "");
    details_t d = {0};
    bool ok = ((bool (*)(void *, uint64_t, uint32_t, details_t *))u[5])(ugc, query, 0, &d);
    printf("GetQueryUGCResult = %d\n  id %llu, result %d, type %d, apps %u/%u\n  title \"%.60s\"\n  owner %#llx\n"
           "  created %u updated %u visibility %d banned %d accepted %d\n  tags \"%.60s\"\n  file %#llx preview %#llx\n"
           "  file name \"%.60s\" size %d preview size %d\n  url \"%.60s\"\n  votes %u up %u down, score %.3f, children %u\n",
           ok, (unsigned long long)d.id, d.result, d.file_type, d.creator_app, d.consumer_app, d.title,
           (unsigned long long)d.owner, d.created, d.updated, d.visibility, d.banned, d.accepted_for_use, d.tags,
           (unsigned long long)d.file, (unsigned long long)d.preview_file, d.file_name, d.file_size, d.preview_file_size,
           d.url, d.votes_up, d.votes_down, d.score, d.num_children);
    uint64_t size = 0; uint32_t stamp = 0;
    bool installed = ((bool (*)(void *, uint64_t, uint64_t *, char *, uint32_t, uint32_t *))u[65])(ugc, d.id, &size, dir, sizeof dir, &stamp);
    printf("GetItemInstallInfo(%llu) = %d \"%s\"\n", (unsigned long long)d.id, installed, installed ? dir : "");
    ((void (*)(void *, uint64_t))u[15])(ugc, query);
    ((void (*)(void))sym("SteamAPI_Shutdown"))();
    return 0;
}
