// SPDX-License-Identifier: AGPL-3.0-or-later
/* A scanline onto the screen. At twice the size 256 x 240 becomes 512 x 480,
 * the whole height of the T48S's screen: each pixel is looked up once and
 * written four times. On a smaller screen (the T46S's 480 x 272) it goes on as
 * it is, one pixel for one, which is also a quarter of the writing. */
#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>
#include <string.h>

struct scale_target {
    uint32_t *px;     /* where the picture's top left corner goes */
    int stride;       /* pixels from one row to the next */
    /* Optional: what each line of px shows already (240 x 256 indexes and
     * 240 x 32 colours). A line that would come out the same is not written
     * again: on the phone the writing costs more than the emulation. */
    uint8_t *seen;
    uint32_t *seen_rgb;
    int lo, hi;       /* the lines written since the caller last set lo over hi */
};

#define SCALE_SEEN_BYTES (240 * 256)
#define SCALE_SEEN_RGB_BYTES (240 * 32 * sizeof(uint32_t))

static inline void scale_line_2x(void *ctx, int y, const uint8_t *idx, const uint32_t *rgb)
{
    struct scale_target *t = ctx;
    uint32_t *d = t->px + (size_t)(y * 2) * (size_t)t->stride;
    if (t->seen) {
        uint8_t *was = t->seen + y * 256;
        uint32_t *was_rgb = t->seen_rgb + y * 32;
        if (!memcmp(was, idx, 256) && !memcmp(was_rgb, rgb, 32 * sizeof *rgb)) return;
        memcpy(was, idx, 256);
        memcpy(was_rgb, rgb, 32 * sizeof *rgb);
    }
    if (y < t->lo) t->lo = y;
    if (y > t->hi) t->hi = y;
    for (int x = 0; x < 256; x += 4) {
        uint32_t a = rgb[idx[x]], b = rgb[idx[x + 1]], c = rgb[idx[x + 2]], e = rgb[idx[x + 3]];
        d[0] = a; d[1] = a; d[2] = b; d[3] = b;
        d[4] = c; d[5] = c; d[6] = e; d[7] = e;
        d += 8;
    }
    d -= 512;
    memcpy(d + t->stride, d, 512 * sizeof *d);
}

static inline void scale_line_1x(void *ctx, int y, const uint8_t *idx, const uint32_t *rgb)
{
    struct scale_target *t = ctx;
    uint32_t *d = t->px + (size_t)y * (size_t)t->stride;
    if (t->seen) {
        uint8_t *was = t->seen + y * 256;
        uint32_t *was_rgb = t->seen_rgb + y * 32;
        if (!memcmp(was, idx, 256) && !memcmp(was_rgb, rgb, 32 * sizeof *rgb)) return;
        memcpy(was, idx, 256);
        memcpy(was_rgb, rgb, 32 * sizeof *rgb);
    }
    if (y < t->lo) t->lo = y;
    if (y > t->hi) t->hi = y;
    for (int x = 0; x < 256; x += 4) {
        d[0] = rgb[idx[x]]; d[1] = rgb[idx[x + 1]]; d[2] = rgb[idx[x + 2]]; d[3] = rgb[idx[x + 3]];
        d += 4;
    }
}

#endif
