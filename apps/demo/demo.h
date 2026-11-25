// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: what every file shares. Fixed point throughout: the phone's ARM1176
 * has no FPU the userland can use, so floats appear only where tables are
 * made at start. */
#ifndef DEMO_H
#define DEMO_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "t48.h"

#define MAXW 800
#define MAXH 480

/* ---------- frontend (fe.c): the one place that knows how frames reach the screen ---------- */

#define FE_MAXBUF 4
typedef struct rect { int x0, y0, x1, y1; } rect;   /* x1, y1 are one past the end */

int fe_init(void);              /* 0, or -1: not started by a shell */
int fe_period_us(void);         /* one refresh of the screen (or the pace kept without one), microseconds */
int fe_vsync(void);             /* 1: frames are flipped at the screen's refresh */
/* The buffer to draw this frame in, and which one it is: 0..FE_MAXBUF-1, or -1
 * for one not tracked (treat as holding nothing known) */
t48_surface *fe_begin(int *buf);
void fe_touch(int x0, int y0, int x1, int y1);   /* this frame changed these pixels */
void fe_touch_all(void);
void fe_end(void);              /* show the frame */
int fe_wait_ms(void);           /* ms until the next frame should start: time to spend on events */
void fe_event(const t48_event *ev);   /* every event passes through here */
void fe_pace(int div);          /* show each frame for div refreshes (flipping only) */
int fe_pace_now(void);          /* the pace in force: fe_pace's, or slower if that could not be kept */
int fe_step_us(int dt);         /* how far the clock moves for this frame; dt: microseconds since the last */
int fe_ahead_us(void);          /* from starting to draw a frame to its being on the screen, about */
long long now_us(void);

/* ---------- maths (util.c) ---------- */

void util_init(void);
int fsin(unsigned a);           /* a: 65536 a turn. Q14, smooth between table steps */
static inline int fcos(unsigned a) { return fsin(a + 16384); }
unsigned isqrt(unsigned v);
unsigned iatan2(int y, int x);  /* 65536 a turn */
unsigned rnd(void);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* 0..256 over the first and last ms of a span, 256 between */
int envelope(int t, int len, int in_ms, int out_ms);
/* smooth 0..256 from 0..256 */
static inline int ease(int x) { x = clampi(x, 0, 256); return (x * x * (768 - 2 * x)) >> 16; }

/* ---------- pixels (util.c) ---------- */

void fill32(uint32_t *p, uint32_t c, int n);
void clear_rect(t48_surface *s, rect r, uint32_t c);
static inline uint32_t mix32(uint32_t d, uint32_t c, unsigned a)   /* a: 0..256 */
{
    uint32_t rb = ((d & 0xff00ff) * (256 - a) + (c & 0xff00ff) * a) >> 8;
    uint32_t g = ((d & 0x00ff00) * (256 - a) + (c & 0x00ff00) * a) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}
static inline uint32_t scale32(uint32_t c, unsigned a)             /* a: 0..256 */
{
    return ((((c & 0xff00ff) * a) >> 8) & 0xff00ff) | ((((c & 0x00ff00) * a) >> 8) & 0x00ff00);
}
/* each channel added, stopping at 255: one instruction on ARMv6 */
static inline uint32_t add_sat(uint32_t a, uint32_t b)
{
#ifdef __arm__
    uint32_t r;
    __asm__("uqadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
#else
    uint32_t r = (a & 0xff) + (b & 0xff), g = (a & 0xff00) + (b & 0xff00), x = (a & 0xff0000) + (b & 0xff0000);
    return (r > 0xff ? 0xff : r) | (g > 0xff00 ? 0xff00 : g) | (x > 0xff0000 ? 0xff0000 : x);
#endif
}
/* n colours of src, each scaled by fade (0..256) */
void pal_fade(uint32_t *dst, const uint32_t *src, int n, int fade);
/* each colour brighter by k (0..256) of itself: black stays black */
void pal_lift(uint32_t *pal, int n, int k);
/* a ramp through stops[] (0xRRGGBB, evenly spaced) into n entries */
void pal_ramp(uint32_t *dst, int n, const uint32_t *stops, int nstops);

/* text as 8-bit coverage, so it can be scaled, scrolled and coloured */
typedef struct bitmap8 { uint8_t *a; int w, h; } bitmap8;
void text8(bitmap8 *b, const t48_font *f, const char *str, int twice);   /* twice: double size, smoothed */
/* coverage times alpha (0..256) of colour c over what is there */
void blit8(t48_surface *s, int x, int y, const bitmap8 *b, uint32_t c, int alpha);

/* ---------- the timeline (demo.c) ---------- */

/* What a part is told about one frame. Time is the music's clock: the tune is
 * kept to it (sound.c), so beat and beat_frac are where the tune is as heard,
 * and kick, snare and bass are read from its score. */
typedef struct frame {
    t48_surface *s;      /* the buffer to draw in */
    int buf;             /* which buffer, 0..FE_MAXBUF-1: what it holds is what this part last drew in it */
    int fresh;           /* 1: this buffer holds something else. Paint all of it */
    int t, len;          /* ms into the part, and how long it runs */
    int tu;              /* the same in microseconds: motion is worked out from this, so it is even */
    int dtu;             /* microseconds since the last frame */
    int gt;              /* ms into the loop */
    int beat, beat_frac; /* beats into the loop, and 0..65535 through this one */
    int kick, snare;     /* 0..256: 256 as the drum hits, gone 200 ms later */
    int bass;            /* the same for each note of the bass, over 240 ms */
} frame;

typedef struct part {
    const char *name;
    int bars;
    int pace;            /* refreshes of the screen each frame is shown for: 1 is every refresh */
    void (*draw)(const frame *f);
} part;

/* ---------- the tune's way out (sound.c; the tune itself is music.h) ---------- */

int snd_start(void);            /* 0, or -1: no sound on this phone */
/* the picture will show song_us (into the loop) at at_us on now_us()'s clock: the tune keeps to that */
void snd_sync(long long song_us, long long at_us);
/* since the last call: what making the sound took, in ms a second of it (-1: no sound), and gaps heard */
int snd_stats(int *gaps);

/* ---------- shared layers ---------- */

void stars_init(void);
/* fly dz (Q12 units of depth) further on, rub out the stars this buffer shows and draw them again.
 * bright: 0..256. Returns nothing to remember: the layer keeps what it drew in each buffer. */
void stars_draw(const frame *f, int dz, int bright);
int stars_cruise(void);         /* the stars' usual speed, depth a second */

/* ---------- parts ---------- */

void intro_init(void);  void intro_draw(const frame *f);
void solid_init(void);  void gem_draw(const frame *f);  void knot_draw(const frame *f);  void outro_draw(const frame *f);
int outro_ms(void);     /* how long the outro's text takes to pass */
void fx_init(void);     void plasma_draw(const frame *f);  void roto_draw(const frame *f);  void tunnel_draw(const frame *f);

#endif
