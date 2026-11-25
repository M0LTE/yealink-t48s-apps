// SPDX-License-Identifier: AGPL-3.0-or-later
/* snake: its few sounds, made as they play (sfx.c). On a phone without sound
 * for apps (before API level 4) every one of these does nothing. */
#ifndef SFX_H
#define SFX_H

#include <stdint.h>

#define SFX_RATE 16000

void sfx_init(void);            /* once: the tables, and the way to the loudspeaker if there is one */
void sfx_start(void);           /* a game begins */
void sfx_eat(int score);        /* an apple: a touch higher with each one */
void sfx_turn(void);
void sfx_crash(void);
void sfx_best(void);            /* the game ended on a new best */
void sfx_quiet(void);           /* drop whatever is sounding */

/* the synth by itself, for wav.c: n samples of whatever is sounding; 0 once nothing is */
void sfx_tables(void);
int sfx_render(int16_t *out, int n);

#endif
