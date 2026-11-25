// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the tune to the loudspeaker (API level 4), in time with the picture.
 *
 * The picture's clock is the master: it moves by whole refreshes of the
 * screen, which is what keeps motion even. The tune follows it. A thread
 * makes a tick of sound (20 ms) at a time and writes it as room appears; for
 * each it works out when that tick will be heard (what is queued, plus the
 * way to the loudspeaker: about 65 ms on the T48S) and where the picture's
 * clock will be by then. A little out, and the tick is made a few samples
 * shorter or longer: the tempo gives, the pitch does not. A long way out (a
 * key that steps to another part, coming back from Home), and the tune jumps.
 *
 * A thread, because the service takes sound every 20 ms and a frame can take
 * 30: filled from the drawing loop the queue would run dry.
 *
 * No sound (an older phone, no permission): nothing here runs, and the
 * picture carries on by itself. */
#include <pthread.h>
#include <unistd.h>
#include "demo.h"
#include "music.h"

#if T48_API_LEVEL >= 4

#define QUEUE 640               /* samples that may wait: 40 ms */
#define STALE_US 300000         /* no frame for this long: nothing is being drawn, make no sound */
#define JUMP_US 100000          /* further out than this, do not chase: jump */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long long seen_song, seen_at;    /* the picture shows seen_song (us into the loop) at seen_at */
static long long made_us, cost_us;      /* sound made, and what making it took */
static int running;

static void *run(void *arg)
{
    static int16_t buf[MUSIC_TICK + 16];
    const long long loop = (long long)music_ticks() * MUSIC_TICK_US;
    int drift = 0, lost = 1;
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&mu);
        long long song = seen_song, at = seen_at;
        pthread_mutex_unlock(&mu);
        long long now = now_us();
        if (!at || now - at > STALE_US) { lost = 1; usleep(MUSIC_TICK_US); continue; }

        /* This thread wakes as the service takes its 320 samples, so what it writes
         * waits whole takes: half a take longer than the average libt48 reckons with. */
        int delay = t48_sound_latency_us() + (t48_sound_playing() ? MUSIC_TICK_US / 2 : 0);
        long long want = ((song + now + delay - at) % loop + loop) % loop;
        long long err = want - (long long)music_tick() * MUSIC_TICK_US;
        if (err > loop / 2) err -= loop;
        if (err < -loop / 2) err += loop;
        int n = MUSIC_TICK;
        if (lost || err > JUMP_US || err < -JUMP_US) {
            music_seek((int)(want / MUSIC_TICK_US));
            n -= (int)(want % MUSIC_TICK_US) * MUSIC_TICK / MUSIC_TICK_US;     /* the rest of the tick it landed in */
            if (n < 16) n = 16;
            drift = 0; lost = 0;
        } else {
            drift += ((int)err - drift) / 8;
            n -= clampi(drift / 1500, -4, 4);       /* behind: shorter ticks */
        }
        long long t0 = now_us();
        music_render(buf, n);
        long long took = now_us() - t0;
        pthread_mutex_lock(&mu);
        cost_us += took;
        made_us += (long long)n * 1000000 / MUSIC_RATE;
        pthread_mutex_unlock(&mu);
        t48_sound_write(buf, n);
    }
    return NULL;
}

int snd_start(void)
{
    pthread_t th;
    if (running || music_ticks() <= 0 || t48_sound_open() < 0) return running ? 0 : -1;
    t48_sound_buffer(QUEUE);
    if (pthread_create(&th, NULL, run, NULL)) { t48_sound_close(); return -1; }
    pthread_detach(th);
    running = 1;
    return 0;
}

void snd_sync(long long song_us, long long at_us)
{
    if (!running) return;
    pthread_mutex_lock(&mu);
    seen_song = song_us; seen_at = at_us;
    pthread_mutex_unlock(&mu);
}

int snd_stats(int *gaps)
{
    static long long made0, cost0;
    static unsigned dry0;
    if (!running) { *gaps = 0; return -1; }
    pthread_mutex_lock(&mu);
    long long made = made_us - made0, cost = cost_us - cost0;
    made0 = made_us; cost0 = cost_us;
    pthread_mutex_unlock(&mu);
    unsigned dry = t48_sound_dry();
    *gaps = (int)(dry - dry0);
    dry0 = dry;
    return made > 0 ? (int)(cost * 1000 / made) : 0;
}

#else

int snd_start(void) { return -1; }
void snd_sync(long long song_us, long long at_us) { (void)song_us; (void)at_us; }
int snd_stats(int *gaps) { *gaps = 0; return -1; }

#endif
