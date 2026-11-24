// SPDX-License-Identifier: AGPL-3.0-or-later
/* The 6502 (a 2A03: no decimal mode), with the unofficial opcodes games use. */
#include "nes.h"

/* CPU cycles for each opcode, before page crossings and taken branches */
static const uint8_t cycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

void cpu_reset(nes_t *n)
{
    struct nes_cpu *c = &n->st.cpu;
    c->s = (uint8_t)(c->s - 3);
    c->p |= 0x04;
    c->pc = (uint16_t)(n->prg[3][0x1FFC] | n->prg[3][0x1FFD] << 8);
    c->nmi_pending = 0;
}

void cpu_run(nes_t *n, int32_t until)
{
    struct nes_cpu *c = &n->st.cpu;
    uint8_t *ram = n->st.ram;
    const uint8_t *const *prg = n->prg;
    int32_t clk = c->clk;
    uint16_t pc = c->pc;
    unsigned a = c->a, x = c->x, y = c->y, s = c->s;
    /* the flags apart: Z is "fz is zero", N is bit 7 of fn */
    unsigned fc = c->p & 1, fz = !(c->p & 2), fi = c->p & 4, fd = c->p & 8, fv = c->p & 0x40, fn = c->p;
    unsigned ea = 0, m, t, u, hold = 0;   /* hold: CLI and PLP let one instruction by before an IRQ */

/* registers and cartridge RAM go the slow way, and may read or move the clock */
#define IO_RD(ad) (c->clk = clk, t = nes_read_io(n, (ad)), clk = c->clk, t)
#define IO_WR(ad, val) (c->clk = clk, nes_write_io(n, (ad), (val)), clk = c->clk)
#define RD(ad) (likely((ad) >= 0x8000) ? prg[((ad) >> 13) & 3][(ad) & 0x1FFF] : (ad) < 0x2000 ? ram[(ad) & 0x7FF] : IO_RD(ad))
#define WR(ad, val) do { if ((ad) < 0x2000) ram[(ad) & 0x7FF] = (uint8_t)(val); else IO_WR((ad), (val)); } while (0)
#define PUSH(val) (ram[0x100 + s] = (uint8_t)(val), s = (s - 1) & 0xFF)
#define PULL() (s = (s + 1) & 0xFF, ram[0x100 + s])
#define NZ(val) (fz = fn = (val))
#define GETP() (fc | (fz ? 0 : 2) | fi | fd | (fv ? 0x40 : 0) | (fn & 0x80) | 0x20)
#define SETP(val) (t = (val), fc = t & 1, fz = !(t & 2), fi = t & 4, fd = t & 8, fv = t & 0x40, fn = t)

/* addressing modes: leave the address in ea. The R forms pay for a page crossing. */
#define IMM ea = pc++;
#define ZP ea = RD(pc); pc++;
#define ZPX ea = (RD(pc) + x) & 0xFF; pc++;
#define ZPY ea = (RD(pc) + y) & 0xFF; pc++;
#define ABS ea = RD(pc); ea |= RD((uint16_t)(pc + 1)) << 8; pc += 2;
#define ABX ABS ea = (ea + x) & 0xFFFF;
#define ABY ABS ea = (ea + y) & 0xFFFF;
#define ABXR ABS m = ea; ea = (ea + x) & 0xFFFF; if ((ea ^ m) & 0x100) clk += 3;
#define ABYR ABS m = ea; ea = (ea + y) & 0xFFFF; if ((ea ^ m) & 0x100) clk += 3;
#define IZX m = (RD(pc) + x) & 0xFF; pc++; ea = ram[m] | ram[(m + 1) & 0xFF] << 8;
#define IZY m = RD(pc); pc++; ea = ((ram[m] | ram[(m + 1) & 0xFF] << 8) + y) & 0xFFFF;
#define IZYR m = RD(pc); pc++; m = ram[m] | ram[(m + 1) & 0xFF] << 8; ea = (m + y) & 0xFFFF; if ((ea ^ m) & 0x100) clk += 3;

#define ADC_M do { t = a + m + fc; fv = ~(a ^ m) & (a ^ t) & 0x80; fc = t >> 8; a = t & 0xFF; NZ(a); } while (0)
#define CMP_M(r) do { t = (r) - m; fc = (r) >= m; NZ(t & 0xFF); } while (0)
#define ASL_M do { fc = m >> 7; m = (m << 1) & 0xFF; NZ(m); } while (0)
#define LSR_M do { fc = m & 1; m >>= 1; NZ(m); } while (0)
#define ROL_M do { t = m >> 7; m = ((m << 1) | fc) & 0xFF; fc = t; NZ(m); } while (0)
#define ROR_M do { t = m & 1; m = (m >> 1) | (fc << 7); fc = t; NZ(m); } while (0)
#define INC_M do { m = (m + 1) & 0xFF; NZ(m); } while (0)
#define DEC_M do { m = (m - 1) & 0xFF; NZ(m); } while (0)

/* read, change, write back. Hardware writes the old value first: MMC1 counts on it. */
#define RMW(MODE, WHAT) MODE m = RD(ea); if (unlikely(ea >= 0x2000)) IO_WR(ea, m); WHAT; WR(ea, m);
#define LOAD(MODE) MODE m = RD(ea);
#define BRANCH(cond) do { m = RD(pc); pc++; if (cond) { t = (uint16_t)(pc + (int8_t)m); clk += ((t ^ pc) & 0xFF00) ? 6 : 3; pc = (uint16_t)t; } } while (0)
#define INTERRUPT(vec, flags) do { PUSH(pc >> 8); PUSH(pc); PUSH(flags); fi = 4; pc = (uint16_t)(prg[3][(vec) & 0x1FFF] | prg[3][((vec) + 1) & 0x1FFF] << 8); } while (0)

    while (clk < until) {
        unsigned op;
        if (unlikely(c->nmi_pending)) {
            if (c->nmi_pending > 1) {
                c->nmi_pending = 1;     /* asked for by a write to $2000: one more instruction first */
            } else {
                c->nmi_pending = 0;
                INTERRUPT(0xFFFA, GETP());
                clk += 21;
            }
        } else if (unlikely(c->irq_line) && !fi && !hold) {
            INTERRUPT(0xFFFE, GETP());
            clk += 21;
        }
        hold = 0;
        op = RD(pc);
        pc++;
        clk += cycles[op] * 3;
        switch (op) {
        /* loads and stores */
        case 0xA9: LOAD(IMM) a = m; NZ(a); break;
        case 0xA5: LOAD(ZP) a = m; NZ(a); break;
        case 0xB5: LOAD(ZPX) a = m; NZ(a); break;
        case 0xAD: LOAD(ABS) a = m; NZ(a); break;
        case 0xBD: LOAD(ABXR) a = m; NZ(a); break;
        case 0xB9: LOAD(ABYR) a = m; NZ(a); break;
        case 0xA1: LOAD(IZX) a = m; NZ(a); break;
        case 0xB1: LOAD(IZYR) a = m; NZ(a); break;
        case 0xA2: LOAD(IMM) x = m; NZ(x); break;
        case 0xA6: LOAD(ZP) x = m; NZ(x); break;
        case 0xB6: LOAD(ZPY) x = m; NZ(x); break;
        case 0xAE: LOAD(ABS) x = m; NZ(x); break;
        case 0xBE: LOAD(ABYR) x = m; NZ(x); break;
        case 0xA0: LOAD(IMM) y = m; NZ(y); break;
        case 0xA4: LOAD(ZP) y = m; NZ(y); break;
        case 0xB4: LOAD(ZPX) y = m; NZ(y); break;
        case 0xAC: LOAD(ABS) y = m; NZ(y); break;
        case 0xBC: LOAD(ABXR) y = m; NZ(y); break;
        case 0x85: ZP WR(ea, a); break;
        case 0x95: ZPX WR(ea, a); break;
        case 0x8D: ABS WR(ea, a); break;
        case 0x9D: ABX WR(ea, a); break;
        case 0x99: ABY WR(ea, a); break;
        case 0x81: IZX WR(ea, a); break;
        case 0x91: IZY WR(ea, a); break;
        case 0x86: ZP WR(ea, x); break;
        case 0x96: ZPY WR(ea, x); break;
        case 0x8E: ABS WR(ea, x); break;
        case 0x84: ZP WR(ea, y); break;
        case 0x94: ZPX WR(ea, y); break;
        case 0x8C: ABS WR(ea, y); break;
        /* transfers, stack */
        case 0xAA: x = a; NZ(x); break;
        case 0xA8: y = a; NZ(y); break;
        case 0x8A: a = x; NZ(a); break;
        case 0x98: a = y; NZ(a); break;
        case 0xBA: x = s; NZ(x); break;
        case 0x9A: s = x; break;
        case 0x48: PUSH(a); break;
        case 0x68: a = PULL(); NZ(a); break;
        case 0x08: PUSH(GETP() | 0x10); break;
        case 0x28:   /* PLP: the IRQ line is looked at with the flag as it was */
            u = fi;
            SETP(PULL());
            if (u) {
                hold = 1;
            } else if (fi && unlikely(c->irq_line)) {
                INTERRUPT(0xFFFE, GETP());
                clk += 21;
            }
            break;
        /* logic */
        case 0x29: LOAD(IMM) a &= m; NZ(a); break;
        case 0x25: LOAD(ZP) a &= m; NZ(a); break;
        case 0x35: LOAD(ZPX) a &= m; NZ(a); break;
        case 0x2D: LOAD(ABS) a &= m; NZ(a); break;
        case 0x3D: LOAD(ABXR) a &= m; NZ(a); break;
        case 0x39: LOAD(ABYR) a &= m; NZ(a); break;
        case 0x21: LOAD(IZX) a &= m; NZ(a); break;
        case 0x31: LOAD(IZYR) a &= m; NZ(a); break;
        case 0x09: LOAD(IMM) a |= m; NZ(a); break;
        case 0x05: LOAD(ZP) a |= m; NZ(a); break;
        case 0x15: LOAD(ZPX) a |= m; NZ(a); break;
        case 0x0D: LOAD(ABS) a |= m; NZ(a); break;
        case 0x1D: LOAD(ABXR) a |= m; NZ(a); break;
        case 0x19: LOAD(ABYR) a |= m; NZ(a); break;
        case 0x01: LOAD(IZX) a |= m; NZ(a); break;
        case 0x11: LOAD(IZYR) a |= m; NZ(a); break;
        case 0x49: LOAD(IMM) a ^= m; NZ(a); break;
        case 0x45: LOAD(ZP) a ^= m; NZ(a); break;
        case 0x55: LOAD(ZPX) a ^= m; NZ(a); break;
        case 0x4D: LOAD(ABS) a ^= m; NZ(a); break;
        case 0x5D: LOAD(ABXR) a ^= m; NZ(a); break;
        case 0x59: LOAD(ABYR) a ^= m; NZ(a); break;
        case 0x41: LOAD(IZX) a ^= m; NZ(a); break;
        case 0x51: LOAD(IZYR) a ^= m; NZ(a); break;
        case 0x24: LOAD(ZP) fz = a & m; fn = m; fv = m & 0x40; break;
        case 0x2C: LOAD(ABS) fz = a & m; fn = m; fv = m & 0x40; break;
        /* arithmetic */
        case 0x69: LOAD(IMM) ADC_M; break;
        case 0x65: LOAD(ZP) ADC_M; break;
        case 0x75: LOAD(ZPX) ADC_M; break;
        case 0x6D: LOAD(ABS) ADC_M; break;
        case 0x7D: LOAD(ABXR) ADC_M; break;
        case 0x79: LOAD(ABYR) ADC_M; break;
        case 0x61: LOAD(IZX) ADC_M; break;
        case 0x71: LOAD(IZYR) ADC_M; break;
        case 0xE9: case 0xEB: LOAD(IMM) m ^= 0xFF; ADC_M; break;
        case 0xE5: LOAD(ZP) m ^= 0xFF; ADC_M; break;
        case 0xF5: LOAD(ZPX) m ^= 0xFF; ADC_M; break;
        case 0xED: LOAD(ABS) m ^= 0xFF; ADC_M; break;
        case 0xFD: LOAD(ABXR) m ^= 0xFF; ADC_M; break;
        case 0xF9: LOAD(ABYR) m ^= 0xFF; ADC_M; break;
        case 0xE1: LOAD(IZX) m ^= 0xFF; ADC_M; break;
        case 0xF1: LOAD(IZYR) m ^= 0xFF; ADC_M; break;
        case 0xC9: LOAD(IMM) CMP_M(a); break;
        case 0xC5: LOAD(ZP) CMP_M(a); break;
        case 0xD5: LOAD(ZPX) CMP_M(a); break;
        case 0xCD: LOAD(ABS) CMP_M(a); break;
        case 0xDD: LOAD(ABXR) CMP_M(a); break;
        case 0xD9: LOAD(ABYR) CMP_M(a); break;
        case 0xC1: LOAD(IZX) CMP_M(a); break;
        case 0xD1: LOAD(IZYR) CMP_M(a); break;
        case 0xE0: LOAD(IMM) CMP_M(x); break;
        case 0xE4: LOAD(ZP) CMP_M(x); break;
        case 0xEC: LOAD(ABS) CMP_M(x); break;
        case 0xC0: LOAD(IMM) CMP_M(y); break;
        case 0xC4: LOAD(ZP) CMP_M(y); break;
        case 0xCC: LOAD(ABS) CMP_M(y); break;
        /* increments, shifts */
        case 0xE6: RMW(ZP, INC_M) break;
        case 0xF6: RMW(ZPX, INC_M) break;
        case 0xEE: RMW(ABS, INC_M) break;
        case 0xFE: RMW(ABX, INC_M) break;
        case 0xC6: RMW(ZP, DEC_M) break;
        case 0xD6: RMW(ZPX, DEC_M) break;
        case 0xCE: RMW(ABS, DEC_M) break;
        case 0xDE: RMW(ABX, DEC_M) break;
        case 0xE8: x = (x + 1) & 0xFF; NZ(x); break;
        case 0xC8: y = (y + 1) & 0xFF; NZ(y); break;
        case 0xCA: x = (x - 1) & 0xFF; NZ(x); break;
        case 0x88: y = (y - 1) & 0xFF; NZ(y); break;
        case 0x0A: m = a; ASL_M; a = m; break;
        case 0x06: RMW(ZP, ASL_M) break;
        case 0x16: RMW(ZPX, ASL_M) break;
        case 0x0E: RMW(ABS, ASL_M) break;
        case 0x1E: RMW(ABX, ASL_M) break;
        case 0x4A: m = a; LSR_M; a = m; break;
        case 0x46: RMW(ZP, LSR_M) break;
        case 0x56: RMW(ZPX, LSR_M) break;
        case 0x4E: RMW(ABS, LSR_M) break;
        case 0x5E: RMW(ABX, LSR_M) break;
        case 0x2A: m = a; ROL_M; a = m; break;
        case 0x26: RMW(ZP, ROL_M) break;
        case 0x36: RMW(ZPX, ROL_M) break;
        case 0x2E: RMW(ABS, ROL_M) break;
        case 0x3E: RMW(ABX, ROL_M) break;
        case 0x6A: m = a; ROR_M; a = m; break;
        case 0x66: RMW(ZP, ROR_M) break;
        case 0x76: RMW(ZPX, ROR_M) break;
        case 0x6E: RMW(ABS, ROR_M) break;
        case 0x7E: RMW(ABX, ROR_M) break;
        /* jumps */
        case 0x4C: ABS pc = (uint16_t)ea; break;
        case 0x6C: ABS m = RD(ea); ea = (ea & 0xFF00) | ((ea + 1) & 0xFF); pc = (uint16_t)(m | RD(ea) << 8); break;
        case 0x20: ABS pc--; PUSH(pc >> 8); PUSH(pc); pc = (uint16_t)ea; break;
        case 0x60: m = PULL(); pc = (uint16_t)((m | PULL() << 8) + 1); break;
        case 0x40: SETP(PULL()); m = PULL(); pc = (uint16_t)(m | PULL() << 8); break;
        case 0x00: pc++; INTERRUPT(0xFFFE, GETP() | 0x10); break;
        case 0x10: BRANCH(!(fn & 0x80)); break;
        case 0x30: BRANCH(fn & 0x80); break;
        case 0x50: BRANCH(!fv); break;
        case 0x70: BRANCH(fv); break;
        case 0x90: BRANCH(!fc); break;
        case 0xB0: BRANCH(fc); break;
        case 0xD0: BRANCH(fz); break;
        case 0xF0: BRANCH(!fz); break;
        /* flags */
        case 0x18: fc = 0; break;
        case 0x38: fc = 1; break;
        case 0x58: fi = 0; hold = 1; break;
        case 0x78:   /* SEI: an IRQ already asking still gets in, with the flag set in what it pushes */
            if (unlikely(c->irq_line) && !fi) {
                fi = 4;
                INTERRUPT(0xFFFE, GETP());
                clk += 21;
            }
            fi = 4;
            break;
        case 0xB8: fv = 0; break;
        case 0xD8: fd = 0; break;
        case 0xF8: fd = 8; break;
        case 0xEA: break;

        /* unofficial: the ones that do something */
        case 0xA7: LOAD(ZP) a = x = m; NZ(a); break;      /* LAX */
        case 0xB7: LOAD(ZPY) a = x = m; NZ(a); break;
        case 0xAF: LOAD(ABS) a = x = m; NZ(a); break;
        case 0xBF: LOAD(ABYR) a = x = m; NZ(a); break;
        case 0xA3: LOAD(IZX) a = x = m; NZ(a); break;
        case 0xB3: LOAD(IZYR) a = x = m; NZ(a); break;
        case 0x87: ZP WR(ea, a & x); break;                /* SAX */
        case 0x97: ZPY WR(ea, a & x); break;
        case 0x8F: ABS WR(ea, a & x); break;
        case 0x83: IZX WR(ea, a & x); break;
#define DCP DEC_M; t = a - m; fc = a >= m; NZ(t & 0xFF)
#define ISB INC_M; u = m; m ^= 0xFF; ADC_M; m = u
#define SLO ASL_M; a |= m; NZ(a)
#define RLA ROL_M; a &= m; NZ(a)
#define SRE LSR_M; a ^= m; NZ(a)
#define RRA ROR_M; u = m; ADC_M; m = u
#define SEVEN(base, WHAT) \
        case base + 0x07: RMW(ZP, WHAT) break; case base + 0x17: RMW(ZPX, WHAT) break; \
        case base + 0x0F: RMW(ABS, WHAT) break; case base + 0x1F: RMW(ABX, WHAT) break; \
        case base + 0x1B: RMW(ABY, WHAT) break; case base + 0x03: RMW(IZX, WHAT) break; \
        case base + 0x13: RMW(IZY, WHAT) break;
        SEVEN(0xC0, DCP)
        SEVEN(0xE0, ISB)
        SEVEN(0x00, SLO)
        SEVEN(0x20, RLA)
        SEVEN(0x40, SRE)
        SEVEN(0x60, RRA)
        case 0x0B: case 0x2B: LOAD(IMM) a &= m; NZ(a); fc = a >> 7; break;                     /* ANC */
        case 0x4B: LOAD(IMM) a &= m; fc = a & 1; a >>= 1; NZ(a); break;                         /* ALR */
        case 0x6B: LOAD(IMM) a = ((a & m) >> 1) | (fc << 7); NZ(a); fc = (a >> 6) & 1; fv = (a ^ (a << 1)) & 0x40; break;   /* ARR */
        case 0xCB: LOAD(IMM) t = (a & x) - m; fc = (a & x) >= m; x = t & 0xFF; NZ(x); break;    /* AXS */
        /* unofficial: do nothing, but take their operands */
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: pc++; break;
        case 0x04: case 0x44: case 0x64: pc++; break;
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: pc++; break;
        case 0x0C: pc += 2; break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: ABXR break;
        default: break;   /* the rest halt a real CPU or are unstable: one byte, nothing done */
        }
    }

    c->clk = clk;
    c->pc = pc;
    c->a = (uint8_t)a; c->x = (uint8_t)x; c->y = (uint8_t)y; c->s = (uint8_t)s;
    c->p = (uint8_t)GETP();
}
