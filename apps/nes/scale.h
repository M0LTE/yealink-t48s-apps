// SPDX-License-Identifier: AGPL-3.0-or-later
/* A scanline onto the screen at twice the size: 256 x 240 becomes 512 x 480,
 * which is the whole height of the phone's screen. Each pixel is looked up
 * once and written four times. */
#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>
#include <string.h>

struct scale_target {
    uint32_t *px;     /* where the picture's top left corner goes */
    int stride;       /* pixels from one row to the next */
};

static inline void scale_line_2x(void *ctx, int y, const uint8_t *idx, const uint32_t *rgb)
{
    const struct scale_target *t = ctx;
    uint32_t *d = t->px + (size_t)(y * 2) * (size_t)t->stride;
    for (int x = 0; x < 256; x += 4) {
        uint32_t a = rgb[idx[x]], b = rgb[idx[x + 1]], c = rgb[idx[x + 2]], e = rgb[idx[x + 3]];
        d[0] = a; d[1] = a; d[2] = b; d[3] = b;
        d[4] = c; d[5] = c; d[6] = e; d[7] = e;
        d += 8;
    }
    d -= 512;
    memcpy(d + t->stride, d, 512 * sizeof *d);
}

#endif
