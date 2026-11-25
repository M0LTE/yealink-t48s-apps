// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The APU. Its timing is what games read back and must be right whether or
 * not anything is heard: the frame sequencer and its IRQ, the length counters
 * that $4015 reports, the triangle's linear counter, the DMC's appetite for
 * bytes, the cycles it takes from the CPU for each and its IRQ.
 *
 * The sound: two pulses with envelope and sweep, triangle, noise and the DMC,
 * through the console's non-linear mixer and its two high-pass filters. The
 * channels are run from one change of output to the next, not a cycle at a
 * time, and each change of the mixed level goes into a buffer at the output
 * rate as a band-limited step (a windowed sinc, placed to 1/128 of a sample).
 * Summing that buffer gives the sound with nothing folded back from above
 * half the output rate, which at 16 kHz is most of what a square wave has.
 */
#include <limits.h>
#include <math.h>
#include <string.h>
#include "nes.h"

#define CPU_HZ_NUM 39375000     /* the CPU's clock: 39375000 / 22 Hz */
#define CPU_HZ_DEN 22
#define PHASES 128
#define TAPS NES_SND_TAPS
#define KERNEL_ONE 16384        /* what each row of the kernel adds up to */
#define LEVEL_ONE 32768.0       /* the mixer at full output */
#define GAIN 131072             /* 16-bit samples for a mixer output of 1: games use a fraction of it */
#define KNEE 20000              /* above this the output is eased towards full scale, not cut off */
#define MAX_RUN 4096            /* CPU cycles of sound made in one go */

static const uint8_t len_table[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                      12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const uint16_t dmc_rates[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
static const uint16_t noise_periods[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
/* the four pulse shapes, bit i for step i: 12.5%, 25%, 50% and 25% upside down */
static const uint8_t duty[4] = {0x02, 0x06, 0x1E, 0xF9};
/* CPU cycles into the sequence at which each step falls; the last one wraps it */
static const int32_t steps[2][4] = {{7457, 14913, 22371, 29829}, {7457, 14913, 22371, 37281}};

static uint16_t pulse_mix[31], tnd_mix[203];
static int16_t kernel[PHASES][TAPS];
static int tables_made;

/* The mixer's two curves (the resistor ladders load each other, so two
 * channels together are quieter than their sum), and the step: a low-pass at
 * 0.46 of the output rate under a Hann window, one row for each place in a
 * sample that a step can fall. */
static void make_tables(void)
{
    const double pi = 3.14159265358979323846, fc = 0.46;
    if (tables_made) return;
    for (int i = 1; i < 31; i++) pulse_mix[i] = (uint16_t)(LEVEL_ONE * 95.52 / (8128.0 / i + 100) + 0.5);
    for (int i = 1; i < 203; i++) tnd_mix[i] = (uint16_t)(LEVEL_ONE * 163.67 / (24329.0 / i + 100) + 0.5);
    for (int p = 0; p < PHASES; p++) {
        double h[TAPS], sum = 0;
        int total = 0, big = 0;
        for (int k = 0; k < TAPS; k++) {
            double u = k + 1 - (double)p / PHASES, x = 2 * fc * pi * (u - TAPS / 2);
            h[k] = (x > -1e-9 && x < 1e-9 ? 1.0 : sin(x) / x) * (0.5 - 0.5 * cos(2 * pi * u / TAPS));
            sum += h[k];
        }
        for (int k = 0; k < TAPS; k++) {
            kernel[p][k] = (int16_t)floor(h[k] / sum * KERNEL_ONE + 0.5);
            total += kernel[p][k];
            if (kernel[p][k] > kernel[p][big]) big = k;
        }
        kernel[p][big] = (int16_t)(kernel[p][big] + KERNEL_ONE - total);   /* exactly one, or a held level would drift */
    }
    tables_made = 1;
}

/* ---------- the sound buffer ---------- */

static void set_rate(struct nes_sound *s, int ppm)
{
    s->spc = (uint32_t)(((uint64_t)s->rate * CPU_HZ_DEN << 24) * (uint64_t)(1000000 + ppm) / ((uint64_t)CPU_HZ_NUM * 1000000));
}

void nes_sound_start(nes_t *n, int rate)
{
    struct nes_sound *s = &n->snd;
    memset(s, 0, sizeof *s);
    if (rate < 8000 || rate > 48000) return;
    make_tables();
    s->rate = rate;
    set_rate(s, 0);
    /* the console's output stage: high-pass at 90 Hz and at 440 Hz. Its
     * low-pass is at 14 kHz, above anything a 16 kHz output can carry. */
    s->c1 = (int32_t)(65536 * (1 - exp(-2 * 3.14159265358979323846 * 90 / rate)));
    s->c2 = (int32_t)(65536 * (1 - exp(-2 * 3.14159265358979323846 * 440 / rate)));
    s->on = s->fresh = 1;
}

void nes_sound_tune(nes_t *n, int ppm)
{
    if (!n->snd.on) return;
    set_rate(&n->snd, ppm < -20000 ? -20000 : ppm > 20000 ? 20000 : ppm);
}

/* out NULL: they are thrown away, and the filters still follow */
int nes_sound_read(nes_t *n, int16_t *out, int max)
{
    struct nes_sound *s = &n->snd;
    int count = (int)(s->at >> 20);
    int32_t sum = s->sum, hp1 = s->hp1, hp2 = s->hp2;
    if (!s->on) return 0;
    if (count > max) count = max;
    for (int i = 0; i < count; i++) {
        int32_t was = hp1, v;
        sum = (int32_t)((uint32_t)sum + s->buf[i]);          /* the level itself, kept only to be looked at */
        hp1 = (int32_t)((uint32_t)hp1 + s->buf[i]);
        hp1 -= (int32_t)(((int64_t)hp1 * s->c1) >> 16);
        hp2 += hp1 - was;
        hp2 -= (int32_t)(((int64_t)hp2 * s->c2) >> 16);
        if (out) {
            int32_t over;
            v = (int32_t)(((int64_t)hp2 * GAIN) >> 29);
            over = (v < 0 ? -v : v) - KNEE;
            if (over > 0) {
                over = KNEE + (int32_t)((int64_t)over * (32767 - KNEE) / (over + 32767 - KNEE));
                v = v < 0 ? -over : over;
            }
            out[i] = (int16_t)v;
        }
    }
    s->sum = sum;
    s->hp1 = hp1;
    s->hp2 = hp2;
    memmove(s->buf, s->buf + count, (NES_SND_LEN + TAPS - (size_t)count) * sizeof s->buf[0]);
    memset(s->buf + NES_SND_LEN + TAPS - count, 0, (size_t)count * sizeof s->buf[0]);
    s->at -= (uint32_t)count << 20;
    return count;
}

/* a change of level d, t CPU cycles into this run */
static inline void step_at(struct nes_sound *s, int32_t t, int32_t d)
{
    uint32_t pos = s->at + ((s->sub + (uint32_t)t * s->spc) >> 4);
    uint32_t *b = s->buf + (pos >> 20);
    const int16_t *k = kernel[(pos >> 13) & (PHASES - 1)];
    for (int i = 0; i < TAPS; i++) b[i] += (uint32_t)(d * k[i]);
}

/* ---------- the channels ---------- */

/* a pulse's volume now: 0 while its length has run out or the sweep mutes it */
static unsigned pulse_vol(const struct nes_apu *a, int i)
{
    const uint8_t *r = a->reg + i * 4;
    unsigned p = a->period[i];
    if (!a->len[i] || p < 8) return 0;
    if (!(r[1] & 8) && p + (p >> (r[1] & 7)) > 0x7FF) return 0;
    return (r[0] & 0x10) ? r[0] & 15u : a->env_vol[i];
}

static void run_pulses(nes_t *n, int32_t cyc)
{
    struct nes_apu *a = &n->st.apu;
    struct nes_sound *s = &n->snd;
    int32_t t[2], step[2], level;
    unsigned vol[2], out[2], shape[2], seq[2];

    for (int i = 0; i < 2; i++) {
        step[i] = (a->period[i] + 1) * 2;
        vol[i] = pulse_vol(a, i);
        shape[i] = duty[a->reg[i * 4] >> 6];
        seq[i] = a->pseq[i];
        t[i] = a->ptimer[i];
        out[i] = (shape[i] >> seq[i] & 1) ? vol[i] : 0;
        if (!vol[i]) {
            /* silent: only its place in the wave moves */
            if (t[i] < cyc) {
                int32_t k = (cyc - 1 - t[i]) / step[i] + 1;
                seq[i] = (seq[i] + (unsigned)k) & 7;
                t[i] += k * step[i];
            }
            a->ptimer[i] = t[i] - cyc;
            t[i] = INT32_MAX;
        }
    }
    level = pulse_mix[out[0] + out[1]];
    if (s->fresh) s->pulse_level = level;
    if (level != s->pulse_level) {
        step_at(s, 0, level - s->pulse_level);
        s->pulse_level = level;
    }
    for (;;) {
        int i = t[1] < t[0];
        int32_t now = t[i];
        if (now >= cyc) break;
        seq[i] = (seq[i] + 1) & 7;
        t[i] += step[i];
        out[i] = (shape[i] >> seq[i] & 1) ? vol[i] : 0;
        level = pulse_mix[out[0] + out[1]];
        if (level != s->pulse_level) {
            step_at(s, now, level - s->pulse_level);
            s->pulse_level = level;
        }
    }
    for (int i = 0; i < 2; i++) {
        a->pseq[i] = (uint8_t)seq[i];
        if (vol[i]) a->ptimer[i] = t[i] - cyc;
    }
}

static void dmc_restart(struct nes_apu *a)
{
    a->dmc_addr = (uint16_t)(0xC000 | a->reg[0x12] << 6);
    a->dmc_left = (uint16_t)(a->reg[0x13] * 16 + 1);
}

static void dmc_fill(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    if (a->dmc_buf_full || !a->dmc_left) return;
    a->dmc_buf = n->prg[(a->dmc_addr >> 13) & 3][a->dmc_addr & 0x1FFF];
    a->dmc_buf_full = 1;
    a->dmc_addr = (uint16_t)((a->dmc_addr + 1) | 0x8000);
    if (--a->dmc_left == 0) {
        if (a->dmc_loop) dmc_restart(a);
        else if (a->dmc_irq_on) n->st.cpu.irq_line |= IRQ_DMC;
    }
}

/* one tick of the DMC's timer: a bit moves the level, and every eighth takes the next byte */
static void dmc_clock(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    if (!a->dmc_silent) {
        if (a->dmc_shift & 1) {
            if (a->dmc_dac <= 125) a->dmc_dac += 2;
        } else if (a->dmc_dac >= 2) {
            a->dmc_dac -= 2;
        }
        a->dmc_shift >>= 1;
    }
    if (--a->dmc_bits == 0) {
        a->dmc_bits = 8;
        a->dmc_silent = !a->dmc_buf_full;
        if (a->dmc_buf_full) {
            a->dmc_shift = a->dmc_buf;
            a->dmc_buf_full = 0;
            if (a->dmc_left) {
                dmc_fill(n);
                n->st.cpu.clk += 4 * 3;    /* the CPU is held while the byte is read */
            }
        }
    }
}

/* Noise faster than this many CPU cycles a step is not followed flip by flip:
 * that is up to 220000 steps a second for a hiss. It is taken a slot at a
 * time instead, and the slot's level is the share of it the noise was high. */
#define NOISE_SLOT 32

/* the mixer's other curve: triangle, noise (high for nfrac eighths of the time) and DMC */
static inline int32_t tnd_level(unsigned tri_out, unsigned nvol, unsigned nfrac, unsigned dac)
{
    int32_t lo = tnd_mix[3 * tri_out + dac];
    if (!nfrac) return lo;
    return lo + (((int32_t)tnd_mix[3 * tri_out + 2 * nvol + dac] - lo) * (int32_t)nfrac >> 3);
}

/* triangle, noise and DMC */
static void run_tnd(nes_t *n, int32_t cyc)
{
    struct nes_apu *a = &n->st.apu;
    struct nes_sound *s = &n->snd;
    unsigned tri_period = a->reg[0x0A] | (a->reg[0x0B] & 7u) << 8;
    unsigned nvol = !a->len[3] ? 0 : (a->reg[0x0C] & 0x10) ? a->reg[0x0C] & 15u : a->env_vol[2];
    /* a triangle too high to hear is held where it is: stepping it would only make a whine */
    int tri_on = s->on && a->len[2] && a->lin && tri_period >= 2, noise_on = s->on && nvol;
    int32_t tri_step = (int32_t)tri_period + 1, noise_step = noise_periods[a->reg[0x0E] & 15], dmc_step = dmc_rates[a->dmc_rate];
    int fast = noise_step < NOISE_SLOT, per_slot = NOISE_SLOT / noise_step;
    int32_t tt = tri_on ? a->tri_timer : INT32_MAX, tn = noise_on ? a->noise_timer : INT32_MAX, td = a->dmc_timer - 1;
    unsigned tri_seq = a->tri_seq, lfsr = a->lfsr, tap = (a->reg[0x0E] & 0x80) ? 6 : 1;
    unsigned tri_out = tri_seq < 16 ? 15 - tri_seq : tri_seq - 16;
    unsigned nfrac = !noise_on ? 0 : fast ? s->nfrac : (lfsr & 1) ? 0 : 8;
    int32_t level;

    if (s->on) {
        level = tnd_level(tri_out, nvol, nfrac, a->dmc_dac);
        if (s->fresh) s->tnd_level = level;
        if (level != s->tnd_level) {
            step_at(s, 0, level - s->tnd_level);
            s->tnd_level = level;
        }
    }
    for (;;) {
        int32_t now = td;
        int which = 2;
        if (tt < now) { now = tt; which = 0; }
        if (tn < now) { now = tn; which = 1; }
        if (now >= cyc) break;
        if (which == 0) {
            tri_seq = (tri_seq + 1) & 31;
            tri_out = tri_seq < 16 ? 15 - tri_seq : tri_seq - 16;
            tt += tri_step;
        } else if (which == 1) {
            unsigned was = nfrac;
            if (fast) {
                unsigned high = 0;
                for (int i = 0; i < per_slot; i++) {
                    lfsr = lfsr >> 1 | ((lfsr ^ lfsr >> tap) & 1) << 14;
                    high += ~lfsr & 1;
                }
                nfrac = high * 8 / (unsigned)per_slot;
                tn += NOISE_SLOT;
            } else {
                lfsr = lfsr >> 1 | ((lfsr ^ lfsr >> tap) & 1) << 14;
                nfrac = (lfsr & 1) ? 0 : 8;
                tn += noise_step;
            }
            if (nfrac == was) continue;
        } else {
            unsigned was = a->dmc_dac;
            dmc_clock(n);
            td += dmc_step;
            if (a->dmc_dac == was) continue;
        }
        if (!s->on) continue;
        level = tnd_level(tri_out, nvol, nfrac, a->dmc_dac);
        if (level != s->tnd_level) {
            step_at(s, now, level - s->tnd_level);
            s->tnd_level = level;
        }
    }
    a->tri_seq = (uint8_t)tri_seq;
    a->lfsr = (uint16_t)lfsr;
    s->nfrac = (uint8_t)nfrac;
    if (tri_on) a->tri_timer = tt - cyc;
    if (noise_on) a->noise_timer = tn - cyc;
    a->dmc_timer = td + 1 - cyc;
}

/* cyc CPU cycles in which no register and no envelope changes */
static void run_sound(nes_t *n, int32_t cyc)
{
    struct nes_sound *s = &n->snd;
    if (s->on) {
        uint32_t total;
        if ((s->at >> 20) > NES_SND_LEN - 256) nes_sound_read(n, NULL, NES_SND_LEN);   /* nobody is reading */
        run_pulses(n, cyc);
        run_tnd(n, cyc);
        s->fresh = 0;
        total = s->sub + (uint32_t)cyc * s->spc;
        s->at += total >> 4;
        s->sub = total & 15;
    } else {
        run_tnd(n, cyc);
    }
}

void apu_reset(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    int32_t clk = n->st.cpu.clk;
    *a = (struct nes_apu){0};
    a->clk = clk;
    a->dmc_bits = 8;
    a->dmc_timer = dmc_rates[0];
    a->dmc_silent = 1;
    a->lfsr = 1;
    n->snd.fresh = 1;
    n->st.cpu.irq_line &= (uint8_t)~(IRQ_FRAME | IRQ_DMC);
}

static void quarter_frame(struct nes_apu *a)
{
    if (a->lin_reload) a->lin = a->reg[0x08] & 0x7F;
    else if (a->lin) a->lin--;
    if (!(a->reg[0x08] & 0x80)) a->lin_reload = 0;
    /* the envelopes: a volume that falls from 15, and may go round again */
    for (int i = 0; i < 3; i++) {
        unsigned r = a->reg[i == 2 ? 0x0C : i * 4];
        if (a->env_start[i]) {
            a->env_start[i] = 0;
            a->env_vol[i] = 15;
            a->env_div[i] = r & 15;
        } else if (a->env_div[i]) {
            a->env_div[i]--;
        } else {
            a->env_div[i] = r & 15;
            if (a->env_vol[i]) a->env_vol[i]--;
            else if (r & 0x20) a->env_vol[i] = 15;
        }
    }
}

static void half_frame(struct nes_apu *a)
{
    if (a->len[0] && !(a->reg[0x00] & 0x20)) a->len[0]--;
    if (a->len[1] && !(a->reg[0x04] & 0x20)) a->len[1]--;
    if (a->len[2] && !(a->reg[0x08] & 0x80)) a->len[2]--;
    if (a->len[3] && !(a->reg[0x0C] & 0x20)) a->len[3]--;
    /* the sweeps: each pulse's period slides by a fraction of itself */
    for (int i = 0; i < 2; i++) {
        unsigned r = a->reg[i * 4 + 1], p = a->period[i], shift = r & 7;
        if (!a->sw_div[i] && (r & 0x80) && shift && p >= 8) {
            int target = (r & 8) ? (int)p - (int)(p >> shift) - !i : (int)(p + (p >> shift));   /* pulse 1 goes down one more */
            if (target < 0) target = 0;
            if (target <= 0x7FF) a->period[i] = (uint16_t)target;
        }
        if (!a->sw_div[i] || a->sw_reload[i]) {
            a->sw_div[i] = (r >> 4) & 7;
            a->sw_reload[i] = 0;
        } else {
            a->sw_div[i]--;
        }
    }
}

void apu_run(nes_t *n, int32_t clk)
{
    struct nes_apu *a = &n->st.apu;
    int32_t cyc = (clk - a->clk) / 3, left;
    if (cyc <= 0) return;
    a->clk += cyc * 3;

    for (left = cyc; left > 0;) {
        int step = 0;
        int32_t next, go;
        while (step < 3 && a->seq >= steps[a->mode5][step]) step++;
        next = steps[a->mode5][step];
        go = next - a->seq < left ? next - a->seq : left;
        if (go > MAX_RUN) go = MAX_RUN;
        run_sound(n, go);
        a->seq += go;
        left -= go;
        if (a->seq < next) continue;
        quarter_frame(a);
        if (step == 1 || step == 3) half_frame(a);
        if (step == 3) {
            if (!a->mode5 && !a->irq_off) n->st.cpu.irq_line |= IRQ_FRAME;
            a->seq = -1;
        }
    }
}

uint8_t apu_read_status(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    struct nes_cpu *c = &n->st.cpu;
    int frame_irq_early = !a->mode5 && !a->irq_off && a->seq == steps[0][3] - 1;   /* the flag is up a cycle before the IRQ */
    unsigned r = (a->len[0] ? 1 : 0) | (a->len[1] ? 2 : 0) | (a->len[2] ? 4 : 0) | (a->len[3] ? 8 : 0) |
                 (a->dmc_left ? 0x10 : 0) | ((c->irq_line & IRQ_FRAME) || frame_irq_early ? 0x40 : 0) | ((c->irq_line & IRQ_DMC) ? 0x80 : 0);
    c->irq_line &= (uint8_t)~IRQ_FRAME;
    return (uint8_t)r;
}

void apu_write(nes_t *n, unsigned addr, unsigned v)
{
    struct nes_apu *a = &n->st.apu;
    struct nes_cpu *c = &n->st.cpu;
    unsigned r = addr - 0x4000;
    if (r >= sizeof a->reg) return;
    a->reg[r] = (uint8_t)v;
    switch (r) {
    case 0x01: case 0x05: a->sw_reload[r >> 2] = 1; break;
    case 0x02: case 0x06: a->period[r >> 2] = (uint16_t)((a->period[r >> 2] & 0x700) | v); break;
    case 0x03: case 0x07:
        if (a->enabled & (1 << (r >> 2))) a->len[r >> 2] = len_table[v >> 3];
        a->period[r >> 2] = (uint16_t)((a->period[r >> 2] & 0xFF) | (v & 7) << 8);
        a->pseq[r >> 2] = 0;       /* the wave starts again, and so does the envelope */
        a->env_start[r >> 2] = 1;
        break;
    case 0x0B:
        if (a->enabled & 4) a->len[2] = len_table[v >> 3];
        a->lin_reload = 1;
        break;
    case 0x0F:
        if (a->enabled & 8) a->len[3] = len_table[v >> 3];
        a->env_start[2] = 1;
        break;
    case 0x11: a->dmc_dac = v & 0x7F; break;
    case 0x10:
        a->dmc_irq_on = v & 0x80;
        a->dmc_loop = v & 0x40;
        a->dmc_rate = v & 15;
        if (!a->dmc_irq_on) c->irq_line &= (uint8_t)~IRQ_DMC;
        break;
    case 0x15:
        a->enabled = v & 0x1F;
        for (int i = 0; i < 4; i++)
            if (!(v & 1 << i)) a->len[i] = 0;
        c->irq_line &= (uint8_t)~IRQ_DMC;
        if (!(v & 0x10)) {
            a->dmc_left = 0;
        } else if (!a->dmc_left) {
            dmc_restart(a);
            dmc_fill(n);
        }
        break;
    case 0x17:
        a->mode5 = v >> 7;
        a->irq_off = v & 0x40;
        if (a->irq_off) c->irq_line &= (uint8_t)~IRQ_FRAME;
        a->seq = -3;    /* the sequence restarts a few cycles after the write */
        if (a->mode5) {
            quarter_frame(a);
            half_frame(a);
        }
        break;
    }
}

/* When the next IRQ will be raised (the frame sequencer's, or the DMC's as it
 * takes its last byte), so the CPU can be stopped there. */
/* 1 while the DMC is reading a sample: it holds the CPU as it goes, so it
 * cannot be left to catch up later. */
int apu_dmc_busy(const nes_t *n)
{
    return n->st.apu.dmc_left || n->st.apu.dmc_buf_full;
}

int32_t apu_next_irq(const nes_t *n)
{
    const struct nes_apu *a = &n->st.apu;
    int32_t frame = INT32_MAX, dmc = INT32_MAX;
    if (!a->mode5 && !a->irq_off) frame = a->clk + (steps[0][3] - a->seq) * 3;
    if (a->dmc_irq_on && !a->dmc_loop && a->dmc_left && a->dmc_buf_full) {
        int32_t rate = dmc_rates[a->dmc_rate];
        dmc = a->clk + (a->dmc_timer + (a->dmc_bits - 1 + (a->dmc_left - 1) * 8) * rate) * 3;
    }
    return frame < dmc ? frame : dmc;
}
