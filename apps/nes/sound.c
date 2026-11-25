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
 *
 * Starting is the other half. The service may take a tenth of a second to
 * wake the hardware, and nothing says when it has. So a start puts 40 ms of
 * silence in the ring, keeps what the game makes to one side until the
 * service is seen to take some, and then lets go of the newest of it, enough
 * to bring what waits to the usual amount. The sound of those first frames
 * is lost; nothing in the middle ever is.
 *
 * A frame that is late is followed by frames that come early, and their
 * samples with them. So what is counted as waiting is what is in the ring
 * and what the game still owes the clock: how much later than usual this
 * frame's samples were written.
 */
#include <stdio.h>
#include <string.h>
#include "sound.h"
#include "t48.h"

#if T48_API_LEVEL >= 4

#define TARGET 720            /* samples waiting after a frame's are written, on average */
#define ROOM 1600             /* more than this waiting and samples are dropped */
#define TICK 320              /* what the service takes at a time */
#define PPM_PER_SAMPLE 20     /* how hard the rate is bent for each sample off the target */
#define PPM_MAX 5000
#define HOLD 1024             /* samples kept aside while the service starts */
#define START_FRAMES 30       /* frames to wait for it before carrying on regardless */
#define FADE 32               /* samples over which a cut-in is eased */

static int have;
static int starting;          /* frames spent waiting for the service to take; 0: it is taking */
static int primed;            /* what was waiting when the start began */
static long usual_us = 6000;  /* from a frame's time to its samples being written, when nothing is late */
static int16_t hold[HOLD];
static int held;
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

static void put_quiet(int n)
{
    static const int16_t quiet[TICK];
    for (; n > 0; n -= TICK) t48_sound_put(quiet, n < TICK ? n : TICK);
}

/* While the service starts. 1 once it is taking and the ring has been brought to the usual amount. */
static int start(const int16_t *pcm, int got, int owed)
{
    int waiting, room, cut;

    if (got > HOLD) got = HOLD;
    cut = held + got - HOLD;
    if (cut > 0) {
        memmove(hold, hold + cut, (size_t)(held - cut) * sizeof hold[0]);
        held -= cut;
    }
    memcpy(hold + held, pcm, (size_t)got * sizeof hold[0]);
    held += got;

    waiting = t48_sound_queued();
    if (waiting >= primed && starting++ < START_FRAMES) return 0;

    room = TARGET - waiting - owed;
    if (room < 0) room = 0;
    cut = held - room;
    if (cut > 0) {
        /* the newest of it, eased in */
        const int16_t *from = hold + cut;
        held = room;
        for (int i = 0; i < held; i++) hold[i] = (int16_t)(i < FADE ? from[i] * i / FADE : from[i]);
    } else {
        put_quiet(-cut);
    }
    t48_sound_put(hold, held);
    held = starting = 0;
    return 1;
}

void sound_frame(nes_t *n, long late_us)
{
    int16_t pcm[512];
    int got, waiting, owed;

    /* the usual is the least seen, let rise slowly in case the game has become heavier */
    if (late_us < usual_us) usual_us = late_us;
    else usual_us += (late_us - usual_us) / 256;
    late_us -= usual_us;
    owed = (int)(late_us > 100000 ? 100000 : late_us) * 16 / 1000;

    if (!have) return;
    got = nes_sound_read(n, pcm, (int)(sizeof pcm / sizeof pcm[0]));
    waiting = t48_sound_queued();
    if (!starting && waiting + got < TICK) {
        /* the first frame, or back from the menu, or the phone was busy and it ran out */
        put_quiet(2 * TICK - waiting);
        primed = t48_sound_queued();
        starting = 1;
        held = 0;
        restarts++;
    }
    if (starting) {
        if (!start(pcm, got, owed)) return;
        level = TARGET << 8;
    } else {
        dropped += got - t48_sound_put(pcm, got);
    }
    waiting = t48_sound_queued() + owed;

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
void sound_frame(nes_t *n, long late_us) { (void)n; (void)late_us; }
void sound_stats(char *out, size_t n) { if (n) out[0] = 0; }

#endif
