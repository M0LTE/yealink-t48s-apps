// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the phone's lamps, in time with the tune (API level 5).
 *
 * Read from the score, like the cues the picture gets, so the lamps are right
 * with or without sound. The three key lamps are one above another, Headset,
 * Mute, Speaker:
 *
 *   kick        the bottom lamp flashes and dies away
 *   snare       the top one
 *   chords      the red one in the middle swells and fades with each
 *   bass        a low glow on the bottom lamp, where there is no kick
 *   a fill      (toms, or snares one straight after another) runs down the
 *               three, a lamp a hit
 *   a crash     all three full, fading, and the red lamp at the top of the
 *               case flashes
 *
 * Levels are worked out for a frame and sent when they have changed, forty
 * times a second at most: each costs the shell a write for every lamp, and a
 * flash that takes 200 ms to die looks no smoother for more.
 * An older shell, or no permission: nothing here does anything. */
#include "demo.h"
#include "music.h"

#if T48_API_LEVEL >= 5

#define ROW_US (MUSIC_ROW_TICKS * MUSIC_TICK_US)
#define BACK 8                  /* rows looked back over: the longest fade is over by then */
#define KEYS (1u << T48_LAMP_HEADSET | 1u << T48_LAMP_MUTE | 1u << T48_LAMP_SPEAKER)
#define APART_US 24000          /* between two sends, at least */

static int sent[T48_LAMPS] = {-1, -1, -1, -1};
static long long sent_at;

void lamps_reset(void)
{
    for (int i = 0; i < T48_LAMPS; i++) sent[i] = -1;
}

/* from peak at once down to nothing over ms */
static int fall(int age, int ms, int peak)
{
    return age < 0 || age >= ms ? 0 : peak - age * peak / ms;
}

/* up to peak over in ms, then down over out */
static int swell(int age, int in, int out, int peak)
{
    if (age < 0 || age >= in + out) return 0;
    return age < in ? age * peak / in : peak - (age - in) * peak / out;
}

static int hit(int ev) { return ev & (MUSIC_EV_SNARE | MUSIC_EV_TOM); }

void lamps_frame(long long us)
{
    int rows = music_rows();
    long long now = now_us();
    if (!t48_lamps_ok() || rows <= 0 || now - sent_at < APART_US) return;
    long long loop = (long long)rows * ROW_US;
    us = ((us % loop) + loop) % loop;
    int row = (int)(us / ROW_US);
    int lv[T48_LAMPS] = {0};

    for (int k = 0; k <= BACK; k++) {
        int r = ((row - k) % rows + rows) % rows;
        int age = (int)((us - (long long)row * ROW_US) / 1000) + k * (ROW_US / 1000);   /* ms since row r began */
        int ev = music_row_events(r), v;
        if (!ev) continue;
        /* a hit straight after another is part of a fill: count how far into it this one is */
        int run = 0;
        while (run < 8 && hit(music_row_events(((r - run - 1) % rows + rows) % rows))) run++;
        if (hit(ev) && (run || (ev & MUSIC_EV_TOM))) {
            int lamp = T48_LAMP_HEADSET + run % 3;
            if ((v = fall(age, 170, 255)) > lv[lamp]) lv[lamp] = v;
        } else if (ev & MUSIC_EV_SNARE) {
            if ((v = fall(age, 180, 230)) > lv[T48_LAMP_HEADSET]) lv[T48_LAMP_HEADSET] = v;
        }
        if (ev & MUSIC_EV_KICK) {
            if ((v = fall(age, 200, 255)) > lv[T48_LAMP_SPEAKER]) lv[T48_LAMP_SPEAKER] = v;
        } else if (ev & MUSIC_EV_BASS) {
            if ((v = fall(age, 240, 70)) > lv[T48_LAMP_SPEAKER]) lv[T48_LAMP_SPEAKER] = v;
        }
        if (ev & MUSIC_EV_CHORD) {
            if ((v = swell(age, 50, 650, 170)) > lv[T48_LAMP_MUTE]) lv[T48_LAMP_MUTE] = v;
        }
        if (ev & MUSIC_EV_CRASH) {
            v = fall(age, 700, 255);
            for (int i = T48_LAMP_HEADSET; i <= T48_LAMP_SPEAKER; i++)
                if (v > lv[i]) lv[i] = v;
            if (age < 160) lv[T48_LAMP_MESSAGE] = 255;
        }
    }

    uint8_t out[T48_LAMPS];
    unsigned mask = 0;
    for (int i = 0; i < T48_LAMPS; i++) {
        out[i] = (uint8_t)lv[i];
        if (lv[i] != sent[i]) mask |= i == T48_LAMP_MESSAGE ? 1u << i : KEYS;   /* the three key lamps go together */
    }
    if (!mask) return;
    sent_at = now;
    /* the red lamp is the phone's own between flashes: a missed call still shows */
    if ((mask & 1u << T48_LAMP_MESSAGE) && !lv[T48_LAMP_MESSAGE]) {
        t48_lamps_release(1u << T48_LAMP_MESSAGE);
        sent[T48_LAMP_MESSAGE] = 0;
        mask &= ~(1u << T48_LAMP_MESSAGE);
    }
    t48_lamps_set(mask, out);
    for (int i = 0; i < T48_LAMPS; i++)
        if (mask >> i & 1) sent[i] = lv[i];
}

#else   /* an SDK from before lamps */

void lamps_frame(long long us) { (void)us; }
void lamps_reset(void) {}

#endif
