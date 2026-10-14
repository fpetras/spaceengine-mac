// libtiff, bridged to the native library.
//
// The game reads TIFF data through its own I/O callbacks (the data usually
// comes out of an archive), so TIFFClientOpen gets host callbacks that call
// the game's as translated code. TIFFGetField and TIFFSetField take
// arguments whose types depend on the tag.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <tiffio.h>

#include "host.h"
#include "paths.h"

typedef struct { uint64_t data, read, write, seek, close, size; } client_t;

static tmsize_t on_read(thandle_t h, void *buf, tmsize_t n) {
    client_t *k = h;
    uint64_t args[3] = {k->data, (uint64_t)buf, (uint64_t)n};
    return (tmsize_t)(int64_t)host_call_guest(host_cpu, k->read, 3, args);
}
static tmsize_t on_write(thandle_t h, void *buf, tmsize_t n) {
    client_t *k = h;
    uint64_t args[3] = {k->data, (uint64_t)buf, (uint64_t)n};
    return (tmsize_t)(int64_t)host_call_guest(host_cpu, k->write, 3, args);
}
static toff_t on_seek(thandle_t h, toff_t offset, int whence) {
    client_t *k = h;
    uint64_t args[3] = {k->data, offset, (uint64_t)whence};
    return host_call_guest(host_cpu, k->seek, 3, args);
}
static int on_close(thandle_t h) {
    client_t *k = h;
    int r = k->close ? (int)host_call_guest(host_cpu, k->close, 1, &k->data) : 0;
    free(k);
    return r;
}
static toff_t on_size(thandle_t h) {
    client_t *k = h;
    return k->size ? host_call_guest(host_cpu, k->size, 1, &k->data) : 0;
}
static void quiet(const char *module, const char *format, va_list args) { (void)module; (void)format; (void)args; }

// The game's handler would be called by native code; libtiff's messages are
// dropped instead. Failures still show up as return values.
IMPORT(TIFFSetErrorHandler) { TIFFSetErrorHandler(quiet); TIFFSetWarningHandler(quiet); RET(0); }
IMPORT(TIFFClientOpen) {
    client_t *k = calloc(1, sizeof *k);
    k->data = ARG(2); k->read = ARG(3); k->write = host_arg(c, 4); k->seek = host_arg(c, 5);
    k->close = host_arg(c, 6); k->size = host_arg(c, 7);
    TIFF *t = TIFFClientOpen(PTR(0), PTR(1), k, on_read, on_write, on_seek, on_close, on_size, NULL, NULL);
    if (!t) free(k);
    RET(t);
}
IMPORT(TIFFOpen) {
    const char *mode = PTR(1);
    char *path = host_path(PTR(0), mode[0] != 'r');
    TIFF *t = TIFFOpen(path, mode);
    free(path);
    RET(t);
}
IMPORT(TIFFClose) { TIFFClose(PTR(0)); }
IMPORT(TIFFScanlineSize) { RET(TIFFScanlineSize(PTR(0))); }
IMPORT(TIFFReadScanline) { RET((uint32_t)TIFFReadScanline(PTR(0), PTR(1), (uint32_t)ARG(2), (uint16_t)ARG(3))); }
IMPORT(TIFFWriteScanline) { RET((uint32_t)TIFFWriteScanline(PTR(0), PTR(1), (uint32_t)ARG(2), (uint16_t)ARG(3))); }
IMPORT(TIFFDefaultStripSize) { RET(TIFFDefaultStripSize(PTR(0), (uint32_t)ARG(1))); }
// Every tag the game reads takes pointers to where the values go.
IMPORT(TIFFGetField) { RET((uint32_t)TIFFGetField(PTR(0), (uint32_t)ARG(1), PTR(2), PTR(3), PTR(4))); }
IMPORT(TIFFSetField) {
    TIFF *t = PTR(0);
    uint32_t tag = (uint32_t)ARG(1);
    switch (tag) {
        case TIFFTAG_XRESOLUTION: case TIFFTAG_YRESOLUTION:   // a float, passed as a double
            RET((uint32_t)TIFFSetField(t, tag, c->x[2].f64[0]));
        case TIFFTAG_EXTRASAMPLES:                            // a count and an array
            RET((uint32_t)TIFFSetField(t, tag, (int)ARG(2), PTR(3)));
        case TIFFTAG_SOFTWARE: case TIFFTAG_IMAGEDESCRIPTION: case TIFFTAG_DATETIME: case TIFFTAG_ARTIST:
            RET((uint32_t)TIFFSetField(t, tag, (const char *)PTR(2)));
        default:                                              // a 16- or 32-bit integer
            RET((uint32_t)TIFFSetField(t, tag, (uint32_t)ARG(2)));
    }
}
