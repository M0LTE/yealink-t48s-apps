// SPDX-License-Identifier: AGPL-3.0-or-later
/* Cartridge boards: NROM, MMC1, UxROM, CNROM, MMC3, AxROM, Color Dreams,
 * BNROM, GxROM and Camerica. Bus conflicts are not modelled. */
#include <limits.h>
#include <string.h>
#include "nes.h"

int mapper_supported(int mapper)
{
    static const uint8_t known[] = {0, 1, 2, 3, 4, 7, 11, 34, 66, 71};
    for (unsigned i = 0; i < sizeof known; i++)
        if (known[i] == mapper) return 1;
    return 0;
}

/* banks count from 0; a negative one counts back from the end */
static void prg8(nes_t *n, int slot, int bank)
{
    int count = (int)(n->prg_size / 0x2000);
    bank %= count;
    if (bank < 0) bank += count;
    n->prg[slot] = n->prg_rom + bank * 0x2000;
}

static void prg16(nes_t *n, int slot, int bank)
{
    prg8(n, slot * 2, bank * 2);
    prg8(n, slot * 2 + 1, bank * 2 + 1);
}

static void prg32(nes_t *n, int bank)
{
    for (int i = 0; i < 4; i++) prg8(n, i, bank * 4 + i);
}

static void chr1(nes_t *n, int slot, int bank)
{
    if (n->chr_is_ram) n->chr[slot] = n->st.chr_ram + (bank & 7) * 0x400;
    else n->chr[slot] = n->chr_rom + (unsigned)bank % (n->chr_size / 0x400) * 0x400;
}

static void chr4(nes_t *n, int slot, int bank)
{
    for (int i = 0; i < 4; i++) chr1(n, slot * 4 + i, bank * 4 + i);
}

static void chr8(nes_t *n, int bank)
{
    for (int i = 0; i < 8; i++) chr1(n, i, bank * 8 + i);
}

void mapper_sync(nes_t *n)
{
    struct nes_mapper *m = &n->st.map;
    int mirror = n->hdr_mirror;
    switch (n->mapper) {
    case 0:
        prg16(n, 0, 0);
        prg16(n, 1, -1);
        chr8(n, 0);
        break;
    case 1: {
        static const uint8_t mirrors[4] = {MIRROR_ONE_LO, MIRROR_ONE_HI, MIRROR_V, MIRROR_H};
        int hi = n->prg_size > 0x40000 ? (m->chr0 & 0x10) : 0, p = m->prg & 0x0F;   /* 512 KB boards borrow a CHR bit */
        switch ((m->ctrl >> 2) & 3) {
        case 0: case 1: prg16(n, 0, hi | (p & 0x0E)); prg16(n, 1, hi | p | 1); break;
        case 2: prg16(n, 0, hi); prg16(n, 1, hi | p); break;
        default: prg16(n, 0, hi | p); prg16(n, 1, hi | 0x0F); break;
        }
        if (m->ctrl & 0x10) {
            chr4(n, 0, m->chr0);
            chr4(n, 1, m->chr1);
        } else {
            chr8(n, m->chr0 >> 1);
        }
        mirror = mirrors[m->ctrl & 3];
        break;
    }
    case 2:
    case 71:
        prg16(n, 0, m->reg[0]);
        prg16(n, 1, -1);
        chr8(n, 0);
        break;
    case 3:
        prg16(n, 0, 0);
        prg16(n, 1, -1);
        chr8(n, m->reg[0]);
        break;
    case 4: {
        int swap = (m->sel & 0x40) ? 2 : 0, inv = (m->sel & 0x80) ? 4 : 0;
        prg8(n, swap, m->reg[6]);
        prg8(n, 1, m->reg[7]);
        prg8(n, 2 - swap, -2);
        prg8(n, 3, -1);
        chr1(n, inv ^ 0, m->reg[0] & 0xFE);
        chr1(n, inv ^ 1, m->reg[0] | 1);
        chr1(n, inv ^ 2, m->reg[1] & 0xFE);
        chr1(n, inv ^ 3, m->reg[1] | 1);
        for (int i = 0; i < 4; i++) chr1(n, inv ^ (4 + i), m->reg[2 + i]);
        mirror = m->mirror;
        break;
    }
    case 7:
        prg32(n, m->reg[0] & 7);
        chr8(n, 0);
        mirror = (m->reg[0] & 0x10) ? MIRROR_ONE_HI : MIRROR_ONE_LO;
        break;
    case 11:
        prg32(n, m->reg[0] & 3);
        chr8(n, m->reg[0] >> 4);
        break;
    case 34:
        prg32(n, m->reg[0]);
        chr8(n, 0);
        break;
    case 66:
        prg32(n, (m->reg[0] >> 4) & 3);
        chr8(n, m->reg[0] & 3);
        break;
    }
    ppu_set_mirror(n, n->four_screen ? MIRROR_FOUR : mirror);
}

void mapper_reset(nes_t *n)
{
    struct nes_mapper *m = &n->st.map;
    memset(m, 0, sizeof *m);
    m->mirror = (uint8_t)n->hdr_mirror;
    m->ctrl = 0x0C;
    m->last_wr = INT32_MIN;
    if (n->mapper == 4) {
        static const uint8_t r[8] = {0, 2, 4, 5, 6, 7, 0, 1};
        memcpy(m->reg, r, 8);
        m->ram_ctl = 0x80;
    }
    mapper_sync(n);
}

void mapper_write(nes_t *n, unsigned addr, unsigned v)
{
    struct nes_mapper *m = &n->st.map;
    switch (n->mapper) {
    case 0: return;
    case 1:
        /* the second write of one instruction is not seen */
        if (n->st.cpu.clk == m->last_wr) return;
        m->last_wr = n->st.cpu.clk;
        if (v & 0x80) {
            m->shift = m->shift_n = 0;
            m->ctrl |= 0x0C;
        } else {
            m->shift |= (v & 1) << m->shift_n;
            if (++m->shift_n < 5) return;
            switch ((addr >> 13) & 3) {
            case 0: m->ctrl = m->shift; break;
            case 1: m->chr0 = m->shift; break;
            case 2: m->chr1 = m->shift; break;
            default: m->prg = m->shift; break;
            }
            m->shift = m->shift_n = 0;
        }
        break;
    case 4:
        switch (addr & 0xE001) {
        case 0x8000: m->sel = (uint8_t)v; break;
        case 0x8001: m->reg[m->sel & 7] = (uint8_t)v; break;
        case 0xA000: m->mirror = (v & 1) ? MIRROR_H : MIRROR_V; break;
        case 0xA001: m->ram_ctl = (uint8_t)v; return;
        case 0xC000: m->irq_latch = (uint8_t)v; return;
        case 0xC001: m->irq_count = 0; m->irq_reload = 1; return;
        case 0xE000: m->irq_on = 0; n->st.cpu.irq_line &= (uint8_t)~IRQ_MAPPER; return;
        case 0xE001: m->irq_on = 1; return;
        }
        break;
    case 71:
        if (addr < 0xC000) return;
        m->reg[0] = (uint8_t)v;
        break;
    default:
        m->reg[0] = (uint8_t)v;
        break;
    }
    mapper_sync(n);
}

void mapper_scanline(nes_t *n)
{
    struct nes_mapper *m = &n->st.map;
    if (n->mapper != 4) return;
    if (!m->irq_count || m->irq_reload) {
        m->irq_count = m->irq_latch;
        m->irq_reload = 0;
    } else {
        m->irq_count--;
    }
    if (!m->irq_count && m->irq_on) n->st.cpu.irq_line |= IRQ_MAPPER;
}

int mapper_ram(const nes_t *n)
{
    const struct nes_mapper *m = &n->st.map;
    if (n->mapper == 1) return (m->prg & 0x10) ? 0 : 2;
    if (n->mapper == 4) return !(m->ram_ctl & 0x80) ? 0 : (m->ram_ctl & 0x40) ? 1 : 2;
    return 2;
}
