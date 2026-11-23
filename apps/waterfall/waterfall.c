// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * waterfall: live receive waterfall of a pdn-soundmodem station, as an app.
 * Moved from the firmware repo's native/waterfall.c (M0LTE/yealink-t48s#13).
 *
 * Settings: "url", the station page's WebSocket (ws://host:port/, GB7RDG's
 * unless changed), and "name", shown in the header (empty: the page's title,
 * else the host).
 *
 * Data: the station page WebSocket. Listen only: after the upgrade we send
 * nothing but what WebSocket itself requires (libt48 answers pings and
 * closes) and {"type":"pong"}, the answer to the page's own keep-alive
 * {"type":"ping"} (a viewer that stays silent for 60 s is dropped). The page
 * protocol also has commands that key the transmitter; none is ever sent.
 * Binary messages are spectrum lines (0x01 heard, 0x03 our own transmission:
 * type, line index u32 LE, then lineLength bytes); text messages are JSON and
 * only "config", "frame", "history" and "tx" are read.
 *
 * Drawing: spectrum lines are folded to 600 columns, a few lines per row, and
 * each finished row is coloured once (a 256-entry table) into a ring of rows.
 * A screen update copies the ring into the screen buffer, draws the frame
 * tags on top and tells the shell which part changed. Header and the recent
 * frames panel are redrawn only when they change.
 *
 * Paused (Home, or a call): the connection is closed; it opens again when the
 * app is back in front. A tap on a recent frame shows its details.
 *
 * Protocol and design follow M0LTE/esp32-knob-pages (AGPL-3.0); wf_fold.c is
 * adapted from it.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "t48.h"
#include "wf_fold.h"

#define W 800
#define H 480

/* layout */
#define HDR_H 36
#define SCALE_Y 36
#define SPEC_TOP 58
#define SPEC_BOT 122
#define BAR_Y 125
#define WF_TOP 131
#define WF_VIS (H - WF_TOP)
#define PANEL_X 602
#define PANEL_W (W - PANEL_X)
#define PANEL_TOP (HDR_H + 28)
#define PANEL_ROW 48

#define C_BG T48_RGB(8, 10, 16)
#define C_HDR T48_RGB(18, 24, 36)
#define C_PANEL T48_RGB(14, 19, 29)
#define C_LINE T48_RGB(40, 52, 72)
#define C_GRID T48_RGB(24, 31, 44)
#define C_TEXT T48_RGB(226, 232, 240)
#define C_DIM T48_RGB(128, 142, 162)
#define C_FAINT T48_RGB(78, 90, 110)
#define C_AMBER T48_RGB(255, 176, 60)
#define C_GOOD T48_RGB(110, 214, 130)
#define C_BAD T48_RGB(240, 96, 86)
#define C_COOL T48_RGB(120, 196, 255)
#define C_TAG T48_RGB(240, 244, 250)

/* fonts */
#define F_TINY (&t48_font_tiny)
#define F_SMALL (&t48_font_small)
#define F_MONO (&t48_font_mono)
#define F_BOLD (&t48_font_bold)
#define F_BIG (&t48_font_head)

#define DEFAULT_URL "ws://10.45.0.37:8099/"   /* GB7RDG */
#define DEFAULT_NAME "GB7RDG"
#define MSG_MAX (256 * 1024)
#define FD_WS 1
#define TIMER_SECOND 0

static const unsigned band_rgb[] = {0x4CC2FF, 0xB68CFF, 0xE8C27A, 0x6FE3C1, 0xFF8FA3, 0x9AD0FF, 0xD7E36F, 0xFFB86B};

static t48_surface *s;
#define PX(x, y) (s->px[(y) * s->stride + (x)])

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d waterfall: ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* a dark see-through box: three quarters background, one quarter what is under it */
static void shade(int x, int y, int w, int h) { t48_fill_alpha(s, x, y, w, h, C_BG, 192); }

/* ---------- station state ---------- */

#define MAX_BANDS 8
#define NTAGS 64
#define NO_SNR (-100000)
#define NO_OFFSET (-1000000)

typedef struct {
    float lo_hz, hi_hz;
    int sub;
    char name[24];
} band_t;

typedef struct {
    uint32_t row;      /* first row of the burst */
    uint16_t rows;
    uint8_t placed;    /* row/rows are valid (live frames only) */
    uint8_t tx;
    int sub;
    int snr10;         /* SNR in tenths of a dB, NO_SNR if none */
    int len_bytes;     /* -1 if not given */
    int offset_hz;     /* NO_OFFSET if not given */
    char from[12], to[12], at[9];
    char mode[24];
    char plain[120];   /* the frame's text, when the page sends it */
} tag_t;

static struct {
    char name[32];          /* the setting, else the page's title, else the host */
    int name_from_setting;
    char url[160];
    char url_host[64];
    int url_port;
    int url_ok;
    /* config */
    int have_config;
    int line_len, lpr;
    float bin_hz, lines_per_s, lo_hz, hi_hz;
    double dial_hz;
    int lsb;
    char version[24];
    band_t bands[MAX_BANDS];
    int n_bands;
    /* connection */
    t48_ws *ws;
    int up;                 /* connected and upgraded */
    int attempts, backoff;
    long long retry_at;     /* when the next try starts, while down */
    long long last_line_ms, last_rx_ms;
    char why[64];           /* why the last try failed */
    int tx_keyed;
    int paused;
} st;

static wf_map_t map;
static wf_rows_t rows;
static uint8_t cols[WF_W];
static uint32_t ring[WF_HIST][WF_W];   /* coloured rows; row r lives at r % WF_HIST */
static uint32_t row_seq;               /* next row number */
static uint32_t row_first;             /* oldest row with data */
static int have_rows;
static int floor_q8, top_q8, have_levels;  /* auto levels, byte scale x 256 */
static uint32_t ramp_rx[256], ramp_tx[256], lut[256];
static int spec16[WF_W];               /* smoothed spectrum trace, byte scale x 16 */
static uint32_t spec_bg[WF_W];         /* spectrum background per column (band tint) */
static tag_t tags[NTAGS];
static uint32_t tag_seq;
static int dirty_wf, dirty_hdr, dirty_panel, dirty_static;
static int detail_open;                /* the details of one frame are showing */
static tag_t detail;

static int hz_x(float hz)
{
    if (st.hi_hz <= st.lo_hz) return 0;
    return (int)((hz - st.lo_hz) * WF_W / (st.hi_hz - st.lo_hz));
}

static const band_t *band_of(int sub)
{
    for (int i = 0; i < st.n_bands; i++)
        if (st.bands[i].sub == sub) return &st.bands[i];
    return NULL;
}

static uint32_t band_colour(int sub) { return band_rgb[(unsigned)sub % 8]; }

static void build_ramp(uint32_t *out, const uint32_t *stops, int n)
{
    for (int i = 0; i < 256; i++) {
        int x = i * (n - 1) * 256 / 255, a = x >> 8, f = x & 255;
        if (a >= n - 1) { a = n - 2; f = 256; }
        uint32_t c0 = stops[a], c1 = stops[a + 1];
        int r = (((c0 >> 16) & 255) * (256 - f) + ((c1 >> 16) & 255) * f) >> 8;
        int g = (((c0 >> 8) & 255) * (256 - f) + ((c1 >> 8) & 255) * f) >> 8;
        int b = ((c0 & 255) * (256 - f) + (c1 & 255) * f) >> 8;
        out[i] = T48_RGB(r, g, b);
    }
}

/* the colour table for the current levels */
static void build_lut(const uint32_t *ramp)
{
    int span = top_q8 - floor_q8;
    if (span < 20 * 256) span = 20 * 256;
    for (int b = 0; b < 256; b++) {
        int v = (b * 256 - floor_q8) * 255 / span;
        lut[b] = ramp[v < 0 ? 0 : v > 255 ? 255 : v];
    }
}

/* a finished row: levels, colour, into the ring */
static void on_row(void *ctx, uint32_t row, const uint8_t c[WF_W], bool tx)
{
    (void)ctx;
    int hist[256] = {0};
    for (int i = 0; i < WF_W; i++) hist[c[i]]++;
    int acc = 0, q = -1, mx = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (q < 0 && acc >= WF_W / 4) q = i;
        if (hist[i]) mx = i;
    }
    if (!tx) {
        /* floor a little under the quarter point, top over the peak; eased */
        int f = (q + 1) * 256, t = (mx + 6) * 256;
        if (t < f + 56 * 256) t = f + 56 * 256;
        if (!have_levels) { floor_q8 = f; top_q8 = t; have_levels = 1; }
        floor_q8 += (f - floor_q8) / 20;
        top_q8 += (t > top_q8 ? (t - top_q8) / 5 : (t - top_q8) / 100);
        build_lut(ramp_rx);
    } else {
        /* our own transmission: levels of its own, so it shows at any strength */
        int keep_f = floor_q8, keep_t = top_q8;
        floor_q8 = q * 256;
        top_q8 = mx * 256 > floor_q8 + 30 * 256 ? mx * 256 : floor_q8 + 30 * 256;
        build_lut(ramp_tx);
        floor_q8 = keep_f;
        top_q8 = keep_t;
    }
    uint32_t *px = ring[row % WF_HIST];
    for (int i = 0; i < WF_W; i++) px[i] = lut[c[i]];
    if (!have_rows) { row_first = row; have_rows = 1; }
    row_seq = row + 1;
    if (row_seq - row_first > WF_HIST) row_first = row_seq - WF_HIST;
    dirty_wf = 1;
}

/* a dim marker row where the feed was lost */
static void gap_row(void)
{
    if (!have_rows) return;
    uint32_t *px = ring[row_seq % WF_HIST];
    for (int i = 0; i < WF_W; i++) px[i] = (i / 4) % 2 ? C_LINE : C_BG;
    row_seq++;
    if (row_seq - row_first > WF_HIST) row_first = row_seq - WF_HIST;
    dirty_wf = 1;
}

/* ---------- messages ---------- */

static int jnum(const char *v, const char *e, double *out) { return t48_json_num(v, e, out); }
static int jstr(const char *v, const char *e, char *out, size_t n) { return t48_json_str(v, e, out, n); }
#define jget t48_json_get

static void on_config(const char *p, const char *e)
{
    double d;
    float sr = jnum(jget(p, e, "sampleRate"), e, &d) ? (float)d : 48000;
    st.bin_hz = jnum(jget(p, e, "binWidthHz"), e, &d) ? (float)d : 0;
    st.line_len = jnum(jget(p, e, "lineLength"), e, &d) ? (int)d : 0;
    st.lines_per_s = jnum(jget(p, e, "linesPerSecond"), e, &d) ? (float)d : 0;
    st.dial_hz = jnum(jget(p, e, "dialHz"), e, &d) ? d : 0;
    char sb[8] = "";
    jstr(jget(p, e, "sideband"), e, sb, sizeof sb);
    st.lsb = !strcmp(sb, "lsb");
    st.version[0] = 0;
    jstr(jget(jget(p, e, "daemonVersion"), e, "version"), e, st.version, sizeof st.version);
    char title[32];
    if (!st.name_from_setting && jstr(jget(p, e, "title"), e, title, sizeof title) && title[0])
        snprintf(st.name, sizeof st.name, "%s", title);

    float lo[MAX_BANDS], hi[MAX_BANDS];
    st.n_bands = 0;
    for (const char *m = t48_json_first(jget(p, e, "modems"), e); m && st.n_bands < MAX_BANDS; m = t48_json_next(m, e)) {
        double l, h, sub;
        if (!jnum(jget(m, e, "lowHz"), e, &l) || !jnum(jget(m, e, "highHz"), e, &h)) continue;
        band_t *b = &st.bands[st.n_bands];
        memset(b, 0, sizeof *b);
        b->lo_hz = lo[st.n_bands] = (float)l;
        b->hi_hz = hi[st.n_bands] = (float)h;
        b->sub = jnum(jget(m, e, "sub"), e, &sub) ? (int)sub : 0;
        if (!jstr(jget(m, e, "modeName"), e, b->name, sizeof b->name)) jstr(jget(m, e, "mode"), e, b->name, sizeof b->name);
        st.n_bands++;
    }
    wf_span(lo, hi, st.n_bands, sr, &st.lo_hz, &st.hi_hz);
    st.have_config = st.bin_hz > 0 && st.line_len > 0 && st.lines_per_s > 0;
    if (st.have_config) {
        wf_map_build(&map, st.lo_hz, st.hi_hz, st.bin_hz, st.line_len);
        /* about 8 rows a second whatever the line rate */
        st.lpr = (int)(st.lines_per_s / 8 + 0.5f);
        if (st.lpr < 1) st.lpr = 1;
        if (st.lpr > 16) st.lpr = 16;
    }
    wf_rows_reset(&rows, row_seq, st.lpr);
    for (int x = 0; x < WF_W; x++) spec_bg[x] = C_BG;
    for (int i = 0; i < st.n_bands; i++) {
        uint32_t tint = t48_mix(C_BG, band_colour(st.bands[i].sub), 22);
        for (int x = hz_x(st.bands[i].lo_hz); x < hz_x(st.bands[i].hi_hz); x++)
            if (x >= 0 && x < WF_W) spec_bg[x] = tint;
    }
    dirty_static = dirty_hdr = dirty_panel = dirty_wf = 1;
    logf_("config: %d bands, %.0f to %.0f Hz, %.0f lines/s, %d lines per row", st.n_bands, st.lo_hz, st.hi_hz,
          st.lines_per_s, st.lpr);
}

static void read_frame(const char *f, const char *e, tag_t *t)
{
    double d;
    memset(t, 0, sizeof *t);
    t->sub = jnum(jget(f, e, "sub"), e, &d) ? (int)d : -1;
    t->tx = t48_json_true(jget(f, e, "tx"), e);
    t->snr10 = jnum(jget(f, e, "snrDb"), e, &d) ? (int)(d * 10 + (d < 0 ? -0.5 : 0.5)) : NO_SNR;
    t->len_bytes = jnum(jget(f, e, "lenBytes"), e, &d) ? (int)d : -1;
    t->offset_hz = jnum(jget(f, e, "offsetHz"), e, &d) ? (int)(d + (d < 0 ? -0.5 : 0.5)) : NO_OFFSET;
    jstr(jget(f, e, "from"), e, t->from, sizeof t->from);
    jstr(jget(f, e, "to"), e, t->to, sizeof t->to);
    jstr(jget(f, e, "mode"), e, t->mode, sizeof t->mode);
    jstr(jget(f, e, "plain"), e, t->plain, sizeof t->plain);
    char at[40] = "";
    if (jstr(jget(f, e, "at"), e, at, sizeof at) && strlen(at) >= 19) memcpy(t->at, at + 11, 8);
    if (!t->at[0]) {
        time_t now = time(0);
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(t->at, sizeof t->at, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

static void on_frame(const char *p, const char *e)
{
    tag_t t;
    double d;
    read_frame(p, e, &t);
    if (jnum(jget(p, e, "line"), e, &d)) {
        double b;
        uint32_t burst = jnum(jget(p, e, "burstLines"), e, &b) && b > 0 ? (uint32_t)b : 0;
        t.placed = wf_tag_rows(&rows, (uint32_t)d, burst, &t.row, &t.rows);
    }
    tags[tag_seq % NTAGS] = t;
    tag_seq++;
    dirty_panel = dirty_wf = 1;
}

/* the station's recent log: fills the panel before anything new is heard */
static void on_history(const char *p, const char *e)
{
    if (tag_seq) return;  /* only on a fresh start */
    const char *fr = jget(p, e, "frames");
    int n = 0;
    for (const char *f = t48_json_first(fr, e); f; f = t48_json_next(f, e)) n++;
    int skip = n > NTAGS ? n - NTAGS : 0, i = 0;
    for (const char *f = t48_json_first(fr, e); f; f = t48_json_next(f, e), i++) {
        if (i < skip) continue;
        read_frame(f, e, &tags[tag_seq % NTAGS]);
        tag_seq++;
    }
    dirty_panel = 1;
}

static void on_msg(void *ctx, int type, unsigned char *p, size_t len)
{
    (void)ctx;
    st.last_rx_ms = t48_now_ms();
    if (type == T48_WS_BINARY) {
        if (st.have_config && len == (size_t)st.line_len + 5 && (p[0] == 0x01 || p[0] == 0x03)) {
            uint32_t line = p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24;
            wf_fold_line(&map, p + 5, cols);
            for (int i = 0; i < WF_W; i++) {  /* the trace: quick up, slow down */
                int v = cols[i] * 16, sp = spec16[i];
                spec16[i] = sp + (v > sp ? (v - sp) / 4 : (v - sp) / 16);
            }
            wf_rows_feed(&rows, line, cols, p[0] == 0x03, on_row, NULL);
            st.last_line_ms = t48_now_ms();
        }
        return;
    }
    const char *m = (const char *)p, *e = m + len;
    size_t head = len < 48 ? len : 48;
    if (memmem(m, head, "\"type\":\"ping\"", 13)) {
        static const char pong[] = "{\"type\":\"pong\"}";
        t48_ws_send_text(st.ws, pong, sizeof pong - 1);
    } else if (memmem(m, head, "\"type\":\"config\"", 15)) {
        on_config(m, e);
    } else if (memmem(m, head, "\"type\":\"frame\"", 14)) {
        on_frame(m, e);
    } else if (memmem(m, head, "\"type\":\"history\"", 16)) {
        on_history(m, e);
    } else if (memmem(m, head, "\"type\":\"tx\"", 11)) {
        int k = t48_json_true(jget(m, e, "keyed"), e);
        if (k != st.tx_keyed) { st.tx_keyed = k; dirty_hdr = 1; }
    }
}

/* ---------- drawing the parts ---------- */

static void draw_header(void)
{
    char b[96];
    t48_fill(s, 0, 0, W, HDR_H, C_HDR);
    t48_fill(s, 0, HDR_H - 1, W, 1, C_LINE);
    int x = t48_text(s, 12, 7, F_BOLD, C_TEXT, st.name);
    if (st.dial_hz > 0) {
        snprintf(b, sizeof b, "%.3f kHz %s", st.dial_hz / 1000.0, st.lsb ? "LSB" : "USB");
        x = t48_text(s, x + 16, 10, F_MONO, C_AMBER, b);
    }
    if (st.version[0]) {
        snprintf(b, sizeof b, "pdn-soundmodem %s", st.version);
        t48_text(s, x + 16, 10, F_SMALL, C_FAINT, b);
    }
    /* right: TX pill, link state, UTC clock */
    time_t now = time(0);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(b, sizeof b, "%02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
    int rx = W - 12 - t48_text_width(F_MONO, b);
    t48_text(s, rx, 10, F_MONO, C_DIM, b);
    long long t = t48_now_ms();
    const char *label;
    uint32_t c;
    if (st.up && t - st.last_line_ms < 3000) label = "LIVE", c = C_GOOD;
    else if (st.up) label = st.have_config ? "NO SPECTRUM" : "WAITING", c = C_AMBER;
    else if (!st.url_ok) label = "NO ADDRESS", c = C_BAD;
    else if (st.retry_at > t) {
        snprintf(b, sizeof b, "RETRY IN %lld s", (st.retry_at - t + 999) / 1000);
        label = b, c = C_BAD;
    } else label = "CONNECTING", c = C_AMBER;
    int w = t48_text_width(F_BOLD, label);
    rx -= w + 34;
    t48_fill(s, rx - 4, 0, w + 30, HDR_H - 1, C_HDR);
    t48_round_fill(s, rx + 2, 14, 9, 9, 4, c);
    t48_text(s, rx + 16, 7, F_BOLD, c, label);
    if (st.tx_keyed) {
        int pw = t48_text_width(F_BOLD, "TX") + 16;
        rx -= pw + 12;
        t48_fill(s, rx, 6, pw, 24, C_BAD);
        t48_text(s, rx + 8, 7, F_BOLD, T48_RGB(255, 255, 255), "TX");
    }
    t48_damage(0, 0, W, HDR_H);
}

/* frequency scale and band bars: change only with the config */
static void draw_static(void)
{
    char b[16];
    t48_fill(s, 0, SCALE_Y, PANEL_X, WF_TOP - SCALE_Y, C_BG);
    t48_fill(s, PANEL_X - 2, HDR_H, 2, H - HDR_H, C_LINE);
    if (st.hi_hz > st.lo_hz) {
        float span = st.hi_hz - st.lo_hz;
        int step = span > 6000 ? 1000 : span > 2500 ? 500 : 250;
        for (int hz = ((int)st.lo_hz / step + 1) * step; hz < st.hi_hz; hz += step) {
            int x = hz_x((float)hz), major = hz % 1000 == 0;
            t48_fill(s, x, SPEC_TOP - (major ? 7 : 4), 1, major ? 7 : 4, major ? C_DIM : C_FAINT);
            if (major) {
                snprintf(b, sizeof b, "%d", hz / 1000);
                t48_text_centre(s, x, SCALE_Y + 2, F_MONO, C_DIM, b);
            }
        }
        t48_text(s, PANEL_X - 34, SCALE_Y + 4, F_TINY, C_FAINT, "kHz");
    }
    t48_fill(s, 0, BAR_Y - 2, PANEL_X - 2, 6, C_BG);
    for (int i = 0; i < st.n_bands; i++) {
        int x0 = hz_x(st.bands[i].lo_hz), x1 = hz_x(st.bands[i].hi_hz);
        t48_fill(s, x0, BAR_Y, x1 - x0, 3, band_colour(st.bands[i].sub));
    }
    t48_fill(s, 0, WF_TOP - 1, PANEL_X - 2, 1, C_LINE);
    t48_damage(0, SCALE_Y, W, WF_TOP - SCALE_Y);
}

static void draw_spectrum(void)
{
    const int h = SPEC_BOT - SPEC_TOP;
    int lo = floor_q8 - 10 * 256, span = top_q8 - lo;  /* the trace shows the noise too */
    if (span < 20 * 256) span = 20 * 256;
    for (int y = SPEC_TOP; y < SPEC_BOT; y++) memcpy(&PX(0, y), spec_bg, WF_W * 4);
    for (int g = 1; g < 4; g++)
        for (int x = 0; x < WF_W; x += 3) PX(x, SPEC_TOP + g * h / 4) = C_GRID;
    if (have_levels) {
        int prev = -1;
        for (int x = 0; x < WF_W; x++) {
            int v = (spec16[x] * 16 - lo) * h / span;
            v = v < 0 ? 0 : v > h - 1 ? h - 1 : v;
            int y = SPEC_BOT - 1 - v;
            for (int j = y + 1; j < SPEC_BOT; j++) PX(x, j) = t48_mix(PX(x, j), C_AMBER, 50);
            int a = prev < 0 ? y : prev < y ? prev : y, z = prev < 0 ? y : prev > y ? prev : y;
            for (int j = a; j <= z; j++) PX(x, j) = C_AMBER;
            prev = y;
        }
    }
    for (int i = 0; i < st.n_bands; i++) {
        const band_t *bd = &st.bands[i];
        int x0 = hz_x(bd->lo_hz);
        /* the full name, else its first word, else cut; a label may run on
         * past its band, up to where the next one starts */
        int room = WF_W - x0;
        for (int k = 0; k < st.n_bands; k++) {
            int xk = hz_x(st.bands[k].lo_hz);
            if (xk > x0 && xk - x0 < room) room = xk - x0;
        }
        char nm[24];
        snprintf(nm, sizeof nm, "%s", bd->name);
        if (t48_text_width(F_TINY, nm) > room - 6 && strchr(nm, ' ')) *strchr(nm, ' ') = 0;
        t48_text_fit(s, x0 + 3, SPEC_TOP + 2, room - 6, F_TINY, band_colour(bd->sub), nm);
    }
    t48_damage(0, SPEC_TOP, WF_W, h);
}

typedef struct { int x, y, w, h; } rect_t;

static int overlaps(const rect_t *a, const rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void snr_text(int snr10, char *b, size_t n)
{
    snprintf(b, n, "%d dB", (snr10 + (snr10 < 0 ? -5 : 5)) / 10);
}

static void tag_label(const tag_t *t, char *b, size_t n)
{
    char snr[16];
    if (t->tx) snprintf(b, n, "TX > %s", t->to[0] ? t->to : "?");
    else if (t->snr10 != NO_SNR) { snr_text(t->snr10, snr, sizeof snr); snprintf(b, n, "%s %s", t->from[0] ? t->from : "?", snr); }
    else snprintf(b, n, "%s", t->from[0] ? t->from : "?");
}

/* the details of one frame, over the waterfall */
static void draw_detail(void)
{
    char b[160], snr[16];
    const tag_t *t = &detail;
    const band_t *bd = band_of(t->sub);
    int x = 40, y = WF_TOP + 22, w = WF_W - 80, h = WF_VIS - 44;
    shade(x, y, w, h);
    t48_fill(s, x, y, w, 2, t->tx ? C_COOL : band_colour(t->sub));
    snprintf(b, sizeof b, "%s > %s", t->from[0] ? t->from : "?", t->to[0] ? t->to : "?");
    t48_text_fit(s, x + 16, y + 14, w - 32, F_BIG, t->tx ? C_COOL : C_TEXT, b);
    y += 56;
    snprintf(b, sizeof b, "%s UTC%s", t->at, t->tx ? ", sent by this station" : "");
    t48_text(s, x + 16, y, F_SMALL, C_DIM, b);
    y += 24;
    snprintf(b, sizeof b, "%s", t->mode[0] ? t->mode : bd ? bd->name : "mode not given");
    t48_text_fit(s, x + 16, y, w - 32, F_SMALL, C_DIM, b);
    y += 24;
    b[0] = 0;
    if (t->snr10 != NO_SNR) { snr_text(t->snr10, snr, sizeof snr); snprintf(b, sizeof b, "SNR %s", snr); }
    if (t->len_bytes >= 0) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%d bytes", b[0] ? ", " : "", t->len_bytes);
    if (t->offset_hz != NO_OFFSET) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%+d Hz off", b[0] ? ", " : "", t->offset_hz);
    if (b[0]) { t48_text(s, x + 16, y, F_SMALL, C_DIM, b); y += 24; }
    if (t->plain[0]) {
        /* the text, wrapped by characters at the mono font's fixed width */
        int per = (w - 32) / t48_text_width(F_MONO, "M"), len = (int)strlen(t->plain);
        y += 6;
        for (int at = 0; at < len && y < WF_TOP + WF_VIS - 60; at += per, y += 20) {
            char line[128];
            snprintf(line, sizeof line, "%.*s", per < 127 ? per : 127, t->plain + at);
            t48_text(s, x + 16, y, F_MONO, C_TEXT, line);
        }
    }
    t48_text_right(s, x + w - 16, WF_TOP + WF_VIS - 22 - 30, F_TINY, C_FAINT, "tap or X to close");
}

static void draw_waterfall(void)
{
    /* rows, newest at the top */
    for (int y = 0; y < WF_VIS; y++) {
        uint32_t r = row_seq - 1 - (uint32_t)y;
        uint32_t *d = &PX(0, WF_TOP + y);
        if (have_rows && (uint32_t)y < row_seq - row_first) memcpy(d, ring[r % WF_HIST], WF_W * 4);
        else for (int x = 0; x < WF_W; x++) d[x] = C_BG;
    }
    /* band edges, dotted */
    for (int i = 0; i < st.n_bands; i++) {
        uint32_t c = t48_dim(band_colour(st.bands[i].sub), 1, 2);
        int xs[2] = {hz_x(st.bands[i].lo_hz), hz_x(st.bands[i].hi_hz) - 1};
        for (int k = 0; k < 2; k++)
            if (xs[k] >= 0 && xs[k] < WF_W)
                for (int y = WF_TOP; y < H; y += 4) PX(xs[k], y) = c;
    }
    /* time marks every 10 s down the left */
    if (st.have_config) {
        float rps = st.lines_per_s / st.lpr;
        char b[12];
        for (int sec = 10;; sec += 10) {
            int y = WF_TOP + (int)(sec * rps);
            if (y > H - 8) break;
            t48_fill(s, 0, y, 5, 1, C_DIM);
            snprintf(b, sizeof b, "-%ds", sec);
            int w = t48_text_width(F_TINY, b);
            shade(6, y - 7, w + 4, 14);
            t48_text(s, 8, y - 7, F_TINY, C_DIM, b);
        }
    }
    /* frame tags: a bracket on each burst, a label on the newest ones */
    rect_t used[12];
    int nu = 0, labels = 0;
    int n = tag_seq < NTAGS ? (int)tag_seq : NTAGS;
    for (int k = 0; k < n; k++) {
        const tag_t *t = &tags[(tag_seq - 1 - k) % NTAGS];
        if (!t->placed) continue;
        const band_t *bd = band_of(t->sub);
        if (!bd) continue;
        int y_new = WF_TOP + (int)(row_seq - 1 - (t->row + t->rows - 1));
        int y_old = WF_TOP + (int)(row_seq - 1 - t->row);
        if ((int32_t)(row_seq - 1 - (t->row + t->rows - 1)) < 0) y_new = WF_TOP;
        if (y_new >= H) continue;
        if (y_old >= H) y_old = H - 1;
        int x0 = hz_x(bd->lo_hz), x1 = hz_x(bd->hi_hz);
        uint32_t c = t->tx ? C_COOL : C_TAG;
        t48_fill(s, x0 - 4, y_new, 2, y_old - y_new + 1, c);
        t48_fill(s, x0 - 4, y_new, 6, 2, c);
        if (y_old < H - 1) t48_fill(s, x0 - 4, y_old - 1, 6, 2, c);
        if (labels >= 10) continue;
        char b[40];
        tag_label(t, b, sizeof b);
        int w = t48_text_width(F_MONO, b) + 10;
        rect_t r = {x0 - 8 - w, y_new, w, 19};
        if (r.x < 0) r.x = x1 + 4;
        if (r.x + w > WF_W) r.x = x0 + 4;
        for (int tries = 0; tries < 6; tries++) {
            int hit = 0;
            for (int u = 0; u < nu; u++)
                if (overlaps(&r, &used[u])) { r.y = used[u].y + used[u].h + 1; hit = 1; }
            if (!hit) break;
        }
        if (r.y + r.h > H) continue;
        shade(r.x, r.y, r.w, r.h);
        t48_fill(s, r.x, r.y, 2, r.h, t->tx ? C_COOL : band_colour(t->sub));
        t48_text(s, r.x + 6, r.y + 1, F_MONO, c, b);
        if (nu < 12) used[nu++] = r;
        labels++;
    }
    /* state over the picture */
    long long now = t48_now_ms();
    const char *big = NULL;
    char small[96] = "";
    if (!st.url_ok) {
        big = "NO ADDRESS";
        snprintf(small, sizeof small, "set the station's ws:// address in the app's settings");
    } else if (!st.up) {
        big = have_rows ? "FEED LOST" : "CONNECTING";
        if (st.retry_at > now)
            snprintf(small, sizeof small, "%s:%d  -  %s  -  retry %d in %lld s", st.url_host, st.url_port,
                     st.why[0] ? st.why : "no connection", st.attempts + 1, (st.retry_at - now + 999) / 1000);
        else
            snprintf(small, sizeof small, "%s:%d", st.url_host, st.url_port);
    } else if (!st.have_config) {
        big = "WAITING";
        snprintf(small, sizeof small, "connected, waiting for the station's settings");
    } else if (now - st.last_line_ms > 3000) {
        big = "NO SPECTRUM";
        snprintf(small, sizeof small, "connected, no spectrum lines for %lld s", (now - st.last_line_ms) / 1000);
    }
    if (big && !detail_open) {
        int bw = t48_text_width(F_BIG, big), sw = t48_text_width(F_SMALL, small);
        int w = (bw > sw ? bw : sw) + 48, cy = WF_TOP + WF_VIS / 2;
        if (w > WF_W - 8) w = WF_W - 8;
        shade(WF_W / 2 - w / 2, cy - 40, w, 80);
        t48_fill(s, WF_W / 2 - w / 2, cy - 40, w, 2, st.up ? C_AMBER : C_BAD);
        t48_text_centre(s, WF_W / 2, cy - 28, F_BIG, st.up || !have_rows ? C_AMBER : C_BAD, big);
        if (sw <= w - 48) t48_text_centre(s, WF_W / 2, cy + 12, F_SMALL, C_DIM, small);
        else t48_text_fit(s, WF_W / 2 - w / 2 + 24, cy + 12, w - 48, F_SMALL, C_DIM, small);
    }
    if (detail_open) draw_detail();
    t48_damage(0, WF_TOP, WF_W, WF_VIS);
}

static void draw_panel(void)
{
    char b[64], snr[16];
    t48_fill(s, PANEL_X, HDR_H, PANEL_W, H - HDR_H, C_PANEL);
    t48_text(s, PANEL_X + 10, HDR_H + 8, F_TINY, C_FAINT, "RECENT FRAMES");
    int y = PANEL_TOP;
    int n = tag_seq < NTAGS ? (int)tag_seq : NTAGS;
    if (!n) t48_text(s, PANEL_X + 10, y + 4, F_SMALL, C_FAINT, "none yet");
    for (int k = 0; k < n && y + PANEL_ROW <= H; k++) {
        const tag_t *t = &tags[(tag_seq - 1 - k) % NTAGS];
        uint32_t c = t->tx ? C_COOL : band_colour(t->sub);
        t48_fill(s, PANEL_X + 8, y + 3, 3, PANEL_ROW - 10, c);
        /* our own transmissions are told by their colour, and "TX" below */
        snprintf(b, sizeof b, "%s > %s", t->from[0] ? t->from : "?", t->to[0] ? t->to : "?");
        t48_text_fit(s, PANEL_X + 18, y + 1, PANEL_W - 24, F_BOLD, t->tx ? C_COOL : C_TEXT, b);
        const band_t *bd = band_of(t->sub);
        snr[0] = 0;
        if (t->snr10 != NO_SNR) { snprintf(snr, sizeof snr, "  "); snr_text(t->snr10, snr + 2, sizeof snr - 2); }
        snprintf(b, sizeof b, "%s  %s%s%s", t->at, t->tx ? "TX  " : "", bd ? bd->name : "", snr);
        t48_text_fit(s, PANEL_X + 18, y + 25, PANEL_W - 24, F_TINY, C_DIM, b);
        t48_fill(s, PANEL_X + 8, y + PANEL_ROW - 4, PANEL_W - 16, 1, C_GRID);
        y += PANEL_ROW;
    }
    t48_damage(PANEL_X, HDR_H, PANEL_W, H - HDR_H);
}

static void redraw(int force)
{
    if (st.paused) return;
    if (force || dirty_static) draw_static();
    if (force || dirty_hdr) draw_header();
    if (force || dirty_panel) draw_panel();
    if (force || dirty_wf) {
        draw_spectrum();
        draw_waterfall();
    }
    dirty_static = dirty_hdr = dirty_panel = dirty_wf = 0;
}

/* ---------- the connection ---------- */

static void drop(const char *why, int gap)
{
    if (st.ws) {
        t48_unwatch_fd(t48_ws_fd(st.ws));
        t48_ws_close(st.ws);
        st.ws = NULL;
    }
    if (st.up) {
        logf_("%s", why);
        if (gap) gap_row();
    }
    st.up = 0;
    snprintf(st.why, sizeof st.why, "%s", why);
    st.retry_at = t48_now_ms() + st.backoff * 1000LL;
    st.backoff = st.backoff < 30 ? st.backoff * 2 : 30;
    dirty_hdr = dirty_wf = 1;
}

static void try_connect(void)
{
    char err[64] = "";
    st.retry_at = 0;
    dirty_hdr = dirty_wf = 1;
    redraw(0);  /* "CONNECTING" while we wait */
    st.ws = t48_ws_open(st.url, 4000, MSG_MAX, err, sizeof err);
    if (!st.ws) {
        st.attempts++;
        logf_("cannot connect to %s: %s, retry in %d s", st.url, err, st.backoff);
        drop(err, 0);
        return;
    }
    logf_("listening to %s", st.url);
    st.up = 1;
    st.attempts = 0;
    st.backoff = 1;
    st.why[0] = 0;
    st.last_line_ms = st.last_rx_ms = t48_now_ms();
    wf_rows_reset(&rows, row_seq, st.lpr ? st.lpr : 4);
    t48_watch_fd(t48_ws_fd(st.ws), FD_WS);
    if (t48_ws_read(st.ws, on_msg, NULL) < 0) drop("the station closed the connection", 1);
    dirty_hdr = dirty_wf = 1;
}

/* ---------- touch ---------- */

static void on_tap(int x, int y)
{
    if (detail_open) {
        detail_open = 0;
        dirty_wf = 1;
        return;
    }
    if (x < PANEL_X || y < PANEL_TOP) return;
    int k = (y - PANEL_TOP) / PANEL_ROW, n = tag_seq < NTAGS ? (int)tag_seq : NTAGS;
    if (k >= n || PANEL_TOP + (k + 1) * PANEL_ROW > H) return;
    detail = tags[(tag_seq - 1 - k) % NTAGS];
    detail_open = 1;
    dirty_wf = 1;
}

/* ---------- main ---------- */

int main(void)
{
    t48_event ev;
    long long last_draw = 0;

    if (t48_app_init() < 0) return 1;
    s = t48_screen();
    if (s->w < W || s->h < H) {
        fprintf(stderr, "waterfall: needs an 800 x 480 screen\n");
        return 1;
    }

    const char *name = t48_setting("name", "");
    snprintf(st.url, sizeof st.url, "%s", t48_setting("url", DEFAULT_URL));
    char path[128];
    st.url_ok = t48_ws_parse_url(st.url, st.url_host, sizeof st.url_host, &st.url_port, path, sizeof path);
    if (name[0]) {
        snprintf(st.name, sizeof st.name, "%s", name);
        st.name_from_setting = 1;
    } else {
        snprintf(st.name, sizeof st.name, "%s", !strcmp(st.url, DEFAULT_URL) ? DEFAULT_NAME : st.url_ok ? st.url_host : "Waterfall");
    }
    st.backoff = 1;

    static const uint32_t rxs[] = {0x080A10, 0x0B1030, 0x2A1450, 0x7A2448, 0xD9601E, 0xFF9F1C, 0xFFE8B8};
    static const uint32_t txs[] = {0x080A10, 0x06182A, 0x0E3A5C, 0x2A7FB0, 0x9AD8FF};
    for (int x = 0; x < WF_W; x++) spec_bg[x] = C_BG;
    build_ramp(ramp_rx, rxs, 7);
    build_ramp(ramp_tx, txs, 5);
    wf_rows_reset(&rows, 0, 4);

    t48_fill(s, 0, 0, s->w, s->h, C_BG);
    redraw(1);
    t48_damage_all();
    t48_timer(TIMER_SECOND, 1000, 1);

    for (;;) {
        long long now = t48_now_ms();
        if (!st.paused && st.url_ok && !st.ws && now >= st.retry_at) try_connect();

        /* wait for the next thing: a quick wake-up if a redraw is held back */
        int timeout = -1;
        now = t48_now_ms();
        if (dirty_wf) timeout = 35;
        else if (!st.paused && st.url_ok && !st.ws) timeout = st.retry_at > now ? (int)(st.retry_at - now) : 0;
        int r = t48_poll(&ev, timeout);
        if (r < 0) break;
        if (r > 0) switch (ev.type) {
        case T48_EV_FD:
            if (ev.id == FD_WS && st.ws && t48_ws_read(st.ws, on_msg, NULL) < 0)
                drop("the station closed the connection", 1);
            break;
        case T48_EV_TIMER:  /* clock, countdowns, stale states, a silent station */
            dirty_hdr = 1;
            now = t48_now_ms();
            if (!st.up || now - st.last_line_ms > 3000) dirty_wf = 1;
            if (st.up && now - st.last_rx_ms > 60000) drop("silent for 60 s", 1);
            break;
        case T48_EV_KEY:
            if (!ev.state) break;
            if (ev.key == T48_KEY_BACK) {
                if (detail_open) { detail_open = 0; dirty_wf = 1; break; }
                goto out;
            }
            if (ev.key == T48_KEY_OK && !st.ws) { st.retry_at = 0; st.backoff = 1; }  /* try again now */
            break;
        case T48_EV_TOUCH:
            if (ev.state == T48_TOUCH_DOWN) on_tap(ev.x, ev.y);
            break;
        case T48_EV_PAUSE:
            /* off the screen: let go of the station, freeze */
            if (st.ws) drop("paused", 1);
            st.paused = 1;
            t48_paused();
            break;
        case T48_EV_RESUME:
            st.paused = 0;
            st.retry_at = 0;
            st.backoff = 1;
            redraw(1);
            break;
        case T48_EV_QUIT:
            goto out;
        }
        /* at most about 30 waterfall updates a second; the rest at once */
        now = t48_now_ms();
        int keep = dirty_wf && now - last_draw < 33;
        if (dirty_wf && !keep) last_draw = now;
        if (keep) dirty_wf = 0;
        redraw(0);
        if (keep) dirty_wf = 1;
    }
out:
    if (st.ws) t48_ws_close(st.ws);
    return 0;
}
