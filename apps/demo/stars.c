// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the starfield, shared by every part with a black sky, and the title
 * cards that open the demo.
 *
 * A star is four pixels weighted by where in its pixel it really is, added
 * to what is there, so it drifts instead of hopping. Only the stars are
 * rubbed out and drawn each frame: the sky around them is never touched. */
#include "demo.h"

#define NSTAR 500
#define ZFAR 65536
#define ZNEAR 2600
#define MAXDOTS 4096    /* a fast star is a short row of dots */

static struct star { int16_t x, y; int32_t z; uint32_t tint; int px, py; } st[NSTAR];   /* px: where it was last drawn, 28.4, or none */
static uint32_t *seen[FE_MAXBUF][MAXDOTS];
static int nseen[FE_MAXBUF];
#define NOWHERE (-(1 << 30))

static void star_new(struct star *s)
{
    static const uint32_t tints[4] = {0xffffff, 0xffffff, 0xc4d8ff, 0xffe6c8};
    s->x = (int16_t)rnd(); s->y = (int16_t)rnd();
    s->tint = tints[rnd() & 3];
    s->px = NOWHERE;
}

void stars_init(void)
{
    for (int i = 0; i < NSTAR; i++) { star_new(&st[i]); st[i].z = ZNEAR + (int32_t)(rnd() % (ZFAR - ZNEAR)); }
}

/* one dot: four pixels sharing its light by where in its pixel it is. px, py: 28.4; b: 0..512 */
static inline uint32_t *dot(t48_surface *s, int px, int py, uint32_t col, int b)
{
    int xi = px >> 4, yi = py >> 4, fx = px & 15, fy = py & 15, stride = s->stride, w;
    if (xi < 0 || yi < 0 || xi >= s->w - 1 || yi >= s->h - 1) return NULL;
    uint32_t *p = s->px + yi * stride + xi;
    w = ((16 - fx) * (16 - fy) * b) >> 8; p[0] = add_sat(p[0], scale32(col, (unsigned)(w > 256 ? 256 : w)));
    w = (fx * (16 - fy) * b) >> 8;        p[1] = add_sat(p[1], scale32(col, (unsigned)(w > 256 ? 256 : w)));
    w = ((16 - fx) * fy * b) >> 8;        p[stride] = add_sat(p[stride], scale32(col, (unsigned)(w > 256 ? 256 : w)));
    w = (fx * fy * b) >> 8;               p[stride + 1] = add_sat(p[stride + 1], scale32(col, (unsigned)(w > 256 ? 256 : w)));
    return p;
}

void stars_draw(const frame *f, int dz, int bright)
{
    t48_surface *s = f->s;
    int stride = s->stride, n = 0, cx = s->w * 8, cy = s->h * 8;
    uint32_t **mine = seen[f->buf];

    if (f->fresh) nseen[f->buf] = 0;
    for (int i = 0; i < nseen[f->buf]; i++) {
        uint32_t *p = mine[i];
        p[0] = 0; p[1] = 0; p[stride] = 0; p[stride + 1] = 0;
    }
    for (int i = 0; i < NSTAR; i++) {
        struct star *a = &st[i];
        a->z -= dz;
        if (a->z < ZNEAR) { a->z += ZFAR - ZNEAR; star_new(a); }
        int px = cx + a->x * 16000 / a->z, py = cy + a->y * 16000 / a->z;
        int ox = a->px, oy = a->py;
        a->px = px; a->py = py;
        if (bright <= 0) continue;
        /* out of the dark at the far end, then brighter the nearer it is */
        int d = (ZFAR - a->z) >> 6, b = d < 128 ? d * 2 : 256;
        b = (b * (120 + ((d * d) >> 11)) * bright) >> 16;
        if (b > 512) b = 512;
        /* how far it moved since the last frame: more than a pixel and it is a streak */
        int steps = 1;
        if (ox != NOWHERE) {
            int mx = px - ox, my = py - oy, len = ((mx < 0 ? -mx : mx) + (my < 0 ? -my : my)) >> 4;
            steps = len < 1 ? 1 : len > 20 ? 20 : len;
        }
        if (steps == 1 || n + steps > MAXDOTS) {
            uint32_t *p = dot(s, px, py, a->tint, b);
            if (p && n < MAXDOTS) mine[n++] = p;
        } else {
            int sb = b * 2 / (steps + 1) + 24;
            for (int k = 0; k < steps; k++) {
                uint32_t *p = dot(s, px - (px - ox) * k / steps, py - (py - oy) * k / steps, a->tint, sb);
                if (p) mine[n++] = p;
            }
        }
    }
    nseen[f->buf] = n;
    fe_touch_all();
}

/* ---------- the opening: stars, and three cards ---------- */

static const char *const card_text[3][2] = {
    {"T48S", "open firmware"},
    {"ARM1176", "500 MHz, no GPU"},
    {"800 x 480", "every pixel drawn in software"},
};
static bitmap8 card[3][2];
static rect shown[FE_MAXBUF];
static int cruise;      /* how fast the stars pass when nothing else is happening, depth a second */

void intro_init(void)
{
    cruise = 9000;
    for (int i = 0; i < 3; i++) {
        text8(&card[i][0], &t48_font_big, card_text[i][0], 1);
        text8(&card[i][1], &t48_font_big, card_text[i][1], 0);
    }
}

int stars_cruise(void) { return cruise; }

void intro_draw(const frame *f)
{
    t48_surface *s = f->s;
    if (f->fresh) { clear_rect(s, (rect){0, 0, s->w, s->h}, 0); shown[f->buf] = (rect){0, 0, 0, 0}; }
    clear_rect(s, shown[f->buf], 0);

    /* the stars gather speed towards the end, into the next part */
    int rush = ease((f->t - (f->len - 2500)) * 256 / 2500);
    int speed = cruise + rush * cruise * 3 / 256;
    /* the stars flare with the kick drum, here and behind every solid */
    stars_draw(f, (int)((long long)speed * f->dtu / 1000000), envelope(f->t, f->len, 1500, 0) * (256 + f->kick / 2) >> 8);

    int slot = (f->len - 1200) / 3, i = (f->t - 600) / slot, t = (f->t - 600) % slot;
    shown[f->buf] = (rect){0, 0, 0, 0};
    if (f->t < 600 || i > 2) return;
    int a = envelope(t, slot - 300, 500, 500);
    const bitmap8 *top = &card[i][0], *sub = &card[i][1];
    int ty = s->h / 2 - top->h + 10, sy = s->h / 2 + 34;
    blit8(s, (s->w - top->w) / 2, ty, top, 0xfff2dc, a);
    blit8(s, (s->w - sub->w) / 2, sy, sub, 0x58c8d8, a);
    int w = top->w > sub->w ? top->w : sub->w;
    shown[f->buf] = (rect){(s->w - w) / 2, ty, (s->w + w) / 2 + 1, sy + sub->h};
}
