// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Keeping the sound in step. The game runs from the clock, a frame every
 * 16.6 ms, and each frame makes 266 samples; the audio service takes 320
 * every 20 ms by its own clock. The two agree only nearly, so the number of
 * samples waiting is watched and the core's output rate is bent by a few
 * parts in ten thousand to hold it steady: never enough to hear as pitch,
 * and the game's speed is not touched.
 *
 * How much waits is the price of never running dry: the service must find
 * 320 samples each time it comes, a frame may be a little late, and the two
 * rhythms beat against each other. 720 after a write, on average, is 45 ms.
 */
#include <stdio.h>
#include "sound.h"
#include "t48.h"

#if T48_API_LEVEL >= 4

#define TARGET 720            /* samples waiting after a frame's are written, on average */
#define ROOM 1280             /* more than this waiting and samples are dropped */
#define TICK 320              /* what the service takes at a time */
#define PPM_PER_SAMPLE 20     /* how hard the rate is bent for each sample off the target */
#define PPM_MAX 5000

static int have;
static int level;             /* samples waiting after a write, smoothed, in 1/256 */
static int ppm;
static long frames, level_sum, dropped, restarts;
static int ppm_min, ppm_max;
static unsigned dry_was;

void sound_init(nes_t *n)
{
    if (t48_sound_open() < 0) return;
    t48_sound_buffer(ROOM);
    nes_sound_start(n, t48_sound_rate());
    level = TARGET << 8;
    dry_was = t48_sound_dry();
    have = 1;
}

void sound_frame(nes_t *n)
{
    static const int16_t quiet[TICK];
    int16_t pcm[512];
    int got, waiting;

    if (!have) return;
    got = nes_sound_read(n, pcm, (int)(sizeof pcm / sizeof pcm[0]));
    waiting = t48_sound_queued();
    if (waiting + got < TICK) {
        /* starting, or back from the menu, or the phone was busy and it ran
         * out: silence in front, so the usual amount is waiting again at once */
        for (int need = TARGET - waiting - got; need > 0; need -= TICK) t48_sound_put(quiet, need < TICK ? need : TICK);
        level = TARGET << 8;
        restarts++;
    }
    dropped += got - t48_sound_put(pcm, got);
    waiting = t48_sound_queued();

    level += ((waiting << 8) - level) / 64;
    ppm = ((TARGET << 8) - level) * PPM_PER_SAMPLE / 256;
    if (ppm > PPM_MAX) ppm = PPM_MAX;
    if (ppm < -PPM_MAX) ppm = -PPM_MAX;
    nes_sound_tune(n, ppm);

    if (!frames || ppm < ppm_min) ppm_min = ppm;
    if (!frames || ppm > ppm_max) ppm_max = ppm;
    frames++;
    level_sum += waiting;
}

void sound_stats(char *out, size_t n)
{
    unsigned dry = t48_sound_dry();
    out[0] = 0;
    if (have && frames)
        snprintf(out, n, "; sound: %ld waiting on average, rate %+d to %+d ppm, %u gaps, %ld starts, %ld samples dropped, %s",
                 level_sum / frames, ppm_min, ppm_max, dry - dry_was, restarts, dropped, t48_sound_playing() ? "playing" : "not playing");
    dry_was = dry;
    frames = level_sum = dropped = restarts = 0;
}

#else   /* a libt48 from before sound */

void sound_init(nes_t *n) { (void)n; }
void sound_frame(nes_t *n) { (void)n; }
void sound_stats(char *out, size_t n) { if (n) out[0] = 0; }

#endif
