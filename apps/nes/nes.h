// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * A small NES core, written for this app: 6502, PPU drawn a scanline at a
 * time, the APU with its sound and the common cartridge boards.
 * It knows nothing about the phone: the frontend hands it a ROM image and a
 * function that takes each finished scanline.
 *
 * Time is counted in PPU dots (three to a CPU cycle), from the start of the
 * frame. A frame here starts at the vertical blank (scanline 241) and ends
 * after the last visible line, so a picture shows the pad as it was read
 * for it.
 */
#ifndef NES_H
#define NES_H

#include <stddef.h>
#include <stdint.h>

#define NES_W 256
#define NES_H_PX 240
#define NES_DOTS_PER_LINE 341
#define NES_DOTS_PER_FRAME (341 * 262)
#define NES_FPS_MILLI 60099   /* NTSC: 60.099 frames a second */

enum { NES_A = 1, NES_B = 2, NES_SELECT = 4, NES_START = 8, NES_UP = 16, NES_DOWN = 32, NES_LEFT = 64, NES_RIGHT = 128 };

enum { MIRROR_H, MIRROR_V, MIRROR_ONE_LO, MIRROR_ONE_HI, MIRROR_FOUR };

/* interrupt sources on the CPU's IRQ line */
enum { IRQ_FRAME = 1, IRQ_DMC = 2, IRQ_MAPPER = 4 };

typedef struct nes nes_t;

/* One finished scanline: 256 indexes into rgb (0 to 31), y from 0 to 239. */
typedef void (*nes_line_fn)(void *ctx, int y, const uint8_t *idx, const uint32_t *rgb);

struct nes_cpu {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint8_t nmi_pending;
    uint8_t irq_line;     /* IRQ_* that are asking */
    int32_t clk;          /* dots since the start of the frame */
};

struct nes_ppu {
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;        /* the scroll registers, as the hardware keeps them */
    uint8_t x, w;
    uint8_t rdbuf, latch;
    uint8_t s0_found;     /* sprite 0 has hit in this frame */
    int32_t s0_clk;       /* when the hit shows in $2002, or INT32_MAX */
    uint8_t oam[256];
    uint8_t pal[32];
    uint8_t nt[0x1000];   /* 2 KB in use, 4 KB with four screens */
};

struct nes_apu {
    int32_t clk;          /* dots: how far it has been run */
    int32_t seq;          /* CPU cycles into the frame sequence */
    uint8_t reg[0x18];    /* the registers as last written */
    uint8_t mode5, irq_off, enabled;
    uint8_t len[4];       /* length counters: pulse 1, pulse 2, triangle, noise */
    uint8_t lin, lin_reload;      /* the triangle's linear counter */
    /* DMC: the reader, then the shifter and the 7-bit level it moves */
    uint8_t dmc_irq_on, dmc_loop, dmc_rate, dmc_buf_full, dmc_buf, dmc_bits;
    uint16_t dmc_addr, dmc_left;
    int32_t dmc_timer;    /* CPU cycles to the next output bit */
    uint8_t dmc_shift, dmc_silent, dmc_dac;
    /* the two pulses */
    uint8_t pseq[2];              /* where each is in its eight steps */
    uint16_t period[2];           /* as written, then as the sweep has moved it */
    int32_t ptimer[2];            /* CPU cycles to the next step */
    uint8_t sw_div[2], sw_reload[2];
    /* envelopes: pulse 1, pulse 2, noise */
    uint8_t env_start[3], env_div[3], env_vol[3];
    /* triangle and noise */
    uint8_t tri_seq;
    uint16_t lfsr;
    int32_t tri_timer, noise_timer;
};

/* The sound on its way out: not part of a save state. Every change of level
 * is put down as a band-limited step at its exact time, so what is read back
 * at the output rate has no aliasing to speak of. */
#define NES_SND_TAPS 16
#define NES_SND_LEN 1024          /* output samples that can wait to be read */
struct nes_sound {
    int on;
    int fresh;                    /* take the mixer's level as it stands, without a step: a start or a reset is not a sound */
    uint32_t spc;                 /* output samples in a CPU cycle, 8.24 */
    uint32_t at, sub;             /* now, in samples from buf[0] (12.20), and the 4 bits below that */
    int32_t pulse_level, tnd_level;   /* what the two halves of the mixer last gave */
    int32_t sum, hp1, hp2;        /* the running level, and the two high-pass filters */
    int32_t c1, c2;
    uint8_t nfrac;                /* fast noise: eighths of its last slot it was high */
    int rate;
    uint32_t buf[NES_SND_LEN + NES_SND_TAPS];
};

struct nes_mapper {
    uint8_t mirror;
    uint8_t reg[8];       /* simple boards: reg[0] is the bank latch; MMC3: R0 to R7 */
    /* MMC1 */
    uint8_t shift, shift_n, ctrl, chr0, chr1, prg;
    int32_t last_wr;      /* clk of the last write: a second one in the same instruction is ignored */
    /* MMC3 */
    uint8_t sel, irq_latch, irq_count, irq_reload, irq_on;
    uint8_t ram_ctl;      /* $A001: bit 7 cartridge RAM on, bit 6 read only */
};

/* everything a save state holds */
struct nes_state {
    struct nes_cpu cpu;
    struct nes_ppu ppu;
    struct nes_apu apu;
    struct nes_mapper map;
    uint8_t pad[2], pad_shift[2], pad_strobe;
    uint8_t ram[0x800];
    uint8_t prg_ram[0x2000];
    uint8_t chr_ram[0x2000];
};

struct nes {
    struct nes_state st;
    /* not saved: the cartridge and what follows from the state */
    const uint8_t *prg[4];    /* $8000 to $FFFF in 8 KB pieces */
    uint8_t *chr[8];          /* pattern tables in 1 KB pieces */
    uint8_t *nt[4];           /* the four name tables */
    uint8_t *rom;             /* the whole file */
    const uint8_t *prg_rom;
    uint8_t *chr_rom;
    uint32_t prg_size, chr_size;   /* bytes; powers of two are not assumed */
    int mapper, chr_is_ram, battery, four_screen, hdr_mirror;
    uint32_t crc;             /* of PRG and CHR, to tie saves to a game */
    uint32_t rgb[32];         /* the palette as screen colours */
    nes_line_fn line;
    void *line_ctx;
    struct nes_sound snd;
};

/* nes.c */
/* Take a .nes image (the core keeps and later frees data). NULL, or what is wrong with it. */
const char *nes_load(nes_t *n, uint8_t *data, size_t len);
void nes_unload(nes_t *n);
void nes_reset(nes_t *n, int power);
void nes_set_video(nes_t *n, nes_line_fn fn, void *ctx);
/* Run one frame with this pad state (NES_* bits). draw 0 skips the picture. */
void nes_frame(nes_t *n, unsigned pad1, int draw);
/* Save states: the size of one, and to and from memory. load returns 0, -1 for
 * something this version did not write, -2 for another game's state. */
size_t nes_state_size(void);
void nes_state_save(const nes_t *n, uint8_t *out);
int nes_state_load(nes_t *n, const uint8_t *in, size_t len);
uint8_t nes_read_io(nes_t *n, unsigned addr);
void nes_write_io(nes_t *n, unsigned addr, unsigned v);

/* cpu.c */
void cpu_reset(nes_t *n);
void cpu_run(nes_t *n, int32_t until);

/* ppu.c */
void ppu_reset(nes_t *n);
uint8_t ppu_read(nes_t *n, unsigned reg);
void ppu_write(nes_t *n, unsigned reg, unsigned v);
void ppu_line(nes_t *n, int y, int32_t line_clk, int draw);
void ppu_hblank(nes_t *n);            /* end of a drawn line: move v down one */
void ppu_prerender(nes_t *n);         /* clear the flags for a new picture */
void ppu_frame_start(nes_t *n);       /* take the vertical scroll */
void ppu_palette_changed(nes_t *n);
void ppu_set_mirror(nes_t *n, int mirror);

/* apu.c */
void apu_reset(nes_t *n);
void apu_run(nes_t *n, int32_t clk);
uint8_t apu_read_status(nes_t *n);
void apu_write(nes_t *n, unsigned addr, unsigned v);
int32_t apu_next_irq(const nes_t *n);
int apu_dmc_busy(const nes_t *n);
/* Sound. Off until started: 16-bit mono at rate samples a second (8000 to
 * 48000), 0 for off again. nes_sound_read takes what the frames run so far
 * have made, at most max, and returns how many. nes_sound_tune bends the rate
 * by parts in a million, to keep step with whatever plays it. */
void nes_sound_start(nes_t *n, int rate);
void nes_sound_tune(nes_t *n, int ppm);
int nes_sound_read(nes_t *n, int16_t *out, int max);

/* cart.c */
int mapper_supported(int mapper);
void mapper_reset(nes_t *n);
void mapper_write(nes_t *n, unsigned addr, unsigned v);
void mapper_sync(nes_t *n);           /* banks and mirroring from the registers */
void mapper_scanline(nes_t *n);       /* MMC3's counter */
int mapper_ram(const nes_t *n);       /* cartridge RAM at $6000: 0 off, 1 read only, 2 on */

#endif
