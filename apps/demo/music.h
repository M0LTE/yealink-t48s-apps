// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the tune. music.c is a small tracker-style player and its synth,
 * tune.c the score, sound.c what gets it to the loudspeaker in time with the
 * picture. Nothing here knows about libt48, so the same player renders the
 * track to a file on a PC (wav.c, `make wav`).
 *
 * Time: 16000 samples a second. A tick is 320 samples (20 ms), a row six
 * ticks (a sixteenth note at 125 beats a minute), a bar sixteen rows. */
#ifndef MUSIC_H
#define MUSIC_H

#include <stdint.h>

#define MUSIC_RATE 16000
#define MUSIC_TICK 320
#define MUSIC_TICK_US 20000
#define MUSIC_ROW_TICKS 6
#define MUSIC_BAR_ROWS 16
#define MUSIC_FIXED_BARS 49     /* the six parts before the outro: 8 8 7 10 7 9 */
#define MUSIC_MAX_BARS 128

enum { CH_LEAD, CH_LEAD2, CH_CHORD, CH_BASS, CH_DRUM, CH_HAT, MUSIC_CH };
enum { W_PULSE, W_SAW, W_TRI };

typedef struct music_inst {
    uint8_t wave;
    uint8_t duty, pwm, pwm_speed;       /* W_PULSE: width in 256ths, how far it sways and how fast */
    uint8_t attack;                     /* ticks to full level; 0: at once */
    uint8_t decay, sustain, release;    /* level (0..255) lost a tick down to sustain, and after the note is let go */
    uint8_t vib, vib_speed, vib_delay;  /* sixteenths of a semitone each way, speed, ticks before it starts */
    uint8_t arp_speed;                  /* ticks a step of a chord written as one note */
    uint8_t vol;                        /* 0..255 */
    uint8_t echo;                       /* 0: none, else how far down it goes to the echo: 1 half, 2 a quarter... */
} music_inst;

/* ---------- the score (tune.c fills it in) ---------- */

/* Lay text on a channel from bar on, for bars bars, played with instrument inst
 * at vol (0..64); a phrase shorter than that goes round again. A token a row:
 *   a4 c#5      a note            a3/37     a chord: the note, 3 and 7 semitones up, in turn
 *   .           carry on          -         let go
 *   k s x       kick, snare, both           t1 t2 t3  toms, high to low
 *   h o c r     hat, open hat, crash, a rising hiss until the next token
 *   ! or ?      after any of them: louder, softer
 *   |           between bars, checked
 * Returns 0, or -1 for text it cannot read. */
int music_put(int ch, int bar, int bars, int inst, int vol, const char *text);

/* tune.c: the instruments, and the whole score for a loop whose last part is
 * outro_bars long. Returns the bars in the loop, or -1. */
extern const music_inst tune_inst[];
int tune_build(int outro_bars);

/* ---------- the player ---------- */

int music_init(int outro_bars);         /* tables and score. Bars in the loop, or -1. A negative outro_bars: no tune */
int music_ticks(void);                  /* the loop, in ticks */
int music_tick(void);                   /* the tick the next music_render makes */
void music_seek(int tick);
/* The next tick, as n samples: 320, or a few either way to nudge the tempo */
void music_render(int16_t *out, int n);
extern int music_channels;              /* a bit a channel: clear one to leave it out */

/* What the picture may want to know about a moment of the loop (us into it).
 * Read from the score, not from the sound, so it is the same with no sound. */
typedef struct music_cue {
    int kick, snare;    /* 0..256: 256 as it hits, gone 200 ms later */
    int bass;           /* the same for each bass note, over 240 ms */
} music_cue;
void music_cue_at(long long us, music_cue *c);

/* What the score has starting at a row of the loop (sixteen to the bar), for
 * whoever wants more than the cue: MUSIC_EV_* added together; 0 for a row
 * past the end. Soft hits (written with ?) are left out. */
enum { MUSIC_EV_KICK = 1, MUSIC_EV_SNARE = 2, MUSIC_EV_TOM = 4, MUSIC_EV_CRASH = 8, MUSIC_EV_CHORD = 16, MUSIC_EV_BASS = 32 };
int music_rows(void);                   /* rows in the loop */
int music_row_events(int row);

#endif
