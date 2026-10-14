#include <stdlib.h>

#include "host.h"

size_t u16_len(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *u16_to_utf8(const uint16_t *s, long count) {
    size_t n = count < 0 ? u16_len(s) : (size_t)count;
    char *out = malloc(n * 3 + 1), *p = out;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (cp < 0x80) *p++ = (char)cp;
        else if (cp < 0x800) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | (cp >> 6 & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
        else { *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | (cp >> 12 & 0x3f)); *p++ = (char)(0x80 | (cp >> 6 & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
    }
    *p = 0;
    return out;
}

size_t utf8_to_u16(const char *s, uint16_t *out, size_t capacity) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xe0) == 0xc0 && p[1]) { cp = (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f); p += 2; }
        else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) { cp = (uint32_t)(*p & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f); p += 3; }
        else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { cp = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f); p += 4; }
        else { cp = 0xfffd; p++; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (out && n + 1 < capacity) { out[n] = (uint16_t)(0xd800 + (cp >> 10)); out[n + 1] = (uint16_t)(0xdc00 + (cp & 0x3ff)); }
            n += 2;
        } else {
            if (out && n < capacity) out[n] = (uint16_t)cp;
            n++;
        }
    }
    if (out && n < capacity) out[n] = 0;
    return n + 1;
}
