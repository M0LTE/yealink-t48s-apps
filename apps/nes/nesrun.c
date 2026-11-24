// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * nesrun: the core without the phone, for measuring and testing.
 *
 *   nesrun ROM [-f FRAMES] [-s SHOT.ppm] [-k FRAME:BUTTONS:LENGTH,...] [-n] [-t LOG]
 *
 * Runs FRAMES frames (600 unless said) as fast as it can, drawing each at the
 * app's size into memory, and prints frames a second. -n draws nothing (the
 * cost of a skipped frame). -k holds pad BUTTONS (NES_* bits, as a number)
 * from FRAME for LENGTH frames. -s saves the last picture. -t runs nestest's
 * automatic mode and compares each instruction with its log. -b runs one of
 * blargg's test ROMs until it reports at $6000, and prints what it says.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nes.h"
#include "scale.h"

static uint32_t screen[512 * 480];

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int nestest(nes_t *n, const char *log)
{
    FILE *f = fopen(log, "r");
    char line[256];
    long cyc = 7;
    int count = 0;
    if (!f) return perror(log), 1;
    n->st.cpu.pc = 0xC000;
    n->st.cpu.p = 0x24;
    n->st.cpu.clk = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned pc, a, x, y, p, sp;
        long want;
        const struct nes_cpu *c = &n->st.cpu;
        const char *r = strstr(line, "A:"), *q = strstr(line, "CYC:");
        if (!r || !q || sscanf(line, "%4x", &pc) != 1 || sscanf(r, "A:%2x X:%2x Y:%2x P:%2x SP:%2x", &a, &x, &y, &p, &sp) != 5) continue;
        want = atol(q + 4);
        cyc = 7 + c->clk / 3;
        if (c->pc != pc || c->a != a || c->x != x || c->y != y || (c->p | 0x20) != (p | 0x20) || c->s != sp ||
            (want > 400 && cyc != want)) {
            printf("differs at instruction %d:\nwant %sgot  %04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%ld\n", count, line,
                   c->pc, c->a, c->x, c->y, c->p, c->s, cyc);
            return 1;
        }
        cpu_run(n, c->clk + 1);
        count++;
    }
    printf("nestest: %d instructions match, $02=%02X $03=%02X\n", count, n->st.ram[2], n->st.ram[3]);
    return n->st.ram[2] || n->st.ram[3];
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *shot = NULL, *keys = "", *log = NULL, *err;
    int frames = 600, draw = 1, blargg = 0;
    struct scale_target target = {screen, 512};
    nes_t *n = calloc(1, sizeof *n);
    uint8_t *data;
    long len;
    FILE *f;
    double t0, t;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "-n")) draw = 0;
        else if (!strcmp(argv[i], "-b")) blargg = 1;
        else rom = argv[i];
    }
    if (!rom || !n || !(f = fopen(rom, "rb"))) return fprintf(stderr, "usage: nesrun ROM [-f FRAMES] [-s SHOT.ppm] [-k F:B:L,...] [-n] [-t LOG]\n"), 2;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);
    if ((err = nes_load(n, data, (size_t)len))) return fprintf(stderr, "%s\n", err), 1;
    nes_set_video(n, scale_line_2x, &target);
    if (log) return nestest(n, log);

    t0 = now();
    for (int i = 0; i < frames; i++) {
        unsigned pad = 0;
        for (const char *k = keys; *k; k = strchr(k, ',') ? strchr(k, ',') + 1 : "") {
            int at, b, l;
            if (sscanf(k, "%d:%d:%d", &at, &b, &l) == 3 && i >= at && i < at + l) pad |= (unsigned)b;
        }
        nes_frame(n, pad, draw || i == frames - 1);
        if (blargg && !memcmp(n->st.prg_ram + 1, "\xDE\xB0\x61", 3)) {
            const uint8_t *r = n->st.prg_ram;
            if (r[0] == 0x81) {
                for (int j = 0; j < 10; j++) nes_frame(n, 0, 0);
                nes_reset(n, 0);
            } else if (r[0] < 0x80) {
                printf("%s: %d %s\n", rom, r[0], (const char *)r + 4);
                return r[0];
            }
        }
    }
    if (blargg) return printf("%s: no result\n", rom), 1;
    t = now() - t0;
    printf("%d frames in %.3f s: %.0f fps, %.2f ms a frame, mapper %d\n", frames, t, frames / t, t * 1000 / frames, n->mapper);
    if (shot && (f = fopen(shot, "wb"))) {
        fprintf(f, "P6\n512 480\n255\n");
        for (int i = 0; i < 512 * 480; i++) {
            uint8_t p[3] = {(uint8_t)(screen[i] >> 16), (uint8_t)(screen[i] >> 8), (uint8_t)screen[i]};
            fwrite(p, 1, 3, f);
        }
        fclose(f);
    }
    return 0;
}
