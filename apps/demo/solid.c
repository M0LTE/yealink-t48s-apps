// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the parts with a solid in them: a flat-shaded gem, a Gouraud-shaded
 * knot, and a mirror torus over the scrolling text that ends the loop.
 *
 * These draw at the full 800 x 480. The sky is black, so each frame only the
 * box the solid covered last time in this buffer is cleared, and the solid
 * drawn again: a fraction of the screen's pixels. */
#include <math.h>
#include "r3d.h"

static mesh gem, knot, torus;
static rect last[FE_MAXBUF];

/* ---------- gem: every face one colour, from two lamps ---------- */

static int gem_fade, gem_flash;

static uint32_t gem_face(int nx, int ny, int nz)
{
    /* a warm lamp up and to the left of the eye, a cold one low on the right */
    int d1 = (-nx * 8700 - ny * 9800 - nz * 9900) >> 14, d2 = (nx * 11500 + ny * 9000 - nz * 7400) >> 14;
    if (d1 < 0) d1 = 0;
    if (d2 < 0) d2 = 0;
    int sp = (d1 * d1) >> 14; sp = (sp * sp) >> 14; sp = (sp * sp) >> 14;       /* a glint, n.l to the 8th */
    int r = 14 + ((d1 * 240 + d2 * 30 + sp * 120) >> 14);
    int g = 12 + ((d1 * 150 + d2 * 96 + sp * 120) >> 14);
    int b = 26 + ((d1 * 52 + d2 * 250 + sp * 120) >> 14);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    uint32_t c = scale32(T48_RGB(r, g, b), (unsigned)gem_fade);
    return add_sat(c, scale32(c, (unsigned)gem_flash));
}

static const material gem_mat = {.mode = M_FLAT, .face = gem_face};

/* ---------- knot: brightness at each corner, a ramp of colours ---------- */

static uint16_t knot_shade[256];
static uint32_t knot_ramp[256], knot_pal[256];
static material knot_mat = {.mode = M_GOURAUD, .pal = knot_pal, .shade = knot_shade, .lx = -7400, .ly = -9000, .lz = -11500};

/* ---------- torus: a mirror, the world it reflects painted in a map ---------- */

static uint8_t env_tex[65536];
static uint32_t env_ramp[256], env_pal[256];
static material env_mat = {.mode = M_ENV, .pal = env_pal, .tex = env_tex};

/* ---------- the scrolling text ---------- */

static const char scroll_text[] =
    "T48S open firmware      a desk phone running its own software      "
    "ARM1176 at 500 MHz, no GPU, no usable FPU      800 x 480, every pixel drawn in software      "
    "fixed point, sub-pixel edges, smoothed outlines      apps are plain C against libt48      "
    "github.com/M0LTE/yealink-t48s      73 de M0LTE";
static bitmap8 strip;
static uint32_t strip_ramp[128];
#define SCROLL_STEP 4           /* pixels a refresh of the screen: a whole number, so the text never judders */
#define BAND_H 184
#define OUTRO_LEAD 1500         /* ms before the text starts, and after it has gone */

int outro_ms(void) { return OUTRO_LEAD * 2 + (int)((long long)(strip.w + MAXW) * fe_period_us() / (SCROLL_STEP * 1000)); }

void solid_init(void)
{
    mesh_gem(&gem, 1.22);
    mesh_knot(&knot, 2, 3, 128, 14, 0.30);
    mesh_torus(&torus, 56, 22, 0.42);

    for (int i = 0; i < 256; i++) {
        double d = (i - 128) / 127.5, l = d > 0 ? d : 0;
        double v = 0.07 + 0.60 * l + 0.33 * pow(l, 26);
        knot_shade[i] = (uint16_t)lrint(256 + v * 253 * 256);
    }
    static const uint32_t kstops[] = {0x020308, 0x06202e, 0x0b5560, 0x18a39a, 0x6fe0c6, 0xe8fff4, 0xffffff};
    pal_ramp(knot_ramp, 256, kstops, 7);

    /* the mirror's world, by where the surface faces: a dusk sky above, dark ground below, a low sun */
    static uint8_t sky[256], sun[256];
    for (int i = 0; i < 256; i++) {
        double y = (i - 128) / 126.0;             /* -1 up, 1 down */
        double v = y < 0 ? 0.42 + 0.56 * pow(1 + y, 5) : 0.10 + 0.38 * pow(1 - y, 4);
        double edge = (y + 0.02) / 0.04;          /* the horizon, a few texels soft so it does not stair-step */
        if (edge > 0 && edge < 1) v = (0.42 + 0.56) * (1 - edge) + (0.10 + 0.38) * edge;
        sky[i] = (uint8_t)lrint(v * 255);
        sun[i] = (uint8_t)lrint(255 * exp(-i / 14.0));
    }
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int dx = x - 78, dy = y - 70, d2 = (dx * dx + dy * dy) >> 4;
            int rx = x - 128, ry = y - 128, rim = (rx * rx + ry * ry) >> 6;        /* 0..256 at the edge */
            int v = sky[y] + (d2 < 256 ? sun[d2] : 0);
            v = v * (256 - (rim > 256 ? 256 : rim) * rim / 640) >> 8;
            env_tex[y * 256 + x] = (uint8_t)clampi(v, 0, 255);
        }
    static const uint32_t estops[] = {0x040208, 0x24103a, 0x6a1f5c, 0xc23c58, 0xff8a3c, 0xffd47a, 0xfff8e8, 0xffffff};
    pal_ramp(env_ramp, 256, estops, 8);

    text8(&strip, &t48_font_big, scroll_text, 1);
    static const uint32_t sstops[] = {0xfff6e0, 0xffd47a, 0xff8a3c, 0xc23c58, 0x6a2a8c};
    pal_ramp(strip_ramp, 128, sstops, 5);
}

/* clear what this buffer last showed of a solid, then the stars */
static void sky(const frame *f, int star_speed, int star_bright)
{
    t48_surface *s = f->s;
    if (f->fresh) { clear_rect(s, (rect){0, 0, s->w, s->h}, 0); last[f->buf] = (rect){0, 0, 0, 0}; }
    clear_rect(s, last[f->buf], 0);
    stars_draw(f, (int)((long long)star_speed * f->dtu / 1000000), star_bright * (256 + f->kick / 2) >> 8);
}

/* the distance of a solid that arrives from far off, rests, and leaves again. Q12 */
static int fly(const frame *f, int rest, int in_ms, int out_ms)
{
    int far = rest * 14;
    if (f->t < in_ms) { int k = ease(f->t * 256 / in_ms); return far + (((rest - far) >> 4) * ease(k) >> 4); }
    if (f->t > f->len - out_ms) { int k = ease((f->t - (f->len - out_ms)) * 256 / out_ms); return rest + (((far - rest) >> 4) * ease(k) >> 4); }
    return rest;
}

void gem_draw(const frame *f)
{
    unsigned t = (unsigned)(f->tu / 256);
    int cr = stars_cruise(), rush = 256 - ease(f->t * 256 / 2500);
    sky(f, cr + rush * cr * 3 / 256, envelope(f->t, f->len, 0, 900));
    /* each kick lights the gem and brings it a touch nearer */
    pose p = {.ax = t * 5, .ay = t * 7 + 9000, .az = t * 2, .z = fly(f, 14600, 2200, 1500) - f->kick * 2, .focal = 640};
    gem_fade = envelope(f->t, f->len, 1200, 900);
    gem_flash = f->kick * 5 / 8;
    mesh_draw(f->s, &gem, &p, &gem_mat, &last[f->buf]);
}

void knot_draw(const frame *f)
{
    unsigned t = (unsigned)(f->tu / 256);
    sky(f, stars_cruise(), envelope(f->t, f->len, 900, 900));
    pose p = {.ax = t * 4 + 6000, .ay = t * 6, .az = t * 3, .z = fly(f, 13200, 2200, 1500), .focal = 640};
    pal_fade(knot_pal, knot_ramp, 256, envelope(f->t, f->len, 1200, 900));
    pal_lift(knot_pal, 256, f->kick / 2);
    mesh_draw(f->s, &knot, &p, &knot_mat, &last[f->buf]);
}

/* The text rides a sine: each column of the screen takes its column of the
 * strip, moved up or down, in whole rows of the band so the writes run along
 * memory. Sub-pixel in y (two rows of the strip mixed), whole pixels in x. */
static void scroller(const frame *f, int top, int fade)
{
    t48_surface *s = f->s;
    static int32_t y0[MAXW];
    static uint32_t tc[256];
    /* the wave the text rides swells with each note of the bass */
    int w = s->w, H = strip.h, amp = ((BAND_H - H) / 2 - 2) * (176 + f->bass * 80 / 256) >> 8;
    int sx = (int)((long long)(f->tu - OUTRO_LEAD * 1000) * SCROLL_STEP / fe_period_us()) - w;
    unsigned ph = (unsigned)(f->tu / 256) * 12;

    for (int i = 0; i < H && i < 256; i++) tc[i] = scale32(strip_ramp[i * 128 / H], (unsigned)fade);
    for (int x = 0; x < w; x++)
        y0[x] = ((top + BAND_H / 2 - H / 2) << 8) + ((fsin(ph + (unsigned)x * 150) * amp) >> 6);
    int x0 = sx < 0 ? -sx : 0, x1 = strip.w - sx < w ? strip.w - sx : w;     /* the columns the strip reaches */
    for (int y = top; y < top + BAND_H; y++) {
        uint32_t *p = s->px + y * s->stride;
        if (x1 <= x0) { fill32(p, 0, w); continue; }
        fill32(p, 0, x0);
        fill32(p + x1, 0, w - x1);
        const uint8_t *src = strip.a + sx;
        for (int x = x0; x < x1; x++) {
            int r = (y << 8) - y0[x], ri = r >> 8;
            uint32_t c = 0;
            if ((unsigned)ri < (unsigned)(H - 1)) {
                const uint8_t *a = src + ri * strip.w + x;
                unsigned c0 = a[0], c1 = a[strip.w];
                if (c0 | c1) {
                    unsigned cov = c0 + (((int)(c1 - c0) * (r & 255)) >> 8);
                    c = scale32(tc[ri], cov + (cov >> 7));
                }
            }
            p[x] = c;
        }
    }
    fe_touch(0, top, w, top + BAND_H);
}

void outro_draw(const frame *f)
{
    unsigned t = (unsigned)(f->tu / 256);
    int fade = envelope(f->t, f->len, 1200, 1200);
    sky(f, stars_cruise(), envelope(f->t, f->len, 900, 1200));
    pose p = {.ax = t * 6, .ay = t * 4 + 3000, .az = t * 2, .y = -2500, .z = fly(f, 19000, 2500, 1500), .focal = 640};
    pal_fade(env_pal, env_ramp, 256, fade);
    pal_lift(env_pal, 256, f->kick / 3);
    mesh_draw(f->s, &torus, &p, &env_mat, &last[f->buf]);
    scroller(f, f->s->h - BAND_H - 4, fade);
}
