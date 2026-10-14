// Steam, through Valve's own native library and the running Steam client.
//
// The game imports ten flat functions and uses six interfaces. The flat
// functions are forwarded. Interfaces are C++ objects whose methods the game
// calls through vtables with the Windows calling convention, so each native
// interface is wrapped in a proxy: an object whose vtable entries are marker
// addresses that recomp_call hands back to this file.
//
// A method is forwarded only once its signature has been checked and entered
// in the table below. An unlisted method stops the program with its interface
// and slot number, the same way an unimplemented import does.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "paths.h"

#define STEAM_APP_ID "314650"
#define DEFAULT_LIBRARY "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/" \
                        "Steam Helper.app/Contents/MacOS/libsteam_api.dylib"

static void *library;
static int verbose;

static void *native(const char *name) {
    if (!library) {
        char path[2048];
        const char *override = getenv("SE_STEAM_API");
        if (override) snprintf(path, sizeof path, "%s", override);
        else snprintf(path, sizeof path, "%s%s", getenv("HOME"), DEFAULT_LIBRARY);
        // The library relaunches through Steam unless told which app this is.
        setenv("SteamAppId", STEAM_APP_ID, 1);
        setenv("SteamGameId", STEAM_APP_ID, 1);
        library = dlopen(path, RTLD_NOW);
        verbose = getenv("SE_VERBOSE") != NULL;
        if (!library) host_fatal(71, "Steam's library could not be loaded. Is Steam installed?\n\n%s\n%s", path, dlerror());
    }
    void *p = dlsym(library, name);
    if (!p) { fprintf(stderr, "host: the Steam library has no %s\n", name); exit(71); }
    return p;
}

// ---------------------------------------------------------------- proxies
// Marker "addresses": 0x7e57 in the top 16 bits can never be real code.
#define MARK 0x7e57000000000000ull
enum { MAX_SLOTS = 160, MAX_PROXIES = 32 };
typedef struct proxy {
    uint64_t vtable;            // first field: what the game dereferences
    void *object;               // the native interface
    char version[48];
    uint64_t slots[MAX_SLOTS];
} proxy_t;
static proxy_t *proxies[MAX_PROXIES];
static int proxy_count;

static proxy_t *wrap(void *object, const char *version) {
    if (!object) return NULL;
    for (int i = 0; i < proxy_count; i++)
        if (proxies[i]->object == object && !strcmp(proxies[i]->version, version)) return proxies[i];
    if (proxy_count == MAX_PROXIES) { fprintf(stderr, "host: too many Steam interfaces\n"); exit(71); }
    proxy_t *p = calloc(1, sizeof *p);
    p->object = object;
    snprintf(p->version, sizeof p->version, "%s", version);
    for (int s = 0; s < MAX_SLOTS; s++) p->slots[s] = MARK | (uint64_t)proxy_count << 16 | (uint64_t)s;
    p->vtable = (uint64_t)p->slots;
    proxies[proxy_count++] = p;
    if (verbose) fprintf(stderr, "host: Steam interface %s bridged\n", version);
    return p;
}

// How each bridged method is handled. Slot numbers and signatures are checked
// against Valve's published interface definitions for the exact version the
// game requests. None of these interfaces overloads a method, so the Windows
// and macOS vtable orders agree.
enum {
    PLAIN,      // integers and pointers to plain data in, integer out: forwarded as is
    IGNORED,    // takes a pointer to code in the game, which native code must never call: does nothing
    GET3, GET2, // returns another interface; its version string is argument 3 or 2
    STEAM_ID,   // returns a CSteamID: by value in a register on macOS, through a hidden pointer on Windows
    APP_DIR,    // ISteamApps::GetAppInstallDir: a macOS folder, returned as the game sees it
    ITEM_DIR,   // ISteamUGC::GetItemInstallInfo: the same for a Workshop item
    UGC_DETAILS,// ISteamUGC::GetQueryUGCResult: SteamUGCDetails_t, packed differently on each side
};
#define CLIENT "SteamClient020"
#define USER "SteamUser021"
#define APPS "STEAMAPPS_INTERFACE_VERSION008"
#define UTILS "SteamUtils010"
#define UGC "STEAMUGC_INTERFACE_VERSION014"
// `bits` is the width of the result: only that much of the native return
// register is defined. `bools` marks arguments (bit n = argument n, counting
// the object as 0) that are one byte wide and must be cleaned before the call.
static const struct { const char *version; int slot, kind, bits, bools; const char *name; } methods[] = {
    {CLIENT, 0, PLAIN, 32, 0, "CreateSteamPipe"}, {CLIENT, 1, PLAIN, 8, 0, "BReleaseSteamPipe"},
    {CLIENT, 2, PLAIN, 32, 0, "ConnectToGlobalUser"}, {CLIENT, 3, PLAIN, 32, 0, "CreateLocalUser"},
    {CLIENT, 4, PLAIN, 0, 0, "ReleaseUser"}, {CLIENT, 5, GET3, 64, 0, "GetISteamUser"},
    {CLIENT, 8, GET3, 64, 0, "GetISteamFriends"}, {CLIENT, 9, GET2, 64, 0, "GetISteamUtils"},
    {CLIENT, 12, GET3, 64, 0, "GetISteamGenericInterface"}, {CLIENT, 13, GET3, 64, 0, "GetISteamUserStats"},
    {CLIENT, 15, GET3, 64, 0, "GetISteamApps"}, {CLIENT, 17, GET3, 64, 0, "GetISteamRemoteStorage"},
    {CLIENT, 18, GET3, 64, 0, "GetISteamScreenshots"}, {CLIENT, 20, PLAIN, 0, 0, "RunFrame"},
    {CLIENT, 21, PLAIN, 32, 0, "GetIPCCallCount"}, {CLIENT, 22, IGNORED, 0, 0, "SetWarningMessageHook"},
    {CLIENT, 23, PLAIN, 8, 0, "BShutdownIfAllPipesClosed"}, {CLIENT, 27, GET3, 64, 0, "GetISteamUGC"},

    {USER, 0, PLAIN, 32, 0, "GetHSteamUser"}, {USER, 1, PLAIN, 8, 0, "BLoggedOn"},
    {USER, 2, STEAM_ID, 64, 0, "GetSteamID"}, {USER, 6, PLAIN, 8, 0, "GetUserDataFolder"},
    {USER, 18, PLAIN, 8, 0, "BIsBehindNAT"}, {USER, 23, PLAIN, 32, 0, "GetPlayerSteamLevel"},

    {APPS, 0, PLAIN, 8, 0, "BIsSubscribed"}, {APPS, 1, PLAIN, 8, 0, "BIsLowViolence"},
    {APPS, 2, PLAIN, 8, 0, "BIsCybercafe"}, {APPS, 3, PLAIN, 8, 0, "BIsVACBanned"},
    {APPS, 4, PLAIN, 64, 0, "GetCurrentGameLanguage"}, {APPS, 5, PLAIN, 64, 0, "GetAvailableGameLanguages"},
    {APPS, 6, PLAIN, 8, 0, "BIsSubscribedApp"}, {APPS, 7, PLAIN, 8, 0, "BIsDlcInstalled"},
    {APPS, 8, PLAIN, 32, 0, "GetEarliestPurchaseUnixTime"}, {APPS, 9, PLAIN, 8, 0, "BIsSubscribedFromFreeWeekend"},
    {APPS, 10, PLAIN, 32, 0, "GetDLCCount"}, {APPS, 11, PLAIN, 8, 0, "BGetDLCDataByIndex"},
    {APPS, 15, PLAIN, 8, 0, "GetCurrentBetaName"}, {APPS, 17, PLAIN, 32, 0, "GetInstalledDepots"},
    {APPS, 18, APP_DIR, 32, 0, "GetAppInstallDir"}, {APPS, 19, PLAIN, 8, 0, "BIsAppInstalled"},
    {APPS, 20, STEAM_ID, 64, 0, "GetAppOwner"}, {APPS, 21, PLAIN, 64, 0, "GetLaunchQueryParam"},
    {APPS, 22, PLAIN, 8, 0, "GetDlcDownloadProgress"}, {APPS, 23, PLAIN, 32, 0, "GetAppBuildId"},
    {APPS, 26, PLAIN, 32, 0, "GetLaunchCommandLine"}, {APPS, 27, PLAIN, 8, 0, "BIsSubscribedFromFamilySharing"},

    {UTILS, 0, PLAIN, 32, 0, "GetSecondsSinceAppActive"}, {UTILS, 1, PLAIN, 32, 0, "GetSecondsSinceComputerActive"},
    {UTILS, 2, PLAIN, 32, 0, "GetConnectedUniverse"}, {UTILS, 3, PLAIN, 32, 0, "GetServerRealTime"},
    {UTILS, 4, PLAIN, 64, 0, "GetIPCountry"}, {UTILS, 5, PLAIN, 8, 0, "GetImageSize"},
    {UTILS, 6, PLAIN, 8, 0, "GetImageRGBA"}, {UTILS, 9, PLAIN, 32, 0, "GetAppID"},
    {UTILS, 10, PLAIN, 0, 0, "SetOverlayNotificationPosition"}, {UTILS, 14, PLAIN, 0, 0, "RunFrame"},
    {UTILS, 15, PLAIN, 32, 0, "GetIPCCallCount"}, {UTILS, 16, IGNORED, 0, 0, "SetWarningMessageHook"},
    {UTILS, 17, PLAIN, 8, 0, "IsOverlayEnabled"}, {UTILS, 18, PLAIN, 8, 0, "BOverlayNeedsPresent"},
    {UTILS, 23, PLAIN, 64, 0, "GetSteamUILanguage"}, {UTILS, 24, PLAIN, 8, 0, "IsSteamRunningInVR"},
    {UTILS, 26, PLAIN, 8, 0, "IsSteamInBigPictureMode"}, {UTILS, 34, PLAIN, 8, 0, "IsSteamRunningOnSteamDeck"},

    // Workshop.
    {UGC, 5, UGC_DETAILS, 8, 0, "GetQueryUGCResult"}, {UGC, 65, ITEM_DIR, 8, 0, "GetItemInstallInfo"},
    {UGC, 0, PLAIN, 64, 0, "CreateQueryUserUGCRequest"}, {UGC, 1, PLAIN, 64, 0, "CreateQueryAllUGCRequest(cursor)"},
    {UGC, 2, PLAIN, 64, 0, "CreateQueryAllUGCRequest(page)"}, {UGC, 3, PLAIN, 64, 0, "CreateQueryUGCDetailsRequest"},
    {UGC, 4, PLAIN, 64, 0, "SendQueryUGCRequest"}, {UGC, 6, PLAIN, 8, 0, "GetQueryUGCPreviewURL"},
    {UGC, 7, PLAIN, 8, 0, "GetQueryUGCMetadata"}, {UGC, 8, PLAIN, 8, 0, "GetQueryUGCChildren"},
    {UGC, 9, PLAIN, 8, 0, "GetQueryUGCStatistic"}, {UGC, 10, PLAIN, 32, 0, "GetQueryUGCNumAdditionalPreviews"},
    {UGC, 12, PLAIN, 32, 0, "GetQueryUGCNumKeyValueTags"}, {UGC, 15, PLAIN, 8, 0, "ReleaseQueryUGCRequest"},
    {UGC, 16, PLAIN, 8, 0, "AddRequiredTag"}, {UGC, 18, PLAIN, 8, 0, "AddExcludedTag"},
    {UGC, 19, PLAIN, 8, 4, "SetReturnOnlyIDs"}, {UGC, 20, PLAIN, 8, 4, "SetReturnKeyValueTags"},
    {UGC, 21, PLAIN, 8, 4, "SetReturnLongDescription"}, {UGC, 22, PLAIN, 8, 4, "SetReturnMetadata"},
    {UGC, 23, PLAIN, 8, 4, "SetReturnChildren"}, {UGC, 24, PLAIN, 8, 4, "SetReturnAdditionalPreviews"},
    {UGC, 25, PLAIN, 8, 4, "SetReturnTotalOnly"}, {UGC, 26, PLAIN, 8, 0, "SetReturnPlaytimeStats"},
    {UGC, 27, PLAIN, 8, 0, "SetLanguage"}, {UGC, 28, PLAIN, 8, 0, "SetAllowCachedResponse"},
    {UGC, 29, PLAIN, 8, 0, "SetCloudFileNameFilter"}, {UGC, 30, PLAIN, 8, 4, "SetMatchAnyTag"},
    {UGC, 31, PLAIN, 8, 0, "SetSearchText"}, {UGC, 32, PLAIN, 8, 0, "SetRankedByTrendDays"},
    {UGC, 33, PLAIN, 8, 0, "AddRequiredKeyValueTag"}, {UGC, 62, PLAIN, 32, 0, "GetNumSubscribedItems"},
    {UGC, 63, PLAIN, 32, 0, "GetSubscribedItems"}, {UGC, 64, PLAIN, 32, 0, "GetItemState"},
    {UGC, 66, PLAIN, 8, 0, "GetItemDownloadInfo"}, {UGC, 67, PLAIN, 8, 4, "DownloadItem"},
};

// Where the Windows compiler and the macOS compiler disagree on vtable order:
// overloads of one name are laid out in reverse on Windows. Game slot, native slot.
static const struct { const char *version; int game, native; } reordered[] = {
    {UGC, 1, 2}, {UGC, 2, 1},       // CreateQueryAllUGCRequest
    {UGC, 13, 14}, {UGC, 14, 13},   // GetQueryUGCKeyValueTag
};
static int native_slot(const char *version, int slot) {
    for (size_t i = 0; i < sizeof reordered / sizeof reordered[0]; i++)
        if (reordered[i].game == slot && !strcmp(reordered[i].version, version)) return reordered[i].native;
    return slot;
}

// SteamUGCDetails_t as of Steamworks SDK 1.50 (STEAMUGC_INTERFACE_VERSION014,
// per Proton's lsteamclient), with Valve's callback packing: 8 on Windows,
// 4 on macOS. Only where the 64-bit members fall differs.
#define UGC_DETAILS_FIELDS \
    uint64_t id; int32_t result, file_type; uint32_t creator_app, consumer_app; \
    char title[129], description[8000]; uint64_t owner; \
    uint32_t created, updated, added_to_user_list; int32_t visibility; \
    _Bool banned, accepted_for_use, tags_truncated; char tags[1025]; \
    uint64_t file, preview_file; char file_name[260]; int32_t file_size, preview_file_size; \
    char url[256]; uint32_t votes_up, votes_down; float score; uint32_t num_children;
#pragma pack(push, 8)
typedef struct { UGC_DETAILS_FIELDS } ugc_details_windows;
#pragma pack(pop)
#pragma pack(push, 4)
typedef struct { UGC_DETAILS_FIELDS } ugc_details_mac;
#pragma pack(pop)
_Static_assert(sizeof(ugc_details_windows) == 9776 && sizeof(ugc_details_mac) == 9764, "SteamUGCDetails_t layout");

static void ugc_details_to_windows(const ugc_details_mac *m, ugc_details_windows *w) {
    w->id = m->id; w->result = m->result; w->file_type = m->file_type;
    w->creator_app = m->creator_app; w->consumer_app = m->consumer_app;
    memcpy(w->title, m->title, sizeof w->title); memcpy(w->description, m->description, sizeof w->description);
    w->owner = m->owner; w->created = m->created; w->updated = m->updated;
    w->added_to_user_list = m->added_to_user_list; w->visibility = m->visibility;
    w->banned = m->banned; w->accepted_for_use = m->accepted_for_use; w->tags_truncated = m->tags_truncated;
    memcpy(w->tags, m->tags, sizeof w->tags); w->file = m->file; w->preview_file = m->preview_file;
    memcpy(w->file_name, m->file_name, sizeof w->file_name);
    w->file_size = m->file_size; w->preview_file_size = m->preview_file_size;
    memcpy(w->url, m->url, sizeof w->url); w->votes_up = m->votes_up; w->votes_down = m->votes_down;
    w->score = m->score; w->num_children = m->num_children;
}

// Copies a macOS folder into the game's buffer as the Windows path it should
// use. Returns the length written, or 0 if it does not fit.
static uint32_t put_guest_path(const char *host, char *out, uint32_t cap) {
    char guest[4096];
    guest_path_for_host(host, guest, sizeof guest);
    size_t n = strlen(guest);
    if (!out || n + 1 > cap) return 0;
    memcpy(out, guest, n + 1);
    return (uint32_t)n;
}

static int find_method(const char *version, int slot) {
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        if (methods[i].slot == slot && !strcmp(methods[i].version, version)) return (int)i;
    return -1;
}

int steam_foreign_call(cpu_t *c, uint64_t target) {
    if ((target & 0xffff000000000000ull) != MARK) return 0;
    int index = (int)(target >> 16 & 0xffff), slot = (int)(target & 0xffff);
    proxy_t *p = index < proxy_count ? proxies[index] : NULL;
    if (!p || (uint64_t)p != c->r[RCX]) {
        fprintf(stderr, "host: Steam method called on something that is not its interface\n");
        exit(71);
    }
    int m = find_method(p->version, slot);
    if (m < 0) {
        fprintf(stderr, "host: Steam %s method in vtable slot %d is not bridged yet (called from %#llx)\n",
                p->version, slot, (unsigned long long)ld64(c->r[RSP]));
        exit(70);
    }
    uint64_t r = 0;
    void **vtable = *(void ***)p->object;
    slot = native_slot(p->version, slot);
    if (methods[m].kind == APP_DIR) {
        // (app, folder, size) -> length. The game's own folder is where it sees its files.
        uint32_t (*fn)(void *, uint32_t, char *, uint32_t) = vtable[slot];
        char folder[4096] = "";
        uint32_t app = (uint32_t)c->r[RDX];
        if (app == (uint32_t)atoi(STEAM_APP_ID)) r = put_guest_path(host_game_root, (char *)c->r[R8], (uint32_t)c->r[R9]);
        else if (fn(p->object, app, folder, sizeof folder)) r = put_guest_path(folder, (char *)c->r[R8], (uint32_t)c->r[R9]);
    } else if (methods[m].kind == ITEM_DIR) {
        // (item, &size on disk, folder, folder size, &timestamp) -> bool
        _Bool (*fn)(void *, uint64_t, uint64_t *, char *, uint32_t, uint32_t *) = vtable[slot];
        char folder[4096] = "";
        r = fn(p->object, c->r[RDX], (uint64_t *)c->r[R8], folder, sizeof folder, (uint32_t *)host_arg(c, 5)) & 0xff;
        if (r && c->r[R9]) r = put_guest_path(folder, (char *)c->r[R9], (uint32_t)host_arg(c, 4)) != 0;
    } else if (methods[m].kind == UGC_DETAILS) {
        // (query, index, &details) -> bool
        _Bool (*fn)(void *, uint64_t, uint32_t, ugc_details_mac *) = vtable[slot];
        ugc_details_mac *mac = calloc(1, sizeof *mac);
        r = fn(p->object, c->r[RDX], (uint32_t)c->r[R8], mac) & 0xff;
        if (r && c->r[R9]) ugc_details_to_windows(mac, (ugc_details_windows *)c->r[R9]);
        free(mac);
    } else if (methods[m].kind == STEAM_ID) {
        uint64_t (*fn)(void *) = vtable[slot];
        st64(c->r[RDX], fn(p->object));
        r = c->r[RDX];
    } else if (methods[m].kind != IGNORED) {
        uint64_t a[8] = {0, c->r[RDX], c->r[R8], c->r[R9], host_arg(c, 4), host_arg(c, 5), host_arg(c, 6), host_arg(c, 7)};
        for (int i = 1; i < 8; i++) if (methods[m].bools >> i & 1) a[i] &= 0xff;
        uint64_t (*fn)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = vtable[slot];
        r = fn(p->object, a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        if (methods[m].bits == 8) r &= 0xff;
        else if (methods[m].bits == 32) r &= 0xffffffffu;
        else if (methods[m].bits == 0) r = 0;
        if (methods[m].kind == GET3) r = (uint64_t)wrap((void *)r, (const char *)c->r[R9]);
        if (methods[m].kind == GET2) r = (uint64_t)wrap((void *)r, (const char *)c->r[R8]);
    }
    if (verbose) fprintf(stderr, "host: Steam %s::%s = %#llx\n", p->version, methods[m].name, (unsigned long long)r);
    c->r[RAX] = r;
    c->r[RSP] += 8;
    return 1;
}

// ------------------------------------------------------------ flat imports
static uint64_t context_generation = 1;

IMPORT(SteamAPI_RestartAppIfNecessary) {
    _Bool (*fn)(uint32_t) = native("SteamAPI_RestartAppIfNecessary");
    RET(fn((uint32_t)ARG(0)));
}
IMPORT(SteamAPI_Init) {
    char message[1024] = "";
    int (*fn)(char *) = native("SteamAPI_InitFlat");
    int result = fn(message);
    if (result != 0) fprintf(stderr, "host: Steam did not initialise (%d) %s\n", result, message);
    context_generation++;
    RET(result == 0);
}
IMPORT(SteamAPI_Shutdown) {
    (void)c;
    void (*fn)(void) = native("SteamAPI_Shutdown");
    fn();
    context_generation++;
}
// ------------------------------------------------------------ call results
// An asynchronous Steam request returns a handle; the game registers an
// object of its own to be called when the answer is in. The native library
// cannot call the game's object, so the answers are collected here instead:
// after each round of callbacks, every pending handle is polled, and a
// finished one is fetched and handed to the game's object as translated code.
//
// The answer is a structure that both sides must lay out identically. Only
// structures checked for that are delivered.
enum { MAX_PENDING = 64 };
static struct { uint64_t call, object; } pending[MAX_PENDING];
static int pending_count;
static const struct { int id; int size; const char *name; } deliverable[] = {
    {3401, 280, "SteamUGCQueryCompleted_t"},   // same layout with 4- and 8-byte packing
};

static void deliver_call_results(cpu_t *c) {
    if (!pending_count) return;
    int32_t (*get_user)(void) = native("SteamAPI_GetHSteamUser");
    void *(*find)(int32_t, const char *) = native("SteamInternal_FindOrCreateUserInterface");
    void *utils = find(get_user(), UTILS);
    void **vt = *(void ***)utils;
    _Bool (*completed)(void *, uint64_t, _Bool *) = vt[11];             // ISteamUtils::IsAPICallCompleted
    _Bool (*result)(void *, uint64_t, void *, int, int, _Bool *) = vt[13];  // ISteamUtils::GetAPICallResult
    for (int i = 0; i < pending_count; i++) {
        _Bool failed = 0;
        if (!completed(utils, pending[i].call, &failed)) continue;
        uint64_t call = pending[i].call, object = pending[i].object;
        pending[i--] = pending[--pending_count];
        int id = (int)ld32(object + 12);
        // The game's object: vtable { Run(answer, failed, handle), Run(answer), GetCallbackSizeBytes() }.
        uint64_t vtable = ld64(object);
        int size = (int)host_call_guest(c, ld64(vtable + 16), 1, &object);
        int known = 0;
        for (size_t k = 0; k < sizeof deliverable / sizeof deliverable[0]; k++)
            known |= deliverable[k].id == id && deliverable[k].size == size;
        if (!known) {
            fprintf(stderr, "host: Steam answer %d (%d bytes) has no checked layout; not delivered\n", id, size);
            exit(70);
        }
        void *answer = calloc(1, (size_t)size + 16);
        result(utils, call, answer, size, id, &failed);
        if (verbose) fprintf(stderr, "host: Steam answer %d for call %#llx delivered%s\n", id, (unsigned long long)call, failed ? " (failed)" : "");
        uint64_t args[4] = {object, (uint64_t)answer, failed, call};
        host_call_guest(c, ld64(vtable), 4, args);
        free(answer);
    }
}

IMPORT(SteamAPI_RunCallbacks) {
    void (*fn)(void) = native("SteamAPI_RunCallbacks");
    fn();
    deliver_call_results(c);
}
IMPORT(SteamAPI_GetHSteamUser) { int32_t (*fn)(void) = native("SteamAPI_GetHSteamUser"); RET((uint64_t)(int64_t)fn()); }
IMPORT(SteamInternal_FindOrCreateUserInterface) {
    void *(*fn)(int32_t, const char *) = native("SteamInternal_FindOrCreateUserInterface");
    RET(wrap(fn((int32_t)ARG(0), PTR(1)), PTR(1)));
}
IMPORT(SteamInternal_CreateInterface) {
    void *(*fn)(const char *) = native("SteamInternal_CreateInterface");
    RET(wrap(fn(PTR(0)), PTR(0)));
}
// The game's side keeps { init function, generation, context } and expects the
// context to be (re)initialised by calling its function whenever Steam has
// been initialised or shut down since. The function is the game's, so it is
// called here as translated code rather than handed to the native library.
IMPORT(SteamInternal_ContextInit) {
    uint64_t data = ARG(0);
    if (ld64(data + 8) != context_generation) {
        uint64_t arg = data + 16;
        host_call_guest(c, ld64(data), 1, &arg);
        st64(data + 8, context_generation);
    }
    RET(data + 16);
}
IMPORT(SteamAPI_RegisterCallResult) {
    if (pending_count == MAX_PENDING) { fprintf(stderr, "host: too many pending Steam calls\n"); exit(71); }
    pending[pending_count].object = ARG(0);
    pending[pending_count++].call = ARG(1);
}
IMPORT(SteamAPI_UnregisterCallResult) {
    for (int i = 0; i < pending_count; i++)
        if (pending[i].object == ARG(0) && pending[i].call == ARG(1)) pending[i--] = pending[--pending_count];
}
