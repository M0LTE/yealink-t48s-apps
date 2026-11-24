// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The picture. A whole scanline is drawn at its start from the scroll
 * registers as they stand, which is right for games that change them in the
 * horizontal blank (nearly all). Sprite 0's hit is found while drawing and
 * shows in $2002 from the dot it would have happened at.
 */
#include <limits.h>
#include <string.h>
#include "nes.h"

/* The 2C02's 64 colours as 0x00RRGGBB. */
static const uint32_t colours[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
};

/* Four bits of a pattern byte as four pixels, one a byte, leftmost first in memory. */
static const uint32_t nib[16] = {
    0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
    0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

void ppu_palette_changed(nes_t *n)
{
    const struct nes_ppu *p = &n->st.ppu;
    unsigned grey = (p->mask & 1) ? 0x30 : 0x3F;
    for (int i = 0; i < 32; i++) {
        /* colour 0 of every background palette is the backdrop */
        uint32_t c = colours[p->pal[(i & 3) ? i : 0] & grey];
        if (p->mask & 0xE0) {
            /* emphasis: the channels not picked out go darker */
            unsigned r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF;
            if (!(p->mask & 0x20)) r = r * 3 / 4;
            if (!(p->mask & 0x40)) g = g * 3 / 4;
            if (!(p->mask & 0x80)) b = b * 3 / 4;
            c = r << 16 | g << 8 | b;
        }
        n->rgb[i] = c;
    }
}

void ppu_set_mirror(nes_t *n, int mirror)
{
    static const uint8_t map[5][4] = {{0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 3}};
    for (int i = 0; i < 4; i++) n->nt[i] = n->st.ppu.nt + map[mirror][i] * 0x400;
}

void ppu_reset(nes_t *n)
{
    struct nes_ppu *p = &n->st.ppu;
    p->ctrl = p->mask = p->status = p->oam_addr = 0;
    p->v = p->t = 0;
    p->x = p->w = p->rdbuf = p->latch = p->s0_found = 0;
    p->s0_clk = INT32_MAX;
    ppu_palette_changed(n);
}

static uint8_t mem_read(nes_t *n, unsigned a)
{
    if (a < 0x2000) return n->chr[a >> 10][a & 0x3FF];
    return n->nt[(a >> 10) & 3][a & 0x3FF];
}

/* after a read or write of $2007 */
static void data_step(nes_t *n)
{
    struct nes_ppu *p = &n->st.ppu;
    unsigned v = p->v + ((p->ctrl & 4) ? 32 : 1);
    if (~p->v & v & 0x1000) mapper_scanline(n);
    p->v = (uint16_t)v;
}

uint8_t ppu_read(nes_t *n, unsigned reg)
{
    struct nes_ppu *p = &n->st.ppu;
    unsigned a, r;
    switch (reg) {
    case 2:
        if (n->st.cpu.clk >= p->s0_clk) {
            p->status |= 0x40;
            p->s0_clk = INT32_MAX;
        }
        r = p->status | (p->latch & 0x1F);
        p->status &= 0x7F;
        p->w = 0;
        return p->latch = (uint8_t)r;
    case 4:
        r = p->oam[p->oam_addr];
        if ((p->oam_addr & 3) == 2) r &= 0xE3;
        return p->latch = (uint8_t)r;
    case 7:
        a = p->v & 0x3FFF;
        if (a < 0x3F00) {
            r = p->rdbuf;
            p->rdbuf = mem_read(n, a);
        } else {
            a &= 0x1F;
            if ((a & 0x13) == 0x10) a &= 0x0F;
            r = p->pal[a] & ((p->mask & 1) ? 0x30 : 0x3F);
            p->rdbuf = mem_read(n, (p->v & 0x3FFF) - 0x1000);
        }
        data_step(n);
        return p->latch = (uint8_t)r;
    }
    return p->latch;
}

void ppu_write(nes_t *n, unsigned reg, unsigned v)
{
    struct nes_ppu *p = &n->st.ppu;
    unsigned a;
    p->latch = (uint8_t)v;
    switch (reg) {
    case 0:
        if ((v & 0x80) && !(p->ctrl & 0x80) && (p->status & 0x80)) n->st.cpu.nmi_pending = 2;
        p->ctrl = (uint8_t)v;
        p->t = (uint16_t)((p->t & 0xF3FF) | (v & 3) << 10);
        break;
    case 1:
        a = p->mask;
        p->mask = (uint8_t)v;
        if ((a ^ v) & 0xE1) ppu_palette_changed(n);
        break;
    case 3: p->oam_addr = (uint8_t)v; break;
    case 4: p->oam[p->oam_addr++] = (uint8_t)v; break;
    case 5:
        if (!p->w) {
            p->t = (uint16_t)((p->t & 0xFFE0) | v >> 3);
            p->x = v & 7;
        } else {
            p->t = (uint16_t)((p->t & 0x8C1F) | (v & 7) << 12 | (v & 0xF8) << 2);
        }
        p->w ^= 1;
        break;
    case 6:
        if (!p->w) {
            p->t = (uint16_t)((p->t & 0x00FF) | (v & 0x3F) << 8);
        } else {
            p->t = (uint16_t)((p->t & 0xFF00) | v);
            if (~p->v & p->t & 0x1000) mapper_scanline(n);   /* MMC3 counts rises of this address line */
            p->v = p->t;
        }
        p->w ^= 1;
        break;
    case 7:
        a = p->v & 0x3FFF;
        if (a < 0x2000) {
            if (n->chr_is_ram) n->chr[a >> 10][a & 0x3FF] = (uint8_t)v;
        } else if (a < 0x3F00) {
            n->nt[(a >> 10) & 3][a & 0x3FF] = (uint8_t)v;
        } else {
            a &= 0x1F;
            if ((a & 0x13) == 0x10) a &= 0x0F;
            p->pal[a] = v & 0x3F;
            ppu_palette_changed(n);
        }
        data_step(n);
        break;
    }
}

void ppu_prerender(nes_t *n)
{
    struct nes_ppu *p = &n->st.ppu;
    p->status &= 0x1F;
    p->s0_found = 0;
    p->s0_clk = INT32_MAX;
}

void ppu_frame_start(nes_t *n)
{
    struct nes_ppu *p = &n->st.ppu;
    if (p->mask & 0x18) p->v = p->t;
}

void ppu_hblank(nes_t *n)
{
    struct nes_ppu *p = &n->st.ppu;
    unsigned v = p->v;
    if (!(p->mask & 0x18)) return;
    if ((v & 0x7000) != 0x7000) {
        v += 0x1000;
    } else {
        unsigned cy = (v >> 5) & 31;
        v &= ~0x7000u;
        if (cy == 29) {
            cy = 0;
            v ^= 0x800;
        } else {
            cy = (cy + 1) & 31;
        }
        v = (v & ~0x3E0u) | cy << 5;
    }
    p->v = (uint16_t)((v & ~0x41Fu) | (p->t & 0x41F));
}

void ppu_line(nes_t *n, int y, int32_t line_clk, int draw)
{
    struct nes_ppu *p = &n->st.ppu;
    uint32_t raw[2 * 34 + 2];           /* 34 tiles, on word boundaries */
    uint8_t *buf = (uint8_t *)raw;
    uint8_t list[8];
    int count = 0, h = (p->ctrl & 0x20) ? 16 : 8, s0_here = 0;

    if (!(p->mask & 0x18)) {
        if (draw) {
            memset(buf, 0, NES_W);
            n->line(n->line_ctx, y, buf, n->rgb);
        }
        return;
    }

    /* which sprites are on this line: the first eight, as the hardware has it */
    for (int i = 0; i < 64; i++) {
        unsigned row = (unsigned)(y - p->oam[i * 4] - 1);
        if (row >= (unsigned)h) continue;
        if (count == 8) {
            p->status |= 0x20;
            break;
        }
        list[count++] = (uint8_t)i;
    }
    if (!(p->mask & 0x10)) count = 0;
    s0_here = count && list[0] == 0 && !p->s0_found && (p->mask & 0x18) == 0x18;
    if (!draw && !s0_here) return;      /* a skipped picture still needs sprite 0's hit */

    if (p->mask & 0x08) {
        unsigned v = p->v, fy = (v >> 12) & 7, cx = v & 31, cy = (v >> 5) & 31;
        const uint8_t *nt = n->nt[(v >> 10) & 3];
        const uint8_t *row = nt + cy * 32, *attr = nt + 0x3C0 + (cy >> 2) * 8;
        unsigned base = ((p->ctrl & 0x10) ? 0x1000 : 0) + fy, ashift = (cy & 2) << 1;
        uint32_t *d = raw;
        for (int i = 0; i < 33; i++) {
            unsigned a = base + row[cx] * 16;
            const uint8_t *pat = n->chr[a >> 10] + (a & 0x3FF);
            unsigned lo = pat[0], hi = pat[8];
            uint32_t pal = ((attr[cx >> 2] >> (ashift | (cx & 2))) & 3) * 0x04040404u;
            d[0] = nib[lo >> 4] | nib[hi >> 4] << 1 | pal;
            d[1] = nib[lo & 15] | nib[hi & 15] << 1 | pal;
            d += 2;
            if (++cx == 32) {
                cx = 0;
                nt = n->nt[((v >> 10) & 3) ^ 1];
                row = nt + cy * 32;
                attr = nt + 0x3C0 + (cy >> 2) * 8;
            }
        }
        buf += p->x;
        if (!(p->mask & 0x02)) memset(buf, 0, 8);
    } else {
        memset(buf, 0, NES_W);
    }

    if (count) {
        uint8_t taken[NES_W + 8];       /* a pixel belongs to the first sprite that covers it */
        int clip = !(p->mask & 0x04);
        memset(taken, 0, sizeof taken);
        for (int k = 0; k < count; k++) {
            const uint8_t *o = p->oam + list[k] * 4;
            unsigned row = (unsigned)(y - o[0] - 1), tile = o[1], at = o[2], sx = o[3], a, lo, hi;
            unsigned pal = 0x10 | (at & 3) << 2;
            union { uint32_t w[2]; uint8_t b[8]; } px;
            const uint8_t *pat;
            if (at & 0x80) row = (unsigned)h - 1 - row;
            if (h == 16) {
                a = (tile & 1) << 12 | (tile & 0xFE) << 4;
                if (row >= 8) a += 16, row -= 8;
            } else {
                a = ((p->ctrl & 0x08) ? 0x1000 : 0) + tile * 16;
            }
            a += row;
            pat = n->chr[a >> 10] + (a & 0x3FF);
            lo = pat[0], hi = pat[8];
            if (!(lo | hi)) continue;
            px.w[0] = nib[lo >> 4] | nib[hi >> 4] << 1;
            px.w[1] = nib[lo & 15] | nib[hi & 15] << 1;
            for (unsigned j = 0; j < 8; j++) {
                unsigned c = px.b[(at & 0x40) ? 7 - j : j], xx = sx + j;
                if (!c || xx > 255 || taken[xx] || (clip && xx < 8)) continue;
                taken[xx] = 1;
                if (list[k] == 0 && s0_here && (buf[xx] & 3) && xx != 255 && !p->s0_found) {
                    p->s0_found = 1;
                    p->s0_clk = line_clk + (int32_t)xx + 6;   /* the CPU's clock here is the end of its instruction, a cycle past the read */
                }
                if (!(at & 0x20) || !(buf[xx] & 3)) buf[xx] = (uint8_t)(pal | c);
            }
        }
    }
    if (draw) n->line(n->line_ctx, y, buf, n->rgb);
}
