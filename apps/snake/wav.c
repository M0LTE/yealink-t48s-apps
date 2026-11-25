// SPDX-License-Identifier: AGPL-3.0-or-later
/* snake-wav: every sound the game makes, one after another, as a sound file
 * made on a PC by the synth the phone runs. `make wav` writes build/snake.wav
 * and says how long and how loud each one is. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "sfx.h"

static void le(FILE *f, unsigned v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 255, f); }

static long play(FILE *f, const char *name)
{
    int16_t buf[320];
    int peak = 0, live;
    long n = 0;
    do {
        live = sfx_render(buf, 320);
        for (int i = 0; i < 320; i++) { if (abs(buf[i]) > peak) peak = abs(buf[i]); le(f, (unsigned)buf[i] & 0xffff, 2); }
        n += 320;
    } while (live);
    printf("%-8s %4ld ms  peak %5.1f dBFS\n", name, n * 1000 / SFX_RATE - 20, 20 * log10(peak / 32768.0 + 1e-9));
    for (int i = 0; i < SFX_RATE * 2 / 5; i++) le(f, 0, 2);     /* a gap before the next */
    return n + SFX_RATE * 2 / 5;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: snake-wav OUT.wav\n"); return 2; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    long n = 0;
    fseek(f, 44, SEEK_SET);
    sfx_tables();
    sfx_start(); n += play(f, "start");
    sfx_turn(); n += play(f, "turn");
    sfx_eat(0); n += play(f, "eat 0");
    sfx_eat(18); n += play(f, "eat 18");
    sfx_eat(36); n += play(f, "eat 36");
    sfx_crash(); n += play(f, "crash");
    sfx_best(); n += play(f, "best");
    unsigned bytes = (unsigned)n * 2;
    rewind(f);
    fputs("RIFF", f); le(f, 36 + bytes, 4); fputs("WAVEfmt ", f);
    le(f, 16, 4); le(f, 1, 2); le(f, 1, 2); le(f, SFX_RATE, 4); le(f, SFX_RATE * 2, 4); le(f, 2, 2); le(f, 16, 2);
    fputs("data", f); le(f, bytes, 4);
    fclose(f);
    return 0;
}
