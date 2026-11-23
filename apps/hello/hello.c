// SPDX-License-Identifier: AGPL-3.0-or-later
/* hello: the smallest useful app, and the one to copy when starting a new one.
 * It says hello to whoever the "name" setting names, moves a box with the
 * arrow keys or a touch, and leaves on a short press of X. Its colours are
 * the theme's (Settings, Screen, Theme), so it looks like the rest of the
 * phone; the fonts follow the theme by themselves. */
#include <stdio.h>
#include "t48.h"

#define BOX 60

static t48_surface *s;
static int bx = 370, by = 250;  /* the box's top left corner */
static char status[64] = "Arrows or a touch move the box";

static void draw(void)
{
    char line[96];
    const t48_theme *th = t48_theme_now();  /* colours by what they are for: ground, ink, quiet ink, lit */
    t48_fill(s, 0, 0, s->w, s->h, th->bg);
    snprintf(line, sizeof line, "Hello, %s", t48_setting("name", "world"));
    t48_text_centre(s, s->w / 2, 60, &t48_font_big, th->text, line);
    t48_text_centre(s, s->w / 2, 130, &t48_font_normal, th->dim, status);
    t48_round_fill(s, bx, by, BOX, BOX, th->radius, th->accent);
    t48_text_centre(s, s->w / 2, 440, &t48_font_small, th->dim, "X: close     hold X: home");
    t48_damage_all();  /* simplest: say the whole screen changed */
}

static void move_box(int x, int y)
{
    bx = x < 0 ? 0 : x > s->w - BOX ? s->w - BOX : x;
    by = y < 170 ? 170 : y > 420 - BOX ? 420 - BOX : y;
    snprintf(status, sizeof status, "Box at %d, %d", bx, by);
}

int main(void)
{
    t48_event ev;

    if (t48_app_init() < 0) return 1;  /* not started by the shell or simulator */
    s = t48_screen();
    draw();
    while (t48_poll(&ev, -1) >= 0) {
        switch (ev.type) {
        case T48_EV_KEY:
            if (!ev.state) break;           /* act on presses only */
            if (ev.key == T48_KEY_BACK) return 0;
            if (ev.key == T48_KEY_LEFT) move_box(bx - 40, by);
            else if (ev.key == T48_KEY_RIGHT) move_box(bx + 40, by);
            else if (ev.key == T48_KEY_UP) move_box(bx, by - 40);
            else if (ev.key == T48_KEY_DOWN) move_box(bx, by + 40);
            else snprintf(status, sizeof status, "Key %c", ev.key < 128 ? ev.key : '?');
            draw();
            break;
        case T48_EV_TOUCH:
            if (ev.state == T48_TOUCH_UP) break;
            move_box(ev.x - BOX / 2, ev.y - BOX / 2);
            draw();
            break;
        case T48_EV_PAUSE:
            t48_paused();                    /* nothing to tidy up: freeze now */
            break;
        case T48_EV_RESUME:
        case T48_EV_THEME:                   /* the owner changed the look: draw in the new one */
            draw();
            break;
        case T48_EV_QUIT:
            return 0;
        }
    }
    return 0;
}
