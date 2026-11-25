// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo-bench: how long each part takes to draw, without a shell or a screen.
 * `make bench` builds build/arm/demo-bench; copy it to the phone and run it.
 * It draws into ordinary memory, so it measures the drawing, not the way to
 * the glass: the overlay in the app (the * key) has the last word. */
#include <stdio.h>
#include <time.h>
#include "demo.h"

static t48_surface surf[2];

long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
int fe_period_us(void) { return 14746; }
void fe_touch(int x0, int y0, int x1, int y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void fe_touch_all(void) {}

static const struct { const char *name; void (*draw)(const frame *f); int at_ms, len_ms; } parts[] = {
    {"stars", intro_draw, 3000, 15360}, {"gem", gem_draw, 5000, 15360}, {"plasma", plasma_draw, 3000, 13440},
    {"knot", knot_draw, 5000, 19200}, {"roto", roto_draw, 3000, 13440}, {"tunnel", tunnel_draw, 3000, 17280},
    {"outro", outro_draw, 8000, 50000},
};

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 200;
    long long t0 = now_us();
    for (int i = 0; i < 2; i++) {
        surf[i].w = MAXW; surf[i].h = MAXH; surf[i].stride = MAXW;
        surf[i].px = calloc(MAXW * MAXH, 4);
    }
    util_init(); stars_init(); intro_init(); solid_init(); fx_init();
    printf("start-up %lld ms\n", (now_us() - t0) / 1000);
    for (unsigned p = 0; p < sizeof parts / sizeof parts[0]; p++) {
        long long sum = 0, worst = 0;
        for (int i = 0; i < frames; i++) {
            frame f = {.s = &surf[i & 1], .buf = i & 1, .fresh = i < 2, .len = parts[p].len_ms, .dtu = fe_period_us()};
            f.tu = parts[p].at_ms * 1000 + i * fe_period_us();
            f.t = f.tu / 1000;
            long long a = now_us();
            parts[p].draw(&f);
            long long d = now_us() - a;
            if (i >= 2) { sum += d; if (d > worst) worst = d; }
        }
        printf("%-7s %6.2f ms a frame, worst %6.2f\n", parts[p].name, sum / 1000.0 / (frames - 2), worst / 1000.0);
    }
    return 0;
}
