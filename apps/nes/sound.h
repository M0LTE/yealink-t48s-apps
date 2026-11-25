// SPDX-License-Identifier: AGPL-3.0-or-later
/* The game's sound, from the core to the phone's loudspeaker (API level 4).
 * On a phone or a libt48 without sound these do nothing and the core makes
 * none, so the game plays the same, silent. */
#ifndef SOUND_H
#define SOUND_H

#include <stddef.h>
#include "nes.h"

void sound_init(nes_t *n);                /* once, before any game */
/* After each frame: what it made goes out. late_us is how long it is since the frame was due to begin. */
void sound_frame(nes_t *n, long late_us);
void sound_stats(char *out, size_t n);    /* how it went since the last call, for the log; "" without sound */

#endif
