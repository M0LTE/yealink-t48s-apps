// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the score. An original tune in A minor at 125 beats a minute, written
 * for this demo: a block of bars for each part of the timeline, then blocks
 * that fill the outro however long its text turns out to be.
 *
 * A phrase is text, a token a row (sixteen to the bar): music.h has the
 * notation. The macros below spell out the figures that only change with the
 * chord. */
#include "music.h"

enum { I_LEAD, I_SOFT, I_PLUCK, I_SAW, I_HARM, I_SPARK, I_CHORD, I_STAB, I_BROKEN, I_BASS, I_DEEP, I_FUNK, I_KIT };

const music_inst tune_inst[] = {
    /*             wave     duty pwm spd  att dec sus rel  vib spd dly  arp  vol echo */
    [I_LEAD]   = {W_PULSE,  64, 22, 3,   0, 10, 180, 40,  5, 14, 10,  0,  150, 2},   /* the tune: a quarter pulse that sways */
    [I_SOFT]   = {W_PULSE, 128, 12, 2,   2,  6, 200, 24,  5, 13, 12,  0,  125, 1},   /* a hollow square, sung */
    [I_PLUCK]  = {W_PULSE,  32,  0, 0,   0, 18,  95, 60,  0,  0,  0,  0,  170, 2},   /* thin and short */
    [I_SAW]    = {W_SAW,     0,  0, 0,   0, 20, 125, 50,  0,  0,  0,  0,  195, 2},
    [I_HARM]   = {W_PULSE, 100,  0, 0,   0, 10, 150, 40,  4, 13, 12,  0,   85, 3},   /* the second voice, under the tune */
    [I_SPARK]  = {W_TRI,     0,  0, 0,   0, 14,   0, 30,  0,  0,  0,  0,  190, 1},   /* high and far away */
    [I_CHORD]  = {W_PULSE,  90, 20, 5,   0, 20,  70, 50,  0,  0,  0,  1,   80, 3},   /* three notes in turn, fast: a chord from one voice */
    [I_STAB]   = {W_SAW,     0,  0, 0,   0, 28,   0, 60,  0,  0,  0,  1,  170, 2},
    [I_BROKEN] = {W_PULSE, 128,  0, 0,   0, 26,  30, 50,  0,  0,  0,  0,   78, 1},
    [I_BASS]   = {W_PULSE,  80, 16, 4,   0, 14, 150, 60,  0,  0,  0,  0,  112, 0},
    [I_DEEP]   = {W_PULSE, 128,  8, 1,   1,  4, 200, 30,  0,  0,  0,  0,   84, 0},   /* long notes: a square, so a small loudspeaker has harmonics to play */
    [I_FUNK]   = {W_SAW,     0,  0, 0,   0, 20, 115, 80,  0,  0,  0,  0,  170, 0},
    [I_KIT]    = {0},
};

/* a chord's rhythm: 3 + 3 + 2, twice */
#define C332(x) x " . . " x " . . " x " . " x " . . " x " . . " x " . | "
#define C8(x) x " . " x " . " x " . " x " . " x " . " x " . " x " . " x " . | "
/* off the beat */
#define STAB(x) "- . " x " . - . " x " . - . " x " . - " x " - . | "
/* a chord a note at a time */
#define BRK(a, b, c) a " " b " " c " " b " " a " " b " " c " " b " " a " " b " " c " " b " " a " " b " " c " " b " | "
/* bass figures: r the root, R the octave above it */
#define B8(r, R) r " . " R " . " r " . " R " . " r " . " R " . " r " . " R " . | "
#define BD(r, R) r " . " r " " R " . " r " " R " . " r " . " r " " R " . " r " " R " " r " | "
#define BT(r, R) r " " r " " R " " r " " r " " R " " r " " R " " r " " r " " R " " r " " r " " R " " r " " R " | "
#define B16(r) r "! " r " " r " " r " " r "! " r " " r " " r " " r "! " r " " r " " r " " r "! " r " " r " " r " | "
#define BL(r, R) r " . . . . . . . . . . . " R " . " r " . | "
#define FNK(r, R, a) r " . . " R " . " r " . . " r " . " R " . . " a " " R " . | "

#define AM "a3/37"
#define F "f3/47"
#define C "c4/47"
#define G "g3/47"
#define E "e3/47"
#define EM "e3/37"
#define DM "d4/37"
#define BB "a#3/47"

#define DR_A "k . . . s . . k . . k . s . . . | "
#define DR_B "k . . . s . . k . . k . s . k . | "
#define DR_FILL "k . . . s . . k . . k . s s t2 t3 | "
#define DR_HALF "k . . . . . . . s . . . . . k? . | "
#define DR_FOUR "k . . . x . . . k . . . x . . k | "
#define HT8 "h . h? . h . h? . h . h? . h . o . | "
#define HT8C "c . h? . h . h? . h . h? . h . o . | "
#define HT16 "h h? h h? h h? h h? h h? h h? h h? o . | "
#define HT16C "c h? h h? h h? h h? h h? h h? h h? o . | "

/* the tune, and the same with its second voice */
static const char theme[] =
    "e5 . . c5 . . a4 . b4 . c5 . e5 . . . | f5 . . e5 . . c5 . a4 . . . c5 . d5 . | "
    "e5 . . g5 . . e5 . d5 . c5 . d5 . e5 . | d5 . . . b4 . g4 . b4 . d5 . g5 . f5 . | "
    "e5 . . c5 . . a4 . b4 . c5 . e5 . a5 . | g5 . . f5 . . c5 . f5 . . . a5 . g5 . | "
    "g5 . . d5 . . b4 . d5 . g5 . b5 . a5 . | g#5 . . . e5 . . . b4 . e5 . g#5 . b5 . | ";
static const char theme_under[] =
    "c5 . . a4 . . e4 . g4 . a4 . c5 . e5 . | e5 . . c5 . . a4 . c5 . . . f5 . e5 . | "
    "d5 . . b4 . . g4 . b4 . d5 . g5 . f5 . | e5 . . . b4 . . . g#4 . b4 . e5 . g#5 . | ";
static const char theme_chords[] = C332(AM) C332(F) C332(C) C332(G) C332(AM) C332(F) C332(G) C332(E);
static const char theme_bass[] =
    BD("a2", "a3") BD("f2", "f3") BD("c3", "c4") BD("g2", "g3") BD("a2", "a3") BD("f2", "f3") BD("g2", "g3") BD("e2", "e3");

static int base, limit, bad;

/* a phrase on a channel, at bars into the block being written, cut off where the block ends */
static void put(int ch, int inst, int vol, int at, int bars, const char *text)
{
    if (at >= limit) return;
    if (at + bars > limit) bars = limit - at;
    if (music_put(ch, base + at, bars, inst, vol, text) < 0) bad = 1;
}

/* stars: out of nothing. Chords and far-off notes, then a slow voice and a beat that gathers */
static void intro(void)
{
    static const char chords[] = C332(AM) C332(AM) C332(F) C332(F) C332(C) C332(C) C332(G) C332(E);
    put(CH_CHORD, I_CHORD, 26, 0, 2, chords);
    put(CH_CHORD, I_CHORD, 44, 2, 2, C332(F));
    put(CH_CHORD, I_CHORD, 58, 4, 4, C332(C) C332(C) C332(G) C332(E));
    put(CH_LEAD2, I_SPARK, 52, 0, 4,
        "a5 . . . . . e6 . . . . . c6 . . . | . . . . b5 . . . . . e6 . . . a5 . | "
        "a5 . . . . . f6 . . . . . c6 . . . | . . . . a5 . . . . . c6 . . . f5 . | ");
    put(CH_LEAD, I_SOFT, 56, 4, 4,
        "e5 . . . . . . . g5 . . . . . e5 . | d5 . . . . . c5 . . . . . . . . . | "
        "d5 . . . . . . . g5 . . . . . b4 . | b4 . . . . . . . g#4 . . . b4 . d5 . | ");
    put(CH_BASS, I_DEEP, 54, 2, 2, "f2 . . . . . . . . . . . . . . . | f2 . . . . . . . . . . . f3 . f2 . | ");
    put(CH_BASS, I_BASS, 54, 4, 4, B8("c3", "c4") B8("c3", "c4") B8("g2", "g3") B8("e2", "e3"));
    put(CH_HAT, I_KIT, 40, 4, 3, HT8);
    put(CH_HAT, I_KIT, 56, 7, 1, "r . . . . . . . . . . . . . . . | ");
    put(CH_DRUM, I_KIT, 52, 4, 2, "k . . . . . . . k . . . . . . . | ");
    put(CH_DRUM, I_KIT, 58, 6, 2, "k . . . k . . . k . . . k . . . | k . . . k . . . s? . s? . s s s! s! | ");
}

/* gem: the tune */
static void gem(void)
{
    put(CH_LEAD, I_LEAD, 64, 0, 8, theme);
    put(CH_LEAD2, I_HARM, 64, 4, 4, theme_under);
    put(CH_CHORD, I_CHORD, 60, 0, 8, theme_chords);
    put(CH_BASS, I_BASS, 64, 0, 8, theme_bass);
    put(CH_DRUM, I_KIT, 64, 0, 8, DR_A DR_B DR_A DR_B DR_A DR_B DR_A DR_FILL);
    put(CH_HAT, I_KIT, 56, 0, 8, HT8C HT8 HT8 HT8 HT8 HT8 HT8 HT8);
}

/* plasma: slower on its feet. A sung line over broken chords, seven bars, the last one leaning forward */
static void plasma(void)
{
    put(CH_LEAD, I_SOFT, 58, 0, 7,
        "a5 . . . . . g5 . f5 . . . . . c5 . | d5 . . . . . . . b4 . . . d5 . g5 . | "
        "g5 . . . . . f#5 . e5 . . . . . b4 . | c5 . . . . . . . a4 . . . c5 . e5 . | "
        "f5 . . . . . a5 . c6 . . . . . a5 . | b5 . . . . . g5 . d5 . . . g5 . b5 . | "
        "b5 . . . . . g#5 . e5 . . . g#5 . b5 . | ");
    put(CH_CHORD, I_BROKEN, 52, 0, 7,
        BRK("f4", "a4", "c5") BRK("g4", "b4", "d5") BRK("e4", "g4", "b4") BRK("a4", "c5", "e5")
        BRK("f4", "a4", "c5") BRK("g4", "b4", "d5") BRK("e4", "g#4", "b4"));
    put(CH_BASS, I_DEEP, 52, 0, 7,
        BL("f2", "f3") BL("g2", "g3") BL("e2", "e3") BL("a2", "a3") BL("f2", "f3") BL("g2", "g3") B8("e2", "e3"));
    put(CH_DRUM, I_KIT, 54, 0, 7, "k . . . . . . . . . . . . . . . | " DR_HALF DR_HALF DR_HALF DR_HALF DR_HALF
        "k . . . . . k . s? . s? . s . s s! | ");
    put(CH_HAT, I_KIT, 34, 0, 6, "c? . h? . h . h? . h . h? . h . h? . | h . h? . h . h? . h . h? . h . h? . | ");
    put(CH_HAT, I_KIT, 50, 6, 1, "r . . . . . . . . . . . . . . . | ");
}

/* knot: two bars winding up, then the tune's quick cousin */
static void knot(void)
{
    put(CH_BASS, I_BASS, 60, 0, 2, B16("a2"));
    put(CH_CHORD, I_CHORD, 44, 0, 1, C8(AM));
    put(CH_CHORD, I_CHORD, 60, 1, 1, C8(AM));
    put(CH_LEAD, I_LEAD, 60, 1, 1, "a4 . b4 . c5 . d5 . e5 . f5 . g5 . g#5 . | ");
    put(CH_DRUM, I_KIT, 62, 0, 2, "k . . . k . . . k . . . k . . . | k . . . k . . . k . k . s s s! s! | ");
    put(CH_HAT, I_KIT, 50, 0, 2, "c . h? . h . h? . h . h? . h . h? . | o . h? . o . h? . o . h? . o . o . | ");

    put(CH_LEAD, I_LEAD, 64, 2, 8,
        "a5 . e5 a5 . e5 c6 . b5 . a5 . e5 . c5 . | a5 . f5 a5 . f5 c6 . a5 . . . f5 . c5 . | "
        "g5 . e5 g5 . e5 c6 . b5 . g5 . e5 . g5 . | b5 . g5 b5 . g5 d6 . b5 . . . g5 a5 b5 . | "
        "a5 . f5 a5 . f5 d6 . c6 . a5 . f5 . d5 . | c6 . a5 c6 . a5 f5 . a5 . c6 . a5 . f5 . | "
        "e5 . g#5 . b5 . e6 . . . d6 . b5 . g#5 . | b5 . g#5 . e5 . b4 . e5 . g#5 . b5 . e6 . | ");
    put(CH_LEAD2, I_HARM, 64, 6, 4,
        "f5 . d5 f5 . d5 a5 . a5 . f5 . d5 . a4 . | a5 . f5 a5 . f5 c5 . f5 . a5 . f5 . c5 . | "
        "b4 . e5 . g#5 . b5 . . . b5 . g#5 . e5 . | g#5 . e5 . b4 . g#4 . b4 . e5 . g#5 . b5 . | ");
    put(CH_CHORD, I_CHORD, 60, 2, 8, C332(AM) C332(F) C332(C) C332(G) C332(DM) C332(F) C332(E) C8(E));
    put(CH_BASS, I_BASS, 64, 2, 8,
        BD("a2", "a3") BD("f2", "f3") BD("c3", "c4") BD("g2", "g3") BD("d3", "d4") BD("f2", "f3") BD("e2", "e3") BD("e2", "e3"));
    put(CH_DRUM, I_KIT, 64, 2, 8, DR_A DR_B DR_A DR_B DR_A DR_B DR_A DR_FILL);
    put(CH_HAT, I_KIT, 50, 2, 8, HT16C HT16 HT16 HT16 HT16 HT16 HT16 HT16);
}

/* roto: on the off beat, in D minor */
static void roto(void)
{
    put(CH_LEAD, I_PLUCK, 64, 0, 7,
        "d5 . d5 f5 . d5 . a5 . g5 . f5 d5 . c5 d5 | . . . . . . . . a4 c5 d5 . f5 . d5 . | "
        "d5 . d5 f5 . d5 . a#5 . a5 . f5 d5 . f5 a5 | g5 . . e5 . . c5 . e5 . g5 . c6 . a#5 . | "
        "a5 . a5 d6 . a5 . f5 . g5 . a5 f5 . d5 f5 | c6 . a5 . f5 . a5 . c6 . . a5 . . f5 . | "
        "e5 . g#5 . b5 . d6 . . . b5 . g#5 . e5 . | ");
    put(CH_CHORD, I_STAB, 60, 0, 7, STAB(DM) STAB(DM) STAB(BB) STAB(C) STAB(DM) STAB(F) C8(E));
    put(CH_BASS, I_FUNK, 64, 0, 7,
        FNK("d3", "d4", "c4") FNK("d3", "d4", "c4") FNK("a#2", "a#3", "g#3") FNK("c3", "c4", "a#3")
        FNK("d3", "d4", "c4") FNK("f2", "f3", "d#3") B8("e2", "e3"));
    put(CH_DRUM, I_KIT, 64, 0, 7,
        "k . . . s . . k . k . . s . k . | k . . k s . . k . k . . s . . s? | "
        "k . . . s . . k . k . . s . k . | k . . k s . . k . k . . s . . s? | "
        "k . . . s . . k . k . . s . k . | k . . k s . . k . k . . s . . s? | "
        "k . . . s . . k . . s . s t1 t2 t3 | ");
    put(CH_HAT, I_KIT, 50, 0, 7,
        "c h? h h? h? h? o . h? h? h h? h? h? o . | h? h? h h? h? h? o . h? h? h h? h? h? o . | ");
}

/* tunnel: four to the floor, a lead that runs then holds, and a bar of held breath before the end part */
static void tunnel(void)
{
    put(CH_LEAD, I_SAW, 64, 0, 9,
        "a5 e5 c5 e5 a5 e5 c5 e5 a5 e5 c5 e5 b5 e5 c6 e5 | c6 . . . b5 . a5 . . . e5 . . . a5 . | "
        "g5 d5 b4 d5 g5 d5 b4 d5 g5 d5 b4 d5 a5 d5 b5 d5 | b5 . . . a5 . g5 . . . d5 . . . g5 . | "
        "f5 c5 a4 c5 f5 c5 a4 c5 f5 c5 a4 c5 g5 c5 a5 c5 | a5 . . . g5 . f5 . . . c5 . f5 . a5 . | "
        "g#5 e5 b4 e5 g#5 e5 b4 e5 b5 e5 g#5 e5 b5 e5 d6 e5 | e6 . . . d6 . b5 . . . g#5 . b5 . e5 . | "
        "e5 . . . . . . . . . . . - . . . | ");
    put(CH_LEAD2, I_HARM, 64, 0, 8,
        "- . . . . . . . . . . . . . . . | a5 . . . g5 . e5 . . . c5 . . . e5 . | "
        "- . . . . . . . . . . . . . . . | g5 . . . d5 . b4 . . . b4 . . . d5 . | "
        "- . . . . . . . . . . . . . . . | f5 . . . e5 . c5 . . . a4 . c5 . f5 . | "
        "- . . . . . . . . . . . . . . . | b5 . . . b5 . g#5 . . . e5 . g#5 . b4 . | ");
    put(CH_CHORD, I_CHORD, 58, 0, 9, C8(AM) C8(AM) C8(G) C8(G) C8(F) C8(F) C8(E) C8(E) C332(E));
    put(CH_BASS, I_BASS, 64, 0, 8,
        BT("a2", "a3") BT("a2", "a3") BT("g2", "g3") BT("g2", "g3") BT("f2", "f3") BT("f2", "f3") BT("e2", "e3") BT("e2", "e3"));
    put(CH_BASS, I_DEEP, 56, 8, 1, "e2 . . . . . . . . . . . . . . . | ");
    put(CH_DRUM, I_KIT, 64, 0, 9, DR_FOUR DR_FOUR DR_FOUR DR_FOUR DR_FOUR DR_FOUR DR_FOUR
        "k . . . x . . . k . . . x . t1 t2 | k . . . . . . . s? . s? . s . s s! | ");
    put(CH_HAT, I_KIT, 50, 0, 8,
        "c h? o . h h? o . h h? o . h h? o . | h h? o . h h? o . h h? o . h h? o . | ");
    put(CH_HAT, I_KIT, 56, 8, 1, "r . . . . . . . . . . . . . . . | ");
}

/* The outro is as long as its text. Three blocks of eight bars take turns until
 * two bars from the end: a calm line in C, the tune again, and the calm line
 * with a second voice, turning home on its last bar. */
#define CALM_1_7 \
    "e5 . . . g5 . . . c6 . . . b5 . g5 . | a5 . g5 . . . d5 . . . . . b4 . d5 . | " \
    "c5 . . . e5 . . . a5 . . . g5 . e5 . | f5 . e5 . . . c5 . . . . . a4 . c5 . | " \
    "e5 . . . g5 . . . c6 . . . d6 . e6 . | d6 . b5 . . . g5 . . . . . b5 . d6 . | " \
    "c6 . . . a5 . . . f5 . . . a5 . c6 . | "

static void calm(int second)
{
    put(CH_LEAD, I_SOFT, 64, 0, 8, second ? CALM_1_7 "b5 . . . g#5 . . . e5 . g#5 . b5 . e6 . | "
                                          : CALM_1_7 "b5 . . . g5 . . . d5 . g5 . b5 . d6 . | ");
    if (second)
        put(CH_LEAD2, I_HARM, 64, 0, 8,
            "c5 . . . e5 . . . g5 . . . g5 . e5 . | d5 . d5 . . . b4 . . . . . g4 . b4 . | "
            "a4 . . . c5 . . . e5 . . . e5 . c5 . | c5 . c5 . . . a4 . . . . . f4 . a4 . | "
            "c5 . . . e5 . . . g5 . . . b5 . c6 . | b5 . g5 . . . d5 . . . . . g5 . b5 . | "
            "a5 . . . f5 . . . c5 . . . f5 . a5 . | g#5 . . . e5 . . . b4 . e5 . g#5 . b5 . | ");
    put(CH_CHORD, I_CHORD, 54, 0, 8, second ? C332(C) C332(G) C332(AM) C332(F) C332(C) C332(G) C332(F) C8(E)
                                            : C332(C) C332(G) C332(AM) C332(F) C332(C) C332(G) C332(F) C332(G));
    put(CH_BASS, I_BASS, 64, 0, 8, second
        ? B8("c3", "c4") B8("g2", "g3") B8("a2", "a3") B8("f2", "f3") B8("c3", "c4") B8("g2", "g3") B8("f2", "f3") B8("e2", "e3")
        : B8("c3", "c4") B8("g2", "g3") B8("a2", "a3") B8("f2", "f3") B8("c3", "c4") B8("g2", "g3") B8("f2", "f3") B8("g2", "g3"));
    if (second) {
        put(CH_DRUM, I_KIT, 62, 0, 8, DR_A DR_B DR_A DR_B DR_A DR_B DR_A DR_FILL);
        put(CH_HAT, I_KIT, 48, 0, 8, HT8C HT8 HT8 HT8 HT8 HT8 HT8 HT8);
    } else {
        put(CH_DRUM, I_KIT, 58, 0, 8, DR_HALF DR_HALF DR_HALF "k . . . . . k . s . . . . . k . | "
                                      DR_HALF DR_HALF DR_HALF "k . . . . . k . s . . k . s t2 t3 | ");
        put(CH_HAT, I_KIT, 42, 0, 8, HT8C HT8 HT8 HT8 HT8 HT8 HT8 HT8);
    }
}

/* the last two bars: home, and let it ring */
static void ending(void)
{
    put(CH_LEAD, I_SOFT, 64, 0, 2, "a5 . . . . . . . . . . . . . . . | . . . . - . . . . . . . . . . . | ");
    put(CH_LEAD2, I_HARM, 64, 0, 2, "e5 . . . . . . . . . . . . . . . | - . . . . . . . . . . . . . . . | ");
    put(CH_CHORD, I_CHORD, 60, 0, 2, "a3/37 . . . . . . . a3/37? . . . . . . . | - . . . . . . . . . . . . . . . | ");
    put(CH_BASS, I_DEEP, 62, 0, 2, "a2 . . . . . . . . . . . . . . . | - . . . . . . . . . . . . . . . | ");
    put(CH_DRUM, I_KIT, 64, 0, 1, "x . . . . . . . . . . . . . . . | ");
    put(CH_HAT, I_KIT, 56, 0, 1, "c . . . . . . . . . . . . . . . | ");
}

int tune_build(int outro_bars)
{
    static const struct { void (*write)(void); int bars; } fixed[] = {
        {intro, 8}, {gem, 8}, {plasma, 7}, {knot, 10}, {roto, 7}, {tunnel, 9},
    };
    bad = 0;
    base = 0;
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        limit = fixed[i].bars;
        fixed[i].write();
        base += fixed[i].bars;
    }
    if (outro_bars < 4) outro_bars = 4;
    if (base + outro_bars > MUSIC_MAX_BARS) return -1;
    int left = outro_bars - 2;
    for (int k = 0; left > 0; k++) {
        limit = left < 8 ? left : 8;
        if (k % 3 == 0) calm(0);
        else if (k % 3 == 1) gem();
        else calm(1);
        base += limit;
        left -= limit;
    }
    limit = 2;
    ending();
    base += 2;
    return bad ? -1 : base;
}
