// SPDX-License-Identifier: AGPL-3.0-or-later
/* Where the game's picture goes. Everything that differs between copying
 * (API level 2: draw in the screen buffer, send DAMAGE) and flipping (level 3:
 * draw a whole buffer, the screen is pointed at it) is behind these calls. */
#ifndef VIDEO_H
#define VIDEO_H

#include "scale.h"

/* The picture: twice the NES's 256 x 240 where the screen has room (the T48S),
 * otherwise as it is. video_size() decides, once the screen is known. */
extern int pic_scale;
#define PIC_W (256 * pic_scale)
#define PIC_H (240 * pic_scale)
#define PIC_MAX (512 * 480)
void video_size(int w, int h);

void video_enter(void);                       /* the game takes the screen */
struct scale_target *video_target(void);      /* where to draw the next picture */
void video_present(void);                     /* show it */
/* The UI takes the screen back; pic (PIC_W x PIC_H) gets the last picture shown. */
void video_leave(uint32_t *pic);
int video_flips(void);                        /* 1: flipping, so pictures keep time with the screen */

#endif
