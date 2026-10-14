// Where the Windows paths the game uses live on this machine.
//
//   C:\SpaceEngine\...    the game's installation (the Steam depot). Never
//                         written: anything the game creates or changes there
//                         goes to an overlay directory, and reads prefer the
//                         overlay copy. DLC depots downloaded beside the game's
//                         (depot_<id> folders next to it, as Steam's
//                         download_depot leaves them) are layered underneath,
//                         as if installed into the same folder.
//   C:\Users\player\...   the player's Windows profile (Documents, AppData).
//   Z:\...                the Mac's own files, for folders Steam names (Workshop
//                         items); like the installation, never written: writes
//                         go to an overlay.
//   anything else         a private directory per drive letter.
//
// The host directories are under ~/Library/Application Support/SpaceEngine-native.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"
#include "paths.h"

static char guest_cwd[1024] = "C:/SpaceEngine/system";
static char support_dir[1024];

static const char *support(void) {
    if (!support_dir[0]) {
        const char *home = getenv("HOME");
        snprintf(support_dir, sizeof support_dir, "%s/Library/Application Support/SpaceEngine-native", home ? home : "/tmp");
    }
    return support_dir;
}

static void make_parents(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

// Absolute, forward-slashed, with "." and ".." resolved. Result in `out`.
void guest_normalize(const char *win, char *out, size_t cap) {
    char buf[2048];
    int absolute = (isalpha((unsigned char)win[0]) && win[1] == ':') || win[0] == '\\' || win[0] == '/';
    if (absolute && win[1] != ':') snprintf(buf, sizeof buf, "C:%s", win);
    else if (absolute) snprintf(buf, sizeof buf, "%s", win);
    else snprintf(buf, sizeof buf, "%s/%s", guest_cwd, win);
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    buf[0] = (char)toupper((unsigned char)buf[0]);

    char *parts[128];
    int n = 0;
    for (char *tok = strtok(buf + 2, "/"); tok; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n < 128) parts[n++] = tok;
    }
    size_t len = (size_t)snprintf(out, cap, "%c:", buf[0]);
    for (int i = 0; i < n && len < cap; i++) len += (size_t)snprintf(out + len, cap - len, "/%s", parts[i]);
    if (n == 0 && len + 1 < cap) { out[len] = '/'; out[len + 1] = 0; }
}

// The DLC depots beside the game's own, found once.
static char dlc_roots[HOST_MAX_ROOTS - 2][1024];
static int dlc_count = -1;
static int by_name(const void *a, const void *b) { return strcmp(a, b); }
static void find_dlc_roots(void) {
    if (dlc_count >= 0) return;
    dlc_count = 0;
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", host_game_root);
    char *slash = strrchr(parent, '/');
    if (!slash || strncmp(slash + 1, "depot_", 6) != 0) return;
    *slash = 0;
    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && dlc_count < HOST_MAX_ROOTS - 2) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", parent, e->d_name);
        if (strncmp(e->d_name, "depot_", 6) != 0 || !strcmp(path, host_game_root)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(dlc_roots[dlc_count++], 1024, "%s", path);
    }
    closedir(d);
    qsort(dlc_roots, (size_t)dlc_count, sizeof dlc_roots[0], by_name);
    if (getenv("SE_VERBOSE")) for (int i = 0; i < dlc_count; i++) fprintf(stderr, "host: DLC files from %s\n", dlc_roots[i]);
}

static const char *after_prefix(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncasecmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == 0)) return path + n;
    return NULL;
}

char *host_path(const char *win, int for_write) {
    char norm[2048], out[4096];
    guest_normalize(win, norm, sizeof norm);
    const char *rest;
    if ((rest = after_prefix(norm, "C:/SpaceEngine"))) {
        snprintf(out, sizeof out, "%s/overlay%s", support(), rest);
        if (for_write) make_parents(out);
        else if (access(out, F_OK) != 0) {
            snprintf(out, sizeof out, "%s%s", host_game_root, rest);
            find_dlc_roots();
            for (int i = 0; i < dlc_count && access(out, F_OK) != 0; i++) {
                char dlc[4096];
                snprintf(dlc, sizeof dlc, "%s%s", dlc_roots[i], rest);
                if (access(dlc, F_OK) == 0) snprintf(out, sizeof out, "%s", dlc);
            }
        }
    } else if (toupper((unsigned char)norm[0]) == 'Z' && norm[1] == ':') {
        rest = norm + 2;
        snprintf(out, sizeof out, "%s/drive_z%s", support(), rest);
        if (for_write) make_parents(out);
        else if (access(out, F_OK) != 0) snprintf(out, sizeof out, "%s", *rest ? rest : "/");
    } else if ((rest = after_prefix(norm, "C:/Users/player"))) {
        snprintf(out, sizeof out, "%s/home%s", support(), rest);
        if (for_write) make_parents(out);
    } else {
        snprintf(out, sizeof out, "%s/drive_%c%s", support(), tolower((unsigned char)norm[0]), norm + 2);
        if (for_write) make_parents(out);
    }
    return strdup(out);
}

char *host_path_w(const uint16_t *win, int for_write) {
    char *utf8 = u16_to_utf8(win, -1);
    char *out = host_path(utf8, for_write);
    free(utf8);
    return out;
}

const char *guest_current_directory(void) { return guest_cwd; }
void guest_set_current_directory(const char *win) { guest_normalize(win, guest_cwd, sizeof guest_cwd); }

// The game-root overlay and depot together, for directory listings.
int host_list_roots(const char *win_dir, char roots[HOST_MAX_ROOTS][4096]) {
    char norm[2048];
    guest_normalize(win_dir, norm, sizeof norm);
    const char *rest = after_prefix(norm, "C:/SpaceEngine");
    if (rest) {
        snprintf(roots[0], 4096, "%s/overlay%s", support(), rest);
        snprintf(roots[1], 4096, "%s%s", host_game_root, rest);
        find_dlc_roots();
        for (int i = 0; i < dlc_count; i++) snprintf(roots[2 + i], 4096, "%s%s", dlc_roots[i], rest);
        return 2 + dlc_count;
    }
    if (toupper((unsigned char)norm[0]) == 'Z' && norm[1] == ':') {
        snprintf(roots[0], 4096, "%s/drive_z%s", support(), norm + 2);
        snprintf(roots[1], 4096, "%s", norm[2] ? norm + 2 : "/");
        return 2;
    }
    char *one = host_path(win_dir, 0);
    snprintf(roots[0], 4096, "%s", one);
    free(one);
    return 1;
}

// The Windows path the game should use for a macOS path, with backslashes.
void guest_path_for_host(const char *host, char *out, size_t cap) {
    char home[2048];
    snprintf(home, sizeof home, "%s/home", support());
    const char *rest;
    if ((rest = after_prefix(host, host_game_root))) snprintf(out, cap, "C:\\SpaceEngine%s", rest);
    else if ((rest = after_prefix(host, home))) snprintf(out, cap, "C:\\Users\\player%s", rest);
    else snprintf(out, cap, "Z:%s", host);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}
