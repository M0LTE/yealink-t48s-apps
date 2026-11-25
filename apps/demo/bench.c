// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo-bench: how long each part takes to draw, without a shell or a screen.
 * `make bench` builds build/arm/demo-bench; copy it to the phone and run it.
 * It draws into ordinary memory, so it measures the drawing, not the way to
 * the glass: the overlay in the app (the * key) has the last word.
 *
 * It times the tune's synth by itself, then every part twice: alone, and with
 * the synth making the tune in a thread at the pace sound plays, as it does
 * in the app. The parts are told the drums, so their flashes are in the
 * figures too. */
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "demo.h"
#include "music.h"

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

static const struct { const char *name; void (*draw)(const frame *f); int at_ms, len_ms, bar; } parts[] = {
    {"stars", intro_draw, 3000, 15360, 0}, {"gem", gem_draw, 5000, 15360, 8}, {"plasma", plasma_draw, 3000, 13440, 16},
    {"knot", knot_draw, 5000, 19200, 23}, {"roto", roto_draw, 3000, 13440, 33}, {"tunnel", tunnel_draw, 3000, 17280, 40},
    {"outro", outro_draw, 8000, 50000, 49},
};
#define BAR_US 1920000

/* the tune, a tick every 20 ms, into nothing */
static volatile int playing;
static void *play(void *arg)
{
    static int16_t buf[MUSIC_TICK];
    long long due = now_us();
    (void)arg;
    for (;;) {
        if (playing) music_render(buf, MUSIC_TICK);
        due += MUSIC_TICK_US;
        long long wait = due - now_us();
        if (wait > 0) usleep((useconds_t)wait);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 200;
    long long t0 = now_us();
    for (int i = 0; i < 2; i++) {
        surf[i].w = MAXW; surf[i].h = MAXH; surf[i].stride = MAXW;
        surf[i].px = calloc(MAXW * MAXH, 4);
    }
    util_init(); stars_init(); intro_init(); solid_init(); fx_init();
    int bars = music_init((outro_ms() + BAR_US / 1000 - 1) / (BAR_US / 1000));
    printf("start-up %lld ms\n", (now_us() - t0) / 1000);

    if (bars > 0) {
        static int16_t buf[MUSIC_TICK];
        long long a = now_us();
        for (int i = 0; i < music_ticks(); i++) music_render(buf, MUSIC_TICK);
        long long d = now_us() - a, len = (long long)music_ticks() * MUSIC_TICK_US;
        printf("tune    %d bars, %lld s of sound made in %lld ms: %.2f%% of the processor\n", bars, len / 1000000, d / 1000, d * 100.0 / len);
    }
    pthread_t th;
    int threaded = bars > 0 && pthread_create(&th, NULL, play, NULL) == 0;
    for (int pass = 0; pass <= threaded; pass++) {
        printf(pass ? "with the tune playing:\n" : "drawing alone:\n");
        for (unsigned p = 0; p < sizeof parts / sizeof parts[0]; p++) {
            long long sum = 0, worst = 0;
            music_seek((int)(((long long)parts[p].bar * BAR_US + parts[p].at_ms * 1000LL) / MUSIC_TICK_US));
            playing = pass;
            for (int i = 0; i < frames; i++) {
                frame f = {.s = &surf[i & 1], .buf = i & 1, .fresh = i < 2, .len = parts[p].len_ms, .dtu = fe_period_us()};
                music_cue cue;
                f.tu = parts[p].at_ms * 1000 + i * fe_period_us();
                f.t = f.tu / 1000;
                music_cue_at((long long)parts[p].bar * BAR_US + f.tu, &cue);
                f.kick = cue.kick; f.snare = cue.snare; f.bass = cue.bass;
                long long a = now_us();
                parts[p].draw(&f);
                long long d = now_us() - a;
                if (i >= 2) { sum += d; if (d > worst) worst = d; }
            }
            playing = 0;
            printf("%-7s %6.2f ms a frame, worst %6.2f\n", parts[p].name, sum / 1000.0 / (frames - 2), worst / 1000.0);
        }
    }
    return 0;
}
