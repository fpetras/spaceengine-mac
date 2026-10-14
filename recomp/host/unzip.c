// The minizip "unz" interface the game imports from zlibwapi.dll, implemented
// on the system zlib. The game uses it to read its .pak archives, which are
// ZIP files; it supplies whatever a member needs to be opened, exactly as it
// does on Windows.
//
// Structure layouts are the Windows ones: `uLong` is 32 bits there.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "host.h"
#include "paths.h"

enum { UNZ_OK = 0, UNZ_END_OF_LIST = -100, UNZ_ERRNO = -1, UNZ_PARAMERROR = -102, UNZ_BADZIPFILE = -103,
       UNZ_INTERNALERROR = -104, UNZ_CRCERROR = -105 };

typedef struct {
    uint16_t made_by, needed, flags, method, time, date;
    uint32_t crc, compressed, uncompressed;
    uint16_t name_len, extra_len, comment_len, disk, internal_attr;
    uint32_t external_attr, local_offset;
    uint64_t directory_offset;       // of this entry
} entry_t;

typedef struct {
    int fd;
    uint64_t directory, count, index, entry_offset;
    entry_t entry;
    int entry_valid;
    // the member currently open for reading
    int open, encrypted, stream_ready;
    z_stream stream;
    uint64_t in_offset, in_left, out_done;
    uint32_t crc, keys[3];
    uint8_t buffer[65536];
} archive_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int load_entry(archive_t *a) {
    uint8_t h[46];
    a->entry_valid = 0;
    if (a->index >= a->count) return UNZ_END_OF_LIST;
    if (pread(a->fd, h, 46, (off_t)a->entry_offset) != 46 || rd32(h) != 0x02014b50) return UNZ_BADZIPFILE;
    entry_t *e = &a->entry;
    e->made_by = rd16(h + 4); e->needed = rd16(h + 6); e->flags = rd16(h + 8); e->method = rd16(h + 10);
    e->time = rd16(h + 12); e->date = rd16(h + 14); e->crc = rd32(h + 16);
    e->compressed = rd32(h + 20); e->uncompressed = rd32(h + 24);
    e->name_len = rd16(h + 28); e->extra_len = rd16(h + 30); e->comment_len = rd16(h + 32);
    e->disk = rd16(h + 34); e->internal_attr = rd16(h + 36); e->external_attr = rd32(h + 38);
    e->local_offset = rd32(h + 42);
    e->directory_offset = a->entry_offset;
    a->entry_valid = 1;
    return UNZ_OK;
}

IMPORT(unzOpen) {
    char *path = host_path(PTR(0), 0);
    int fd = open(path, O_RDONLY);
    free(path);
    if (fd < 0) RET(0);
    off_t size = lseek(fd, 0, SEEK_END);
    // The end-of-central-directory record is within the last 64 KB.
    size_t tail = size < 65557 ? (size_t)size : 65557;
    uint8_t *buf = malloc(tail);
    archive_t *a = NULL;
    if (pread(fd, buf, tail, size - (off_t)tail) == (ssize_t)tail)
        for (long i = (long)tail - 22; i >= 0; i--)
            if (rd32(buf + i) == 0x06054b50) {
                a = calloc(1, sizeof *a);
                a->fd = fd;
                a->count = rd16(buf + i + 10);
                a->directory = rd32(buf + i + 16);
                if (a->count == 0xffff || a->directory == 0xffffffffu) {   // ZIP64: the real numbers are in a second record
                    uint8_t loc[20], rec[56];
                    off_t at = size - (off_t)tail + i - 20;
                    if (at >= 0 && pread(fd, loc, 20, at) == 20 && rd32(loc) == 0x07064b50 &&
                        pread(fd, rec, 56, (off_t)rd64(loc + 8)) == 56 && rd32(rec) == 0x06064b50) {
                        a->count = rd64(rec + 32);
                        a->directory = rd64(rec + 48);
                    }
                }
                break;
            }
    free(buf);
    if (!a) { close(fd); RET(0); }
    a->entry_offset = a->directory;
    load_entry(a);
    RET(a);
}
static void close_member(archive_t *a) {
    if (a->stream_ready) { inflateEnd(&a->stream); a->stream_ready = 0; }
    a->open = 0;
}
IMPORT(unzClose) {
    archive_t *a = PTR(0);
    if (!a) RET(UNZ_PARAMERROR);
    close_member(a);
    close(a->fd);
    free(a);
    RET(UNZ_OK);
}
IMPORT(unzGetGlobalInfo) {
    archive_t *a = PTR(0);
    if (!a) RET(UNZ_PARAMERROR);
    st32(ARG(1), (uint32_t)a->count); st32(ARG(1) + 4, 0);
    RET(UNZ_OK);
}
IMPORT(unzGoToFirstFile) {
    archive_t *a = PTR(0);
    if (!a) RET(UNZ_PARAMERROR);
    a->index = 0; a->entry_offset = a->directory;
    RET((uint32_t)load_entry(a));
}
IMPORT(unzGoToNextFile) {
    archive_t *a = PTR(0);
    if (!a || !a->entry_valid) RET((uint32_t)UNZ_END_OF_LIST);
    a->entry_offset += 46u + a->entry.name_len + a->entry.extra_len + a->entry.comment_len;
    a->index++;
    RET((uint32_t)load_entry(a));
}
// unz_file_pos: offset of the entry in the directory, then its number.
IMPORT(unzGetFilePos) {
    archive_t *a = PTR(0);
    if (!a || !a->entry_valid) RET((uint32_t)UNZ_END_OF_LIST);
    st32(ARG(1), (uint32_t)(a->entry_offset - a->directory)); st32(ARG(1) + 4, (uint32_t)a->index);
    RET(UNZ_OK);
}
IMPORT(unzGoToFilePos) {
    archive_t *a = PTR(0);
    if (!a) RET(UNZ_PARAMERROR);
    a->entry_offset = a->directory + ld32(ARG(1)); a->index = ld32(ARG(1) + 4);
    RET((uint32_t)load_entry(a));
}
// unz_file_info: fourteen 32-bit fields, then the date as six more.
IMPORT(unzGetCurrentFileInfo) {
    archive_t *a = PTR(0);
    if (!a || !a->entry_valid) RET((uint32_t)UNZ_PARAMERROR);
    entry_t *e = &a->entry;
    if (ARG(1)) {
        uint32_t dos = (uint32_t)e->date << 16 | e->time;
        uint32_t v[20] = {e->made_by, e->needed, e->flags, e->method, dos, e->crc, e->compressed, e->uncompressed,
                          e->name_len, e->extra_len, e->comment_len, e->disk, e->internal_attr, e->external_attr,
                          (e->time & 31u) * 2, e->time >> 5 & 63u, e->time >> 11, e->date & 31u,
                          (e->date >> 5 & 15u) - 1, (e->date >> 9) + 1980u};
        memcpy(PTR(1), v, sizeof v);
    }
    if (ARG(2) && ARG(3)) {
        size_t want = e->name_len < ARG(3) ? e->name_len : ARG(3);
        if (pread(a->fd, PTR(2), want, (off_t)(e->directory_offset + 46)) != (ssize_t)want) RET((uint32_t)UNZ_ERRNO);
        if (want < ARG(3)) ((char *)PTR(2))[want] = 0;
    }
    if (ARG(4) && ARG(5)) {
        size_t want = e->extra_len < ARG(5) ? e->extra_len : ARG(5);
        if (pread(a->fd, PTR(4), want, (off_t)(e->directory_offset + 46 + e->name_len)) != (ssize_t)want) RET((uint32_t)UNZ_ERRNO);
    }
    if (ARG(6) && host_arg(c, 7)) ((char *)PTR(6))[0] = 0;
    RET(UNZ_OK);
}

// ------------------------------------------------- the ZIP stream cipher
// The ZIP format's original password scheme, as the format's public
// specification describes it: three 32-bit keys updated per byte.
static void keys_update(uint32_t *k, uint8_t byte) {
    const z_crc_t *table = get_crc_table();
    k[0] = (uint32_t)table[(k[0] ^ byte) & 0xff] ^ (k[0] >> 8);
    k[1] = (k[1] + (k[0] & 0xff)) * 134775813u + 1;
    k[2] = (uint32_t)table[(k[2] ^ (k[1] >> 24)) & 0xff] ^ (k[2] >> 8);
}
static uint8_t keys_byte(const uint32_t *k) {
    uint32_t t = (k[2] & 0xffff) | 2;
    return (uint8_t)((t * (t ^ 1)) >> 8);
}
static void decrypt(archive_t *a, uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        data[i] ^= keys_byte(a->keys);
        keys_update(a->keys, data[i]);
    }
}

static int open_member(archive_t *a, const char *password) {
    if (!a || !a->entry_valid) return UNZ_PARAMERROR;
    close_member(a);
    entry_t *e = &a->entry;
    uint8_t local[30];
    if (pread(a->fd, local, 30, (off_t)e->local_offset) != 30 || rd32(local) != 0x04034b50) return UNZ_BADZIPFILE;
    if (e->method != 0 && e->method != 8) return UNZ_BADZIPFILE;
    a->in_offset = (uint64_t)e->local_offset + 30 + rd16(local + 26) + rd16(local + 28);
    a->in_left = e->compressed;
    a->out_done = 0;
    a->crc = (uint32_t)crc32(0, NULL, 0);
    a->encrypted = e->flags & 1;
    if (a->encrypted) {
        if (!password) return UNZ_PARAMERROR;
        a->keys[0] = 305419896u; a->keys[1] = 591751049u; a->keys[2] = 878082192u;
        for (const char *p = password; *p; p++) keys_update(a->keys, (uint8_t)*p);
        uint8_t header[12];
        if (a->in_left < 12 || pread(a->fd, header, 12, (off_t)a->in_offset) != 12) return UNZ_BADZIPFILE;
        decrypt(a, header, 12);
        a->in_offset += 12; a->in_left -= 12;
    }
    if (e->method == 8) {
        memset(&a->stream, 0, sizeof a->stream);
        if (inflateInit2(&a->stream, -MAX_WBITS) != Z_OK) return UNZ_INTERNALERROR;
        a->stream_ready = 1;
    }
    a->open = 1;
    return UNZ_OK;
}
IMPORT(unzOpenCurrentFile) { RET((uint32_t)open_member(PTR(0), NULL)); }
IMPORT(unzOpenCurrentFilePassword) { RET((uint32_t)open_member(PTR(0), PTR(1))); }

static size_t fill(archive_t *a) {
    size_t n = a->in_left < sizeof a->buffer ? (size_t)a->in_left : sizeof a->buffer;
    if (n == 0) return 0;
    ssize_t got = pread(a->fd, a->buffer, n, (off_t)a->in_offset);
    if (got <= 0) return 0;
    a->in_offset += (uint64_t)got; a->in_left -= (uint64_t)got;
    if (a->encrypted) decrypt(a, a->buffer, (size_t)got);
    return (size_t)got;
}
IMPORT(unzReadCurrentFile) {
    archive_t *a = PTR(0);
    uint8_t *out = PTR(1);
    size_t want = (uint32_t)ARG(2), done = 0;
    if (!a || !a->open) RET((uint32_t)UNZ_PARAMERROR);
    uint64_t remaining = a->entry.uncompressed - a->out_done;
    if (want > remaining) want = (size_t)remaining;
    if (a->entry.method == 0) {
        while (done < want) {
            if (a->stream.avail_in == 0) { a->stream.next_in = a->buffer; a->stream.avail_in = (uInt)fill(a); if (!a->stream.avail_in) break; }
            size_t n = want - done < a->stream.avail_in ? want - done : a->stream.avail_in;
            memcpy(out + done, a->stream.next_in, n);
            a->stream.next_in += n; a->stream.avail_in -= (uInt)n;
            done += n;
        }
    } else {
        a->stream.next_out = out;
        a->stream.avail_out = (uInt)want;
        while (a->stream.avail_out) {
            if (a->stream.avail_in == 0) { a->stream.next_in = a->buffer; a->stream.avail_in = (uInt)fill(a); }
            int r = inflate(&a->stream, Z_SYNC_FLUSH);
            if (r == Z_STREAM_END) break;
            if (r != Z_OK) RET((uint32_t)(r == Z_BUF_ERROR ? UNZ_ERRNO : r));
        }
        done = want - a->stream.avail_out;
    }
    a->crc = (uint32_t)crc32(a->crc, out, (uInt)done);
    a->out_done += done;
    RET(done);
}
IMPORT(unztell) { archive_t *a = PTR(0); RET(a && a->open ? (uint32_t)a->out_done : (uint32_t)UNZ_PARAMERROR); }
IMPORT(unzCloseCurrentFile) {
    archive_t *a = PTR(0);
    if (!a || !a->open) RET((uint32_t)UNZ_PARAMERROR);
    int bad = a->out_done == a->entry.uncompressed && a->crc != a->entry.crc;
    memset(&a->stream.next_in, 0, sizeof a->stream.next_in);
    a->stream.avail_in = 0;
    close_member(a);
    RET((uint32_t)(bad ? UNZ_CRCERROR : UNZ_OK));
}
