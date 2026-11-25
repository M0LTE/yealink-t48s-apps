// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the tune's player and synth. Six channels in the manner of a tracker:
 * four that play notes (pulse of any width, saw, triangle) and two of drums
 * made from a swept sine and noise. Each tick (20 ms) the envelopes, chords,
 * vibrato and pulse widths move; in between it is one short loop a voice.
 *
 * Whole numbers once the tables are made. Saw and pulse come from tables that
 * hold only the harmonics under 6.8 kHz for the octave being played, so
 * nothing folds back as a whistle at 16000 samples a second and the top end
 * stays soft for a small loudspeaker. A pulse is one saw minus the same saw a
 * little later, which gives every width from one set of tables. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "music.h"

/* ---------- tables ---------- */

#define NBAND 8
#define TLEN 512
static int16_t sine[2048];              /* Q14 */
static int16_t saw[NBAND][TLEN + 1];    /* 8192 down to -8192 over a cycle; band b is for 32.7 Hz << b and the octave above */
static int16_t tri[TLEN + 1];
static uint32_t top_inc[192];           /* phase a sample for the octave from note 120 up, in sixteenths of a semitone */
#define BAND0_INC 8778000u              /* 32.7 Hz */

static void tables(void)
{
    for (int i = 0; i <= 512; i++) {
        int v = (int)lrint(16384 * sin(i * M_PI / 1024));
        sine[i] = (int16_t)v;
        if (i) sine[1024 - i] = (int16_t)v;
    }
    for (int i = 1; i < 1024; i++) sine[1024 + i] = (int16_t)-sine[i];
    for (int i = 0; i < 192; i++) top_inc[i] = (uint32_t)llrint(8372.018 * pow(2, i / 192.0) / MUSIC_RATE * 4294967296.0);

    for (int b = 0; b < NBAND; b++) {
        int h = (int)(6800 / (65.41 * (1 << b)));
        int32_t c[128];
        if (h < 1) h = 1;
        if (h > 127) h = 127;
        /* a saw is every harmonic at 1/k; the top ones are eased off so the edge does not ring */
        for (int k = 1; k <= h; k++) {
            double x = M_PI * k / (h + 1), sigma = h > 1 ? sin(x) / x : 1;
            c[k] = (int32_t)lrint(sigma / k * 2 / M_PI * 8192 * 1024);
        }
        for (int i = 0; i < TLEN; i++) {
            int64_t acc = 0;
            for (int k = 1; k <= h; k++) acc += (int64_t)c[k] * sine[(k * i * 4) & 2047];
            saw[b][i] = (int16_t)(acc >> 24);
        }
        saw[b][TLEN] = saw[b][0];
    }
    for (int i = 0; i <= TLEN; i++) {
        int64_t acc = 0;
        for (int k = 1, sign = 1; k <= 13; k += 2, sign = -sign)
            acc += (int64_t)sign * (int32_t)(8192.0 * 1024 * 8 / (M_PI * M_PI) / (k * k)) * sine[(k * i * 4) & 2047];
        tri[i] = (int16_t)(acc >> 24);
    }
}

/* ---------- the score ---------- */

#define NOTE_OFF 255
enum { D_KICK = 1, D_SNARE, D_BOTH, D_TOM1, D_TOM2, D_TOM3, D_HAT, D_OPEN, D_CRASH, D_RISE };

typedef struct cell { uint8_t note, arp, acc; } cell;      /* acc: 0 as written, 1 louder, 2 softer */
static cell cells[MUSIC_CH][MUSIC_MAX_BARS * MUSIC_BAR_ROWS];
static uint8_t bar_inst[MUSIC_CH][MUSIC_MAX_BARS], bar_vol[MUSIC_CH][MUSIC_MAX_BARS], bar_used[MUSIC_CH][MUSIC_MAX_BARS];
static int16_t last_kick[MUSIC_MAX_BARS * MUSIC_BAR_ROWS], last_snare[MUSIC_MAX_BARS * MUSIC_BAR_ROWS],
               last_bass[MUSIC_MAX_BARS * MUSIC_BAR_ROWS];
static int nbars, nticks;

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* one token into a cell. 0, or -1 */
static int token(const char *t, int n, int drums, cell *c)
{
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};     /* a b c d e f g */
    int i = 0;
    c->note = c->arp = c->acc = 0;
    if (n == 1 && t[0] == '.') return 0;
    if (n == 1 && t[0] == '-') { c->note = NOTE_OFF; return 0; }
    if (drums) {
        switch (t[i++]) {
        case 'k': c->note = D_KICK; break;
        case 's': c->note = D_SNARE; break;
        case 'x': c->note = D_BOTH; break;
        case 'h': c->note = D_HAT; break;
        case 'o': c->note = D_OPEN; break;
        case 'c': c->note = D_CRASH; break;
        case 'r': c->note = D_RISE; break;
        case 't':
            if (i >= n || t[i] < '1' || t[i] > '3') return -1;
            c->note = (uint8_t)(D_TOM1 + t[i++] - '1');
            break;
        default: return -1;
        }
    } else {
        if (t[0] < 'a' || t[0] > 'g') return -1;
        int s = semis[t[i++] - 'a'];
        if (i < n && t[i] == '#') { s++; i++; }
        if (i >= n || t[i] < '0' || t[i] > '8') return -1;
        c->note = (uint8_t)(12 * (t[i++] - '0' + 1) + s);    /* a4 is 69 */
        if (i < n && t[i] == '/') {
            if (i + 2 >= n || hexval(t[i + 1]) < 0 || hexval(t[i + 2]) < 0) return -1;
            c->arp = (uint8_t)(hexval(t[i + 1]) << 4 | hexval(t[i + 2]));
            i += 3;
        }
    }
    if (i < n && t[i] == '!') { c->acc = 1; i++; }
    else if (i < n && t[i] == '?') { c->acc = 2; i++; }
    return i == n ? 0 : -1;
}

int music_put(int ch, int bar, int bars, int inst, int vol, const char *text)
{
    cell ph[16 * MUSIC_BAR_ROWS];
    int n = 0;
    const char *p = text;

    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == 1 && *p == '|') {
            if (n % MUSIC_BAR_ROWS) goto bad;
        } else {
            if (n >= (int)(sizeof ph / sizeof ph[0]) || token(p, (int)(e - p), ch >= CH_DRUM, &ph[n]) < 0) goto bad;
            n++;
        }
        p = e;
    }
    if (!n || n % MUSIC_BAR_ROWS) goto bad;
    for (int b = 0; b < bars; b++) {
        if (bar + b < 0 || bar + b >= MUSIC_MAX_BARS) continue;
        bar_inst[ch][bar + b] = (uint8_t)inst;
        bar_vol[ch][bar + b] = (uint8_t)vol;
        bar_used[ch][bar + b] = 1;
        for (int r = 0; r < MUSIC_BAR_ROWS; r++) cells[ch][(bar + b) * MUSIC_BAR_ROWS + r] = ph[(b * MUSIC_BAR_ROWS + r) % n];
    }
    return 0;
bad:
    fprintf(stderr, "music: channel %d bar %d: cannot read \"%.40s\" (%d rows)\n", ch, bar, text, n);
    return -1;
}

/* ---------- voices ---------- */

typedef struct voice {
    /* what the sample loop reads */
    uint32_t ph, inc, width;
    const int16_t *tab;
    int32_t g, gt;                  /* gain now and where it is going, 8.8 */
    int wave, echo;
    /* what moves each tick */
    const music_inst *in;
    int note, arp, age, stage, level, vol, pwm_ph;
} voice;
enum { S_OFF, S_ATTACK, S_DECAY, S_RELEASE };

typedef struct drum {
    uint32_t ph, inc, inc_to;       /* the tone: a sine falling from one pitch to another */
    int sweep;
    int32_t ta;                     /* its level, Q15 */
    int tdec;
    uint32_t nph, nrate, lfsr;      /* the noise: a new random value nrate times a second */
    int32_t na, nval, nprev;
    int ndec, hp;
    int32_t rate_step, na_step;     /* the rising hiss: added each tick */
} drum;

static const struct kit {
    uint16_t f0, f1;                /* tone from, to, Hz; 0: none */
    uint8_t sweep, tdec;
    uint16_t ta;
    uint16_t nrate;                 /* Hz; 0: none */
    uint16_t na;
    uint8_t ndec, hp;
} kit[] = {
    [D_KICK]  = {360, 92, 8, 10, 15500, 16000, 2600, 6, 0},
    [D_SNARE] = {310, 190, 8, 9, 6500, 16000, 6600, 10, 0},
    [D_BOTH]  = {360, 92, 8, 10, 14000, 16000, 6000, 10, 0},
    [D_TOM1]  = {340, 200, 10, 11, 10500, 16000, 1400, 7, 0},
    [D_TOM2]  = {260, 150, 10, 11, 11000, 16000, 1400, 7, 0},
    [D_TOM3]  = {196, 112, 10, 11, 11500, 16000, 1400, 7, 0},
    [D_HAT]   = {0, 0, 0, 0, 0, 16000, 4000, 8, 1},
    [D_OPEN]  = {0, 0, 0, 0, 0, 16000, 3600, 10, 1},
    [D_CRASH] = {0, 0, 0, 0, 0, 7000, 4200, 13, 0},
    [D_RISE]  = {0, 0, 0, 0, 0, 700, 500, 30, 0},
};

static voice voices[CH_DRUM];
static drum drums[2];
static int tick_now;
int music_channels = (1 << MUSIC_CH) - 1;      /* which channels sound: all of them, unless demo-wav is asked for some */

#define ECHO_LEN 8192
#define ECHO_DELAY (3 * MUSIC_ROW_TICKS * MUSIC_TICK)   /* three rows: a dotted eighth */
#define ECHO_BACK 96                                    /* of 256 goes round again */
static int16_t echo_buf[ECHO_LEN];
static int echo_at;
static int32_t echo_lp, hp_state;
#define MASTER 400                                      /* of 256 */
#define KNEE 25000

static uint32_t hz_inc(int hz) { return (uint32_t)(((uint64_t)hz << 32) / MUSIC_RATE); }

static void drum_hit(drum *d, int what, int acc, int vol)
{
    const struct kit *k = &kit[what];
    int a = vol * (acc == 1 ? 80 : acc == 2 ? 30 : 64);   /* 4096 is as written */
    d->rate_step = d->na_step = 0;
    if (k->f0) {
        d->ph = 0;
        d->inc = hz_inc(k->f0); d->inc_to = hz_inc(k->f1);
        d->sweep = k->sweep; d->tdec = k->tdec;
        d->ta = (k->ta * a) >> 12;
    } else d->ta = 0;
    d->nrate = k->nrate >= MUSIC_RATE ? 0xffffffffu : hz_inc(k->nrate);
    d->na = (k->na * a) >> 12;
    d->ndec = k->ndec; d->hp = k->hp;
    if (what == D_RISE) {
        /* over a bar: from a low rumble of a hiss to a bright one, getting louder */
        d->rate_step = (int32_t)((hz_inc(7600) - d->nrate) / (MUSIC_BAR_ROWS * MUSIC_ROW_TICKS));
        d->na_step = (4200 * a >> 12) / (MUSIC_BAR_ROWS * MUSIC_ROW_TICKS);
    }
}

static void drum_render(drum *d, int32_t *out, int n)
{
    if (d->rate_step && d->nrate < 0xe0000000u) { d->nrate += (uint32_t)d->rate_step; d->na += d->na_step; }
    if (d->ta < 16 && d->na < 16) return;
    uint32_t ph = d->ph, inc = d->inc, nph = d->nph, lfsr = d->lfsr;
    int32_t ta = d->ta, na = d->na, nval = d->nval, nprev = d->nprev;
    for (int i = 0; i < n; i++) {
        int32_t o = 0;
        if (ta >= 16) {
            inc -= (uint32_t)((int32_t)(inc - d->inc_to) >> d->sweep);
            ph += inc;
            o = (sine[ph >> 21] * ta) >> 15;
            ta -= ta >> d->tdec;
        }
        nph += d->nrate;
        if (nph < d->nrate) {
            lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
            nprev = nval;
            nval = (int16_t)lfsr;
        }
        o += ((d->hp ? (nval - nprev) >> 1 : nval) * na) >> 15;
        na -= na >> d->ndec;
        out[i] += o;
    }
    d->ph = ph; d->inc = inc; d->nph = nph; d->lfsr = lfsr;
    d->ta = ta; d->na = na; d->nval = nval; d->nprev = nprev;
}

/* a triangle of -32..32 from a counter, 128 a cycle */
static inline int lfo(int p) { p &= 127; return p < 64 ? p - 32 : 96 - p; }

/* a tick of one note channel: envelope, pitch, width, and the gain to head for */
static void voice_tick(voice *v)
{
    const music_inst *in = v->in;
    if (!in || v->stage == S_OFF) { v->gt = 0; return; }
    switch (v->stage) {
    case S_ATTACK:
        v->level += 256 / (in->attack ? in->attack : 1);
        if (v->level >= 256) { v->level = 256; v->stage = S_DECAY; }
        break;
    case S_DECAY:
        if (v->level > in->sustain) { v->level -= in->decay; if (v->level < in->sustain) v->level = in->sustain; }
        break;
    default:
        v->level -= in->release ? in->release : 256;
        if (v->level <= 0) { v->level = 0; v->stage = S_OFF; }
    }
    int p = v->note * 16;
    if (v->arp) {
        int step = (v->age / (in->arp_speed ? in->arp_speed : 1)) % 3;
        p += (step == 1 ? v->arp >> 4 : step == 2 ? v->arp & 15 : 0) * 16;
    }
    if (in->vib && v->age >= in->vib_delay) p += (lfo((v->age - in->vib_delay) * in->vib_speed + 32) * in->vib) >> 5;
    if (p < 24 * 16) p = 24 * 16;
    if (p > 119 * 16) p = 119 * 16;
    v->inc = top_inc[p % 192] >> (10 - p / 192);
    int band = 0;
    while (band < NBAND - 1 && v->inc >= (BAND0_INC << (band + 1))) band++;
    v->wave = in->wave;
    v->tab = in->wave == W_TRI ? tri : saw[band];
    v->pwm_ph += in->pwm_speed;
    int width = in->duty + ((lfo(v->pwm_ph) * in->pwm) >> 5);
    v->width = (uint32_t)(width < 12 ? 12 : width > 244 ? 244 : width) << 24;
    v->echo = in->echo;
    v->gt = (v->level * in->vol * v->vol) >> 6;      /* 8.8: 256 * 255 * 64 >> 6 is all but 65536 */
    v->age++;
}

#define LERP(t, p) ((t)[(p) >> 23] + ((((t)[((p) >> 23) + 1] - (t)[(p) >> 23]) * (int32_t)(((p) >> 15) & 255)) >> 8))

static void voice_render(voice *v, int32_t *dry, int32_t *snd, int n)
{
    if (!v->gt && v->g < 64) { v->g = 0; return; }
    uint32_t ph = v->ph, inc = v->inc, width = v->width;
    const int16_t *t = v->tab;
    int32_t g = v->g, gt = v->gt;
    int echo = v->echo, pulse = v->wave == W_PULSE;
    for (int i = 0; i < n; i++) {
        int32_t s = LERP(t, ph);
        if (pulse) { uint32_t q = ph + width; s -= LERP(t, q); }
        ph += inc;
        g += (gt - g) >> 4;                 /* a millisecond or so to get there: no clicks */
        s = (s * (g >> 4)) >> 12;
        dry[i] += s;
        if (echo) snd[i] += s >> echo;
    }
    v->ph = ph; v->g = g;
}

/* ---------- the player ---------- */

static void row_start(int row)
{
    int bar = row / MUSIC_BAR_ROWS;
    for (int ch = 0; ch < MUSIC_CH; ch++) {
        const cell *c = &cells[ch][row];
        if (!c->note || !(music_channels >> ch & 1)) continue;
        int vol = bar_vol[ch][bar];
        if (ch >= CH_DRUM) {
            drum *d = &drums[ch - CH_DRUM];
            if (c->note == NOTE_OFF) { d->rate_step = d->na_step = 0; d->ndec = d->tdec = 6; }
            else drum_hit(d, c->note, c->acc, vol);
            continue;
        }
        voice *v = &voices[ch];
        if (c->note == NOTE_OFF) { if (v->stage != S_OFF) v->stage = S_RELEASE; continue; }
        v->in = &tune_inst[bar_inst[ch][bar]];
        v->note = c->note; v->arp = c->arp;
        v->vol = c->acc == 1 ? (vol * 5) >> 2 : c->acc == 2 ? vol >> 1 : vol;
        if (v->vol > 64) v->vol = 64;
        v->age = 0;
        if (v->in->attack) { v->stage = S_ATTACK; v->level = 0; }
        else { v->stage = S_DECAY; v->level = 256 + v->in->decay; }     /* the tick takes one step off again */
    }
}

int music_ticks(void) { return nticks; }
int music_tick(void) { return tick_now; }

void music_seek(int tick)
{
    if (nticks <= 0) return;
    tick_now = ((tick % nticks) + nticks) % nticks;
    for (int i = 0; i < CH_DRUM; i++) { voices[i].stage = S_OFF; voices[i].gt = 0; }
    for (int i = 0; i < 2; i++) { drums[i].rate_step = drums[i].na_step = 0; drums[i].ndec = drums[i].tdec = 6; }
}

void music_render(int16_t *out, int n)
{
    static int32_t dry[MUSIC_TICK + 16], snd[MUSIC_TICK + 16];

    if (n > MUSIC_TICK + 16) n = MUSIC_TICK + 16;
    if (nticks <= 0) { memset(out, 0, (size_t)n * sizeof *out); return; }
    if (tick_now % MUSIC_ROW_TICKS == 0) row_start(tick_now / MUSIC_ROW_TICKS);
    memset(dry, 0, (size_t)n * sizeof dry[0]);
    memset(snd, 0, (size_t)n * sizeof snd[0]);
    for (int i = 0; i < CH_DRUM; i++) {
        voice_tick(&voices[i]);
        voice_render(&voices[i], dry, snd, n);
    }
    drum_render(&drums[0], dry, n);
    drum_render(&drums[1], dry, n);

    int at = echo_at;
    int32_t lp = echo_lp, hp = hp_state;
    for (int i = 0; i < n; i++) {
        /* the echo: what went in three rows ago, duller each time round */
        lp += (echo_buf[(at - ECHO_DELAY) & (ECHO_LEN - 1)] - lp) >> 1;
        int32_t in = snd[i] + ((lp * ECHO_BACK) >> 8);
        echo_buf[at] = (int16_t)(in > 32767 ? 32767 : in < -32768 ? -32768 : in);
        at = (at + 1) & (ECHO_LEN - 1);
        int32_t x = dry[i] + lp;
        /* nothing under 100 Hz: the loudspeaker cannot play it, and it would only eat headroom */
        hp += ((x * 256 - hp) * 10) >> 8;
        x = ((x - (hp >> 8)) * MASTER) >> 8;
        /* the odd peak over the knee is squeezed, never clipped */
        if (x > KNEE) { x = KNEE + ((x - KNEE) >> 2); if (x > 32000) x = 32000; }
        else if (x < -KNEE) { x = -KNEE + ((x + KNEE) >> 2); if (x < -32000) x = -32000; }
        out[i] = (int16_t)x;
    }
    echo_at = at; echo_lp = lp; hp_state = hp;
    if (++tick_now >= nticks) tick_now = 0;
}

/* ---------- start ---------- */

int music_init(int outro_bars)
{
    tables();
    memset(cells, 0, sizeof cells);
    memset(bar_used, 0, sizeof bar_used);
    nbars = outro_bars < 0 ? -1 : tune_build(outro_bars);
    if (nbars <= 0 || nbars > MUSIC_MAX_BARS) { nbars = nticks = 0; return -1; }
    int rows = nbars * MUSIC_BAR_ROWS;
    nticks = rows * MUSIC_ROW_TICKS;

    /* a channel with nothing written in a bar is quiet there: let go of whatever the bar before left sounding */
    for (int ch = 0; ch < MUSIC_CH; ch++)
        for (int b = 0; b < nbars; b++)
            if (!bar_used[ch][b] && bar_used[ch][(b + nbars - 1) % nbars]) cells[ch][b * MUSIC_BAR_ROWS].note = NOTE_OFF;

    /* for the picture: the row of the latest kick, snare and bass note at or before each row */
    int k = -1, s = -1, b = -1;
    for (int r = 0; r < rows; r++) {
        int d = cells[CH_DRUM][r].note, soft = cells[CH_DRUM][r].acc == 2;
        if ((d == D_KICK || d == D_BOTH) && !soft) k = r;
        if ((d == D_SNARE || d == D_BOTH) && !soft) s = r;
        if (cells[CH_BASS][r].note && cells[CH_BASS][r].note != NOTE_OFF) b = r;
        last_kick[r] = (int16_t)k; last_snare[r] = (int16_t)s; last_bass[r] = (int16_t)b;
    }
    drums[0].lfsr = 0x2545f491; drums[1].lfsr = 0x9e3779b9;
    music_seek(0);
    return nbars;
}

static int fall(int row, long long us, int ms)
{
    if (row < 0) return 0;
    int age = (int)((us - (long long)row * MUSIC_ROW_TICKS * MUSIC_TICK_US) / 1000);
    return age >= ms ? 0 : 256 - age * 256 / ms;
}

void music_cue_at(long long us, music_cue *c)
{
    c->kick = c->snare = c->bass = 0;
    if (nticks <= 0 || us < 0) return;
    int row = (int)(us / (MUSIC_ROW_TICKS * MUSIC_TICK_US));
    if (row >= nbars * MUSIC_BAR_ROWS) return;
    c->kick = fall(last_kick[row], us, 200);
    c->snare = fall(last_snare[row], us, 200);
    c->bass = fall(last_bass[row], us, 240);
}
