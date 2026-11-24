// SPDX-License-Identifier: AGPL-3.0-or-later
/* snake: the classic. Arrows, 2 4 6 8, a swipe or a tap beside the head steer;
 * OK pauses; X goes back. It gets faster as the snake grows, and the best
 * score is kept in the app's data folder.
 *
 * How it draws: the whole picture is one function, scene(), that can paint any
 * rectangle of the screen. Each frame the few rectangles that changed are
 * painted into a back surface and only then copied to the screen (present()),
 * so nothing half-drawn is ever shown. A later full-screen, double-buffered
 * mode needs a different present() and nothing else.
 *
 * The game moves a cell at a time; the picture slides the head and the tail
 * between cells, so the snake glides. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "t48.h"

#define CELL 40                 /* pixels */
#define MAXC 32
#define MAXR 20
#define MAXN (MAXC * MAXR)
#define HEAD_H T48_UI_HEAD_H
#define EDGE 8                  /* the wall's thickness */
#define BODY_R 15
#define HEAD_R 18
#define FRAME_MS 33
#define STEP_SLOWEST 170        /* ms a cell at the start */
#define STEP_FASTEST 90
#define STEP_GAIN 2             /* ms quicker for each apple */
#define STEP_TITLE 150
#define SWIPE 28                /* pixels a finger moves before it is a swipe */
#define DYING_MS 820

enum { UP, RIGHT, DOWN, LEFT };
static const int DX[] = {0, 1, 0, -1}, DY[] = {-1, 0, 1, 0};

enum { TITLE, PLAY, PAUSED, DYING, OVER };

/* ---------- the game ---------- */

static int W, H;                        /* the screen */
static int cols, rows, FX, FY, FW, FH;  /* the field: cells, and where it is */
static int walls = 1;                   /* 0: the edges wrap round */

static int state = TITLE;
static short sx[MAXN], sy[MAXN];        /* a ring: the head is at hi, the tail n - 1 after it */
static unsigned char sd[MAXN];          /* the way the snake moved to get into that cell */
static int hi, n;
static unsigned char occ[MAXC][MAXR];
static int dir;
static int turns[2], nturns;            /* turns asked for and not yet taken */
static int tail_x = -1, tail_y;         /* the cell the tail is leaving; -1: it is staying (the snake grew) */
static int food_x = -1, food_y;
static int eat_x = -1, eat_y;           /* the apple the head is about to cover */
static long long food_at;               /* when the apple appeared: it pops up */
static int score, best, new_best, won;

static long long step_at;               /* when the last step was taken */
static int step_ms;                     /* how long a step takes now */
static int slide = CELL;                /* how far into the step the picture is, 0 to CELL */
static long long held_ms;               /* paused this far into a step */
static long long die_at, over_at;
static int bump, flash;                 /* the dying snake: a nudge at what it hit, then it blinks */

static uint32_t rng;
static int test_step;                   /* from the tests: a fixed step time */

#define SEG(i) ((hi + (i)) % MAXN)

static unsigned rnd(unsigned below)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng % below;
}

static const char *data_path(const char *name)
{
    static char p[300];
    const char *d = getenv("T48_APP_DATA");
    snprintf(p, sizeof p, "%s/%s", d && *d ? d : ".", name);
    return p;
}

/* the best scores: one with walls, one without */
static void best_file(int save)
{
    int v[2] = {0, 0};
    char k[16];
    int x;
    FILE *f = fopen(data_path("best"), "r");
    if (f) {
        while (fscanf(f, "%15s %d", k, &x) == 2)
            if (!strcmp(k, "walls")) v[1] = x; else if (!strcmp(k, "wrap")) v[0] = x;
        fclose(f);
    }
    if (!save) { best = v[walls]; return; }
    v[walls] = best;
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", data_path("best.new"));
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "walls %d\nwrap %d\n", v[1], v[0]);
    if (fclose(f) == 0) rename(tmp, data_path("best"));
}

/* The tests put a file named "test" in the data folder: "seed N" fixes where
 * the apples appear, "step MS" the speed. Without it the game is its usual self. */
static void seed(void)
{
    char k[16];
    int x;
    FILE *f = fopen(data_path("test"), "r");
    rng = (uint32_t)time(NULL) ^ (uint32_t)t48_now_ms() * 2654435761u;
    test_step = 0;
    if (f) {
        while (fscanf(f, "%15s %d", k, &x) == 2)
            if (!strcmp(k, "seed")) rng = (uint32_t)x; else if (!strcmp(k, "step")) test_step = x;
        fclose(f);
    }
    if (!rng) rng = 1;
}

static void place_food(void)
{
    int space = cols * rows - n;
    food_x = -1;
    if (space <= 0) return;
    int k = (int)rnd((unsigned)space);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            if (!occ[x][y] && k-- == 0) { food_x = x; food_y = y; food_at = t48_now_ms(); return; }
}

static void lay_snake(int x, int y, int len)
{
    memset(occ, 0, sizeof occ);
    hi = 0; n = len;
    for (int i = 0; i < len; i++) {
        sx[i] = (short)(x - i); sy[i] = (short)y; sd[i] = RIGHT;
        occ[x - i][y] = 1;
    }
    dir = RIGHT; nturns = 0;
    tail_x = x - len; tail_y = y;
    eat_x = -1;
    slide = 0;
    step_at = t48_now_ms();
}

static void new_title(void)
{
    state = TITLE;
    lay_snake(7, 0, 6);
    food_x = -1;
    step_ms = STEP_TITLE;
}

static void new_game(void)
{
    seed();
    state = PLAY;
    score = 0; new_best = 0; won = 0;
    lay_snake(4, rows / 2, 3);
    step_ms = test_step ? test_step : STEP_SLOWEST;
    place_food();
}

static void steer(int d)
{
    int last = nturns ? turns[nturns - 1] : dir;
    if (state != PLAY || d == last || d == (last + 2) % 4 || nturns == 2) return;
    turns[nturns++] = d;
}

static void end_game(int now_state, long long now)
{
    state = now_state;
    die_at = over_at = now;
    if (score > best) { best = score; new_best = 1; best_file(1); }
}

/* one cell on. On the title screen the snake walks round the edge by itself. */
static void step(long long now)
{
    int hx = sx[hi], hy = sy[hi];
    if (state == TITLE) {
        dir = hy == 0 && hx < cols - 1 ? RIGHT : hx == cols - 1 && hy < rows - 1 ? DOWN : hy == rows - 1 && hx > 0 ? LEFT : UP;
    } else if (nturns) {
        dir = turns[0];
        turns[0] = turns[1];
        nturns--;
    }
    int nx = hx + DX[dir], ny = hy + DY[dir];
    int t = SEG(n - 1);
    if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) {
        if (walls) { end_game(DYING, now); return; }
        nx = (nx + cols) % cols; ny = (ny + rows) % rows;
    }
    int grow = nx == food_x && ny == food_y;
    if (occ[nx][ny] && (grow || nx != sx[t] || ny != sy[t])) { end_game(DYING, now); return; }   /* the tail's cell is free: it moves on */
    eat_x = -1;
    if (grow) {
        n++;
        tail_x = -1;
        eat_x = nx; eat_y = ny;
        score++;
        if (!test_step && step_ms - STEP_GAIN >= STEP_FASTEST) step_ms -= STEP_GAIN;
    } else {
        tail_x = sx[t]; tail_y = sy[t];
        occ[tail_x][tail_y] = 0;
    }
    hi = (hi + MAXN - 1) % MAXN;
    sx[hi] = (short)nx; sy[hi] = (short)ny; sd[hi] = (unsigned char)dir;
    occ[nx][ny] = 1;
    if (grow) {
        place_food();
        if (food_x < 0) { won = 1; end_game(OVER, now); }   /* nowhere left to put one */
    }
}

/* ---------- drawing ---------- */

/* a window on a surface: the part of the screen from ox, oy that s holds */
typedef struct { t48_surface s; int ox, oy; } target;

static t48_surface back;        /* the finished picture; the screen is a copy of it */

static int window(const target *in, int x, int y, int w, int h, target *out)
{
    int x0 = x > in->ox ? x : in->ox, y0 = y > in->oy ? y : in->oy;
    int x1 = x + w < in->ox + in->s.w ? x + w : in->ox + in->s.w, y1 = y + h < in->oy + in->s.h ? y + h : in->oy + in->s.h;
    if (x1 <= x0 || y1 <= y0) return 0;
    out->s.px = in->s.px + (y0 - in->oy) * in->s.stride + (x0 - in->ox);
    out->s.w = x1 - x0; out->s.h = y1 - y0; out->s.stride = in->s.stride;
    out->ox = x0; out->oy = y0;
    return 1;
}

static int touches(const target *t, int x, int y, int w, int h)
{
    return x < t->ox + t->s.w && x + w > t->ox && y < t->oy + t->s.h && y + h > t->oy;
}

#define S(t) ((t48_surface *)&(t)->s)
static void box(const target *t, int x, int y, int w, int h, uint32_t c) { t48_fill(S(t), x - t->ox, y - t->oy, w, h, c); }
static void blob(const target *t, int x, int y, int w, int h, int r, uint32_t c)
{
    if (touches(t, x, y, w, h)) t48_round_fill(S(t), x - t->ox, y - t->oy, w, h, r, c);
}
static void disc(const target *t, int cx, int cy, int r, uint32_t c) { blob(t, cx - r, cy - r, 2 * r, 2 * r, r, c); }
static void words(const target *t, int cx, int y, const t48_font *f, uint32_t c, const char *str)
{
    t48_text_centre(S(t), cx - t->ox, y - t->oy, f, c, str);
}

/* a round-ended bar from one point to another on the same row or column */
static void bar(const target *t, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    int x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1;
    blob(t, x - r, y - r, abs(x1 - x0) + 2 * r, abs(y1 - y0) + 2 * r, r, c);
}

static int px(int cx) { return FX + cx * CELL + CELL / 2; }
static int py(int cy) { return FY + cy * CELL + CELL / 2; }

/* does the snake leave one side and come back on the other between these two cells? */
static int wrapped(int a, int b) { return sx[b] != sx[a] + DX[sd[b]] || sy[b] != sy[a] + DY[sd[b]]; }

static void head(const target *t, int x, int y, int d, uint32_t c)
{
    int fx = DX[d], fy = DY[d];
    if (!touches(t, x - HEAD_R, y - HEAD_R, 2 * HEAD_R, 2 * HEAD_R)) return;
    disc(t, x, y, HEAD_R, c);
    for (int side = -1; side <= 1; side += 2) {
        int ex = x + fx * 5 - fy * 8 * side, ey = y + fy * 5 + fx * 8 * side;
        disc(t, ex, ey, 5, 0xFFFFFF);
        disc(t, ex + fx * 2, ey + fy * 2, 2, 0x101010);
    }
}

static void apple(const target *t, int cx, int cy, int size)   /* size: 0 to 100 and a little over */
{
    const t48_theme *th = t48_theme_now();
    int x = px(cx), y = py(cy) + 2, r = 12 * size / 100;
    if (r < 1) return;
    disc(t, x, y, r, th->bad);
    if (size < 90) return;
    blob(t, x + 1, y - r - 5, 9, 6, 3, th->good);
    disc(t, x - 5, y - 5, 3, t48_mix(th->bad, 0xFFFFFF, 110));
}

static void snake(const target *t)
{
    const t48_theme *th = t48_theme_now();
    uint32_t c = flash ? th->bad : th->good;
    target f;
    if (!window(t, FX, FY, FW, FH, &f)) return;    /* a snake going round the edge is cut off at it */

    for (int i = 1; i < n - 1; i++) {              /* the body, between cells that are not moving */
        int a = SEG(i + 1), b = SEG(i), d = sd[b];
        bar(&f, px(sx[a]), py(sy[a]), px(sx[a]) + DX[d] * CELL, py(sy[a]) + DY[d] * CELL, BODY_R, c);
        if (wrapped(a, b)) bar(&f, px(sx[b]) - DX[d] * CELL, py(sy[b]) - DY[d] * CELL, px(sx[b]), py(sy[b]), BODY_R, c);
    }
    if (tail_x >= 0 && slide < CELL) {             /* the tail, sliding out of the cell it has left */
        int b = SEG(n - 1), d = sd[b], back_by = CELL - slide;
        bar(&f, px(tail_x) + DX[d] * slide, py(tail_y) + DY[d] * slide, px(tail_x) + DX[d] * CELL, py(tail_y) + DY[d] * CELL, BODY_R, c);
        if (sx[b] != tail_x + DX[d] || sy[b] != tail_y + DY[d])
            bar(&f, px(sx[b]) - DX[d] * back_by, py(sy[b]) - DY[d] * back_by, px(sx[b]), py(sy[b]), BODY_R, c);
    }
    {                                               /* the head, sliding into its cell */
        int a = SEG(1), b = SEG(0), d = sd[b], on = slide + bump, back_by = CELL - on;
        bar(&f, px(sx[a]), py(sy[a]), px(sx[a]) + DX[d] * on, py(sy[a]) + DY[d] * on, BODY_R, c);
        head(&f, px(sx[a]) + DX[d] * on, py(sy[a]) + DY[d] * on, d, c);
        if (wrapped(a, b)) {
            bar(&f, px(sx[b]) - DX[d] * CELL, py(sy[b]) - DY[d] * CELL, px(sx[b]) - DX[d] * back_by, py(sy[b]) - DY[d] * back_by, BODY_R, c);
            head(&f, px(sx[b]) - DX[d] * back_by, py(sy[b]) - DY[d] * back_by, d, c);
        }
    }
}

static void header(const target *t)
{
    const t48_theme *th = t48_theme_now();
    char num[16];
    int y = (HEAD_H - t48_font_head.height) / 2, ly = (HEAD_H - t48_font_normal.height) / 2, x = W - 24;
    box(t, 0, 0, W, HEAD_H, th->head);
    if (th->head_rule) box(t, 0, HEAD_H - 2, W, 2, th->line);
    t48_text(S(t), 24 - t->ox, y - t->oy, &t48_font_head, th->on_head, "Snake");
    snprintf(num, sizeof num, "%d", best);
    t48_text_right(S(t), x - t->ox, y - t->oy, &t48_font_head, th->on_head, num);
    x -= t48_text_width(&t48_font_head, num) + 10;
    t48_text_right(S(t), x - t->ox, ly - t->oy, &t48_font_normal, th->on_head_dim, "Best");
    if (state == TITLE) return;
    x -= t48_text_width(&t48_font_normal, "Best") + 36;
    snprintf(num, sizeof num, "%d", score);
    t48_text_right(S(t), x - t->ox, y - t->oy, &t48_font_head, th->on_head, num);
    x -= t48_text_width(&t48_font_head, num) + 10;
    t48_text_right(S(t), x - t->ox, ly - t->oy, &t48_font_normal, th->on_head_dim, "Score");
}

static void field(const target *t)
{
    const t48_theme *th = t48_theme_now();
    uint32_t a = t48_mix(th->bg, th->text, 12), b = t48_mix(th->bg, th->text, 26);
    target f;
    blob(t, FX - EDGE, FY - EDGE, FW + 2 * EDGE, FH + 2 * EDGE, EDGE, walls ? t48_mix(th->bg, th->text, 150) : b);
    if (!window(t, FX, FY, FW, FH, &f)) return;
    int x0 = (f.ox - FX) / CELL, x1 = (f.ox + f.s.w - 1 - FX) / CELL, y0 = (f.oy - FY) / CELL, y1 = (f.oy + f.s.h - 1 - FY) / CELL;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            box(&f, FX + x * CELL, FY + y * CELL, CELL, CELL, (x + y) & 1 ? a : b);
}

/* the box in the middle, and the one button in it */
#define BOX_W 400
#define BTN_W 200
#define BTN_H 60
static int box_h(void) { return state == TITLE ? 276 : state == PAUSED ? 170 : 250; }
static int box_y(void) { return FY + (FH - box_h()) / 2; }
static int btn_y(void) { return box_y() + (state == TITLE ? 120 : state == PAUSED ? 84 : 164); }

static void overlay(const target *t)
{
    t48_ui_ink ink;
    char line[48];
    int x = (W - BOX_W) / 2, y = box_y(), h = box_h(), cx = W / 2;
    if (state == PLAY || state == DYING) return;
    if (state == TITLE) {
        if (!touches(t, x - 4, y - 4, BOX_W + 8, h + 8)) return;
        t48_ui_popup(S(t), x - t->ox, y - t->oy, BOX_W, h, T48_UI_PLAIN);
    } else {
        t48_ui_dialog(S(t), x - t->ox, y - t->oy, BOX_W, h, new_best ? T48_UI_LIT : T48_UI_PLAIN);
    }
    t48_ui_dialog_ink(&ink);
    if (state == TITLE) {
        words(t, cx, y + 30, &t48_font_big, ink.text, "Snake");
        words(t, cx, y + 82, &t48_font_normal, ink.dim, walls ? "Mind the walls" : "No walls");
        words(t, cx, y + 204, &t48_font_small, ink.dim, "Arrows, 2 4 6 8 or a swipe to steer");
        words(t, cx, y + 228, &t48_font_small, ink.dim, "OK: pause     X: close");
    } else if (state == PAUSED) {
        words(t, cx, y + 30, &t48_font_head, ink.text, "Paused");
    } else {
        words(t, cx, y + 28, &t48_font_head, ink.text, won ? "Board full" : "Game over");
        snprintf(line, sizeof line, "%d", score);
        words(t, cx, y + 66, &t48_font_big, ink.text, line);
        if (new_best) snprintf(line, sizeof line, "New best");
        else snprintf(line, sizeof line, "Best %d", best);
        words(t, cx, y + 120, &t48_font_normal, ink.dim, line);
    }
    t48_ui_dialog_btn(S(t), cx - BTN_W / 2 - t->ox, btn_y() - t->oy, BTN_W, BTN_H, state == PAUSED ? "Resume" : "Play", T48_UI_FIRST);
}

/* everything, for the part of the screen t covers */
static void scene(const target *t)
{
    const t48_theme *th = t48_theme_now();
    box(t, 0, 0, W, H, th->bg);
    if (t->oy < HEAD_H) header(t);
    field(t);
    if (food_x >= 0) {
        long long age = t48_now_ms() - food_at;
        apple(t, food_x, food_y, age >= 220 ? 100 : age < 150 ? (int)(age * 115 / 150) : (int)(115 - (age - 150) * 15 / 70));
    }
    if (eat_x >= 0 && slide < CELL / 2) apple(t, eat_x, eat_y, 100);
    snake(t);
    overlay(t);
}

/* ---------- what changed, and getting it to the screen ---------- */

typedef struct { int x, y, w, h; } rect;
static rect dirt[24];
static int ndirt;

static void dirty(int x, int y, int w, int h)
{
    if (ndirt == (int)(sizeof dirt / sizeof dirt[0])) { dirt[0] = (rect){0, 0, W, H}; ndirt = 1; }
    if (ndirt == 1 && dirt[0].w == W && dirt[0].h == H) return;
    dirt[ndirt++] = (rect){x, y, w, h};
}

static void dirty_cell(int cx, int cy) { dirty(FX + cx * CELL, FY + cy * CELL, CELL, CELL); }

/* Copy a finished rectangle to the screen. With API level 2 that is a blit
 * into the shared buffer and a DAMAGE message. */
static void present(int x, int y, int w, int h)
{
    t48_blit(t48_screen(), x, y, back.px + y * back.stride + x, w, h, back.stride);
    t48_damage(x, y, w, h);
}

static void flush(void)
{
    target all = {back, 0, 0}, t;
    for (int i = 0; i < ndirt; i++) {
        if (!window(&all, dirt[i].x, dirt[i].y, dirt[i].w, dirt[i].h, &t)) continue;
        scene(&t);
        present(t.ox, t.oy, t.s.w, t.s.h);
    }
    ndirt = 0;
}

static void redraw(void) { ndirt = 0; dirty(0, 0, W, H); }

/* The moving ends of the snake: the head and the tail, and each a second time
 * when it is half way round an edge. What was there last frame and what is
 * there now are the parts of the picture to paint again. */
typedef struct { int on, x, y; } spot;
static spot was[4];

static void ends(spot *p)
{
    int a = SEG(1), b = SEG(0), d = sd[b], on = slide + bump;
    memset(p, 0, 4 * sizeof *p);
    p[0] = (spot){1, px(sx[a]) + DX[d] * on, py(sy[a]) + DY[d] * on};
    if (wrapped(a, b)) p[1] = (spot){1, px(sx[b]) - DX[d] * (CELL - on), py(sy[b]) - DY[d] * (CELL - on)};
    if (tail_x < 0) return;
    b = SEG(n - 1); d = sd[b];
    p[2] = (spot){1, px(tail_x) + DX[d] * slide, py(tail_y) + DY[d] * slide};
    if (sx[b] != tail_x + DX[d] || sy[b] != tail_y + DY[d])
        p[3] = (spot){1, px(sx[b]) - DX[d] * (CELL - slide), py(sy[b]) - DY[d] * (CELL - slide)};
}

static void dirty_ends(void)
{
    spot now[4];
    int r = HEAD_R + 2;
    ends(now);
    for (int i = 0; i < 4; i++) {
        spot *o = &was[i], *c = &now[i];
        if (o->on && c->on && abs(o->x - c->x) + abs(o->y - c->y) <= 2 * CELL) {
            int x = o->x < c->x ? o->x : c->x, y = o->y < c->y ? o->y : c->y;
            dirty(x - r, y - r, abs(o->x - c->x) + 2 * r, abs(o->y - c->y) + 2 * r);
        } else {
            if (o->on) dirty(o->x - r, o->y - r, 2 * r, 2 * r);
            if (c->on) dirty(c->x - r, c->y - r, 2 * r, 2 * r);
        }
    }
    memcpy(was, now, sizeof was);
}

/* one frame of the title's snake or of the game */
static void frame(void)
{
    long long now = t48_now_ms();

    if (state == DYING) {
        long long t = now - die_at;
        int b = t < 80 ? (int)(t / 8) : t < 160 ? (int)((160 - t) / 8) : 0;
        int f = t >= 160 && ((t - 160) / 130) % 2 == 0;
        if (t >= DYING_MS) { state = OVER; over_at = now; bump = flash = 0; t48_timer(0, 0, 0); redraw(); return; }
        if (f != flash) { flash = f; dirty(FX, FY, FW, FH); }
        if (b != bump) { bump = b; dirty_ends(); }
        return;
    }
    if (state != PLAY && state != TITLE) return;

    long long el = now - step_at;
    if (el >= step_ms) {
        int was_score = score, ate = eat_x, ate_y = eat_y, was_state = state;
        int ox = sx[hi], oy = sy[hi];
        if (tail_x >= 0) dirty_cell(tail_x, tail_y);
        step(now);
        step_at = el >= 2 * step_ms ? now : step_at + step_ms;   /* after a stall, carry on from here: no leaps */
        el = now - step_at;
        if (state != was_state) {            /* it hit something, or filled the board */
            slide = CELL;
            if (state == OVER) { t48_timer(0, 0, 0); redraw(); }
            else dirty(0, 0, W, HEAD_H);
            return;
        }
        dirty_cell(ox, oy);
        dirty_cell(sx[hi], sy[hi]);
        dirty_cell(sx[SEG(n - 1)], sy[SEG(n - 1)]);
        if (ate >= 0) dirty_cell(ate, ate_y);
        if (score != was_score) dirty(0, 0, W, HEAD_H);
    }
    int s = (int)(el * CELL / step_ms);
    if (s > CELL) s = CELL;
    if (eat_x >= 0 && slide < CELL / 2 && s >= CELL / 2) dirty_cell(eat_x, eat_y);   /* the head is on the apple: it goes */
    slide = s;
    dirty_ends();
    if (food_x >= 0 && now - food_at < 260) dirty_cell(food_x, food_y);
}

static void run(int on) { t48_timer(0, on ? FRAME_MS : 0, 1); }

static void play(void)
{
    new_game();
    memset(was, 0, sizeof was);
    run(1);
    redraw();
}

static void title(void)
{
    new_title();
    memset(was, 0, sizeof was);
    run(1);
    redraw();
}

static void pause_game(void)
{
    if (state != PLAY) return;
    held_ms = t48_now_ms() - step_at;
    state = PAUSED;
    run(0);
    redraw();
}

static void resume_game(void)
{
    state = PLAY;
    step_at = t48_now_ms() - held_ms;
    nturns = 0;
    run(1);
    redraw();
}

/* OK, or a touch on the button */
static void go(void)
{
    if (state == TITLE) play();
    else if (state == PAUSED) resume_game();
    else if (state == OVER && t48_now_ms() - over_at >= 400) play();   /* not on a press meant for the game */
    else if (state == PLAY) pause_game();
}

static int back_key(void)   /* 1: leave the app */
{
    if (state == TITLE) return 1;
    if (state == PLAY || state == PAUSED) end_game(OVER, 0);   /* the score still counts */
    bump = flash = 0;
    title();
    return 0;
}

static void key(int k)
{
    switch (k) {
    case T48_KEY_UP: case '2': steer(UP); break;
    case T48_KEY_DOWN: case '8': steer(DOWN); break;
    case T48_KEY_LEFT: case '4': steer(LEFT); break;
    case T48_KEY_RIGHT: case '6': steer(RIGHT); break;
    case T48_KEY_OK: go(); break;
    }
}

/* A swipe steers the way it goes. A tap steers towards the side of the head it
 * is on: above or below a snake going across, left or right of one going up or down. */
static void touch(int st, int x, int y)
{
    static int x0, y0, swiped, down;
    if (state != PLAY) {
        int bx = (W - BOX_W) / 2;
        down = 0;
        if (st != T48_TOUCH_DOWN) return;
        if (state == TITLE ? x >= bx && x < bx + BOX_W && y >= box_y() && y < box_y() + box_h()
                           : abs(x - W / 2) <= BTN_W / 2 + 10 && y >= btn_y() - 10 && y < btn_y() + BTN_H + 10) go();
        return;
    }
    if (st == T48_TOUCH_DOWN) {
        x0 = x; y0 = y; swiped = 0; down = 1;
    } else if (!down) {
        return;                                    /* the touch that pressed Play */
    } else if (st == T48_TOUCH_MOVE) {
        int dx = x - x0, dy = y - y0;
        if (abs(dx) < SWIPE && abs(dy) < SWIPE) return;
        steer(abs(dx) > abs(dy) ? (dx > 0 ? RIGHT : LEFT) : (dy > 0 ? DOWN : UP));
        x0 = x; y0 = y; swiped = 1;                /* and a second leg of the same swipe is a second turn */
    } else {
        int heading = nturns ? turns[nturns - 1] : dir;
        int dx = x - (px(sx[hi]) - DX[dir] * (CELL - slide)), dy = y - (py(sy[hi]) - DY[dir] * (CELL - slide));
        down = 0;
        if (swiped) return;
        if (heading == LEFT || heading == RIGHT) { if (abs(dy) >= CELL / 2) steer(dy < 0 ? UP : DOWN); }
        else if (abs(dx) >= CELL / 2) steer(dx < 0 ? LEFT : RIGHT);
    }
}

int main(void)
{
    t48_event ev;
    const char *v;

    if (t48_app_init() < 0) return 1;
    t48_surface *s = t48_screen();
    W = s->w; H = s->h;
    back.w = W; back.h = H; back.stride = W;
    if (!(back.px = malloc((size_t)W * H * 4))) return 1;
    cols = (W - 2 * EDGE - 8) / CELL; if (cols > MAXC) cols = MAXC;
    rows = (H - HEAD_H - 2 * EDGE - 8) / CELL; if (rows > MAXR) rows = MAXR;
    FW = cols * CELL; FH = rows * CELL;
    FX = (W - FW) / 2; FY = HEAD_H + (H - HEAD_H - FH) / 2;
    v = t48_setting("walls", "true");
    walls = !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1");
    best_file(0);
    title();
    flush();

    while (t48_poll(&ev, -1) >= 0) {
        switch (ev.type) {
        case T48_EV_TIMER:
            frame();
            break;
        case T48_EV_KEY:
            if (!ev.state) break;
            if (ev.key == T48_KEY_BACK) { if (back_key()) return 0; }
            else key(ev.key);
            break;
        case T48_EV_TOUCH:
            touch(ev.state, ev.x, ev.y);
            break;
        case T48_EV_PAUSE:          /* Home, or a call: the game waits */
            pause_game();
            run(0);
            flush();
            t48_paused();
            continue;
        case T48_EV_RESUME:
            if (state == TITLE) step_at = t48_now_ms();
            if (state == TITLE || state == DYING) run(1);
            redraw();
            break;
        case T48_EV_THEME:
            redraw();
            break;
        case T48_EV_QUIT:
            return 0;
        }
        flush();
    }
    return 0;
}
