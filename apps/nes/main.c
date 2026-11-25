// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * nes: plays NES games. A list of the ROMs it can find (its own folder, its
 * data folder, a USB stick), then the game at twice its size in the middle
 * of the screen. X opens a small menu: save and load one state, reset, back
 * to the list. Battery saves are kept beside the states in the data folder.
 *
 * The core is in nes.h; getting a picture on the screen is in video.h, and
 * the sound to the loudspeaker in sound.h.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "nes.h"
#include "sound.h"
#include "t48.h"
#include "video.h"

#define TH t48_theme_now()
#define FRAME_US (1000000000LL / NES_FPS_MILLI)
#define MAX_ROM_BYTES (4u << 20)

/* ---------- the ROMs ---------- */

enum { SRC_BUILT_IN, SRC_PHONE, SRC_USB };
static const char *const src_names[] = {"Built in", "Phone", "USB"};

struct rom {
    char path[300];
    char name[64];
    int src;
};
#define MAX_ROMS 200
static struct rom roms[MAX_ROMS];
static int nroms, sel, top;

/* what the bundled games are called */
static const char *const titles[][2] = {
    {"thwaite", "Thwaite"},
    {"croom", "Concentration Room"},
};

static const char *data_dir(void)
{
    const char *d = getenv("T48_APP_DATA");
    return d && *d ? d : ".";
}

/* "dir/Some Game.nes" to "Some Game" */
static void base_name(const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/'), *dot;
    b = b ? b + 1 : path;
    dot = strrchr(b, '.');
    snprintf(out, n, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
}

static void scan_dir(struct rom *list, int *n, const char *dir, int src)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && *n < MAX_ROMS) {
        size_t len = strlen(e->d_name);
        struct rom *r = &list[*n];
        if (len < 5 || strcasecmp(e->d_name + len - 4, ".nes") || e->d_name[0] == '.') continue;
        if (snprintf(r->path, sizeof r->path, "%s/%s", dir, e->d_name) >= (int)sizeof r->path) continue;
        base_name(r->path, r->name, sizeof r->name);
        for (size_t i = 0; i < sizeof titles / sizeof *titles; i++)
            if (src == SRC_BUILT_IN && !strcmp(r->name, titles[i][0])) snprintf(r->name, sizeof r->name, "%s", titles[i][1]);
        r->src = src;
        (*n)++;
    }
    closedir(d);
}

static int by_name(const void *a, const void *b)
{
    const struct rom *x = a, *y = b;
    if (x->src != y->src) return x->src - y->src;
    return strcasecmp(x->name, y->name);
}

/* a mount table writes a space in a name as \040, and so on */
static void unescape(char *p)
{
    char *o = p;
    for (; *p; p++, o++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *o = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else {
            *o = *p;
        }
    }
    *o = 0;
}

/* USB sticks: whatever is mounted from a disk, wherever the firmware put it.
 * NES_MOUNTS names another table to read, for tests. */
static void scan_usb(struct rom *list, int *n)
{
    const char *table = getenv("NES_MOUNTS");
    FILE *f = fopen(table ? table : "/proc/mounts", "r");
    char line[512], dev[128], dir[256], sub[280];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%127s %255s", dev, dir) != 2 || strncmp(dev, "/dev/sd", 7)) continue;
        unescape(dir);
        scan_dir(list, n, dir, SRC_USB);
        snprintf(sub, sizeof sub, "%s/nes", dir);
        scan_dir(list, n, sub, SRC_USB);
        snprintf(sub, sizeof sub, "%s/roms", dir);
        scan_dir(list, n, sub, SRC_USB);
    }
    fclose(f);
}

/* Look again. 1 if the list changed. The selection stays on its game. */
static int scan(void)
{
    static struct rom found[MAX_ROMS];
    char was[300] = "", sub[280];
    int n = 0, changed;

    scan_dir(found, &n, ".", SRC_BUILT_IN);
    scan_dir(found, &n, data_dir(), SRC_PHONE);
    snprintf(sub, sizeof sub, "%s/roms", data_dir());
    scan_dir(found, &n, sub, SRC_PHONE);
    scan_usb(found, &n);
    qsort(found, (size_t)n, sizeof *found, by_name);

    changed = n != nroms;
    for (int i = 0; i < n && !changed; i++) changed = strcmp(found[i].path, roms[i].path) != 0;
    if (!changed) return 0;
    if (sel < nroms) snprintf(was, sizeof was, "%s", roms[sel].path);
    memcpy(roms, found, sizeof roms);
    nroms = n;
    sel = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(roms[i].path, was)) sel = i;
    return 1;
}

/* ---------- files in the data folder ---------- */

static uint8_t *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size <= 0 || (size_t)size > max || !(buf = malloc((size_t)size))) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

/* all of it or none: written beside, then moved over */
static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[340];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return -1;
    ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;   /* on the flash before it takes the old file's name */
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------- the game ---------- */

enum { M_LIST, M_PLAY, M_MENU };
static int mode = M_LIST;
static t48_surface *s;
static nes_t *nes;
static char game[64];            /* the ROM's file name without ".nes": names its saves */
static unsigned pad;
static long long next_us;        /* when the next frame is due */
static int late, skipped;
static uint8_t battery_was[sizeof ((struct nes_state *)0)->prg_ram];
static long stat_frames, stat_skipped;
static long long stat_us;
static char notice[64];          /* under the list, or in the menu */
static int notice_bad;
static uint32_t pic[PIC_W * PIC_H];   /* the game's last picture, behind the menu */

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Game.1a2b3c4d.sav: the ROM's name and its checksum, so two different ROMs
 * of one name do not share a save */
static void save_path(char *out, size_t n, const char *ext)
{
    snprintf(out, n, "%s/%s.%08x.%s", data_dir(), game, (unsigned)nes->crc, ext);
}

/* the cartridge's battery RAM, when it has changed */
static void battery_save(void)
{
    char path[340];
    if (!nes->rom || !nes->battery || !memcmp(battery_was, nes->st.prg_ram, sizeof battery_was)) return;
    save_path(path, sizeof path, "sav");
    if (!write_file(path, nes->st.prg_ram, sizeof battery_was)) memcpy(battery_was, nes->st.prg_ram, sizeof battery_was);
}

static void battery_load(void)
{
    char path[340];
    size_t len;
    uint8_t *d;
    save_path(path, sizeof path, "sav");
    if (nes->battery && (d = read_file(path, sizeof battery_was, &len))) {
        memcpy(nes->st.prg_ram, d, len);
        free(d);
    }
    memcpy(battery_was, nes->st.prg_ram, sizeof battery_was);
}

static void state_save(void)
{
    char path[340];
    uint8_t *d = malloc(nes_state_size());
    save_path(path, sizeof path, "state");
    if (d) nes_state_save(nes, d);
    notice_bad = !d || write_file(path, d, nes_state_size()) < 0;
    snprintf(notice, sizeof notice, notice_bad ? "State not saved" : "State saved");
    free(d);
}

static int state_load(void)
{
    char path[340];
    size_t len;
    uint8_t *d;
    int r;
    save_path(path, sizeof path, "state");
    d = read_file(path, nes_state_size(), &len);
    r = d ? nes_state_load(nes, d, len) : -1;
    notice_bad = r < 0;
    snprintf(notice, sizeof notice, !d ? "No saved state" : r == -2 ? "State is for another game" : r < 0 ? "State is from another version" : "");
    free(d);
    return notice_bad ? -1 : 0;
}

static void play(void)
{
    mode = M_PLAY;
    pad = 0;
    late = skipped = 0;
    t48_timer(0, 0, 0);
    video_enter();
    next_us = now_us();
}

static const char *game_start(const struct rom *r)
{
    size_t len;
    uint8_t *d = read_file(r->path, MAX_ROM_BYTES, &len);
    const char *err;
    if (!d) return "Cannot read the file";
    if ((err = nes_load(nes, d, len))) {
        free(d);
        return err;
    }
    base_name(r->path, game, sizeof game);
    battery_load();
    stat_frames = stat_skipped = 0;
    stat_us = 0;
    play();
    return NULL;
}

static void game_frame(void)
{
    long long t0 = now_us();
    int draw = !(late && skipped < 3);    /* behind: leave pictures out, up to three in a row */

    if (t0 - next_us > 200000) next_us = t0;   /* far behind (the phone was busy): do not race to catch up */
    if (draw) nes_set_video(nes, scale_line_2x, video_target());
    nes_frame(nes, pad, draw);
    sound_frame(nes, (long)(now_us() - next_us));
    if (draw) {
        video_present();
        skipped = 0;
    } else {
        skipped++;
        stat_skipped++;
    }
    next_us += FRAME_US;
    stat_frames++;
    stat_us += now_us() - t0;
    late = now_us() > next_us;
}

static void game_stats(void)
{
    char snd[160];
    sound_stats(snd, sizeof snd);
    if (stat_frames)
        fprintf(stderr, "nes: %s: %ld frames, %ld without a picture, %.2f ms each%s\n", game, stat_frames, stat_skipped,
                (double)stat_us / 1000.0 / (double)stat_frames, snd);
    stat_frames = 0;
}

static unsigned pad_bit(int key)
{
    switch (key) {
    case T48_KEY_UP: return NES_UP;
    case T48_KEY_DOWN: return NES_DOWN;
    case T48_KEY_LEFT: return NES_LEFT;
    case T48_KEY_RIGHT: return NES_RIGHT;
    case T48_KEY_OK: case '2': return NES_A;
    case '1': return NES_B;
    case T48_KEY_STAR: return NES_SELECT;
    case T48_KEY_HASH: return NES_START;
    }
    return 0;
}

/* ---------- the list ---------- */

#define ROW_H 60
#define LIST_X 16
#define LIST_Y 54
#define VISIBLE 6
#define SCROLL_W 60
#define HINT "Arrows: D-pad    OK or 2: A    1: B    *: Select    #: Start    X: menu"

static int list_w(void)
{
    return s->w - 2 * LIST_X - (nroms > VISIBLE ? SCROLL_W : 0);
}

static void draw_list(void)
{
    int w = list_w(), hint_y = LIST_Y + VISIBLE * ROW_H + (s->h - LIST_Y - VISIBLE * ROW_H - t48_font_small.height) / 2 - 2;

    if (sel < top) top = sel;
    if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
    if (top > nroms - VISIBLE) top = nroms > VISIBLE ? nroms - VISIBLE : 0;

    t48_ui_clear(s, TH->bg);
    t48_ui_header(s, "NES", 1, 0);
    if (!nroms) t48_text_centre(s, s->w / 2, 200, &t48_font_normal, TH->dim, "No ROMs");
    for (int i = top; i < nroms && i < top + VISIBLE; i++) {
        int y = LIST_Y + (i - top) * ROW_H, h = ROW_H - 6, ty = y + (h - t48_font_normal.height) / 2;
        t48_ui_ink ink;
        t48_ui_row(s, LIST_X, y, w, h, i == sel, &ink);
        t48_text_fit(s, LIST_X + 18, ty, w - 150, &t48_font_bold, ink.text, roms[i].name);
        t48_text_right(s, LIST_X + w - 18, ty, &t48_font_normal, ink.dim, src_names[roms[i].src]);
    }
    if (nroms > VISIBLE) {
        int x = s->w - LIST_X - SCROLL_W + 8, hh = VISIBLE * ROW_H / 2 - 6;
        t48_ui_btn(s, x, LIST_Y, SCROLL_W - 8, hh, "^", top > 0 ? T48_UI_PLAIN : T48_UI_OFF);
        t48_ui_btn(s, x, LIST_Y + hh + 6, SCROLL_W - 8, hh, "v", top + VISIBLE < nroms ? T48_UI_PLAIN : T48_UI_OFF);
    }
    if (notice[0]) t48_ui_notice(s, LIST_X, hint_y - 8, notice, notice_bad ? T48_UI_STOP : T48_UI_PLAIN);
    else t48_text_centre(s, s->w / 2, hint_y, &t48_font_small, TH->dim, HINT);
    t48_damage_all();
}

static void show_list(void)
{
    mode = M_LIST;
    scan();
    t48_timer(0, 2000, 1);     /* a stick may be pushed in: look again now and then */
    draw_list();
}

static void list_start(void)
{
    const char *err;
    if (sel < 0 || sel >= nroms) return;
    notice[0] = 0;
    if ((err = game_start(&roms[sel]))) {
        snprintf(notice, sizeof notice, "%s", err);
        notice_bad = 1;
        draw_list();
    }
}

/* ---------- the menu over a stopped game ---------- */

enum { MENU_RESUME, MENU_SAVE, MENU_LOAD, MENU_RESET, MENU_LIST, MENU_ROWS };
static const char *const menu_names[MENU_ROWS] = {"Resume", "Save state", "Load state", "Reset", "Games"};
static int menu_sel;
#define MENU_W 320
#define MENU_BTN_H 50
#define MENU_PAD 16
#define MENU_H (MENU_PAD * 2 + MENU_ROWS * (MENU_BTN_H + 8) + 22)

static void menu_place(int *x, int *y)
{
    *x = (s->w - MENU_W) / 2;
    *y = (s->h - MENU_H) / 2;
}

static void draw_menu(void)
{
    int x, y;
    t48_ui_ink ink;
    menu_place(&x, &y);
    t48_fill(s, 0, 0, s->w, s->h, 0);
    t48_blit(s, (s->w - PIC_W) / 2, (s->h - PIC_H) / 2, pic, PIC_W, PIC_H, PIC_W);
    t48_ui_dialog(s, x, y, MENU_W, MENU_H, T48_UI_PLAIN);
    t48_ui_dialog_ink(&ink);
    for (int i = 0; i < MENU_ROWS; i++)
        t48_ui_dialog_btn(s, x + MENU_PAD, y + MENU_PAD + i * (MENU_BTN_H + 8), MENU_W - 2 * MENU_PAD, MENU_BTN_H, menu_names[i],
                          i == menu_sel ? T48_UI_FIRST : T48_UI_PLAIN);
    t48_text_centre(s, s->w / 2, y + MENU_H - MENU_PAD - 20, &t48_font_small, notice_bad ? TH->bad : ink.dim, notice);
    t48_damage_all();
}

static void show_menu(void)
{
    if (mode == M_PLAY) video_leave(pic);
    mode = M_MENU;
    menu_sel = MENU_RESUME;
    notice[0] = 0;
    battery_save();
    draw_menu();
}

static void game_stop(void)
{
    battery_save();
    game_stats();
    notice[0] = 0;
    show_list();
}

static void menu_do(int row)
{
    switch (row) {
    case MENU_RESUME: play(); return;
    case MENU_SAVE: state_save(); break;
    case MENU_LOAD:
        if (!state_load()) { play(); return; }
        break;
    case MENU_RESET:
        nes_reset(nes, 0);
        play();
        return;
    case MENU_LIST: game_stop(); return;
    }
    draw_menu();
}

/* ---------- keys and touches ---------- */

static int on_key(int key, int down)
{
    if (mode == M_PLAY) {
        if (key == T48_KEY_BACK) {
            if (down) show_menu();
        } else if (down) {
            pad |= pad_bit(key);
        } else {
            pad &= ~pad_bit(key);
        }
        return 0;
    }
    if (!down) return 0;
    if (mode == M_MENU) {
        if (key == T48_KEY_BACK) play();
        else if (key == T48_KEY_OK) menu_do(menu_sel);
        else if (key == T48_KEY_UP || key == T48_KEY_DOWN) {
            menu_sel = (menu_sel + (key == T48_KEY_UP ? MENU_ROWS - 1 : 1)) % MENU_ROWS;
            notice[0] = 0;
            draw_menu();
        }
        return 0;
    }
    if (key == T48_KEY_BACK) return 1;
    if (key == T48_KEY_OK) {
        list_start();
    } else if ((key == T48_KEY_UP && sel > 0) || (key == T48_KEY_DOWN && sel + 1 < nroms)) {
        sel += key == T48_KEY_UP ? -1 : 1;
        notice[0] = 0;
        draw_list();
    }
    return 0;
}

static int on_touch(int x, int y)
{
    if (mode == M_PLAY) {
        show_menu();
    } else if (mode == M_MENU) {
        int mx, my, row;
        menu_place(&mx, &my);
        row = (y - my - MENU_PAD) / (MENU_BTN_H + 8);
        if (x < mx || x >= mx + MENU_W || y < my || y >= my + MENU_H) play();
        else if (y >= my + MENU_PAD && row < MENU_ROWS) menu_do(menu_sel = row);
    } else if (y < T48_UI_HEAD_H) {
        if (x < 130) return 1;
    } else if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
        if (nroms <= VISIBLE && x >= LIST_X + list_w()) {
            /* the margin: no scroll buttons there */
        } else if (x >= LIST_X + list_w()) {
            int step = y < LIST_Y + VISIBLE * ROW_H / 2 ? -(VISIBLE - 1) : VISIBLE - 1;
            sel += step;
            if (sel >= nroms) sel = nroms - 1;
            if (sel < 0) sel = 0;
            draw_list();
        } else if (top + (y - LIST_Y) / ROW_H < nroms) {
            sel = top + (y - LIST_Y) / ROW_H;
            list_start();
        }
    }
    return 0;
}

int main(void)
{
    t48_event ev;

    if (t48_app_init() < 0) return 1;
    s = t48_screen();
    if (!(nes = calloc(1, sizeof *nes))) return 1;
    sound_init(nes);
    show_list();

    for (;;) {
        int wait = -1, r;
        if (mode == M_PLAY) {
            long long left = next_us - now_us();
            wait = left <= 0 ? 0 : (int)((left + 999) / 1000);
        }
        r = t48_poll(&ev, wait);
        if (r < 0) break;
        if (!r) {
            if (mode == M_PLAY) game_frame();
            continue;
        }
        switch (ev.type) {
        case T48_EV_KEY:
            if (on_key(ev.key, ev.state)) goto out;
            break;
        case T48_EV_TOUCH:
            if (ev.state == T48_TOUCH_DOWN && on_touch(ev.x, ev.y)) goto out;
            break;
        case T48_EV_TIMER:
            if (mode == M_LIST && scan()) draw_list();
            break;
        case T48_EV_PAUSE:
            /* off the screen: a game stops where it is, and waits behind its menu */
            if (mode == M_PLAY) show_menu();
            battery_save();
            t48_paused();
            break;
        case T48_EV_RESUME:
        case T48_EV_THEME:
            if (mode == M_LIST) draw_list();
            else if (mode == M_MENU) draw_menu();
            break;
        case T48_EV_QUIT:
            goto out;
        }
    }
out:
    battery_save();
    game_stats();
    return 0;
}
