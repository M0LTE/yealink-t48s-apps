// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo-wav: the whole tune as a sound file, made on a PC by the player the
 * phone runs, sample for sample. `make wav` writes build/demo.wav.
 *
 *   demo-wav OUT.wav [OUTRO_BARS [LOOPS [CHANNELS]]]
 *
 * CHANNELS: which to play, as a sum of 1 lead, 2 second voice, 4 chords,
 * 8 bass, 16 drums, 32 hats.
 * It also says how loud each part is and how long the synth took. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "music.h"

static void le(FILE *f, unsigned v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 255, f); }

int main(int argc, char **argv)
{
    static const struct { const char *name; int bars; } parts[] = {
        {"stars", 8}, {"gem", 8}, {"plasma", 7}, {"knot", 10}, {"roto", 7}, {"tunnel", 9}, {"outro", 0},
    };
    if (argc < 2) { fprintf(stderr, "usage: demo-wav OUT.wav [OUTRO_BARS [LOOPS [CHANNELS]]]\n"); return 2; }
    int outro = argc > 2 ? atoi(argv[2]) : 26, loops = argc > 3 ? atoi(argv[3]) : 1;
    if (argc > 4) music_channels = atoi(argv[4]);
    int bars = music_init(outro);
    if (bars < 0) return 1;
    int ticks = music_ticks() * loops;
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    unsigned bytes = (unsigned)ticks * MUSIC_TICK * 2;
    fputs("RIFF", f); le(f, 36 + bytes, 4); fputs("WAVEfmt ", f);
    le(f, 16, 4); le(f, 1, 2); le(f, 1, 2); le(f, MUSIC_RATE, 4); le(f, MUSIC_RATE * 2, 4); le(f, 2, 2); le(f, 16, 2);
    fputs("data", f); le(f, bytes, 4);

    int part = 0, start = 0, peak = 0, over = 0, all_peak = 0;
    double sq = 0, all_sq = 0;
    long n = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int t = 0; t < ticks; t++) {
        int16_t buf[MUSIC_TICK];
        music_render(buf, MUSIC_TICK);
        for (int i = 0; i < MUSIC_TICK; i++) {
            int a = abs(buf[i]);
            if (a > peak) peak = a;
            if (a > 25000) over++;
            sq += (double)buf[i] * buf[i];
            le(f, (unsigned)buf[i] & 0xffff, 2);
        }
        n += MUSIC_TICK;
        int len = part < 6 ? parts[part].bars : outro;
        if (t < music_ticks() && t + 1 == (start + len) * MUSIC_BAR_ROWS * MUSIC_ROW_TICKS) {
            printf("%-7s %2d bars  peak %5.1f dBFS  level %5.1f dBFS\n", parts[part].name, len,
                   20 * log10(peak / 32768.0 + 1e-9), 10 * log10(sq / n / 32768.0 / 32768.0 + 1e-12));
            if (peak > all_peak) all_peak = peak;
            all_sq += sq; sq = 0; n = 0; peak = 0;
            start += len; part++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fclose(f);
    double secs = (double)ticks * MUSIC_TICK / MUSIC_RATE, took = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%s: %d bars, %.1f s, peak %.1f dBFS, level %.1f dBFS, %d samples squeezed, made in %.0f ms\n", argv[1], bars, secs,
           20 * log10(all_peak / 32768.0 + 1e-9), 10 * log10(all_sq / ((double)music_ticks() * MUSIC_TICK) / 32768.0 / 32768.0 + 1e-12),
           over, took * 1000);
    return 0;
}
