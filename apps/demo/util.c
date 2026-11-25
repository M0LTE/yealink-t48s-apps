// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: tables, fixed-point maths, fills, palettes and text bitmaps. */
#include <math.h>
#include "demo.h"

static int16_t sintab[1025];     /* Q14, one turn in 1024 steps */
static uint16_t atantab[1025];   /* atan(i / 1024), 65536 a turn */

void util_init(void)
{
    /* a quarter of the sines by libm, the rest by symmetry: soft float is slow on the phone */
    for (int i = 0; i <= 256; i++) {
        int v = (int)lrint(sin(i * (M_PI / 512.0)) * 16384.0);
        sintab[i] = sintab[512 - i] = (int16_t)v;
        sintab[512 + i] = sintab[1024 - i] = (int16_t)-v;
    }
    for (int i = 0; i <= 1024; i++) atantab[i] = (uint16_t)lrint(atan(i / 1024.0) * (32768.0 / M_PI));
}

int fsin(unsigned a)
{
    unsigned i = (a >> 6) & 1023, f = a & 63;
    return sintab[i] + (((sintab[i + 1] - sintab[i]) * (int)f) >> 6);
}

unsigned isqrt(unsigned v)
{
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

unsigned iatan2(int y, int x)
{
    unsigned ax = (unsigned)(x < 0 ? -x : x), ay = (unsigned)(y < 0 ? -y : y), a;
    while (ax > 0xffff || ay > 0xffff) { ax >>= 1; ay >>= 1; }
    if (!ax && !ay) return 0;
    if (ax >= ay) {
        unsigned q = (ay << 14) / ax, i = q >> 4, f = q & 15;
        a = atantab[i] + (((atantab[i + (i < 1024)] - atantab[i]) * f) >> 4);
    } else {
        unsigned q = (ax << 14) / ay, i = q >> 4, f = q & 15;
        a = 16384 - (atantab[i] + (((atantab[i + (i < 1024)] - atantab[i]) * f) >> 4));
    }
    if (x < 0) a = 32768 - a;
    if (y < 0) a = 65536 - a;
    return a & 0xffff;
}

unsigned rnd(void)
{
    static unsigned s = 0x2545f491;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

int envelope(int t, int len, int in_ms, int out_ms)
{
    int a = 256;
    if (in_ms > 0 && t < in_ms) a = t * 256 / in_ms;
    if (out_ms > 0 && t > len - out_ms) { int b = (len - t) * 256 / out_ms; if (b < a) a = b; }
    return clampi(a, 0, 256);
}

void fill32(uint32_t *p, uint32_t c, int n)
{
    while (n >= 8) {
        p[0] = c; p[1] = c; p[2] = c; p[3] = c; p[4] = c; p[5] = c; p[6] = c; p[7] = c;
        p += 8; n -= 8;
    }
    while (n-- > 0) *p++ = c;
}

void clear_rect(t48_surface *s, rect r, uint32_t c)
{
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > s->w) r.x1 = s->w;
    if (r.y1 > s->h) r.y1 = s->h;
    if (r.x1 <= r.x0) return;
    for (int y = r.y0; y < r.y1; y++) fill32(s->px + y * s->stride + r.x0, c, r.x1 - r.x0);
}

void pal_fade(uint32_t *dst, const uint32_t *src, int n, int fade)
{
    if (fade >= 256) { memcpy(dst, src, (size_t)n * 4); return; }
    for (int i = 0; i < n; i++) dst[i] = scale32(src[i], (unsigned)fade);
}

void pal_ramp(uint32_t *dst, int n, const uint32_t *stops, int nstops)
{
    for (int i = 0; i < n; i++) {
        int p = i * (nstops - 1) * 256 / (n - 1), k = p >> 8;
        if (k >= nstops - 1) { dst[i] = stops[nstops - 1]; continue; }
        dst[i] = mix32(stops[k], stops[k + 1], (unsigned)(p & 255));
    }
}

void text8(bitmap8 *b, const t48_font *f, const char *str, int twice)
{
    int w = 0, h = f->height;
    for (const char *p = str; *p; p++) w += f->g[(*p >= 32 && *p < 127 ? *p : '?') - 32].adv;
    w += 4;
    uint8_t *a = calloc((size_t)w * h, 1);
    int pen = 2;
    for (const char *p = str; *p; p++) {
        const t48_glyph *g = &f->g[(*p >= 32 && *p < 127 ? *p : '?') - 32];
        for (int y = 0; y < g->h; y++) {
            int dy = g->yoff + y;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < g->w; x++) {
                int dx = pen + g->xoff + x;
                if (dx < 0 || dx >= w) continue;
                unsigned v = a[dy * w + dx] + f->a[g->off + (unsigned)(y * g->w + x)];
                a[dy * w + dx] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
        pen += g->adv;
    }
    if (!twice) { b->a = a; b->w = w; b->h = h; return; }
    /* double it: each new pixel is 9:3:3:1 of the four old ones around it... */
    int W = w * 2, H = h * 2;
    uint8_t *o = malloc((size_t)W * H);
    for (int y = 0; y < H; y++) {
        int y0 = y >> 1, y1 = clampi(y0 + ((y & 1) ? 1 : -1), 0, h - 1);
        for (int x = 0; x < W; x++) {
            int x0 = x >> 1, x1 = clampi(x0 + ((x & 1) ? 1 : -1), 0, w - 1);
            int v = (9 * a[y0 * w + x0] + 3 * a[y0 * w + x1] + 3 * a[y1 * w + x0] + a[y1 * w + x1] + 8) >> 4;
            o[y * W + x] = (uint8_t)clampi((v - 128) * 2 + 128, 0, 255);     /* and its edge made as crisp as the small one's */
        }
    }
    free(a);
    b->a = o; b->w = W; b->h = H;
}

void blit8(t48_surface *s, int x, int y, const bitmap8 *b, uint32_t c, int alpha)
{
    int x0 = x < 0 ? -x : 0, x1 = x + b->w > s->w ? s->w - x : b->w;
    if (alpha <= 0) return;
    for (int j = 0; j < b->h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= s->h) continue;
        const uint8_t *a = b->a + j * b->w;
        uint32_t *d = s->px + dy * s->stride + x;
        for (int i = x0; i < x1; i++) {
            unsigned v = a[i];
            if (v) d[i] = mix32(d[i], c, (v * (unsigned)alpha + 128) >> 8);
        }
    }
}
