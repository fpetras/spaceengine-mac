// libpng, bridged to the native library.
//
// The game gives libpng callbacks of its own for reading, writing and
// errors. Native libpng gets host functions instead, which call the game's
// callbacks as translated code.
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

typedef struct { uint64_t io, read, write, flush, error_data, error, warning; } bridge_t;

static bridge_t *bridge_of(png_structp png) { return png_get_error_ptr(png); }

static void on_read(png_structp png, png_bytep data, size_t length) {
    uint64_t args[3] = {(uint64_t)png, (uint64_t)data, length};
    host_call_guest(host_cpu, bridge_of(png)->read, 3, args);
}
static void on_write(png_structp png, png_bytep data, size_t length) {
    uint64_t args[3] = {(uint64_t)png, (uint64_t)data, length};
    host_call_guest(host_cpu, bridge_of(png)->write, 3, args);
}
static void on_flush(png_structp png) {
    uint64_t arg = (uint64_t)png;
    if (bridge_of(png)->flush) host_call_guest(host_cpu, bridge_of(png)->flush, 1, &arg);
}
static void on_warning(png_structp png, png_const_charp message) {
    uint64_t args[2] = {(uint64_t)png, (uint64_t)message};
    if (bridge_of(png)->warning) host_call_guest(host_cpu, bridge_of(png)->warning, 2, args);
}
// libpng requires its error function not to return. The game's own handler
// leaves by throwing, which translated code cannot do yet, so a PNG error
// ends the run with the message.
static void on_error(png_structp png, png_const_charp message) {
    (void)png;
    fprintf(stderr, "host: libpng error: %s\n", message);
    exit(73);
}

static bridge_t *new_bridge(cpu_t *c) {
    bridge_t *b = calloc(1, sizeof *b);
    b->error_data = ARG(1); b->error = ARG(2); b->warning = ARG(3);
    return b;
}
// The custom allocator the game passes (arguments 4 to 6) is not used: libpng's own is.
IMPORT(png_create_read_struct_2) { RET(png_create_read_struct(PNG_LIBPNG_VER_STRING, new_bridge(c), on_error, on_warning)); }
IMPORT(png_create_write_struct_2) { RET(png_create_write_struct(PNG_LIBPNG_VER_STRING, new_bridge(c), on_error, on_warning)); }
IMPORT(png_destroy_read_struct) {
    png_structpp png = PTR(0);
    bridge_t *b = png && *png ? bridge_of(*png) : NULL;
    png_destroy_read_struct(png, PTR(1), PTR(2));
    free(b);
}
IMPORT(png_destroy_write_struct) {
    png_structpp png = PTR(0);
    bridge_t *b = png && *png ? bridge_of(*png) : NULL;
    png_destroy_write_struct(png, PTR(1));
    free(b);
}
IMPORT(png_set_read_fn) {
    bridge_t *b = bridge_of(PTR(0));
    b->io = ARG(1); b->read = ARG(2);
    png_set_read_fn(PTR(0), b, on_read);
}
IMPORT(png_set_write_fn) {
    bridge_t *b = bridge_of(PTR(0));
    b->io = ARG(1); b->write = ARG(2); b->flush = ARG(3);
    png_set_write_fn(PTR(0), b, on_write, on_flush);
}
IMPORT(png_get_io_ptr) { RET(bridge_of(PTR(0))->io); }

IMPORT(png_sig_cmp) { RET((uint32_t)png_sig_cmp(PTR(0), ARG(1), ARG(2))); }
IMPORT(png_create_info_struct) { RET(png_create_info_struct(PTR(0))); }
IMPORT(png_set_sig_bytes) { png_set_sig_bytes(PTR(0), (int)ARG(1)); }
IMPORT(png_read_info) { png_read_info(PTR(0), PTR(1)); }
IMPORT(png_get_IHDR) {
    uint32_t r = png_get_IHDR(PTR(0), PTR(1), PTR(2), PTR(3), PTR(4), PTR(5), PTR(6), PTR(7), PTR(8));
    if (getenv("SE_VERBOSE") && ARG(2) && ARG(4) && ARG(5))
        fprintf(stderr, "host: png %ux%u, %d bits, colour type %d\n", *(uint32_t *)PTR(2), *(uint32_t *)PTR(3), *(int *)PTR(4), *(int *)PTR(5));
    RET(r);
}
IMPORT(png_get_channels) { RET(png_get_channels(PTR(0), PTR(1))); }
IMPORT(png_read_image) { png_read_image(PTR(0), PTR(1)); }
IMPORT(png_read_end) { png_read_end(PTR(0), PTR(1)); }
IMPORT(png_set_IHDR) {
    png_set_IHDR(PTR(0), PTR(1), (png_uint_32)ARG(2), (png_uint_32)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7), (int)ARG(8));
}
IMPORT(png_write_info) { png_write_info(PTR(0), PTR(1)); }
IMPORT(png_write_row) { png_write_row(PTR(0), PTR(1)); }
IMPORT(png_write_end) { png_write_end(PTR(0), PTR(1)); }

// Writes bottom-up RGBA pixels (as glReadPixels returns them) to a PNG file.
// Used only by the screenshot debugging aid.
void host_write_png(const char *path, const uint8_t *rgba, int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_bytep row = malloc((size_t)width * 3);
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) memcpy(row + 3 * x, rgba + ((size_t)y * (size_t)width + (size_t)x) * 4, 3);
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}
