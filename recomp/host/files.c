// KERNEL32 file and directory functions on top of POSIX.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "handles.h"
#include "host.h"
#include "paths.h"

enum { ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
       ERROR_NO_MORE_FILES = 18, ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183, ERROR_HANDLE_EOF = 38 };
enum { ATTR_DIRECTORY = 0x10, ATTR_ARCHIVE = 0x20, ATTR_NORMAL = 0x80, ATTR_READONLY = 0x01 };
#define INVALID_ATTRIBUTES 0xffffffffu

static uint32_t error_from_errno(void) {
    switch (errno) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EEXIST: return ERROR_FILE_EXISTS;
        case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
        default: return ERROR_ACCESS_DENIED;
    }
}

static uint64_t filetime(time_t sec, long nsec) {
    return ((uint64_t)sec + 11644473600ull) * 10000000ull + (uint64_t)nsec / 100;
}

// ---------------------------------------------------------------- handles
// A file handle's data is its descriptor plus one, so that it is never null.
typedef struct { DIR *dir[HOST_MAX_ROOTS]; int roots, current; char pattern[260]; char seen[4096][96]; int seen_count; char dirpath[HOST_MAX_ROOTS][4096]; } find_t;

static int file_fd(uint64_t h) {
    if (h == H_STDIN) return 0;
    if (h == H_STDOUT) return 1;
    if (h == H_STDERR) return 2;
    void *data = handle_data(h, HK_FILE);
    return data ? (int)(intptr_t)data - 1 : -1;
}

// ------------------------------------------------------------------ files
IMPORT(CreateFileW) {
    uint32_t desired = (uint32_t)ARG(1), disposition = (uint32_t)ARG(4);
    int wants_write = (desired & 0x40000000u) || (desired & 0x10000000u);   // GENERIC_WRITE or GENERIC_ALL
    int writing = wants_write || disposition != 3;
    int flags = wants_write ? (((desired & 0x80000000u) || (desired & 0x10000000u)) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disposition == 1) flags |= O_CREAT | O_EXCL;             // CREATE_NEW
    else if (disposition == 2) flags |= O_CREAT | O_TRUNC;       // CREATE_ALWAYS
    else if (disposition == 4) flags |= O_CREAT;                 // OPEN_ALWAYS
    else if (disposition == 5) flags |= O_TRUNC;                 // TRUNCATE_EXISTING
    char *path = host_path_w(PTR(0), writing);
    if (writing && disposition != 2 && disposition != 1 && access(path, F_OK) != 0) {
        // Opening an installed file for update: start the overlay copy from the original.
        char *original = host_path_w(PTR(0), 0);
        if (strcmp(original, path) != 0 && access(original, R_OK) == 0) {
            int in = open(original, O_RDONLY), out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            char buf[65536]; ssize_t n;
            while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0) write(out, buf, (size_t)n);
            if (in >= 0) close(in);
            if (out >= 0) close(out);
        }
        free(original);
    }
    int existed = access(path, F_OK) == 0;
    int fd = open(path, flags, 0644);
    if (getenv("SE_VERBOSE")) fprintf(stderr, "host: open %s%s -> %s\n", path, writing ? " (write)" : "", fd < 0 ? strerror(errno) : "ok");
    free(path);
    if (fd < 0) { SET_ERROR(error_from_errno()); RET(INVALID_HANDLE); }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); SET_ERROR(ERROR_ACCESS_DENIED); RET(INVALID_HANDLE); }
    SET_ERROR((disposition == 2 || disposition == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    RET(handle_alloc(HK_FILE, (void *)(intptr_t)(fd + 1)));
}
IMPORT(CloseHandle) {
    int fd = handle_kind(ARG(0)) == HK_FILE ? file_fd(ARG(0)) : -1;
    if (fd >= 0) { close(fd); handle_release(ARG(0)); }
    else if (handle_kind(ARG(0)) == HK_THREAD) host_thread_handle_closed(ARG(0));
    RET(1);
}
IMPORT(ReadFile) {
    int fd = file_fd(ARG(0));
    if (fd < 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    ssize_t n = read(fd, PTR(1), (uint32_t)ARG(2));
    if (ARG(3)) st32(ARG(3), n < 0 ? 0 : (uint32_t)n);
    if (n < 0) { SET_ERROR(error_from_errno()); RET(0); }
    RET(1);
}
IMPORT(WriteFile) {
    int fd = file_fd(ARG(0));
    if (fd < 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    ssize_t n = write(fd, PTR(1), (uint32_t)ARG(2));
    if (ARG(3)) st32(ARG(3), n < 0 ? 0 : (uint32_t)n);
    if (n < 0) { SET_ERROR(error_from_errno()); RET(0); }
    RET(1);
}
IMPORT(WriteConsoleW) {
    char *text = u16_to_utf8(PTR(1), (long)(uint32_t)ARG(2));
    fputs(text, ARG(0) == H_STDERR ? stderr : stdout);
    free(text);
    if (ARG(3)) st32(ARG(3), (uint32_t)ARG(2));
    RET(1);
}
IMPORT(SetFilePointerEx) {
    int fd = file_fd(ARG(0));
    if (fd < 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    off_t pos = lseek(fd, (off_t)(int64_t)ARG(1), ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (pos < 0) { SET_ERROR(error_from_errno()); RET(0); }
    if (ARG(2)) st64(ARG(2), (uint64_t)pos);
    RET(1);
}
IMPORT(GetFileSizeEx) {
    struct stat st;
    int fd = file_fd(ARG(0));
    if (fd < 0 || fstat(fd, &st) != 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    st64(ARG(1), (uint64_t)st.st_size);
    RET(1);
}
IMPORT(FlushFileBuffers) { RET(1); }
IMPORT(SetEndOfFile) {
    int fd = file_fd(ARG(0));
    if (fd < 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    RET(ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == 0);
}
static uint32_t attributes_of(const struct stat *st) {
    return S_ISDIR(st->st_mode) ? ATTR_DIRECTORY : ATTR_ARCHIVE;
}
IMPORT(GetFileInformationByHandle) {
    struct stat st;
    int fd = file_fd(ARG(0));
    if (fd < 0 || fstat(fd, &st) != 0) { SET_ERROR(ERROR_INVALID_HANDLE); RET(0); }
    uint64_t p = ARG(1);
    memset((void *)p, 0, 52);
    st32(p, attributes_of(&st));
    st64(p + 4, filetime(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec));
    st64(p + 12, filetime(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec));
    st64(p + 20, filetime(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec));
    st32(p + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    st32(p + 36, (uint32_t)st.st_size);
    st32(p + 40, 1);
    RET(1);
}
IMPORT(GetFileAttributesW) {
    struct stat st;
    char *path = host_path_w(PTR(0), 0);
    int ok = stat(path, &st) == 0;
    free(path);
    if (!ok) { SET_ERROR(error_from_errno()); RET(INVALID_ATTRIBUTES); }
    RET(attributes_of(&st));
}
IMPORT(GetFileAttributesExW) {
    struct stat st;
    char *path = host_path_w(PTR(0), 0);
    int ok = stat(path, &st) == 0;
    free(path);
    if (!ok) { SET_ERROR(error_from_errno()); RET(0); }
    uint64_t p = ARG(2);
    st32(p, attributes_of(&st));
    st64(p + 4, filetime(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec));
    st64(p + 12, filetime(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec));
    st64(p + 20, filetime(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec));
    st32(p + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    st32(p + 32, (uint32_t)st.st_size);
    RET(1);
}
IMPORT(CreateDirectoryW) {
    char *path = host_path_w(PTR(0), 1);
    int r = mkdir(path, 0755);
    if (r != 0) SET_ERROR(errno == EEXIST ? ERROR_ALREADY_EXISTS : error_from_errno());
    free(path);
    RET(r == 0);
}
IMPORT(DeleteFileW) {
    // Only files the game itself created (overlay, profile) can be deleted.
    char *path = host_path_w(PTR(0), 1);
    int r = unlink(path);
    if (r != 0) SET_ERROR(error_from_errno());
    free(path);
    RET(r == 0);
}
IMPORT(MoveFileExW) {
    char *from = host_path_w(PTR(0), 1), *to = host_path_w(PTR(1), 1);
    int r = rename(from, to);
    if (r != 0) SET_ERROR(error_from_errno());
    free(from); free(to);
    RET(r == 0);
}
IMPORT(CopyFileA) {
    char *from = host_path(PTR(0), 0), *to = host_path(PTR(1), 1);
    int ok = 0;
    if (!(ARG(2) && access(to, F_OK) == 0)) {  // bFailIfExists
        int in = open(from, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
        char buf[65536]; ssize_t n;
        ok = in >= 0 && out >= 0;
        while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
        if (in >= 0) close(in);
        if (out >= 0) close(out);
    }
    if (!ok) SET_ERROR(ERROR_FILE_NOT_FOUND);
    free(from); free(to);
    RET(ok);
}

// ------------------------------------------------------ current directory
static void to_backslashes(char *s) { for (; *s; s++) if (*s == '/') *s = '\\'; }
IMPORT(GetCurrentDirectoryW) {
    char win[1024];
    snprintf(win, sizeof win, "%s", guest_current_directory());
    to_backslashes(win);
    size_t need = utf8_to_u16(win, NULL, 0);
    if (need > ARG(0)) RET(need);
    utf8_to_u16(win, PTR(1), ARG(0));
    RET(need - 1);
}
IMPORT(SetCurrentDirectoryW) {
    char *utf8 = u16_to_utf8(PTR(0), -1);
    guest_set_current_directory(utf8);
    free(utf8);
    RET(1);
}
IMPORT(GetFullPathNameW) {
    char *utf8 = u16_to_utf8(PTR(0), -1), norm[2048];
    guest_normalize(utf8, norm, sizeof norm);
    free(utf8);
    to_backslashes(norm);
    size_t need = utf8_to_u16(norm, NULL, 0);
    if (need > ARG(1)) RET(need);
    uint16_t *out = PTR(2);
    utf8_to_u16(norm, out, ARG(1));
    if (ARG(3)) {  // lpFilePart: the final component
        uint16_t *last = out;
        for (uint16_t *p = out; *p; p++) if (*p == '\\') last = p + 1;
        st64(ARG(3), *last ? (uint64_t)last : 0);
    }
    RET(need - 1);
}
IMPORT(PathCanonicalizeA) {
    // Resolves "." and ".." without touching the file system. Relative input stays relative on Windows;
    // the game only passes absolute paths, which is what guest_normalize produces.
    char norm[2048];
    const char *in = PTR(1);
    guest_normalize(in, norm, sizeof norm);
    to_backslashes(norm);
    size_t n = strlen(norm);
    if (n && (in[strlen(in) - 1] == '\\' || in[strlen(in) - 1] == '/') && norm[n - 1] != '\\' && n + 1 < sizeof norm) {
        norm[n] = '\\'; norm[n + 1] = 0;  // a trailing separator is kept
    }
    if (strlen(norm) >= 260) RET(0);
    strcpy(PTR(0), norm);
    RET(1);
}
IMPORT(GetDriveTypeW) { RET(3); }  // DRIVE_FIXED
IMPORT(GetSystemDirectoryW) {
    size_t need = utf8_to_u16("C:\\Windows\\System32", NULL, 0);
    if (need > ARG(1)) RET(need);
    utf8_to_u16("C:\\Windows\\System32", PTR(0), ARG(1));
    RET(need - 1);
}
// -------------------------------------------------------- directory search
// WIN32_FIND_DATA: attributes, three times, size, two reserved, name, short name.
static int find_next(find_t *f, uint64_t out, int wide) {
    while (f->current < f->roots) {
        struct dirent *e = f->dir[f->current] ? readdir(f->dir[f->current]) : NULL;
        if (!e) { f->current++; continue; }
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD) != 0) continue;
        int dup = 0;
        for (int i = 0; i < f->seen_count; i++) dup |= strcasecmp(f->seen[i], e->d_name) == 0;
        if (dup) continue;  // the overlay's copy was already reported
        // A name from an earlier root hides the same name in later ones (the overlay over the game, the game over a DLC).
        if (f->current < f->roots - 1 && f->seen_count < 4096) snprintf(f->seen[f->seen_count++], 96, "%s", e->d_name);
        char full[4400];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dirpath[f->current], e->d_name);
        if (stat(full, &st) != 0) continue;
        memset((void *)out, 0, wide ? 592 : 320);
        st32(out, attributes_of(&st));
        st64(out + 4, filetime(st.st_birthtimespec.tv_sec, 0));
        st64(out + 12, filetime(st.st_atimespec.tv_sec, 0));
        st64(out + 20, filetime(st.st_mtimespec.tv_sec, 0));
        st32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        st32(out + 32, (uint32_t)st.st_size);
        if (wide) utf8_to_u16(e->d_name, (uint16_t *)(out + 44), 260);
        else snprintf((char *)(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
static uint64_t find_first(const char *spec, uint64_t out, int wide) {
    char dir[2048];
    static _Thread_local char roots[HOST_MAX_ROOTS][4096];
    snprintf(dir, sizeof dir, "%s", spec);
    char *slash = strrchr(dir, '\\'), *fwd = strrchr(dir, '/');
    if (fwd > slash) slash = fwd;
    find_t *f = calloc(1, sizeof *f);
    if (slash) { snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1); *slash = 0; }
    else { snprintf(f->pattern, sizeof f->pattern, "%s", dir); snprintf(dir, sizeof dir, "."); }
    if (!strcmp(f->pattern, "*.*")) snprintf(f->pattern, sizeof f->pattern, "*");
    f->roots = host_list_roots(dir, roots);
    int any = 0;
    for (int i = 0; i < f->roots; i++) {
        snprintf(f->dirpath[i], 4096, "%s", roots[i]);
        f->dir[i] = opendir(roots[i]);
        any |= f->dir[i] != NULL;
    }
    if (!any || !find_next(f, out, wide)) {
        for (int i = 0; i < f->roots; i++) if (f->dir[i]) closedir(f->dir[i]);
        free(f);
        SET_ERROR(any ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE;
    }
    return handle_alloc(HK_FIND, f);
}
IMPORT(FindFirstFileExW) {
    char *spec = u16_to_utf8(PTR(0), -1);
    uint64_t h = find_first(spec, ARG(2), 1);
    free(spec);
    RET(h);
}
IMPORT(FindFirstFileA) { RET(find_first(PTR(0), ARG(1), 0)); }
static uint64_t find_more(uint64_t h, uint64_t out, int wide) {
    find_t *f = handle_data(h, HK_FIND);
    if (!f) { SET_ERROR(ERROR_INVALID_HANDLE); return 0; }
    if (find_next(f, out, wide)) return 1;
    SET_ERROR(ERROR_NO_MORE_FILES);
    return 0;
}
IMPORT(FindNextFileW) { RET(find_more(ARG(0), ARG(1), 1)); }
IMPORT(FindNextFileA) { RET(find_more(ARG(0), ARG(1), 0)); }
IMPORT(FindClose) {
    find_t *f = handle_data(ARG(0), HK_FIND);
    if (f) {
        for (int k = 0; k < f->roots; k++) if (f->dir[k]) closedir(f->dir[k]);
        free(f);
        handle_release(ARG(0));
    }
    RET(1);
}
