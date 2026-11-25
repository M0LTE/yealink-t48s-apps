// SPDX-License-Identifier: AGPL-3.0-or-later
/* The machine: loading a cartridge, the CPU's view of the registers, one
 * frame, and save states. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"

static uint32_t crc32_of(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

const char *nes_load(nes_t *n, uint8_t *data, size_t len)
{
    static char err[40];
    size_t prg, chr, off = 16;
    int mapper;

    if (len < 16 || memcmp(data, "NES\x1a", 4)) return "Not a NES ROM";
    prg = (size_t)data[4] * 0x4000;
    chr = (size_t)data[5] * 0x2000;
    mapper = data[6] >> 4;
    if ((data[7] & 0x0C) == 0x08) {
        mapper |= (data[7] & 0xF0) | (data[8] & 0x0F) << 8;   /* NES 2.0 */
    } else if (!(data[12] | data[13] | data[14] | data[15])) {
        mapper |= data[7] & 0xF0;                             /* otherwise byte 7 is somebody's signature */
    }
    if (data[6] & 4) off += 512;
    if (!prg || len < off + prg + chr) return "ROM truncated";
    if (!mapper_supported(mapper)) {
        snprintf(err, sizeof err, "Mapper %d not supported", mapper);
        return err;
    }
    nes_unload(n);
    n->rom = data;
    n->prg_rom = data + off;
    n->prg_size = (uint32_t)prg;
    n->chr_rom = data + off + prg;
    n->chr_size = (uint32_t)chr;
    n->chr_is_ram = !chr;
    n->mapper = mapper;
    n->battery = data[6] & 2;
    n->four_screen = data[6] & 8;
    n->hdr_mirror = (data[6] & 1) ? MIRROR_V : MIRROR_H;
    n->crc = crc32_of(data + off, prg + chr);
    nes_reset(n, 1);
    return NULL;
}

void nes_unload(nes_t *n)
{
    free(n->rom);
    n->rom = NULL;
}

void nes_set_video(nes_t *n, nes_line_fn fn, void *ctx)
{
    n->line = fn;
    n->line_ctx = ctx;
}

void nes_reset(nes_t *n, int power)
{
    if (power) {
        memset(&n->st, 0, sizeof n->st);
        n->st.cpu.p = 0x20;
    }
    n->st.cpu.clk = 0;
    n->st.cpu.irq_line = 0;
    mapper_reset(n);
    ppu_reset(n);
    apu_reset(n);
    cpu_reset(n);
}

uint8_t nes_read_io(nes_t *n, unsigned addr)
{
    struct nes_state *s = &n->st;
    unsigned r;
    if (addr < 0x4000) return ppu_read(n, addr & 7);
    if (addr >= 0x6000) return mapper_ram(n) ? s->prg_ram[addr & 0x1FFF] : (uint8_t)(addr >> 8);
    switch (addr) {
    case 0x4015:
        apu_run(n, s->cpu.clk);
        return apu_read_status(n);
    case 0x4016:
    case 0x4017:
        r = addr & 1;
        if (s->pad_strobe) return 0x40 | (s->pad[r] & 1);
        r = s->pad_shift[r] & 1;
        s->pad_shift[addr & 1] = (uint8_t)(s->pad_shift[addr & 1] >> 1 | 0x80);
        return (uint8_t)(0x40 | r);
    }
    return (uint8_t)(addr >> 8);   /* nothing there: the bus still holds the address */
}

void nes_write_io(nes_t *n, unsigned addr, unsigned v)
{
    struct nes_state *s = &n->st;
    if (addr >= 0x8000) {
        mapper_write(n, addr, v);
    } else if (addr < 0x4000) {
        ppu_write(n, addr & 7, v);
    } else if (addr >= 0x6000) {
        if (mapper_ram(n) == 2) s->prg_ram[addr & 0x1FFF] = (uint8_t)v;
    } else if (addr == 0x4014) {
        /* copy a page to the sprite table; the CPU waits 513 cycles */
        unsigned page = v << 8;
        for (unsigned i = 0; i < 256; i++) {
            unsigned a = page + i, b;
            if (a >= 0x8000) b = n->prg[(a >> 13) & 3][a & 0x1FFF];
            else if (a < 0x2000) b = s->ram[a & 0x7FF];
            else b = nes_read_io(n, a);
            s->ppu.oam[(s->ppu.oam_addr + i) & 0xFF] = (uint8_t)b;
        }
        s->cpu.clk += 513 * 3;
    } else if (addr == 0x4016) {
        if (s->pad_strobe && !(v & 1)) {
            s->pad_shift[0] = s->pad[0];
            s->pad_shift[1] = s->pad[1];
        }
        s->pad_strobe = v & 1;
    } else if (addr < 0x4018) {
        apu_run(n, s->cpu.clk);
        apu_write(n, addr, v);
    }
}

static void run(nes_t *n, int32_t until)
{
    int32_t irq = apu_next_irq(n);
    if (irq < until && irq > n->st.cpu.clk) {
        cpu_run(n, irq);
        apu_run(n, n->st.cpu.clk);
    }
    cpu_run(n, until);
    apu_run(n, n->st.cpu.clk);
}

/* MMC3 counts a line when the PPU goes from the background's patterns to the
 * sprites': at dot 260 as nearly every game sets it up, or at 324 with the
 * two tables the other way round. */
static void mmc3_line(nes_t *n, int32_t base)
{
    if ((n->st.ppu.ctrl & 0x38) == 0x10) run(n, base + 324);
    mapper_scanline(n);
}

void nes_frame(nes_t *n, unsigned pad1, int draw)
{
    struct nes_state *s = &n->st;
    int32_t base = 0;

    s->pad[0] = (uint8_t)pad1;

    /* scanline 241: the vertical blank begins */
    run(n, base + 1);
    s->ppu.status |= 0x80;
    if (s->ppu.ctrl & 0x80) s->cpu.nmi_pending = 1;
    for (int i = 241; i < 261; i++, base += NES_DOTS_PER_LINE) run(n, base + NES_DOTS_PER_LINE);

    /* 261: the line before the picture */
    run(n, base + 1);
    ppu_prerender(n);
    run(n, base + 260);
    if ((s->ppu.mask & 0x18) && n->mapper == 4) mmc3_line(n, base);
    if (s->cpu.clk < base + 280) run(n, base + 280);
    ppu_frame_start(n);
    run(n, base + NES_DOTS_PER_LINE);
    base += NES_DOTS_PER_LINE;

    for (int y = 0; y < NES_H_PX; y++, base += NES_DOTS_PER_LINE) {
        ppu_line(n, y, base, draw);
        run(n, base + 260);
        if (s->ppu.mask & 0x18) {
            ppu_hblank(n);
            if (n->mapper == 4) mmc3_line(n, base);
        }
        run(n, base + NES_DOTS_PER_LINE);
    }

    /* 240: nothing happens */
    run(n, base + NES_DOTS_PER_LINE);

    if (s->ppu.s0_clk != INT32_MAX) {
        s->ppu.status |= 0x40;
        s->ppu.s0_clk = INT32_MAX;
    }
    s->cpu.clk -= NES_DOTS_PER_FRAME;
    s->apu.clk -= NES_DOTS_PER_FRAME;
    s->map.last_wr = INT32_MIN;
}

/* a save state: this header, then struct nes_state as it is in memory */
struct state_hdr {
    char magic[8];
    uint32_t size, crc;
};
/* The digit goes up whenever struct nes_state changes; the size is checked too. */
#define STATE_MAGIC "T48NES2"

size_t nes_state_size(void)
{
    return sizeof(struct state_hdr) + sizeof(struct nes_state);
}

void nes_state_save(const nes_t *n, uint8_t *out)
{
    struct state_hdr h = {STATE_MAGIC, (uint32_t)sizeof n->st, n->crc};
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, &n->st, sizeof n->st);
}

/* A state comes from a file, so nothing in it is trusted to be in range:
 * whatever indexes a table or bounds a loop is brought back inside. */
static void state_clamp(struct nes_state *s)
{
    if (s->cpu.clk < 0 || s->cpu.clk > NES_DOTS_PER_FRAME) s->cpu.clk = 0;
    if (s->apu.clk < s->cpu.clk - NES_DOTS_PER_FRAME || s->apu.clk > s->cpu.clk) s->apu.clk = s->cpu.clk;
    s->apu.mode5 &= 1;
    if (s->apu.seq < -3 || s->apu.seq > 37281) s->apu.seq = 0;
    s->apu.dmc_rate &= 15;
    if (s->apu.dmc_bits < 1 || s->apu.dmc_bits > 8) s->apu.dmc_bits = 8;
    if (s->apu.dmc_timer < -NES_DOTS_PER_FRAME || s->apu.dmc_timer > 428) s->apu.dmc_timer = 428;
    s->apu.dmc_addr |= 0x8000;
    s->ppu.x &= 7;
    s->ppu.w &= 1;
    s->ppu.s0_clk = INT32_MAX;
    if (s->map.mirror > MIRROR_FOUR) s->map.mirror = MIRROR_H;
    if (s->map.shift_n > 4) s->map.shift = s->map.shift_n = 0;
    s->map.last_wr = INT32_MIN;
    s->pad_strobe &= 1;
}

/* 0, -1 if it is not a state this version wrote, -2 if it is another game's */
int nes_state_load(nes_t *n, const uint8_t *in, size_t len)
{
    struct state_hdr h;
    if (len != nes_state_size()) return -1;
    memcpy(&h, in, sizeof h);
    if (memcmp(h.magic, STATE_MAGIC, sizeof h.magic) || h.size != sizeof n->st) return -1;
    if (h.crc != n->crc) return -2;
    memcpy(&n->st, in + sizeof h, sizeof n->st);
    state_clamp(&n->st);
    mapper_sync(n);
    ppu_palette_changed(n);
    return 0;
}
