// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: real-time graphics on a desk phone, in a loop of about two and a half
 * minutes. This file is the timeline, the clock, the keys and the frame-rate
 * overlay; the parts are in stars.c, solid.c and fx.c, and fe.c puts frames
 * on the screen.
 *
 * Keys: * shows or hides the frame rate, left and right step through the
 * parts, any other key leaves. Touches are ignored.
 *
 * The timeline is laid out in the bars of the tune (music.c, tune.c): 125
 * beats a minute, four to the bar. The clock here moves by whole refreshes of
 * the screen and the tune is kept to it (sound.c), so on a phone with no
 * sound for apps everything but the sound is the same. */
#include <stdio.h>
#include "demo.h"
#include "music.h"

#define BEAT_US 480000
#define BAR_US (4 * BEAT_US)

static part parts[] = {
    {"stars", 8, 1, intro_draw},
    {"gem", 8, 1, gem_draw},
    {"plasma", 7, 1, plasma_draw},
    {"knot", 10, 2, knot_draw},
    {"roto", 7, 1, roto_draw},
    {"tunnel", 9, 2, tunnel_draw},
    {"outro", 0, 2, outro_draw},      /* as long as its text: set at start */
};
#define NPARTS ((int)(sizeof parts / sizeof parts[0]))

static long long start_us[NPARTS + 1];   /* where each part begins in the loop */
static long long song;                   /* the music's clock: microseconds into the loop */
static int stale[FE_MAXBUF];             /* this buffer does not hold the current part's last frame */
static int overlay;

/* ---------- frame-rate overlay and log ---------- */

static struct {
    int frames, worst_us;
    long long span_us, draw_us;
} sec, tot;                               /* this second, and this part */
static char line[96] = "";
static FILE *logf;

static void stats_part_end(int i)
{
    if (overlay && tot.frames > 10) {
        if (!logf) {
            char path[256];
            const char *dir = getenv("T48_APP_DATA");
            snprintf(path, sizeof path, "%s/fps.log", dir ? dir : ".");
            logf = fopen(path, "w");
        }
        if (logf) {
            fprintf(logf, "%-7s %5d frames  %5.1f fps  frame %5.2f ms (worst %5.1f)  draw %5.2f ms  %s/%d\n", parts[i].name, tot.frames,
                    tot.frames * 1e6 / (double)tot.span_us, (double)tot.span_us / tot.frames / 1000.0, tot.worst_us / 1000.0,
                    (double)tot.draw_us / tot.frames / 1000.0, fe_vsync() ? "flip" : "copy", fe_pace_now());
            int gaps, synth = snd_stats(&gaps);
            if (synth >= 0) fprintf(logf, "        tune %d.%d%% of the processor, %d gaps\n", synth / 10, synth % 10, gaps);
            fflush(logf);
        }
    }
    memset(&tot, 0, sizeof tot);
}

static void stats_frame(int frame_us, int draw_us, const char *name)
{
    sec.frames++; sec.span_us += frame_us; sec.draw_us += draw_us;
    if (frame_us > sec.worst_us) sec.worst_us = frame_us;
    tot.frames++; tot.span_us += frame_us; tot.draw_us += draw_us;
    if (frame_us > tot.worst_us) tot.worst_us = frame_us;
    if (sec.span_us >= 500000) {
        int fps10 = (int)(sec.frames * 10000000LL / sec.span_us), draw10 = (int)(sec.draw_us / sec.frames / 100);
        snprintf(line, sizeof line, "%d.%d fps  worst %d ms  draw %d.%d ms  %s  %s/%d", fps10 / 10, fps10 % 10,
                 (sec.worst_us + 500) / 1000, draw10 / 10, draw10 % 10, name, fe_vsync() ? "flip" : "copy", fe_pace_now());
        memset(&sec, 0, sizeof sec);
    }
}

static void overlay_draw(t48_surface *s)
{
    t48_fill(s, 0, 0, 520, 22, 0);
    t48_text_fit(s, 6, 3, 508, &t48_font_mono, T48_RGB(140, 255, 170), line);
    fe_touch(0, 0, 520, 22);
}

/* ---------- the timeline ---------- */

static void all_stale(void)
{
    for (int i = 0; i < FE_MAXBUF; i++) stale[i] = 1;
}

static int part_at(long long t)
{
    int i = 0;
    while (i < NPARTS - 1 && t >= start_us[i + 1]) i++;
    return i;
}

int main(void)
{
    t48_event ev;
    int cur = -1, paused = 0;

    if (fe_init() < 0) return 1;
    util_init();
    stars_init();
    intro_init();
    solid_init();
    fx_init();
    parts[NPARTS - 1].bars = (int)(((long long)outro_ms() * 1000 + BAR_US - 1) / BAR_US);
    for (int i = 0; i < NPARTS; i++) start_us[i + 1] = start_us[i] + (long long)parts[i].bars * BAR_US;
    /* the tune is written for these bars: if they ever disagree, better none than one that drifts off the picture */
    if ((long long)music_init(parts[NPARTS - 1].bars) * BAR_US == start_us[NPARTS]) snd_start();
    else music_init(-1);
    all_stale();
    int slow_us = getenv("DEMO_DRAW_MS") ? atoi(getenv("DEMO_DRAW_MS")) * 1000 : 0;
    if (getenv("DEMO_AT")) song = atoll(getenv("DEMO_AT")) * 1000 % start_us[NPARTS];   /* for pictures: start this many ms in */

    long long last = now_us();
    for (;;) {
        /* keys and the shell, for as long as the frontend wants to wait */
        int r;
        while ((r = t48_poll(&ev, paused ? -1 : fe_wait_ms())) != 0) {
            if (r < 0) return 0;
            fe_event(&ev);
            switch (ev.type) {
            case T48_EV_KEY:
                if (!ev.state) break;
                if (ev.key == T48_KEY_STAR) { overlay = !overlay; all_stale(); }
                else if (ev.key == T48_KEY_RIGHT) { stats_part_end(cur); song = start_us[(cur + 1) % NPARTS]; }
                else if (ev.key == T48_KEY_LEFT) { stats_part_end(cur); song = start_us[(cur + NPARTS - 1) % NPARTS]; }
                else return 0;
                break;
            case T48_EV_PAUSE:
                paused = 1;
                t48_paused();
                break;
            case T48_EV_RESUME:
                paused = 0;
                all_stale();
                last = now_us();
                break;
            case T48_EV_QUIT:
                return 0;
            }
        }

        long long now = now_us();
        int dt = (int)(now - last), step = fe_step_us(dt);
        last = now;
        song += step;
        if (song >= start_us[NPARTS]) song -= start_us[NPARTS];

        int i = part_at(song);
        if (i != cur) {
            if (cur >= 0) stats_part_end(cur);
            cur = i;
            all_stale();
            fe_pace(parts[i].pace);
        }
        snd_sync(song, now + fe_ahead_us());
        frame f;
        f.s = fe_begin(&f.buf);
        if (f.buf < 0) { f.buf = 0; all_stale(); }
        f.fresh = stale[f.buf]; stale[f.buf] = 0;
        f.tu = (int)(song - start_us[i]);
        f.t = f.tu / 1000;
        f.len = (int)((long long)parts[i].bars * BAR_US / 1000);
        f.dtu = step;
        f.gt = (int)(song / 1000);
        f.beat = (int)(song / BEAT_US);
        f.beat_frac = (int)((song % BEAT_US) * 65536 / BEAT_US);
        music_cue cue;
        music_cue_at(song, &cue);
        f.kick = cue.kick; f.snare = cue.snare; f.bass = cue.bass;
        long long t0 = now_us();
        parts[i].draw(&f);
        if (slow_us) while (now_us() - t0 < slow_us) ;                 /* DEMO_DRAW_MS: stand in for a slower part */
        int draw_us = (int)(now_us() - t0);
        stats_frame(dt, draw_us, parts[i].name);
        if (overlay) overlay_draw(f.s);
        fe_end();
    }
}
