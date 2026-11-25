// SPDX-License-Identifier: AGPL-3.0-or-later
#include <string.h>
#include "t48.h"
#include "video.h"

static struct scale_target target;
static int fresh;             /* nothing of the game is on the screen yet: the first picture goes with its black surround */

/* What each buffer shows already, so lines that have not changed are left
 * alone (scale.h). One for the screen buffer, or one for each flip buffer. */
#define BUFFERS 4
static struct {
    const uint32_t *px;
    uint8_t seen[SCALE_SEEN_BYTES];
    uint32_t seen_rgb[240 * 32];
} known[BUFFERS];
static int nknown;

/* Aim at a buffer. 1 if it is new to us: nothing of the game is in it. */
static int aim(t48_surface *s)
{
    int i = 0, is_new = 0;
    while (i < nknown && known[i].px != s->px) i++;
    if (i == nknown && nknown < BUFFERS) {
        nknown++;
        known[i].px = s->px;
        memset(known[i].seen, 0xFF, sizeof known[i].seen);   /* no line can match this */
        is_new = 1;
    }
    target.px = s->px + (s->w - PIC_W) / 2 + (s->h - PIC_H) / 2 * s->stride;
    target.stride = s->stride;
    target.seen = i < BUFFERS ? known[i].seen : NULL;
    target.seen_rgb = i < BUFFERS ? known[i].seen_rgb : NULL;
    target.lo = 240;
    target.hi = -1;
    return is_new || i == BUFFERS;
}

static void black(t48_surface *s)
{
    t48_fill(s, 0, 0, s->w, s->h, 0);
}

/* the lines that changed, for the shell to copy */
static void damage_lines(void)
{
    t48_surface *s = t48_screen();
    if (fresh) t48_damage_all();
    else if (target.hi >= target.lo) t48_damage((s->w - PIC_W) / 2, (s->h - PIC_H) / 2 + target.lo * 2, PIC_W, (target.hi - target.lo + 1) * 2);
    fresh = 0;
}

static void copy_out(const t48_surface *s, uint32_t *pic)
{
    const uint32_t *p = s->px + (s->w - PIC_W) / 2 + (s->h - PIC_H) / 2 * s->stride;
    for (int y = 0; y < PIC_H; y++) memcpy(pic + y * PIC_W, p + y * s->stride, PIC_W * sizeof *pic);
}

#if T48_API_LEVEL >= 3

static t48_surface *flip, *shown;

void video_enter(void)
{
    flip = t48_flip_start();
    shown = NULL;
    nknown = 0;
    fresh = 1;
    if (!flip) black(t48_screen());
}

struct scale_target *video_target(void)
{
    if (!flip) aim(t48_screen());
    else if (aim(flip)) black(flip);
    return &target;
}

void video_present(void)
{
    if (flip) {
        shown = flip;
        flip = t48_flip();
    } else {
        damage_lines();
    }
}

void video_leave(uint32_t *pic)
{
    if (flip) {
        if (shown) copy_out(shown, pic);
        else memset(pic, 0, PIC_W * PIC_H * sizeof *pic);
        t48_flip_stop();
        flip = shown = NULL;
    } else {
        copy_out(t48_screen(), pic);
    }
}

int video_flips(void)
{
    return flip != NULL;
}

#else   /* level 2: one buffer, copied to the screen by the shell */

void video_enter(void)
{
    black(t48_screen());
    nknown = 0;
    fresh = 1;
}

struct scale_target *video_target(void)
{
    aim(t48_screen());
    return &target;
}

void video_present(void)
{
    damage_lines();
}

void video_leave(uint32_t *pic)
{
    copy_out(t48_screen(), pic);
}

int video_flips(void)
{
    return 0;
}

#endif
