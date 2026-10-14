#pragma once
#include <stddef.h>
#include <stdint.h>

// Host path for a Windows path. for_write selects where new or changed files
// go and creates the parent directories. The result is malloc'd.
char *host_path(const char *win, int for_write);
char *host_path_w(const uint16_t *win, int for_write);
void guest_normalize(const char *win, char *out, size_t cap);
const char *guest_current_directory(void);            // forward slashes, e.g. C:/SpaceEngine/system
void guest_set_current_directory(const char *win);
// The host directories a Windows directory's listing merges, in order of
// precedence: for the game's folder, the overlay, the game, then each DLC.
enum { HOST_MAX_ROOTS = 16 };
int host_list_roots(const char *win_dir, char roots[HOST_MAX_ROOTS][4096]);
void guest_path_for_host(const char *host, char *out, size_t cap);   // a macOS path as the game should see it
