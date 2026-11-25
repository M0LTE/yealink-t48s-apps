// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the frontend. Everything else draws into the surface fe_begin hands
 * out and says what it changed; how that reaches the glass is decided here.
 *
 * API level 3 (flipping): the shell lends two or more pictures of the screen;
 * fe_begin hands out the one to draw, fe_end has it shown whole at the next
 * refresh and waits for that. No tearing, no copy, and the frame rate is the
 * screen's or a whole fraction of it. Each buffer holds what was drawn in it
 * some frames ago, which is why parts are told which buffer they have.
 *
 * API level 2 (an older shell, or libt48): one shared buffer, and a DAMAGE
 * message that has the shell copy the changed box to the screen. Not locked
 * to the refresh, so it can tear, and the copy costs as much as the drawing. */
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/sockios.h>
#include "demo.h"

static t48_surface *scr;
static rect dmg;
static long long slot;           /* when the next frame is due, us */
static int period_us = 1000000 / 60;

#if T48_API_LEVEL >= 3
static t48_surface *flip;        /* the flip buffer being drawn, or NULL: this shell cannot flip */
static const uint32_t *known[FE_MAXBUF];
static unsigned last_frame;
static int have_frame, refreshes;   /* refreshes: how many the screen has made since fe_step_us last asked */
static int divisor = 1;          /* each frame stays up for this many refreshes */
static int window, late, grace;  /* frames since the pace was last judged, how many overran, and how many more to let off */
static long long tick_us;        /* a moment the screen began a refresh: with period_us, when every other one does */
static long long sent_us;        /* when the last flip was asked for */
#else
#define flip 0
#endif

long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int fe_init(void)
{
    if (t48_app_init() < 0) return -1;
    scr = t48_screen();
    if (scr->w > MAXW || scr->h > MAXH || scr->w < 640 || scr->h < 384) return -1;   /* the parts are laid out for 800 x 480 */
#if T48_API_LEVEL >= 3
    flip = t48_flip_start();
    if (flip && t48_flip_period_us() > 0) period_us = t48_flip_period_us();
#endif
    slot = now_us();
    return 0;
}

int fe_period_us(void) { return period_us; }
int fe_vsync(void) { return flip != 0; }

t48_surface *fe_begin(int *buf)
{
    dmg.x0 = dmg.y0 = 1 << 30;
    dmg.x1 = dmg.y1 = 0;
    *buf = 0;
#if T48_API_LEVEL >= 3
    if (flip) {
        int i = 0;
        while (i < FE_MAXBUF && known[i] && known[i] != flip->px) i++;
        if (i == FE_MAXBUF) *buf = -1;             /* more buffers than we track: nothing is known about this one */
        else { known[i] = flip->px; *buf = i; }
        return flip;
    }
#endif
    return scr;
}

void fe_touch(int x0, int y0, int x1, int y1)
{
    if (x0 < dmg.x0) dmg.x0 = x0;
    if (y0 < dmg.y0) dmg.y0 = y0;
    if (x1 > dmg.x1) dmg.x1 = x1;
    if (y1 > dmg.y1) dmg.y1 = y1;
}

void fe_touch_all(void) { fe_touch(0, 0, scr->w, scr->h); }

void fe_end(void)
{
#if T48_API_LEVEL >= 3
    if (flip) {
        if (divisor > 1 && tick_us && sent_us) {
            /* The last frame went up at the first refresh after it was asked for.
             * This one is for divisor refreshes later, so it must not be asked
             * for until the refresh before that has passed: a frame that
             * happens to be quick is then not shown early. */
            t48_flip_wait();
            long long up = tick_us + ((sent_us - tick_us + period_us - 1) / period_us) * period_us;
            long long due = up + (long long)(divisor - 1) * period_us + 400, now = now_us();
            if (due > now && due - now < 4 * period_us) usleep((useconds_t)(due - now));
        }
        sent_us = now_us();
        flip = t48_flip();                         /* waits for the screen: this is the frame pacing */
        return;
    }
#endif
    /* no more than one frame ahead of the shell: wait until it has taken the last DAMAGE */
    int q, tries = 200;
    while (tries-- > 0 && ioctl(t48_fd(), SIOCOUTQ, &q) == 0 && q > 0) usleep(500);
    if (dmg.x1 > dmg.x0 && dmg.y1 > dmg.y0) t48_damage(dmg.x0, dmg.y0, dmg.x1 - dmg.x0, dmg.y1 - dmg.y0);
    long long now = now_us();
    slot += period_us;
    if (slot < now) slot = now;
}

int fe_wait_ms(void)
{
    if (flip) return 0;
    long long d = slot - now_us();
    return d <= 0 ? 0 : (int)((d + 999) / 1000);
}

void fe_event(const t48_event *ev)
{
#if T48_API_LEVEL >= 3
    if (ev->type == T48_EV_FLIPPED) {
        if (have_frame) refreshes += (int)(ev->frame - last_frame);
        last_frame = ev->frame;
        have_frame = 1;
        if (ev->time_us > tick_us) tick_us = ev->time_us;
    } else if (ev->type == T48_EV_RESUME) {
        have_frame = 0;
        refreshes = 0;
        sent_us = 0;
    }
#else
    (void)ev;
#endif
}

/* How far the music's clock moves for this frame, given the time since the
 * last one. Flipping: whole refreshes, counted by the screen itself, so
 * motion is exact. Otherwise: frames that arrive about on time count as
 * exactly on time, and a late one as what it was. */
int fe_step_us(int dt)
{
    if (dt > 250000 || dt < 0) {                    /* a stall: do not jump */
#if T48_API_LEVEL >= 3
        refreshes = 0;
#endif
        return period_us;
    }
#if T48_API_LEVEL >= 3
    if (flip) {
        int n = refreshes;
        refreshes = 0;
        n = n < 1 ? 1 : n > 8 ? 8 : n;
        /* a part that keeps overrunning its pace is steadier one step slower */
        if (grace > 0) grace--;                    /* a part's first frames repaint whole buffers */
        else if (n > divisor) late++;
        if (++window >= 64) {
            if (late >= 5 && divisor < 4) divisor++;
            window = late = 0;
        }
        return n * period_us;
    }
#endif
    int n = (dt + period_us / 2) / period_us;
    if (n >= 1 && n <= 4 && abs(dt - n * period_us) < period_us / 4) return n * period_us;
    return dt;
}

int fe_ahead_us(void)
{
#if T48_API_LEVEL >= 3
    if (flip) return divisor * period_us;       /* drawn in one slot, on the glass at the start of the next */
#endif
    return period_us;
}

/* A part's pace: each frame shown for div refreshes of the screen. A part that
 * cannot keep it is moved to the next slower one, and stays there until the
 * next part asks: an even 34 frames a second looks better than an uneven 50. */
void fe_pace(int div)
{
#if T48_API_LEVEL >= 3
    divisor = div < 1 ? 1 : div;
    window = late = 0;
    grace = 8;
#else
    (void)div;
#endif
}

int fe_pace_now(void)
{
#if T48_API_LEVEL >= 3
    return divisor;
#else
    return 1;
#endif
}
