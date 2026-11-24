// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The APU's timing, without the sound: the frame sequencer and its IRQ, the
 * length counters that $4015 reports, the triangle's linear counter, and the
 * DMC's appetite for bytes and its IRQ. Games read these, so they must be
 * right even when nothing is heard. The registers are kept as written; sound
 * is a matter of adding the envelopes, sweeps and timers at the two marked
 * places and mixing what they make.
 */
#include "nes.h"

static const uint8_t len_table[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                      12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const uint16_t dmc_rates[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
/* CPU cycles into the sequence at which each step falls; the last one wraps it */
static const int32_t steps[2][4] = {{7457, 14913, 22371, 29829}, {7457, 14913, 22371, 37281}};

void apu_reset(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    int32_t clk = n->st.cpu.clk;
    *a = (struct nes_apu){0};
    a->clk = clk;
    a->dmc_bits = 8;
    a->dmc_timer = dmc_rates[0];
    n->st.cpu.irq_line &= (uint8_t)~(IRQ_FRAME | IRQ_DMC);
}

static void quarter_frame(struct nes_apu *a)
{
    if (a->lin_reload) a->lin = a->reg[0x08] & 0x7F;
    else if (a->lin) a->lin--;
    if (!(a->reg[0x08] & 0x80)) a->lin_reload = 0;
    /* sound: clock the three envelopes here */
}

static void half_frame(struct nes_apu *a)
{
    if (a->len[0] && !(a->reg[0x00] & 0x20)) a->len[0]--;
    if (a->len[1] && !(a->reg[0x04] & 0x20)) a->len[1]--;
    if (a->len[2] && !(a->reg[0x08] & 0x80)) a->len[2]--;
    if (a->len[3] && !(a->reg[0x0C] & 0x20)) a->len[3]--;
    /* sound: clock the two sweeps here */
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
        a->seq += go;
        left -= go;
        if (a->seq < next) break;
        quarter_frame(a);
        if (step == 1 || step == 3) half_frame(a);
        if (step == 3) {
            if (!a->mode5 && !a->irq_off) n->st.cpu.irq_line |= IRQ_FRAME;
            a->seq = -1;
        }
    }

    a->dmc_timer -= cyc;
    while (a->dmc_timer <= 0) {
        a->dmc_timer += dmc_rates[a->dmc_rate];
        if (--a->dmc_bits == 0) {
            a->dmc_bits = 8;
            if (a->dmc_buf_full) {
                a->dmc_buf_full = 0;    /* sound: the byte goes to the shifter */
                dmc_fill(n);
            }
        }
    }
}

uint8_t apu_read_status(nes_t *n)
{
    struct nes_apu *a = &n->st.apu;
    struct nes_cpu *c = &n->st.cpu;
    unsigned r = (a->len[0] ? 1 : 0) | (a->len[1] ? 2 : 0) | (a->len[2] ? 4 : 0) | (a->len[3] ? 8 : 0) |
                 (a->dmc_left ? 0x10 : 0) | ((c->irq_line & IRQ_FRAME) ? 0x40 : 0) | ((c->irq_line & IRQ_DMC) ? 0x80 : 0);
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
    case 0x03: if (a->enabled & 1) a->len[0] = len_table[v >> 3]; break;
    case 0x07: if (a->enabled & 2) a->len[1] = len_table[v >> 3]; break;
    case 0x0B:
        if (a->enabled & 4) a->len[2] = len_table[v >> 3];
        a->lin_reload = 1;
        break;
    case 0x0F: if (a->enabled & 8) a->len[3] = len_table[v >> 3]; break;
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
        a->seq = 0;
        if (a->mode5) {
            quarter_frame(a);
            half_frame(a);
        }
        break;
    }
}
