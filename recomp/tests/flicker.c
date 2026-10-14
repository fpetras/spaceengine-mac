// Finds flicker in consecutive frames: pixels whose brightness goes up and
// down repeatedly by a large step, rather than drifting. Prints how many and
// where, and writes a copy of the first frame with them marked in red.
//
// usage: flicker <mask.png> <frame0.png> <frame1.png> ...
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *path, int *w, int *h) {
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path)) return NULL;
    im.format = PNG_FORMAT_RGBA;
    unsigned char *buf = malloc(PNG_IMAGE_SIZE(im));
    if (!png_image_finish_read(&im, NULL, buf, 0, NULL)) return NULL;
    *w = (int)im.width; *h = (int)im.height;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: flicker <mask.png> <frame0.png> <frame1.png> <frame2.png>...\n"); return 2; }
    int n = argc - 2, w = 0, h = 0;
    unsigned char **f = calloc((size_t)n, sizeof *f);
    for (int i = 0; i < n; i++) {
        int fw, fh;
        f[i] = load(argv[2 + i], &fw, &fh);
        if (!f[i] || (i && (fw != w || fh != h))) { fprintf(stderr, "cannot read %s\n", argv[2 + i]); return 1; }
        w = fw; h = fh;
    }
    const int step = getenv("FLICKER_STEP") ? atoi(getenv("FLICKER_STEP")) : 40;
    long flicker = 0, minx = w, miny = h, maxx = -1, maxy = -1;
    unsigned char *mask = malloc((size_t)w * (size_t)h * 4);
    memcpy(mask, f[0], (size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t o = ((size_t)y * (size_t)w + (size_t)x) * 4;
        int prev = -1, dir = 0, turns = 0;
        for (int i = 0; i < n; i++) {
            int l = (f[i][o] * 30 + f[i][o + 1] * 59 + f[i][o + 2] * 11) / 100;
            if (prev >= 0 && abs(l - prev) >= step) {
                int d = l > prev ? 1 : -1;
                if (dir && d != dir) turns++;
                dir = d;
            }
            prev = l;
        }
        if (turns >= 2) {
            flicker++;
            static int shown;
            // FLICKER_SAMPLES=n prints the brightness series of every 997th flickering pixel, up to n of them.
            if (getenv("FLICKER_SAMPLES") && flicker % 997 == 0 && shown < atoi(getenv("FLICKER_SAMPLES"))) {
                shown++;
                printf("  (%d,%d):", x, y);
                for (int i = 0; i < n; i++) printf(" %d", (f[i][o] * 30 + f[i][o + 1] * 59 + f[i][o + 2] * 11) / 100);
                printf("\n");
            }
            if (x < minx) minx = x; if (y < miny) miny = y; if (x > maxx) maxx = x; if (y > maxy) maxy = y;
            mask[o] = 255; mask[o + 1] = 0; mask[o + 2] = 0; mask[o + 3] = 255;
        }
    }
    // Single-frame spikes: a pixel that differs strongly from the frames on
    // both sides while those two agree. Counted per frame, a glitch frame
    // stands out even when the whole picture is moving.
    for (int i = 1; i + 1 < n; i++) {
        long spikes = 0;
        for (size_t o = 0; o < (size_t)w * (size_t)h * 4; o += 4) {
            int a0 = (f[i - 1][o] * 30 + f[i - 1][o + 1] * 59 + f[i - 1][o + 2] * 11) / 100;
            int a1 = (f[i][o] * 30 + f[i][o + 1] * 59 + f[i][o + 2] * 11) / 100;
            int a2 = (f[i + 1][o] * 30 + f[i + 1][o + 1] * 59 + f[i + 1][o + 2] * 11) / 100;
            if (abs(a1 - a0) >= step && abs(a1 - a2) >= step && abs(a0 - a2) < step / 3) {
                spikes++;
                if (getenv("FLICKER_WHERE") && spikes <= 6) printf("    (%zu,%zu) %d -> %d -> %d\n", (o / 4) % (size_t)w, (o / 4) / (size_t)w, a0, a1, a2);
            }
        }
        if (spikes) printf("frame %d: %ld single-frame spike pixels\n", i, spikes);
    }
    printf("%d frames %dx%d: %ld flickering pixels", n, w, h, flicker);
    if (flicker) printf(", within x %ld..%ld, y %ld..%ld", minx, maxx, miny, maxy);
    printf("\n");
    png_image out;
    memset(&out, 0, sizeof out);
    out.version = PNG_IMAGE_VERSION; out.width = (png_uint_32)w; out.height = (png_uint_32)h; out.format = PNG_FORMAT_RGBA;
    png_image_write_to_file(&out, argv[1], 0, mask, 0, NULL);
    return 0;
}
