// SHELL32: folder-tree copy, move and delete, and "open this with its
// default application".
//
// Everything that changes the file system resolves through host_path() in
// write mode, which only ever yields a location under the port's own support
// directory. remove_tree() checks that again itself before deleting anything.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"
#include "paths.h"

static int inside_support_dir(const char *path) {
    char root[1024];
    snprintf(root, sizeof root, "%s/Library/Application Support/SpaceEngine-native/", getenv("HOME"));
    return strncmp(path, root, strlen(root)) == 0 && !strstr(path, "/../");
}

static int remove_tree(const char *path) {
    if (!inside_support_dir(path)) {
        fprintf(stderr, "host: refusing to delete outside the support directory: %s\n", path);
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int r = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        r |= remove_tree(child);
    }
    closedir(d);
    return r | rmdir(path);
}

static int copy_tree(const char *from, const char *to) {
    struct stat st;
    if (stat(from, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        mkdir(to, 0755);
        DIR *d = opendir(from);
        if (!d) return -1;
        struct dirent *e;
        int r = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[4096], b[4096];
            snprintf(a, sizeof a, "%s/%s", from, e->d_name);
            snprintf(b, sizeof b, "%s/%s", to, e->d_name);
            r |= copy_tree(a, b);
        }
        closedir(d);
        return r;
    }
    int in = open(from, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    char buf[65536];
    ssize_t n;
    int ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return ok ? 0 : -1;
}

// SHFILEOPSTRUCTA: window, operation, source list, destination, flags.
// The source is a list of names, each terminated, ending with an empty one.
// Wildcards in the last component are not supported.
IMPORT(SHFileOperationA) {
    uint64_t op = ARG(0);
    uint32_t func = ld32(op + 8);
    const char *from = (const char *)ld64(op + 16), *to = (const char *)ld64(op + 24);
    int failed = 0;
    for (; from && *from; from += strlen(from) + 1) {
        char *dst = to && func != 3 ? host_path(to, 1) : NULL;
        if (getenv("SE_VERBOSE"))
            fprintf(stderr, "host: shell %s %s%s%s\n", func == 1 ? "move" : func == 2 ? "copy" : func == 3 ? "delete" : "rename",
                    from, to ? " -> " : "", to ? to : "");
        if (strpbrk(from, "*?")) { fprintf(stderr, "host: shell operation with a wildcard is not supported: %s\n", from); failed = 1; }
        else if (func == 3) {                       // FO_DELETE
            char *victim = host_path(from, 1);
            failed |= remove_tree(victim) != 0;
            free(victim);
        } else if (func == 2) {                     // FO_COPY
            char *src = host_path(from, 0);
            failed |= copy_tree(src, dst) != 0;
            free(src);
        } else if (func == 1 || func == 4) {        // FO_MOVE, FO_RENAME
            char *src = host_path(from, 1);
            if (access(src, F_OK) == 0) failed |= rename(src, dst) != 0;
            else {  // only the read-only original exists: the "move" becomes a copy
                char *original = host_path(from, 0);
                failed |= copy_tree(original, dst) != 0;
                free(original);
            }
            free(src);
        }
        free(dst);
    }
    st32(op + 36, 0);                               // fAnyOperationsAborted
    RET(failed ? 2 : 0);
}
// Opening a document or URL with its default application is left to the user.
IMPORT(ShellExecuteA) {
    fprintf(stderr, "host: the game asked to open \"%s\"\n", ARG(2) ? (const char *)PTR(2) : "");
    RET(33);  // any value above 32 means success
}
IMPORT(ShellExecuteW) {
    char *what = ARG(2) ? u16_to_utf8(PTR(2), -1) : NULL;
    fprintf(stderr, "host: the game asked to open \"%s\"\n", what ? what : "");
    free(what);
    RET(33);
}
