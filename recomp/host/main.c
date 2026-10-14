// Boots the translated game: maps the Windows image, attaches the host, and
// runs the program's own entry point.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "window.h"

char host_game_root[1024];

// Where the game is looked for when no path is given: a line in game-path.txt
// in the support directory, then the places Steam puts it.
static const char *find_game(void) {
    static char path[2048];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof path, "%s/Library/Application Support/SpaceEngine-native/game-path.txt", home);
    FILE *f = fopen(path, "r");
    if (f) {
        char *line = fgets(path, sizeof path, f);
        fclose(f);
        if (line) {
            path[strcspn(path, "\r\n")] = 0;
            if (access(path, R_OK) == 0) return path;
        }
    }
    static const char *const places[] = {
        "Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_314650/depot_314651",
        "Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/common/SpaceEngine",
        "Steam/steamapps/content/app_314650/depot_314651",
        "Steam/steamapps/common/SpaceEngine",
    };
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        snprintf(path, sizeof path, "%s/Library/Application Support/%s/system/SpaceEngine.exe", home, places[i]);
        if (access(path, R_OK) == 0) return path;
    }
    return NULL;
}

// Started from the Finder there is no terminal: messages go to a log file.
static void log_to_file_in_app(const char *self) {
    const char *home = getenv("HOME");
    if (!home || !strstr(self, ".app/Contents/MacOS/") || isatty(STDERR_FILENO)) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/Library/Logs/SpaceEngine-native.log", home);
    if (freopen(path, "w", stderr)) { setvbuf(stderr, NULL, _IOLBF, 0); dup2(STDERR_FILENO, STDOUT_FILENO); }
}

int main(int argc, char **argv) {
    log_to_file_in_app(argv[0]);
    const char *exe = argc > 1 && argv[1][0] != '-' ? argv[1] : getenv("SE_EXE");
    if (!exe) exe = find_game();
    if (!exe)
        host_fatal(2, "SpaceEngine's files were not found in Steam's library.\n\n"
                      "Put the full path of SpaceEngine.exe on the first line of\n"
                      "~/Library/Application Support/SpaceEngine-native/game-path.txt");
    // .../<root>/system/SpaceEngine.exe -> <root>
    snprintf(host_game_root, sizeof host_game_root, "%s", exe);
    for (int i = 0; i < 2; i++) {
        char *slash = strrchr(host_game_root, '/');
        if (slash) *slash = 0;
    }

    recomp_set_reserved_range(recomp_reserved_range, RECOMP_RESERVED_SIZE);
    if (recomp_map_image(exe) != 0) return 2;
    host_init();

    cpu_t c;
    memset(&c, 0, sizeof c);
    host_thread_attach(&c);

    const uint64_t base = 0x140000000ull;
    uint64_t entry = base + ld32(base + ld32(base + 0x3c) + 24 + 16);
    recomp_fn fn = recomp_lookup(entry);
    if (!fn) {
        fprintf(stderr, "se: entry point %#llx is not translated\n", (unsigned long long)entry);
        return 2;
    }
    host_display_prepare();
    host_run_tls_callbacks(&c, 1);  // DLL_PROCESS_ATTACH, before the entry point as the Windows loader does
    recomp_invoke(&c, fn);
    return (int)c.r[RAX];
}
