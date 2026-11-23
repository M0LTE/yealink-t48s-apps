// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * links: a LinBPQ packet node at a glance. Open AX.25 links, who is
 * connected, and the neighbours, read from the node's own web server
 * (/Node/Links.html, Users.html and Routes.html) every 15 seconds.
 * Moved from the firmware repo's native/t48ui.c, the GB7RDG links screen.
 *
 * Settings: "url" (the node's web server, http://host:port/, GB7RDG's unless
 * changed) and "name" (the node's callsign, shown at the top; the links
 * section leaves it out of "our call").
 *
 * The pages are fetched without blocking, so keys and touches are answered
 * while a slow node is being read. Nothing is fetched while the app is not
 * on the screen. OK or the Refresh button fetches at once; X leaves.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "t48.h"

#define W 800
#define H 480
#define DEFAULT_URL "http://10.45.0.121:8008/"
#define DEFAULT_NAME "GB7RDG"
#define REFRESH_MS 15000
#define PAGE_TIMEOUT_MS 6000

/* colours are the theme's (Settings, Screen, Theme), by what they are for */
#define TH (t48_theme_now())
#define C_BG (TH->bg)
#define C_LINE (TH->line)
#define C_TEXT (TH->text)
#define C_DIM (TH->dim)
#define C_GOOD (TH->good)
#define C_BAD (TH->bad)

#define F_TITLE (&t48_font_head)
#define F_LABEL (&t48_font_normal)
#define F_BOLD (&t48_font_bold)
#define F_MONO (&t48_font_mono)

static t48_surface *s;
static char host[128], node_name[32];
static int port = 80;
static int url_ok;

/* ---------- reading LinBPQ's tables ---------- */

static const char *find_ci(const char *p, const char *end, const char *needle)
{
    size_t k = strlen(needle);
    for (; p + k <= end; p++)
        if (!strncasecmp(p, needle, k)) return p;
    return NULL;
}

#define MAXROWS 24
#define MAXCOLS 16
struct table { int rows, cols[MAXROWS]; char cell[MAXROWS][MAXCOLS][40]; };

/* cell text without tags or entities, trimmed */
static void cell_text(const char *p, const char *e, char *out, int max)
{
    int n = 0, intag = 0;
    for (; p < e && n < max - 1; p++) {
        if (*p == '<') { intag = 1; continue; }
        if (*p == '>' && intag) { intag = 0; continue; }
        if (intag) continue;
        if (*p == '&') {
            const char *semi = memchr(p, ';', (size_t)(e - p));
            if (semi && semi - p < 8) { if (n && out[n - 1] != ' ') out[n++] = ' '; p = semi; continue; }
        }
        char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
        if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = c;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* the table that follows `marker` (after the page's menu, which also names it) */
static void parse_table(const char *html, const char *marker, struct table *t)
{
    t->rows = 0;
    const char *end = html + strlen(html);
    const char *p = find_ci(html, end, marker);
    if (!p || !(p = find_ci(p, end, "<table"))) return;
    const char *te = find_ci(p, end, "</table");
    if (!te) te = end;
    while (t->rows < MAXROWS) {
        const char *tr = find_ci(p, te, "<tr");
        if (!tr) break;
        const char *trend = find_ci(tr + 3, te, "<tr");
        if (!trend) trend = te;
        int c = 0;
        const char *q = tr;
        while (c < MAXCOLS) {
            const char *td = find_ci(q, trend, "<td");
            if (!td) break;
            const char *gt = memchr(td, '>', (size_t)(trend - td));
            if (!gt) break;
            const char *tdend = find_ci(gt, trend, "</td");
            const char *next = find_ci(gt, trend, "<td");
            if (!tdend || (next && next < tdend)) tdend = next ? next : trend;
            cell_text(gt + 1, tdend, t->cell[t->rows][c], sizeof t->cell[0][0]);
            c++;
            q = tdend;
        }
        if (c > 0) t->cols[t->rows++] = c;
        p = trend;
    }
}

static struct table links, users, routes;
static char status_line[128] = "fetching...";
static int fetch_ok, have_data;

/* ---------- fetching, without blocking ---------- */

static const char *const pages[3] = {"/Node/Links.html", "/Node/Users.html", "/Node/Routes.html"};
static char page_buf[3][65536];
static struct {
    int busy, page, fd, sent;
    size_t len;
    long long deadline;
} f = {.fd = -1};
static long long next_fetch;

static void fetch_done(int ok, const char *why)
{
    time_t t = time(NULL);
    struct tm tm;
    if (f.fd >= 0) close(f.fd);
    f.fd = -1;
    f.busy = 0;
    next_fetch = t48_now_ms() + REFRESH_MS;
    gmtime_r(&t, &tm);
    fetch_ok = ok;
    if (!ok) {
        snprintf(status_line, sizeof status_line, "can't reach %s:%d: %s (%02d:%02d UTC)", host, port, why, tm.tm_hour, tm.tm_min);
        fprintf(stderr, "links: %s\n", status_line);
        return;
    }
    parse_table(page_buf[0], "Links", &links);
    parse_table(page_buf[1], "Sessions", &users);
    parse_table(page_buf[2], "Routes", &routes);
    have_data = 1;
    snprintf(status_line, sizeof status_line, "updated %02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void open_page(void)
{
    char ps[12];
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    snprintf(ps, sizeof ps, "%d", port);
    f.len = 0;
    f.sent = 0;
    page_buf[f.page][0] = 0;
    f.deadline = t48_now_ms() + PAGE_TIMEOUT_MS;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { fetch_done(0, "unknown host"); return; }
    f.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f.fd < 0) { freeaddrinfo(res); fetch_done(0, "no socket"); return; }
    fcntl(f.fd, F_SETFL, O_NONBLOCK);
    int r = connect(f.fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r && errno != EINPROGRESS) fetch_done(0, strerror(errno));
}

static void start_fetch(void)
{
    if (f.busy || !url_ok) return;
    f.busy = 1;
    f.page = 0;
    open_page();
}

/* the page is complete: LinBPQ keeps the connection open, so look for the end */
static int page_complete(const char *b, size_t len)
{
    const char *body = strstr(b, "\r\n\r\n");
    if (!body) return 0;
    const char *cl = find_ci(b, body, "Content-Length:");
    if (cl && (long)(b + len - (body + 4)) >= atol(cl + 15)) return 1;
    return find_ci(body, b + len, "</html>") != NULL;
}

/* the socket is ready: 1 if the page set changed (a redraw is due) */
static int fetch_step(short revents)
{
    char *b = page_buf[f.page];
    if (!f.sent) {
        int err = 0;
        socklen_t el = sizeof err;
        char req[256];
        getsockopt(f.fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { fetch_done(0, strerror(err)); return 1; }
        int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", pages[f.page], host);
        if (send(f.fd, req, (size_t)n, MSG_NOSIGNAL) != n) { fetch_done(0, "cannot send"); return 1; }
        f.sent = 1;
        return 0;
    }
    if (!(revents & (POLLIN | POLLHUP | POLLERR))) return 0;
    ssize_t r = read(f.fd, b + f.len, sizeof page_buf[0] - 1 - f.len);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (r > 0) {
        f.len += (size_t)r;
        b[f.len] = 0;
    }
    if (r <= 0 || f.len >= sizeof page_buf[0] - 1 || page_complete(b, f.len)) {
        close(f.fd);
        f.fd = -1;
        if (!f.len) { fetch_done(0, "empty answer"); return 1; }
        if (++f.page == 3) { fetch_done(1, NULL); return 1; }
        open_page();
        return !f.busy;
    }
    return 0;
}

/* ---------- the page ---------- */

static const struct { int x, y, w, h; const char *label; } refresh_btn = {16, 424, 150, 48, "Refresh"};

static int section(int y, const char *title, int count)
{
    char t[64];
    snprintf(t, sizeof t, "%s (%d)", title, count);
    t48_text(s, 16, y, F_BOLD, C_TEXT, t);
    t48_fill(s, 16, y + 26, W - 32, 1, C_LINE);
    return y + 32;
}

static void draw_page(void)
{
    char l[200];
    time_t t = time(NULL);
    struct tm tm;

    /* the shell's own header, with our words in it: the node, and the time as packet radio keeps it */
    t48_ui_clear(s, C_BG);
    t48_ui_header_ground(s);
    int x = t48_text(s, 16, (T48_UI_HEAD_H - F_TITLE->height) / 2, F_TITLE, TH->on_head, node_name);
    t48_text(s, x + 14, (T48_UI_HEAD_H - F_LABEL->height) / 2 + 2, F_LABEL, TH->on_head_dim, "packet node");
    gmtime_r(&t, &tm);
    snprintf(l, sizeof l, "%02d:%02d UTC", tm.tm_hour, tm.tm_min);
    t48_text_right(s, W - 16, (T48_UI_HEAD_H - F_BOLD->height) / 2, F_BOLD, TH->on_head_dim, l);

    int y = 64;
    if (!url_ok) {
        t48_text(s, 16, y, F_BOLD, C_BAD, "Set the node's address (http://host:port/) in the app's settings");
        y += 34;
    } else if (!fetch_ok && !f.busy) {
        t48_text_fit(s, 16, y, W - 32, F_BOLD, C_BAD, status_line);
        y += 34;
    }
    const int row = 24, bottom = 408;
    /* rows each section may use: what it needs, sharing what is left fairly */
    int need[3] = {links.rows ? links.rows : 1, users.rows ? users.rows : 1, routes.rows ? routes.rows : 1};
    int avail = (bottom - y - 3 * 38) / row, give[3] = {0, 0, 0}, left = avail;
    for (int pass = 0; pass < 3 && left > 0; pass++)
        for (int k = 0; k < 3 && left > 0; k++)
            if (give[k] < need[k]) {
                int add = need[k] - give[k], fair = left / (3 - k);
                if (fair < 1) fair = 1;
                if (add > fair) add = fair;
                give[k] += add;
                left -= add;
            }

    /* Links: far call, our call, port, AX.25 state, link type, AX.25 version */
    y = section(y, "Links", links.rows);
    if (!links.rows) { t48_text(s, 24, y, F_LABEL, C_DIM, have_data ? "no AX.25 links open" : "-"); y += row; }
    for (int r = 0; r < links.rows; r++, y += row) {
        if (r == give[0] - 1 && links.rows > give[0]) {
            snprintf(l, sizeof l, "+%d more", links.rows - r);
            t48_text(s, 24, y, F_LABEL, C_DIM, l);
            y += row;
            break;
        }
        const char *ours = links.cell[r][1];
        x = t48_text(s, 24, y, F_MONO, C_GOOD, links.cell[r][0]);
        if (strcasecmp(ours, node_name)) { snprintf(l, sizeof l, " to %s", ours); t48_text(s, x, y, F_MONO, C_DIM, l); }
        snprintf(l, sizeof l, "port %-3s %-8s %s", links.cell[r][2], links.cell[r][3], links.cell[r][4]);
        t48_text_fit(s, 330, y, W - 346, F_MONO, C_TEXT, l);
    }
    y += 6;

    /* Sessions: the row as LinBPQ gives it */
    y = section(y, "Sessions", users.rows);
    if (!users.rows) { t48_text(s, 24, y, F_LABEL, C_DIM, have_data ? "nobody connected" : "-"); y += row; }
    for (int r = 0; r < users.rows; r++, y += row) {
        if (r == give[1] - 1 && users.rows > give[1]) {
            snprintf(l, sizeof l, "+%d more", users.rows - r);
            t48_text(s, 24, y, F_LABEL, C_DIM, l);
            y += row;
            break;
        }
        l[0] = 0;
        for (int c = 0; c < users.cols[r]; c++) {
            if (!users.cell[r][c][0]) continue;
            if (l[0]) strncat(l, "  <->  ", sizeof l - strlen(l) - 1);
            strncat(l, users.cell[r][c], sizeof l - strlen(l) - 1);
        }
        t48_text_fit(s, 24, y, W - 48, F_MONO, C_TEXT, l);
    }
    y += 6;

    /* Neighbours: port, call, quality, node count, frames, retries, percent, maxframe, frack, last heard */
    y = section(y, "Neighbours", routes.rows);
    for (int r = 0; r < routes.rows; r++, y += row) {
        if (r == give[2] - 1 && routes.rows > give[2]) {
            snprintf(l, sizeof l, "+%d more", routes.rows - r);
            t48_text(s, 24, y, F_LABEL, C_DIM, l);
            break;
        }
        const char *rport = routes.cell[r][0];
        int linked = 0;
        for (int k = 0; k < links.rows; k++)
            if (!strcasecmp(links.cell[k][0], routes.cell[r][1])) linked = 1;
        if (rport[0] == '>') rport++;
        t48_text(s, 24, y, F_MONO, linked ? C_GOOD : C_TEXT, routes.cell[r][1]);
        snprintf(l, sizeof l, "port %-3s q %-4s heard %-6s retries %s", rport, routes.cell[r][2],
                 routes.cols[r] > 9 ? routes.cell[r][9] : "", routes.cols[r] > 6 ? routes.cell[r][6] : "");
        t48_text_fit(s, 330, y, W - 346, F_MONO, C_TEXT, l);
    }

    /* a button as the shell draws one in this theme */
    t48_ui_btn(s, refresh_btn.x, refresh_btn.y, refresh_btn.w, refresh_btn.h, f.busy ? "Reading" : refresh_btn.label,
               f.busy ? T48_UI_OFF : T48_UI_PLAIN);
    t48_text(s, 190, 438, F_LABEL, C_DIM, "OK: refresh    X: close");
    /* the time of the last good read; a failure is said at the top instead */
    if (f.busy && !have_data) t48_text_right(s, W - 16, 438, F_LABEL, C_DIM, "fetching...");
    else if (fetch_ok || f.busy) t48_text_right(s, W - 16, 438, F_LABEL, C_DIM, status_line);
    t48_damage_all();
}

/* ---------- main ---------- */

static int parse_url(const char *url)
{
    if (strncmp(url, "http://", 7)) return 0;
    const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    const char *hend = slash ? slash : h + strlen(h);
    if (colon && colon < hend) { port = atoi(colon + 1); hend = colon; }
    if (hend == h || (size_t)(hend - h) >= sizeof host) return 0;
    memcpy(host, h, (size_t)(hend - h));
    host[hend - h] = 0;
    return port > 0 && port < 65536;
}

int main(void)
{
    t48_event ev;
    int paused = 0;
    long long last_draw = 0;

    if (t48_app_init() < 0) return 1;
    s = t48_screen();
    if (s->w < W || s->h < H) { fprintf(stderr, "links: needs an 800 x 480 screen\n"); return 1; }
    url_ok = parse_url(t48_setting("url", DEFAULT_URL));
    snprintf(node_name, sizeof node_name, "%s", t48_setting("name", DEFAULT_NAME));
    if (!url_ok) snprintf(status_line, sizeof status_line, "no node address");
    start_fetch();
    draw_page();

    /* our own poll loop: the shell's socket and the page being fetched */
    for (;;) {
        struct pollfd pf[2] = {{.fd = t48_fd(), .events = POLLIN}, {.fd = -1}};
        int n = 1;
        long long now = t48_now_ms(), wake = now + 1000;
        if (f.busy && f.fd >= 0) {
            pf[1].fd = f.fd;
            pf[1].events = f.sent ? POLLIN : POLLOUT;
            n = 2;
            if (f.deadline < wake) wake = f.deadline;
        } else if (!paused && url_ok && next_fetch < wake) {
            wake = next_fetch;
        }
        int timeout = wake > now ? (int)(wake - now) : 0;
        if (poll(pf, (nfds_t)n, timeout) < 0 && errno != EINTR) break;

        int redraw = 0;
        if (n == 2 && pf[1].revents) redraw |= fetch_step(pf[1].revents);
        if (f.busy && t48_now_ms() > f.deadline) { fetch_done(0, "no answer"); redraw = 1; }

        /* everything the shell has sent */
        int r;
        while ((r = t48_poll(&ev, 0)) > 0) {
            switch (ev.type) {
            case T48_EV_KEY:
                if (!ev.state) break;
                if (ev.key == T48_KEY_BACK) goto out;
                if (ev.key == T48_KEY_OK) { start_fetch(); redraw = 1; }
                break;
            case T48_EV_TOUCH:
                if (ev.state == T48_TOUCH_DOWN && ev.x >= refresh_btn.x && ev.x < refresh_btn.x + refresh_btn.w &&
                    ev.y >= refresh_btn.y && ev.y < refresh_btn.y + refresh_btn.h) {
                    start_fetch();
                    redraw = 1;
                }
                break;
            case T48_EV_PAUSE:
                paused = 1;
                if (f.fd >= 0) close(f.fd);  /* let go of the node; fetch again on resume */
                f.fd = -1;
                f.busy = 0;
                t48_paused();
                break;
            case T48_EV_RESUME:
                paused = 0;
                start_fetch();  /* fresh data straight away */
                redraw = 1;
                break;
            case T48_EV_THEME:  /* the owner changed the look */
                redraw = 1;
                break;
            case T48_EV_QUIT:
                goto out;
            }
        }
        if (r < 0) break;
        now = t48_now_ms();
        if (!paused && !f.busy && url_ok && now >= next_fetch) { start_fetch(); redraw = 1; }
        if (!paused && (redraw || now - last_draw >= 10000)) {  /* the clock, at least */
            draw_page();
            last_draw = now;
        }
    }
out:
    if (f.fd >= 0) close(f.fd);
    return 0;
}
