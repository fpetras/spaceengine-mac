// The Windows environment the game sees: a small private table, not the
// macOS environment of this process.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host.h"

enum { MAX_VARS = 128 };
static struct { char *name, *value; } vars[MAX_VARS];
static int var_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void set_locked(const char *name, const char *value) {
    for (int i = 0; i < var_count; i++)
        if (strcasecmp(vars[i].name, name) == 0) {
            free(vars[i].value);
            if (value) { vars[i].value = strdup(value); return; }
            free(vars[i].name);
            vars[i] = vars[--var_count];
            return;
        }
    if (value && var_count < MAX_VARS) { vars[var_count].name = strdup(name); vars[var_count++].value = strdup(value); }
}

static void ensure_defaults(void) {
    static int done;
    if (done) return;
    done = 1;
    set_locked("USERPROFILE", GUEST_HOME);
    set_locked("LOCALAPPDATA", GUEST_HOME "\\AppData\\Local");
    set_locked("APPDATA", GUEST_HOME "\\AppData\\Roaming");
    set_locked("TEMP", GUEST_HOME "\\AppData\\Local\\Temp");
    set_locked("TMP", GUEST_HOME "\\AppData\\Local\\Temp");
    set_locked("SystemRoot", "C:\\Windows");
}

// Copies the value into out and returns its length, or -1 if unset.
long host_env_get(const char *name, char *out, size_t cap) {
    long n = -1;
    pthread_mutex_lock(&lock);
    ensure_defaults();
    for (int i = 0; i < var_count; i++)
        if (strcasecmp(vars[i].name, name) == 0) { n = (long)strlen(vars[i].value); snprintf(out, cap, "%s", vars[i].value); }
    pthread_mutex_unlock(&lock);
    return n;
}

IMPORT(GetEnvironmentStringsW) {
    // NAME=value strings, each terminated, with an empty string at the end.
    pthread_mutex_lock(&lock);
    ensure_defaults();
    size_t units = 2;
    for (int i = 0; i < var_count; i++) units += (strlen(vars[i].name) + strlen(vars[i].value) + 2) * 2;
    uint16_t *block = calloc(units, 2), *p = block;
    for (int i = 0; i < var_count; i++) {
        char line[4096];
        snprintf(line, sizeof line, "%s=%s", vars[i].name, vars[i].value);
        p += utf8_to_u16(line, p, (size_t)(block + units - p));
    }
    *p = 0;
    pthread_mutex_unlock(&lock);
    RET(block);
}
IMPORT(FreeEnvironmentStringsW) { free(PTR(0)); RET(1); }
IMPORT(SetEnvironmentVariableW) {
    char *name = u16_to_utf8(PTR(0), -1), *value = ARG(1) ? u16_to_utf8(PTR(1), -1) : NULL;
    pthread_mutex_lock(&lock);
    ensure_defaults();
    set_locked(name, value);
    pthread_mutex_unlock(&lock);
    free(name); free(value);
    RET(1);
}
IMPORT(ExpandEnvironmentStringsA) {
    const char *src = PTR(0);
    char out[4096];
    size_t n = 0;
    while (*src && n + 1 < sizeof out) {
        const char *end = *src == '%' ? strchr(src + 1, '%') : NULL;
        char name[128], value[2048];
        if (end && (size_t)(end - src - 1) < sizeof name) {
            memcpy(name, src + 1, (size_t)(end - src - 1));
            name[end - src - 1] = 0;
            if (host_env_get(name, value, sizeof value) >= 0) {
                n += (size_t)snprintf(out + n, sizeof out - n, "%s", value);
                src = end + 1;
                continue;
            }
        }
        out[n++] = *src++;
    }
    out[n] = 0;
    if (n + 1 <= ARG(2)) memcpy(PTR(1), out, n + 1);
    RET(n + 1);
}
