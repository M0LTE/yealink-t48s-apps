// SPDX-License-Identifier: AGPL-3.0-or-later
/* snake: sound. A handful of notes, each a wave read from a table (or noise)
 * with a pitch that may slide and a level that rises in 2 ms and falls away
 * to nothing, so none of them clicks. An effect is a few such notes, some
 * starting a little later than others. No floating point once the tables are
 * made.
 *
 * To the loudspeaker (API level 4) by a thread, as the demo does it: the
 * audio service takes 20 ms at a time and a frame of the game is 33, so the
 * drawing loop cannot keep it fed. The thread sleeps while nothing sounds and
 * writes nothing, which is silence; an effect wakes it. At most 40 ms waits to
 * be played, so a sound is heard about 65 ms after what caused it. */
#include <math.h>
#include <string.h>
#include "sfx.h"

enum { BRIGHT, SINE, NOISE };
#define TLEN 1024
#define NVOICE 8
#define ATTACK 32               /* samples */

typedef struct {
    int wave, wait, pos, len;   /* samples before it starts, into it, and its length; len 0: free */
    uint32_t ph, inc;
    int32_t slide;              /* added to inc each sample */
    int amp;
    uint32_t lfsr;
    int held;                   /* noise: the value it is holding */
} voice;

static int16_t table[2][TLEN];
static voice voices[NVOICE];
static int next_voice;

void sfx_tables(void)
{
    for (int i = 0; i < TLEN; i++) {
        double x = 2 * M_PI * i / TLEN;
        /* a few harmonics, falling: bright on a small loudspeaker without the edge of a square */
        table[BRIGHT][i] = (int16_t)lrint(16000 * (sin(x) + 0.42 * sin(2 * x) + 0.22 * sin(3 * x) + 0.08 * sin(4 * x)) / 1.45);
        table[SINE][i] = (int16_t)lrint(16000 * sin(x));
    }
}

static uint32_t hz_inc(int hz) { return (uint32_t)(((uint64_t)hz << 32) / SFX_RATE); }

/* a note: after delay ms, for ms, from hz0 sliding to hz1, at amp (of 32768) */
static void note(int wave, int delay, int ms, int hz0, int hz1, int amp)
{
    voice *v = &voices[next_voice];
    next_voice = (next_voice + 1) % NVOICE;
    v->wave = wave;
    v->wait = delay * SFX_RATE / 1000;
    v->pos = 0;
    v->len = ms * SFX_RATE / 1000;
    v->ph = 0;
    v->inc = hz_inc(hz0);
    v->slide = (int32_t)(((int64_t)hz_inc(hz1) - (int64_t)hz_inc(hz0)) / v->len);
    v->amp = amp;
    v->lfsr = 0x2545f491;
    v->held = 0;
}

int sfx_render(int16_t *out, int n)
{
    static int32_t mix[512];
    int live = 0;
    if (n > 512) n = 512;
    memset(mix, 0, (size_t)n * sizeof mix[0]);
    for (voice *v = voices; v < voices + NVOICE; v++) {
        if (!v->len) continue;
        live = 1;
        for (int i = 0; i < n; i++) {
            if (v->wait) { v->wait--; continue; }
            if (v->pos >= v->len) { v->len = 0; break; }
            /* the level: up in 2 ms, then down as the square of what is left, to nothing */
            int left = (int)(((int64_t)(v->len - v->pos) << 12) / v->len);
            int env = (left * left) >> 12;
            if (v->pos < ATTACK) env = env * v->pos / ATTACK;
            int s;
            if (v->wave == NOISE) {
                uint32_t was = v->ph;
                v->ph += v->inc;
                if (v->ph < was || !v->pos) {       /* a new value each cycle: hz is how bright the noise is */
                    v->lfsr ^= v->lfsr << 13; v->lfsr ^= v->lfsr >> 17; v->lfsr ^= v->lfsr << 5;
                    v->held = (int)(v->lfsr & 0x7fff) - 0x4000;
                }
                s = v->held;
            } else {
                s = table[v->wave][v->ph >> 22];
                v->ph += v->inc;
            }
            v->inc += (uint32_t)v->slide;
            v->pos++;
            mix[i] += (s * ((v->amp * env) >> 12)) >> 14;
        }
    }
    for (int i = 0; i < n; i++) out[i] = (int16_t)(mix[i] > 30000 ? 30000 : mix[i] < -30000 ? -30000 : mix[i]);
    return live;
}

/* ---------- the effects ---------- */

static void lock(void);
static void unlock_and_wake(void);

void sfx_start(void)            /* two notes up: off we go */
{
    lock();
    note(BRIGHT, 0, 90, 784, 784, 11000);
    note(BRIGHT, 80, 220, 1047, 1047, 12500);
    unlock_and_wake();
}

void sfx_eat(int score)         /* a blip and its fifth; an octave higher by the 36th apple */
{
    int hz = 660 + 660 * (score > 36 ? 36 : score) / 36;
    lock();
    note(BRIGHT, 0, 60, hz, hz, 12000);
    note(BRIGHT, 45, 130, hz * 3 / 2, hz * 3 / 2, 13500);
    unlock_and_wake();
}

void sfx_turn(void)             /* a tick, barely there */
{
    lock();
    note(SINE, 0, 14, 1500, 1000, 2600);
    unlock_and_wake();
}

void sfx_crash(void)            /* a thud falling away under a burst of noise that dulls */
{
    lock();
    for (voice *v = voices; v < voices + NVOICE; v++) v->len = 0;
    note(NOISE, 0, 260, 5000, 700, 10000);
    note(BRIGHT, 0, 380, 440, 150, 14000);
    unlock_and_wake();
}

void sfx_best(void)             /* up a chord, and the top note rings */
{
    static const int hz[] = {1047, 1319, 1568, 2093};
    lock();
    for (int i = 0; i < 4; i++) note(BRIGHT, i * 85, i == 3 ? 420 : 140, hz[i], hz[i], 10500);
    note(SINE, 3 * 85, 420, 1047, 1047, 5500);
    unlock_and_wake();
}

/* ---------- to the loudspeaker ---------- */

#ifndef SFX_ONLY_SYNTH
#include <pthread.h>
#include "t48.h"
#endif

#if !defined SFX_ONLY_SYNTH && T48_API_LEVEL >= 4

#define TICK 320                /* what the audio service takes at a time: 20 ms */
#define QUEUE 640               /* samples that may wait */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int running, sounding;

static void lock(void) { pthread_mutex_lock(&mu); }

static void unlock_and_wake(void)
{
    sounding = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mu);
}

static void *run(void *arg)
{
    static int16_t buf[TICK];
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!sounding) pthread_cond_wait(&wake, &mu);
        sounding = sfx_render(buf, TICK);
        pthread_mutex_unlock(&mu);
        t48_sound_write(buf, TICK);     /* the last one written tails off into silence and a little more of it */
    }
    return NULL;
}

void sfx_init(void)
{
    pthread_t th;
    sfx_tables();
    if (running || t48_sound_open() < 0) return;
    t48_sound_buffer(QUEUE);
    if (pthread_create(&th, NULL, run, NULL)) { t48_sound_close(); return; }
    pthread_detach(th);
    running = 1;
}

void sfx_quiet(void)
{
    lock();
    for (voice *v = voices; v < voices + NVOICE; v++) v->len = 0;
    pthread_mutex_unlock(&mu);
}

#else   /* no way out: the notes are set down and never played, unless wav.c renders them */

static void lock(void) { }
static void unlock_and_wake(void) { }
void sfx_init(void) { sfx_tables(); }
void sfx_quiet(void) { for (voice *v = voices; v < voices + NVOICE; v++) v->len = 0; }

#endif
