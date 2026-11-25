// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the 3D pipeline.
 *
 * Screen positions are 28.4 fixed point and every edge is stepped from where
 * it really starts, so a slowly turning object glides instead of snapping
 * from pixel to pixel. Shading values are 16.16 planes over the triangle,
 * evaluated at pixel centres. A pixel is filled when its centre is inside
 * (top-left rule), so triangles that share an edge never gap or overlap.
 *
 * Smoothing: after a triangle is filled, each of its edges that is an outline
 * (the triangle across it faces away, or is behind and already drawn) gets
 * one more row of pixels just outside it, mixed in by how much of the pixel
 * the edge covers. Later, nearer triangles paint over it as they should. */
#include <math.h>
#include "r3d.h"

#define MAXV 2600
#define MAXT 5200
#define NBUCKET 1024

/* x, y: 12.4. u, v: 8.8. Eight bytes, so a whole mesh's corners stay in the 16 KB cache */
typedef struct rvert { int16_t x, y; uint16_t u, v; } rvert;

static struct {
    uint32_t *px;
    int stride, w, h, mode;
    uint32_t colour;
    const uint32_t *pal;
    const uint8_t *tex;
} R;

static rvert sv[MAXV];
static int vz[MAXV];
static uint8_t tstate[MAXT];      /* 0 faces away, 1 faces the eye, 2 drawn */
static uint16_t torder[MAXT], tbucket[MAXT];
static uint16_t bcount[NBUCKET + 1];

static inline int ceil4(int v) { return (v + 15) >> 4; }

/* The CPU has no divide instruction, and a triangle needs several: for the
 * usual small divisor, multiply by its reciprocal instead. */
#define NRECIP 2048
static uint32_t recip[NRECIP];    /* 2^28 / i */

/* (n << sh) / d for d > 0; sh is 16 or 12 */
static inline int32_t ratio(int n, int d, int sh)
{
    if (d < NRECIP) return (int32_t)(((int64_t)n * recip[d]) >> (28 - sh));
    return (int32_t)(((int64_t)n << sh) / d);
}

/* ---------- filling ---------- */

static void tri_fill(const rvert *a, const rvert *b, const rvert *c)
{
    const rvert *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < a->y) { t = a; a = c; c = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    int ya = ceil4(a->y), yb = ceil4(b->y), yc = ceil4(c->y);
    if (ya >= yc) return;
    int dx1 = b->x - a->x, dy1 = b->y - a->y, dx2 = c->x - a->x, dy2 = c->y - a->y;
    int area = dx1 * dy2 - dx2 * dy1;
    if (!area) return;

    /* the shading planes: value = c + x * d/dx + y * d/dy, all modulo 2^32 */
    int32_t gux = 0, guy = 0, gvx = 0, gvy = 0;
    uint32_t cu = 0, cv = 0;
    if (R.mode != M_FLAT) {
        unsigned aa = (unsigned)(area < 0 ? -area : area);
        int flat = aa < 256;
        if (!flat) {
            int nb = 32 - __builtin_clz(aa), extra = nb > 16 ? nb - 16 : 0;
            int64_t inv = 0xffffffffu / (aa >> extra);
            int sh = 20 + extra;
            int du1 = b->u - a->u, du2 = c->u - a->u, dv1 = b->v - a->v, dv2 = c->v - a->v;
            int64_t g[4] = {0, 0, 0, 0};
            int ng = R.mode == M_ENV ? 4 : 2;
            g[0] = (((int64_t)du1 * dy2 - (int64_t)du2 * dy1) * inv) >> sh;
            g[1] = (((int64_t)du2 * dx1 - (int64_t)du1 * dx2) * inv) >> sh;
            if (ng == 4) {
                g[2] = (((int64_t)dv1 * dy2 - (int64_t)dv2 * dy1) * inv) >> sh;
                g[3] = (((int64_t)dv2 * dx1 - (int64_t)dv1 * dx2) * inv) >> sh;
            }
            for (int i = 0; i < ng; i++) {
                if (area < 0) g[i] = -g[i];
                if (g[i] > (1 << 30) || g[i] < -(1 << 30)) flat = 1;   /* a sliver: one value will do */
            }
            if (!flat) {
                gux = (int32_t)g[0]; guy = (int32_t)g[1];
                cu = ((uint32_t)a->u << 8) - (uint32_t)(((int64_t)a->x * gux + (int64_t)a->y * guy) >> 4);
                if (ng == 4) {
                    gvx = (int32_t)g[2]; gvy = (int32_t)g[3];
                    cv = ((uint32_t)a->v << 8) - (uint32_t)(((int64_t)a->x * gvx + (int64_t)a->y * gvy) >> 4);
                }
            }
        }
        if (flat) {
            cu = (uint32_t)((a->u + b->u + c->u) / 3) << 8;
            cv = (uint32_t)((a->v + b->v + c->v) / 3) << 8;
        }
    }

    /* edges: x in 16.16 at each row's centre */
    int32_t sl = ratio(dx2, dy2, 16), ss = 0;
    int32_t xl = (a->x << 12) + (int32_t)(((int64_t)((ya << 4) - a->y) * sl) >> 4), xs = 0;
    if (yb > ya) {
        ss = ratio(dx1, dy1, 16);
        xs = (a->x << 12) + (int32_t)(((int64_t)((ya << 4) - a->y) * ss) >> 4);
    }
    int long_left = area > 0;
    cu += (uint32_t)ya * (uint32_t)guy;
    cv += (uint32_t)ya * (uint32_t)gvy;
    const uint32_t *pal = R.pal;
    const uint8_t *tex = R.tex;
    uint32_t *row = R.px + ya * R.stride;

    for (int y = ya; y < yc; y++, xl += sl, xs += ss, cu += (uint32_t)guy, cv += (uint32_t)gvy, row += R.stride) {
        if (y == yb) {
            int dy = c->y - b->y;
            ss = ratio(c->x - b->x, dy, 16);
            xs = (b->x << 12) + (int32_t)(((int64_t)((yb << 4) - b->y) * ss) >> 4);
        }
        if ((unsigned)y >= (unsigned)R.h) continue;
        int x0 = ((long_left ? xl : xs) + 0xffff) >> 16, x1 = ((long_left ? xs : xl) + 0xffff) >> 16;
        if (x0 < 0) x0 = 0;
        if (x1 > R.w) x1 = R.w;
        int n = x1 - x0;
        if (n <= 0) continue;
        uint32_t *p = row + x0;
        if (R.mode == M_FLAT) {
            uint32_t col = R.colour;
            do *p++ = col; while (--n);
        } else if (R.mode == M_GOURAUD) {
            uint32_t u = cu + (uint32_t)x0 * (uint32_t)gux;
            do { *p++ = pal[u >> 16]; u += (uint32_t)gux; } while (--n);
        } else {
            uint32_t u = cu + (uint32_t)x0 * (uint32_t)gux, v = cv + (uint32_t)x0 * (uint32_t)gvx;
            do {
                *p++ = pal[tex[((v >> 8) & 0xff00) + (u >> 16)]];
                u += (uint32_t)gux; v += (uint32_t)gvx;
            } while (--n);
        }
    }
}

/* ---------- smoothing an outline ---------- */

static inline void aa_plot(int x, int y, unsigned alpha, uint32_t u, uint32_t v)
{
    if ((unsigned)x >= (unsigned)R.w || (unsigned)y >= (unsigned)R.h) return;
    uint32_t col = R.mode == M_FLAT ? R.colour
                 : R.mode == M_GOURAUD ? R.pal[(u >> 16) & 255]
                 : R.pal[R.tex[((v >> 8) & 0xff00) + ((u >> 16) & 255)]];
    uint32_t *p = R.px + y * R.stride + x;
    *p = mix32(*p, col, alpha);
}

/* edge a-b of a triangle whose third corner is c: the pixels just outside it */
static void aa_edge(const rvert *a, const rvert *b, const rvert *c)
{
    int dx = b->x - a->x, dy = b->y - a->y;
    int side = dx * (c->y - a->y) - dy * (c->x - a->x);
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (!side) return;
    if (adx >= ady) {            /* shallow: one pixel in each column */
        if (dx < 0) { const rvert *t = a; a = b; b = t; dx = -dx; dy = -dy; side = -side; }
        int m = ratio(dy, dx, 16);
        int x0 = ceil4(a->x), x1 = ceil4(b->x), pre = (x0 << 4) - a->x;
        int32_t du = adx < 16 ? 0 : ratio(b->u - a->u, dx, 12), dv = adx < 16 ? 0 : ratio(b->v - a->v, dx, 12);
        int32_t ye = (a->y << 12) + ((pre * m) >> 4);
        uint32_t u = ((uint32_t)a->u << 8) + (uint32_t)((pre * du) >> 4), v = ((uint32_t)a->v << 8) + (uint32_t)((pre * dv) >> 4);
        for (int x = x0; x < x1; x++, ye += m, u += (uint32_t)du, v += (uint32_t)dv) {
            int j = (ye + 0xffff) >> 16;
            if (side > 0) aa_plot(x, j - 1, (unsigned)((j << 16) - ye) >> 8, u, v);          /* inside is below */
            else aa_plot(x, j, (unsigned)(ye - ((j - 1) << 16)) >> 8, u, v);
        }
    } else {                     /* steep: one pixel in each row */
        if (dy < 0) { const rvert *t = a; a = b; b = t; dx = -dx; dy = -dy; side = -side; }
        int m = ratio(dx, dy, 16);
        int y0 = ceil4(a->y), y1 = ceil4(b->y), pre = (y0 << 4) - a->y;
        int32_t du = ady < 16 ? 0 : ratio(b->u - a->u, dy, 12), dv = ady < 16 ? 0 : ratio(b->v - a->v, dy, 12);
        int32_t xe = (a->x << 12) + ((pre * m) >> 4);
        uint32_t u = ((uint32_t)a->u << 8) + (uint32_t)((pre * du) >> 4), v = ((uint32_t)a->v << 8) + (uint32_t)((pre * dv) >> 4);
        for (int y = y0; y < y1; y++, xe += m, u += (uint32_t)du, v += (uint32_t)dv) {
            int i = (xe + 0xffff) >> 16;
            if (side < 0) aa_plot(i - 1, y, (unsigned)((i << 16) - xe) >> 8, u, v);          /* inside is to the right */
            else aa_plot(i, y, (unsigned)(xe - ((i - 1) << 16)) >> 8, u, v);
        }
    }
}

/* ---------- a frame of one mesh ---------- */

void mesh_draw(t48_surface *s, const mesh *m, const pose *p, const material *mat, rect *box)
{
    int sx = fsin(p->ax), cx = fcos(p->ax), sy = fsin(p->ay), cy = fcos(p->ay), sz = fsin(p->az), cz = fcos(p->az);
    int M[3][3];   /* turn about x, then y, then z. Q14 */
    M[0][0] = (cz * cy) >> 14;
    M[0][1] = ((cz * ((sy * sx) >> 14)) - sz * cx) >> 14;
    M[0][2] = ((cz * ((sy * cx) >> 14)) + sz * sx) >> 14;
    M[1][0] = (sz * cy) >> 14;
    M[1][1] = ((sz * ((sy * sx) >> 14)) + cz * cx) >> 14;
    M[1][2] = ((sz * ((sy * cx) >> 14)) - cz * sx) >> 14;
    M[2][0] = -sy;
    M[2][1] = (cy * sx) >> 14;
    M[2][2] = (cy * cx) >> 14;

    box->x0 = box->y0 = box->x1 = box->y1 = 0;
    if (m->nv > MAXV || m->nt > MAXT || p->z - m->radius < 1024) return;
    R.px = s->px; R.stride = s->stride; R.w = s->w; R.h = s->h;
    R.mode = mat->mode; R.pal = mat->pal; R.tex = mat->tex;

    /* the light as the mesh sees it, so each corner needs one dot product */
    int lx = (M[0][0] * mat->lx + M[1][0] * mat->ly + M[2][0] * mat->lz) >> 14;
    int ly = (M[0][1] * mat->lx + M[1][1] * mat->ly + M[2][1] * mat->lz) >> 14;
    int lz = (M[0][2] * mat->lx + M[1][2] * mat->ly + M[2][2] * mat->lz) >> 14;

    int f16 = p->focal * 16, ox = (s->w / 2) * 16, oy = (s->h / 2) * 16;
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30), minz = 1 << 30, maxz = 0;
    for (int i = 0; i < m->nv; i++) {
        int x = m->v[i][0], y = m->v[i][1], z = m->v[i][2];
        int vx = ((M[0][0] * x + M[0][1] * y + M[0][2] * z) >> 14) + p->x;
        int vy = ((M[1][0] * x + M[1][1] * y + M[1][2] * z) >> 14) + p->y;
        int z1 = ((M[2][0] * x + M[2][1] * y + M[2][2] * z) >> 14) + p->z;
        uint32_t rz = 0xffffffffu / (uint32_t)z1;
        int px = ox + (int32_t)(((int64_t)vx * f16 * rz) >> 32);
        int py = oy + (int32_t)(((int64_t)vy * f16 * rz) >> 32);
        sv[i].x = (int16_t)clampi(px, -32000, 32000); sv[i].y = (int16_t)clampi(py, -32000, 32000); vz[i] = z1;
        if (px < minx) minx = px;
        if (px > maxx) maxx = px;
        if (py < miny) miny = py;
        if (py > maxy) maxy = py;
        if (z1 < minz) minz = z1;
        if (z1 > maxz) maxz = z1;
        if (mat->mode == M_GOURAUD) {
            int d = (m->n[i][0] * lx + m->n[i][1] * ly + m->n[i][2] * lz) >> 14;
            sv[i].u = mat->shade[clampi((d + 16384) >> 7, 0, 255)];
            sv[i].v = 0;
        } else if (mat->mode == M_ENV) {
            int nx = m->n[i][0], ny = m->n[i][1], nz = m->n[i][2];
            sv[i].u = (uint16_t)(32768 + (((M[0][0] * nx + M[0][1] * ny + M[0][2] * nz) >> 14) * 126 >> 6));
            sv[i].v = (uint16_t)(32768 + (((M[1][0] * nx + M[1][1] * ny + M[1][2] * nz) >> 14) * 126 >> 6));
        } else {
            sv[i].u = sv[i].v = 0;
        }
    }
    /* far outside the screen the edge sums would overflow: such a frame is not drawn */
    if (minx < -400 * 16 || miny < -400 * 16 || maxx > (s->w + 400) * 16 || maxy > (s->h + 400) * 16) return;

    /* which triangles face the eye, and how far each is */
    int range = (maxz - minz) * 3 + 1;
    int kz = ((NBUCKET - 1) << 16) / range;
    memset(bcount, 0, sizeof bcount);
    for (int i = 0; i < m->nt; i++) {
        const rvert *a = &sv[m->t[i][0]], *b = &sv[m->t[i][1]], *c = &sv[m->t[i][2]];
        int area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        tstate[i] = area < 0;
        if (area >= 0) continue;
        int zs = vz[m->t[i][0]] + vz[m->t[i][1]] + vz[m->t[i][2]] - 3 * minz;
        int bk = NBUCKET - 1 - ((zs * kz) >> 16);      /* far first */
        tbucket[i] = (uint16_t)bk;
        bcount[bk + 1]++;
    }
    for (int i = 1; i <= NBUCKET; i++) bcount[i] = (uint16_t)(bcount[i] + bcount[i - 1]);
    int nvis = bcount[NBUCKET];
    for (int i = 0; i < m->nt; i++)
        if (tstate[i]) torder[bcount[tbucket[i]]++] = (uint16_t)i;

    int creases = mat->mode == M_FLAT;
    for (int k = 0; k < nvis; k++) {
        int i = torder[k];
        const uint16_t *t = m->t[i];
        if (mat->mode == M_FLAT) {
            int nx = m->fn[i][0], ny = m->fn[i][1], nz = m->fn[i][2];
            R.colour = mat->face((M[0][0] * nx + M[0][1] * ny + M[0][2] * nz) >> 14,
                                 (M[1][0] * nx + M[1][1] * ny + M[1][2] * nz) >> 14,
                                 (M[2][0] * nx + M[2][1] * ny + M[2][2] * nz) >> 14);
        }
        tri_fill(&sv[t[0]], &sv[t[1]], &sv[t[2]]);
        tstate[i] = 2;
        for (int e = 0; e < 3; e++) {
            int st = tstate[m->adj[i][e]];
            /* an outline: the far side faces away. A crease between two flat faces: the far one is drawn already */
            if (st == 0 || (creases && st == 2))
                aa_edge(&sv[t[e]], &sv[t[e == 2 ? 0 : e + 1]], &sv[t[e == 0 ? 2 : e - 1]]);
        }
    }
    box->x0 = (minx >> 4) - 2; box->y0 = (miny >> 4) - 2;
    box->x1 = (maxx >> 4) + 3; box->y1 = (maxy >> 4) + 3;
    if (box->x0 < 0) box->x0 = 0;
    if (box->y0 < 0) box->y0 = 0;
    if (box->x1 > s->w) box->x1 = s->w;
    if (box->y1 > s->h) box->y1 = s->h;
}

/* ---------- meshes, made at start ---------- */

static void mesh_alloc(mesh *m, int nv, int nt)
{
    if (!recip[1]) for (int i = 1; i < NRECIP; i++) recip[i] = (1u << 28) / (unsigned)i;
    m->nv = nv; m->nt = nt;
    m->v = calloc((size_t)nv, sizeof *m->v);
    m->n = calloc((size_t)nv, sizeof *m->n);
    m->t = calloc((size_t)nt, sizeof *m->t);
    m->adj = calloc((size_t)nt, sizeof *m->adj);
    m->fn = calloc((size_t)nt, sizeof *m->fn);
}

static void put(int16_t *d, double x, double y, double z, double k)
{
    d[0] = (int16_t)lrint(x * k); d[1] = (int16_t)lrint(y * k); d[2] = (int16_t)lrint(z * k);
}

/* face normals, every triangle wound the way its corners' normals say, and who is across each edge */
static void mesh_finish(mesh *m)
{
    int r2 = 0;
    for (int i = 0; i < m->nv; i++) {
        int d = m->v[i][0] * m->v[i][0] + m->v[i][1] * m->v[i][1] + m->v[i][2] * m->v[i][2];
        if (d > r2) r2 = d;
    }
    m->radius = (int)isqrt((unsigned)r2) + 1;
    for (int i = 0; i < m->nt; i++) {
        uint16_t *t = m->t[i];
        double e1[3], e2[3], n[3], s = 0;
        for (int k = 0; k < 3; k++) { e1[k] = m->v[t[1]][k] - m->v[t[0]][k]; e2[k] = m->v[t[2]][k] - m->v[t[0]][k]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        for (int k = 0; k < 3; k++) s += n[k] * (m->n[t[0]][k] + m->n[t[1]][k] + m->n[t[2]][k]);
        if (s < 0) { uint16_t x = t[1]; t[1] = t[2]; t[2] = x; for (int k = 0; k < 3; k++) n[k] = -n[k]; }
        double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) put(m->fn[i], n[0] / l, n[1] / l, n[2] / l, 16384);
    }
    /* edges into a hash by (from, to); the triangle across is the one with (to, from) */
    int size = 1;
    while (size < m->nt * 6) size <<= 1;
    uint32_t *keys = calloc((size_t)size, sizeof *keys);
    uint16_t *vals = calloc((size_t)size, sizeof *vals);
    for (int i = 0; i < m->nt; i++)
        for (int e = 0; e < 3; e++) {
            uint32_t k = ((uint32_t)m->t[i][e] << 16 | m->t[i][(e + 1) % 3]) + 1;
            unsigned h = (k * 2654435761u) & (unsigned)(size - 1);
            while (keys[h]) h = (h + 1) & (unsigned)(size - 1);
            keys[h] = k; vals[h] = (uint16_t)i;
        }
    for (int i = 0; i < m->nt; i++)
        for (int e = 0; e < 3; e++) {
            uint32_t k = ((uint32_t)m->t[i][(e + 1) % 3] << 16 | m->t[i][e]) + 1;
            unsigned h = (k * 2654435761u) & (unsigned)(size - 1);
            while (keys[h] && keys[h] != k) h = (h + 1) & (unsigned)(size - 1);
            m->adj[i][e] = keys[h] ? vals[h] : (uint16_t)i;
        }
    free(keys); free(vals);
}

/* a tube of nu x nv quads around a closed curve: ring i at centre c[i], spanned by n[i] and b[i] */
static void tube(mesh *m, int nu, int nv, double (*c)[3], double (*n)[3], double (*b)[3], double r, double scale)
{
    mesh_alloc(m, nu * nv, nu * nv * 2);
    for (int j = 0; j < nv; j++) {
        double cs = cos(j * 2 * M_PI / nv), sn = sin(j * 2 * M_PI / nv);
        for (int i = 0; i < nu; i++) {
            double d[3];
            for (int k = 0; k < 3; k++) d[k] = cs * n[i][k] + sn * b[i][k];
            put(m->v[i * nv + j], c[i][0] + r * d[0], c[i][1] + r * d[1], c[i][2] + r * d[2], scale);
            put(m->n[i * nv + j], d[0], d[1], d[2], 16384);
        }
    }
    int k = 0;
    for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
            int i1 = (i + 1) % nu, j1 = (j + 1) % nv;
            int a = i * nv + j, bb = i1 * nv + j, cc = i1 * nv + j1, d = i * nv + j1;
            /* split each quad the other way from its neighbours: no grain to the shading */
            if ((i + j) & 1) {
                m->t[k][0] = (uint16_t)a; m->t[k][1] = (uint16_t)bb; m->t[k][2] = (uint16_t)cc; k++;
                m->t[k][0] = (uint16_t)a; m->t[k][1] = (uint16_t)cc; m->t[k][2] = (uint16_t)d; k++;
            } else {
                m->t[k][0] = (uint16_t)a; m->t[k][1] = (uint16_t)bb; m->t[k][2] = (uint16_t)d; k++;
                m->t[k][0] = (uint16_t)bb; m->t[k][1] = (uint16_t)cc; m->t[k][2] = (uint16_t)d; k++;
            }
        }
    mesh_finish(m);
}

static void norm3(double *v)
{
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;
}

static void knot_point(double u, int p, int q, double *o)
{
    double r = 1.0 + 0.42 * cos(q * u);
    o[0] = r * cos(p * u); o[1] = r * sin(p * u); o[2] = -0.42 * sin(q * u);
}

void mesh_knot(mesh *m, int p, int q, int nu, int nv, double tube_r)
{
    double (*c)[3] = malloc(sizeof *c * (size_t)nu), (*n)[3] = malloc(sizeof *n * (size_t)nu), (*b)[3] = malloc(sizeof *b * (size_t)nu);
    for (int i = 0; i < nu; i++) {
        double u = i * 2 * M_PI / nu, p2[3], t[3], s[3];
        knot_point(u, p, q, c[i]);
        knot_point(u + 0.01, p, q, p2);
        for (int k = 0; k < 3; k++) { t[k] = p2[k] - c[i][k]; s[k] = p2[k] + c[i][k]; }
        b[i][0] = t[1] * s[2] - t[2] * s[1]; b[i][1] = t[2] * s[0] - t[0] * s[2]; b[i][2] = t[0] * s[1] - t[1] * s[0];
        n[i][0] = b[i][1] * t[2] - b[i][2] * t[1]; n[i][1] = b[i][2] * t[0] - b[i][0] * t[2]; n[i][2] = b[i][0] * t[1] - b[i][1] * t[0];
        norm3(b[i]); norm3(n[i]);
    }
    tube(m, nu, nv, c, n, b, tube_r, 4096 / 1.7);
    free(c); free(n); free(b);
}

void mesh_torus(mesh *m, int nu, int nv, double tube_r)
{
    double (*c)[3] = malloc(sizeof *c * (size_t)nu), (*n)[3] = malloc(sizeof *n * (size_t)nu), (*b)[3] = malloc(sizeof *b * (size_t)nu);
    for (int i = 0; i < nu; i++) {
        double u = i * 2 * M_PI / nu;
        c[i][0] = cos(u); c[i][1] = sin(u); c[i][2] = 0;
        n[i][0] = cos(u); n[i][1] = sin(u); n[i][2] = 0;
        b[i][0] = 0; b[i][1] = 0; b[i][2] = 1;
    }
    tube(m, nu, nv, c, n, b, tube_r, 4096 / (1.0 + tube_r));
    free(c); free(n); free(b);
}

/* an icosahedron with a low pyramid raised on every face: sixty facets */
void mesh_gem(mesh *m, double spike)
{
    static const signed char ico[12][3] = {
        {-1, 2, 0}, {1, 2, 0}, {-1, -2, 0}, {1, -2, 0}, {0, -1, 2}, {0, 1, 2},
        {0, -1, -2}, {0, 1, -2}, {2, 0, -1}, {2, 0, 1}, {-2, 0, -1}, {-2, 0, 1} };
    static const unsigned char face[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1} };
    const double phi = 1.6180339887;
    double v[32][3];
    for (int i = 0; i < 12; i++) {
        for (int k = 0; k < 3; k++) v[i][k] = ico[i][k] == 2 ? phi : ico[i][k] == -2 ? -phi : ico[i][k];
        norm3(v[i]);
    }
    mesh_alloc(m, 32, 60);
    for (int f = 0; f < 20; f++) {
        double *o = v[12 + f];
        for (int k = 0; k < 3; k++) o[k] = v[face[f][0]][k] + v[face[f][1]][k] + v[face[f][2]][k];
        norm3(o);
        for (int k = 0; k < 3; k++) o[k] *= spike;
        for (int e = 0; e < 3; e++) {
            m->t[f * 3 + e][0] = face[f][e]; m->t[f * 3 + e][1] = face[f][(e + 1) % 3]; m->t[f * 3 + e][2] = (uint16_t)(12 + f);
        }
    }
    double big = spike > 1 ? spike : 1;
    for (int i = 0; i < 32; i++) {
        double d[3] = {v[i][0], v[i][1], v[i][2]};
        put(m->v[i], d[0], d[1], d[2], 4096 / big);
        norm3(d);
        put(m->n[i], d[0], d[1], d[2], 16384);
    }
    mesh_finish(m);
}
