// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the full-screen parts: plasma, rotozoomer, tunnel.
 *
 * All three are one renderer. The effect is worked out exactly at the corners
 * of 16 x 16 pixel cells (51 x 31 points a frame) and the pixels between are
 * straight-line steps: three adds, a texture read and a palette read each.
 * Cells are drawn one at a time so the texture reads stay inside a few cache
 * lines whatever the angle. HALF draws 8 x 8 points a cell and doubles them. */
#include <math.h>
#include "demo.h"

#define CELL 16
#define GW (MAXW / CELL + 1)
#define GH (MAXH / CELL + 1)

typedef struct gp { int32_t u, v, s; } gp;   /* texture position 9.16 (or palette position 10.16), shade 8.16 */
static gp grid[GH * GW];

enum { G_PAL, G_TEX, G_SHADE };

/* textures are 512 x 512. The difference of two 9.16 positions the short way round one: */
#define TEX 512
static inline int32_t wd(int32_t d) { return (int32_t)((uint32_t)d << 7) >> 7; }
#define TEXEL(u, v) tex[(((v) >> 7) & 0x3fe00) | (((u) >> 16) & 0x1ff)]

static inline __attribute__((always_inline)) void grid_impl(t48_surface *d, int top, int rows, const uint8_t *tex, const uint32_t *pal,
                                                            const int mode, const int half)
{
    const int n = half ? CELL / 2 : CELL, sh = half ? 3 : 4;
    int cols = d->w / CELL, stride = d->stride;
    for (int cy = 0; cy < rows; cy++)
        for (int cx = 0; cx < cols; cx++) {
            const gp *a = &grid[cy * GW + cx], *b = a + 1, *c = a + GW, *e = c + 1;
            uint32_t *p = d->px + (top + cy * CELL) * stride + cx * CELL;
            if (mode == G_SHADE && !(a->s | b->s | c->s | e->s)) {
                for (int y = 0; y < CELL; y++, p += stride) fill32(p, pal[0], CELL);
                continue;
            }
            int32_t ul = a->u, ur, ub, ue, vl = a->v, vr, vb, ve;
            if (mode == G_PAL) { ur = b->u; ub = c->u; ue = e->u; vr = vb = ve = 0; }
            else {
                ur = ul + wd(b->u - ul); ub = ul + wd(c->u - ul); ue = ul + wd(e->u - ul);
                vr = vl + wd(b->v - vl); vb = vl + wd(c->v - vl); ve = vl + wd(e->v - vl);
            }
            int32_t dul = (ub - ul) >> sh, dur = (ue - ur) >> sh, dvl = (vb - vl) >> sh, dvr = (ve - vr) >> sh;
            int32_t sl = a->s, sr = b->s, dsl = (c->s - sl) >> sh, dsr = (e->s - sr) >> sh;
            for (int y = 0; y < n; y++) {
                int32_t u = ul, v = vl, s = sl, du = (ur - ul) >> sh, dv = (vr - vl) >> sh, ds = (sr - sl) >> sh;
                uint32_t *q = p, *q2 = p + stride;
                /* A texture seen close up would show its texels as blocks. Shifting where
                 * each pixel reads by a different part of a texel, in a 2 x 2 pattern,
                 * mixes neighbouring texels by eye instead: smooth, for no more work. */
                int32_t ju = 0, jv = 0;
                if (mode == G_SHADE) {
                    if (y & 1) { u += 0xc000; v += 0x8000; ju = -0x8000; jv = -0x8000; }
                    else { v += 0x4000; ju = 0x8000; jv = 0x8000; }
                }
                for (int x = 0; x < n; x += 2) {
                    uint32_t col, col2;
                    if (mode == G_PAL) {
                        col = pal[(u >> 16) & 1023]; u += du;
                        col2 = pal[(u >> 16) & 1023]; u += du;
                    } else if (mode == G_TEX) {
                        col = pal[TEXEL(u, v)]; u += du; v += dv;
                        col2 = pal[TEXEL(u, v)]; u += du; v += dv;
                    } else {
                        col = pal[(TEXEL(u, v) * (unsigned)(s >> 16)) >> 8]; u += du + ju; v += dv + jv; s += ds;
                        col2 = pal[(TEXEL(u, v) * (unsigned)(s >> 16)) >> 8]; u += du - ju; v += dv - jv; s += ds;
                    }
                    if (half) {
                        q[0] = col; q[1] = col; q2[0] = col; q2[1] = col;
                        q[2] = col2; q[3] = col2; q2[2] = col2; q2[3] = col2;
                        q += 4; q2 += 4;
                    } else { q[0] = col; q[1] = col2; q += 2; }
                }
                p += half ? 2 * stride : stride;
                ul += dul; ur += dur; vl += dvl; vr += dvr; sl += dsl; sr += dsr;
            }
        }
}

/* The picture is rows cells high, in the middle of the screen, with black above and below */
static void grid_draw(const frame *f, int rows, const uint8_t *tex, const uint32_t *pal, int mode, int half)
{
    t48_surface *d = f->s;
    int top = (d->h - rows * CELL) / 2;
    if (f->fresh) {
        clear_rect(d, (rect){0, 0, d->w, top}, 0);
        clear_rect(d, (rect){0, top + rows * CELL, d->w, d->h}, 0);
    }
    switch (mode * 2 + half) {
    case 0: grid_impl(d, top, rows, tex, pal, G_PAL, 0); break;
    case 1: grid_impl(d, top, rows, tex, pal, G_PAL, 1); break;
    case 2: grid_impl(d, top, rows, tex, pal, G_TEX, 0); break;
    case 3: grid_impl(d, top, rows, tex, pal, G_TEX, 1); break;
    case 4: grid_impl(d, top, rows, tex, pal, G_SHADE, 0); break;
    default: grid_impl(d, top, rows, tex, pal, G_SHADE, 1); break;
    }
}

/* Per part: how many cells high, and whether to draw at half size and double.
 * Set from what the phone manages at every refresh of its screen (README):
 * writing 800 x 480 pixels alone takes 10 of the 14.7 ms there are. */
static const struct { int rows, half; } shape[3] = {{24, 0}, {24, 1}, {24, 0}};

static uint32_t plasma_pal[1024], roto_pal[256], tunnel_pal[256];
static uint8_t roto_tex[TEX * TEX], tunnel_tex[TEX * TEX];
static uint8_t fog[256];

/* ---------- plasma ---------- */

void plasma_draw(const frame *f)
{
    uint32_t pal[1024];
    int fade = envelope(f->t, f->len, 700, 700);
    int cols = f->s->w / CELL, rows = shape[0].rows, top = (f->s->h - rows * CELL) / 2;
    unsigned t = (unsigned)(f->tu / 256);    /* 3906 a second */
    int ox = f->s->w / 2 + ((fsin(t * 2) * 260) >> 14), oy = f->s->h / 2 + ((fcos(t * 3) * 150) >> 14);
    int ox2 = f->s->w / 2 + ((fcos(t * 3 + 9000) * 300) >> 14), oy2 = f->s->h / 2 + ((fsin(t * 2 + 20000) * 170) >> 14);

    int push = f->kick * 9000;       /* the colours jump on with the kick and slide back */

    pal_fade(pal, plasma_pal, 1024, fade);
    pal_lift(pal, 1024, f->snare / 3);
    for (int gy = 0; gy <= rows; gy++)
        for (int gx = 0; gx <= cols; gx++) {
            int x = gx * CELL, y = top + gy * CELL;
            unsigned r1 = isqrt((unsigned)((x - ox) * (x - ox) + (y - oy) * (y - oy)) << 4);    /* x4 */
            unsigned r2 = isqrt((unsigned)((x - ox2) * (x - ox2) + (y - oy2) * (y - oy2)) << 4);
            int v = fsin((unsigned)(x * 130) + t * 5) + fsin((unsigned)(y * 170) - t * 7)
                  + fsin((unsigned)((x + y) * 90) + t * 3) + fsin(r1 * 52 - t * 9) + fsin(r2 * 40 + t * 6);
            grid[gy * GW + gx].u = v * 330 + (int32_t)(t << 11) + push;
        }
    grid_draw(f, rows, NULL, pal, G_PAL, shape[0].half);
    fe_touch_all();
}

/* ---------- rotozoomer ---------- */

void roto_draw(const frame *f)
{
    uint32_t pal[256];
    int fade = envelope(f->t, f->len, 700, 700);
    int cols = f->s->w / CELL, rows = shape[1].rows;
    unsigned t = (unsigned)(f->tu / 256);
    unsigned ang = t * 2 + (unsigned)fsin(t * 3);
    int zoom = 47000 + ((fsin(t * 4 + 30000) * 19000) >> 14);      /* texels a pixel, 16.16: 0.43 to 1.0, so texels are never big */
    zoom += (zoom >> 8) * f->kick >> 3;                             /* the kick knocks it back an eighth */
    int dux = (fcos(ang) * (zoom >> 2)) >> 12, duy = (fsin(ang) * (zoom >> 2)) >> 12;
    /* turn about the middle of the screen, while the texture drifts under it */
    int cu = (int)(t * 1800), cv = (fsin(t * 2) * 180) << 2;
    int hx = f->s->w / 2, hy = rows * CELL / 2;

    pal_fade(pal, roto_pal, 256, fade);
    pal_lift(pal, 256, f->snare / 3);
    for (int gy = 0; gy <= rows; gy++)
        for (int gx = 0; gx <= cols; gx++) {
            int x = gx * CELL - hx, y = gy * CELL - hy;
            gp *g = &grid[gy * GW + gx];
            g->u = (cu + x * dux - y * duy) & 0x1ffffff;
            g->v = (cv + x * duy + y * dux) & 0x1ffffff;
        }
    grid_draw(f, rows, roto_tex, pal, G_TEX, shape[1].half);
    fe_touch_all();
}

/* ---------- tunnel ---------- */

void tunnel_draw(const frame *f)
{
    uint32_t pal[256];
    int fade = envelope(f->t, f->len, 900, 900);
    int cols = f->s->w / CELL, rows = shape[2].rows;
    unsigned t = (unsigned)(f->tu / 256);
    /* the eye stays on the axis and looks about: yaw, pitch, and a slow roll */
    unsigned yaw = (unsigned)(fsin(t * 2) / 5), pitch = (unsigned)(fsin(t * 3 + 12000) / 8), roll = t + (unsigned)(fsin(t * 2 + 40000) / 2);
    int sy = fsin(yaw), cy = fcos(yaw), sp = fsin(pitch), cp = fcos(pitch), sr = fsin(roll), cr = fcos(roll);
    int focal = f->s->w * 9 / 16, hx = f->s->w / 2, hy = rows * CELL / 2;
    int scroll = (int)(t * 20);           /* 24.8 texels */

    pal_fade(pal, tunnel_pal, 256, fade);
    pal_lift(pal, 256, f->kick / 3);
    for (int gy = 0; gy <= rows; gy++)
        for (int gx = 0; gx <= cols; gx++) {
            int x0 = gx * CELL - hx, y0 = gy * CELL - hy;
            int x = (x0 * cr - y0 * sr) >> 14, y = (x0 * sr + y0 * cr) >> 14, z = focal;
            int y1 = (y * cp - z * sp) >> 14, z1 = (y * sp + z * cp) >> 14;
            int x2 = (x * cy + z1 * sy) >> 14, z2 = (z1 * cy - x * sy) >> 14;
            unsigned r16 = isqrt((unsigned)(x2 * x2 + y1 * y1) << 8);
            if (r16 < 8) r16 = 8;
            if (z2 < 1) z2 = 1;
            int vt = (int)((unsigned)z2 * 333742u / r16);            /* depth in texels, 24.8 */
            int zt = vt >> 10;
            gp *g = &grid[gy * GW + gx];
            g->u = (int32_t)(iatan2(y1, x2) << 9);
            g->v = (int32_t)(((unsigned)(vt + scroll) & 0x1ffff) << 8);
            g->s = (int32_t)fog[zt > 255 ? 255 : zt] << 16;
        }
    grid_draw(f, rows, tunnel_tex, pal, G_SHADE, shape[2].half);
    fe_touch_all();
}

/* ---------- tables and textures ---------- */

static unsigned hash2(unsigned a, unsigned b)
{
    unsigned h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* smooth noise, 0..255, wrapping every 256 texels; cell: texels per lattice step (a power of two) */
static int noise(int x, int y, int cell)
{
    int n = 256 / cell, gx = x / cell, gy = y / cell, fx = (x % cell) * 256 / cell, fy = (y % cell) * 256 / cell;
    fx = ease(fx); fy = ease(fy);
    int a = hash2((unsigned)(gx % n), (unsigned)(gy % n) + (unsigned)cell * 131) & 255, b = hash2((unsigned)((gx + 1) % n), (unsigned)(gy % n) + (unsigned)cell * 131) & 255;
    int c = hash2((unsigned)(gx % n), (unsigned)((gy + 1) % n) + (unsigned)cell * 131) & 255, d = hash2((unsigned)((gx + 1) % n), (unsigned)((gy + 1) % n) + (unsigned)cell * 131) & 255;
    int top = a + (((b - a) * fx) >> 8), bot = c + (((d - c) * fx) >> 8);
    return top + (((bot - top) * fy) >> 8);
}

/* how much of a pixel is inside a shape, 0..255, from its distance to the edge in 1/16 pixel (inside is negative) */
static int cover(int d16) { return clampi(128 - d16 * 16, 0, 255); }

void fx_init(void)
{
    /* plasma: dusk colours on a ring, so the palette can turn for ever */
    for (int i = 0; i < 1024; i++) {
        double t = i / 1024.0;
        int r = (int)lrint(255 * (0.50 + 0.50 * cos(2 * M_PI * (t + 0.00))));
        int g = (int)lrint(255 * (0.42 + 0.42 * cos(2 * M_PI * (t + 0.09))));
        int b = (int)lrint(255 * (0.50 + 0.45 * cos(2 * M_PI * (t + 0.21))));
        plasma_pal[i] = T48_RGB(r, g, b);
    }

    /* rotozoomer: a board of 256-texel squares, the name on the dark ones and a ring on the others */
    bitmap8 name;
    text8(&name, &t48_font_big, "T48S", 1);
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            int sqx = x >> 8, sqy = y >> 8, lx = x & 255, ly = y & 255, v;
            if ((sqx ^ sqy) & 1) {
                int dx = lx - 128, dy = ly - 128;
                int d16 = (int)isqrt((unsigned)(dx * dx + dy * dy) << 8);
                int ring = cover(d16 - 84 * 16) - cover(d16 - 62 * 16), dot = cover(d16 - 24 * 16);
                v = 78 + ((ring + dot) * 110 >> 8);
            } else {
                int tx = lx - (256 - name.w) / 2, ty = ly - (256 - name.h) / 2;
                int cov = (tx >= 0 && tx < name.w && ty >= 0 && ty < name.h) ? name.a[ty * name.w + tx] : 0;
                v = 26 + (cov * 229 >> 8);
            }
            roto_tex[y * TEX + x] = (uint8_t)v;
        }
    free(name.a);
    static const uint32_t rstops[] = {0x04060f, 0x101a3c, 0x3a2a78, 0x9a3c7c, 0xf0783c, 0xffd890, 0xfff6e0};
    pal_ramp(roto_pal, 256, rstops, 7);

    /* tunnel: 8 x 4 plates with soft grooves between, a few of them lit, and some grain */
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            int px = x & 63, py = y & 127;
            unsigned h = hash2((unsigned)(x >> 6), (unsigned)(y >> 7));
            int ex = px < 63 - px ? px : 63 - px, ey = py < 127 - py ? py : 127 - py, e = ex < ey ? ex : ey;
            int bevel = ease(e * 26);                               /* 0 in the groove, 256 on the plate */
            int base = 92 + (int)(h & 63);
            if (h % 5 == 0) {                                       /* a lamp: a bright bar along the plate */
                int d = py - 64; if (d < 0) d = -d;
                base += (ease(256 - clampi((d - 12) * 20, 0, 256)) * 150) >> 8;
            }
            int grain = (noise(x >> 1, y >> 1, 8) + noise(x >> 1, y >> 1, 32)) / 2 - 128;
            int v = (base * (70 + (bevel * 186 >> 8)) >> 8) + grain / 5;
            tunnel_tex[y * TEX + x] = (uint8_t)clampi(v, 0, 255);
        }
    static const uint32_t tstops[] = {0x000000, 0x06102c, 0x0c3a6a, 0x1c8fa6, 0x9fe4e6, 0xffffff};
    pal_ramp(tunnel_pal, 256, tstops, 6);
    /* the light is ahead of the eye: walls brighten for a while, then fall away
     * to nothing before the texture would shimmer. Index: depth / 4 texels */
    for (int i = 0; i < 256; i++) {
        int k = 256 - i * 4 * 256 / 800;
        int near = 130 + ease(i * 256 / 36) * 126 / 256;
        fog[i] = (uint8_t)(k <= 0 ? 0 : (ease(k) * near) >> 8);
    }
}
