// Time, machine description, version information, the registry and the few
// window-system calls WinMain makes before it creates a window.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

// --------------------------------------------------------------- time
// SYSTEMTIME: year, month, day of week, day, hour, minute, second, ms (16-bit each).
static void put_systemtime(uint64_t out, const struct tm *tm, long ms) {
    uint16_t v[8] = {(uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                     (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec, (uint16_t)ms};
    memcpy((void *)out, v, 16);
}
IMPORT(GetLocalTime) {
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    put_systemtime(ARG(0), &tm, ts.tv_nsec / 1000000);
}
IMPORT(GetSystemTime) {
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    put_systemtime(ARG(0), &tm, ts.tv_nsec / 1000000);
}
IMPORT(FileTimeToSystemTime) {
    uint64_t ft = ld64(ARG(0));
    time_t sec = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&sec, &tm);
    put_systemtime(ARG(1), &tm, (long)(ft % 10000000ull / 10000));
    RET(1);
}
IMPORT(SystemTimeToTzSpecificLocalTime) {
    uint16_t v[8];
    memcpy(v, PTR(1), 16);
    struct tm tm = {.tm_year = v[0] - 1900, .tm_mon = v[1] - 1, .tm_mday = v[3], .tm_hour = v[4], .tm_min = v[5], .tm_sec = v[6]};
    time_t t = timegm(&tm);
    localtime_r(&t, &tm);
    put_systemtime(ARG(2), &tm, v[7]);
    RET(1);
}
IMPORT(GetTimeZoneInformation) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    memset(PTR(0), 0, 172);
    st32(ARG(0), (uint32_t)(int32_t)(-(tm.tm_gmtoff - (tm.tm_isdst > 0 ? 3600 : 0)) / 60));  // Bias, minutes west of UTC
    st32(ARG(0) + 168, (uint32_t)-60);                                                     // DaylightBias
    RET(tm.tm_isdst > 0 ? 2 : 1);
}

// ------------------------------------------------------------ the machine
IMPORT(GetSystemInfo) {
    uint64_t p = ARG(0);
    memset((void *)p, 0, 48);
    st16(p, 9);                                 // PROCESSOR_ARCHITECTURE_AMD64, what the game was built for
    st32(p + 4, 4096);                          // page size
    st64(p + 8, 0x10000);                       // lowest application address
    st64(p + 16, 0x7ffffffeffffull);            // highest
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    st64(p + 24, cpus >= 64 ? ~0ull : (1ull << cpus) - 1);
    st32(p + 32, (uint32_t)cpus);
    st32(p + 36, 8664);
    st32(p + 40, 65536);                        // allocation granularity
}
IMPORT(GlobalMemoryStatusEx) {
    uint64_t p = ARG(0), total = 0;
    size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, NULL, 0);
    st32(p + 4, 50);                            // memory load, percent
    st64(p + 8, total);  st64(p + 16, total / 2);        // physical: total, available
    st64(p + 24, total); st64(p + 32, total / 2);        // page file
    st64(p + 40, 0x7fffffff0000ull); st64(p + 48, 0x7ffffff00000ull);  // virtual
    st64(p + 56, 0);
    RET(1);
}
IMPORT(K32GetProcessMemoryInfo) { memset(PTR(1), 0, (size_t)ARG(2)); st32(ARG(1), (uint32_t)ARG(2)); RET(1); }
// The game asks whether Windows is new enough. It is told yes.
IMPORT(VerSetConditionMask) { RET(ARG(0) | 1); }
IMPORT(VerifyVersionInfoW) { RET(1); }

// --------------------------------------------- version resource of the exe
// The game reads its own version number from its RT_VERSION resource.
static const uint8_t *version_resource(uint32_t *size) {
    const uint64_t base = 0x140000000ull;
    uint32_t rsrc = ld32(base + ld32(base + 0x3c) + 24 + 112 + 2 * 8);
    if (!rsrc) return NULL;
    uint64_t dir = base + rsrc;
    for (int level = 0; level < 3; level++) {  // type 16, then first name, then first language
        uint32_t entries = (uint32_t)ld16(dir + 12) + ld16(dir + 14), found = 0;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t id = ld32(dir + 16 + 8 * i), off = ld32(dir + 20 + 8 * i);
            if (level > 0 || id == 16) { found = off; break; }
        }
        if (!found) return NULL;
        if (!(found & 0x80000000u)) {
            uint64_t data = base + rsrc + found;
            *size = ld32(data + 4);
            return (const uint8_t *)(base + ld32(data));
        }
        dir = base + rsrc + (found & 0x7fffffffu);
    }
    return NULL;
}
IMPORT(GetFileVersionInfoSizeW) {
    uint32_t size = 0;
    if (ARG(1)) st32(ARG(1), 0);
    RET(version_resource(&size) ? size : 0);
}
IMPORT(GetFileVersionInfoA) {
    uint32_t size = 0;
    const uint8_t *res = version_resource(&size);
    if (!res) RET(0);
    memcpy(PTR(3), res, size < ARG(2) ? size : ARG(2));
    RET(1);
}
// A version resource is a tree of blocks: { length, value length, type,
// UTF-16 key, padding, value, padding, child blocks }.
static uint64_t align4(uint64_t base, uint64_t p) { return base + ((p - base + 3) & ~3ull); }
static uint64_t block_value(uint64_t base, uint64_t node) {
    return align4(base, node + 6 + (u16_len((const uint16_t *)(node + 6)) + 1) * 2);
}
static uint64_t block_child(uint64_t base, uint64_t node, const char *name, size_t name_len) {
    uint64_t end = node + ld16(node);
    uint32_t value_bytes = ld16(node + 2) * (ld16(node + 4) == 1 ? 2u : 1u);  // text values count characters
    uint64_t child = align4(base, block_value(base, node) + value_bytes);
    while (child + 6 <= end && ld16(child)) {
        const uint16_t *key = (const uint16_t *)(child + 6);
        size_t n = u16_len(key), i = 0;
        while (i < n && i < name_len && (key[i] | 0x20) == ((unsigned char)name[i] | 0x20)) i++;
        if (i == n && n == name_len) return child;
        child = align4(base, child + ld16(child));
    }
    return 0;
}
IMPORT(VerQueryValueA) {
    uint64_t base = ARG(0), node = base;
    const char *path = PTR(1);
    while (*path == '\\') path++;
    while (node && *path) {
        size_t n = strcspn(path, "\\");
        node = block_child(base, node, path, n);
        path += n;
        while (*path == '\\') path++;
    }
    if (!node) RET(0);
    uint64_t value = block_value(base, node);
    uint32_t count = ld16(node + 2);
    if (ld16(node + 4) == 1 && node != base) {
        // Text is stored as UTF-16; this is the ANSI call, so hand back a converted copy.
        char *text = u16_to_utf8((const uint16_t *)value, (long)count);
        size_t len = strlen(text);
        value = (uint64_t)text;         // lives as long as the process; a handful of small strings
        count = (uint32_t)len + 1;
    }
    st64(ARG(2), value);
    if (ARG(3)) st32(ARG(3), count);
    RET(1);
}

// ---------------------------------------------------------------- registry
// There is no registry. Opening a key fails, which the game treats as "not
// set"; creating one succeeds so that attempts to write settings are harmless.
IMPORT(RegOpenKeyExW) { RET(2); }       // ERROR_FILE_NOT_FOUND
IMPORT(RegQueryValueExW) { RET(2); }
IMPORT(RegEnumValueW) { RET(259); }     // ERROR_NO_MORE_ITEMS
IMPORT(RegCreateKeyExA) { if (ARG(7)) st64(ARG(7), 0x5000); RET(0); }
IMPORT(RegSetValueExA) { RET(0); }
IMPORT(RegCloseKey) { RET(0); }

// ------------------------------------------------------- before any window
IMPORT(FindWindowW) { RET(0); }         // no other instance is running
IMPORT(MessageBoxW) {
    char *text = u16_to_utf8(PTR(1), -1), *title = ARG(2) ? u16_to_utf8(PTR(2), -1) : NULL;
    host_alert(title ? title : "SpaceEngine", text);
    free(text); free(title);
    RET(1);                             // IDOK
}
IMPORT(MessageBoxA) {
    host_alert(ARG(2) ? (const char *)PTR(2) : "SpaceEngine", (const char *)PTR(1));
    RET(1);
}

// ------------------------------------------------------------- wsprintfW
// USER32's small formatter: %d %i %u %x %X %c %s %%, with flags, width,
// precision and the l / I64 size prefixes. Text is UTF-16; the result is
// limited to 1024 characters as on Windows.
IMPORT(wsprintfW) {
    uint16_t *out = PTR(0);
    const uint16_t *f = PTR(1);
    int n = 0, arg = 2;
    #define PUT(ch) do { if (n < 1023) out[n++] = (uint16_t)(ch); } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        if (*f == '%') { PUT('%'); continue; }
        int left = 0, zero = 0, width = 0, precision = -1, wide64 = 0;
        for (; *f == '-' || *f == '0' || *f == '#'; f++) { if (*f == '-') left = 1; if (*f == '0') zero = 1; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { precision = 0; f++; while (*f >= '0' && *f <= '9') precision = precision * 10 + (*f++ - '0'); }
        if (*f == 'I' && f[1] == '6' && f[2] == '4') { wide64 = 1; f += 3; }
        else if (*f == 'l' || *f == 'h' || *f == 'w') { if (*f == 'l' && f[1] == 'l') { wide64 = 1; f++; } f++; }
        uint64_t v = host_arg(c, arg++);
        uint16_t tmp[80];
        const uint16_t *text = tmp;
        int len = 0;
        if (*f == 's' || *f == 'S') {
            text = v ? (const uint16_t *)v : (const uint16_t *)u"(null)";
            len = (int)u16_len(text);
            if (precision >= 0 && len > precision) len = precision;
        } else if (*f == 'c' || *f == 'C') {
            tmp[0] = (uint16_t)v; len = 1;
        } else if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X') {
            char digits[40];
            if (*f == 'd' || *f == 'i') snprintf(digits, sizeof digits, "%lld", wide64 ? (long long)v : (long long)(int32_t)v);
            else if (*f == 'u') snprintf(digits, sizeof digits, "%llu", wide64 ? (unsigned long long)v : (unsigned long long)(uint32_t)v);
            else snprintf(digits, sizeof digits, *f == 'x' ? "%llx" : "%llX", wide64 ? (unsigned long long)v : (unsigned long long)(uint32_t)v);
            for (len = 0; digits[len]; len++) tmp[len] = (uint16_t)digits[len];
        } else {
            arg--;                       // not a conversion this formatter knows: copy it through
            PUT('%'); if (*f) PUT(*f); else break;
            continue;
        }
        int pad = width > len ? width - len : 0;
        if (!left) while (pad-- > 0) PUT(zero ? '0' : ' ');
        for (int i = 0; i < len; i++) PUT(text[i]);
        if (left) while (pad-- > 0) PUT(' ');
    }
    #undef PUT
    out[n] = 0;
    RET(n);
}
