// SPDX-License-Identifier: AGPL-3.0-or-later
/* demo: the 3D pipeline. Closed triangle meshes, turned and projected in fixed
 * point, sorted far to near, filled with sub-pixel correct edges, and their
 * outlines smoothed. */
#ifndef R3D_H
#define R3D_H

#include "demo.h"

typedef struct mesh {
    int nv, nt;
    int16_t (*v)[3];      /* corners, Q12 */
    int16_t (*n)[3];      /* their normals, Q14 */
    uint16_t (*t)[3];     /* triangles, outside seen anticlockwise */
    uint16_t (*adj)[3];   /* the triangle across each edge (corner i to i + 1) */
    int16_t (*fn)[3];     /* face normals, Q14 */
    int radius;           /* Q12: nothing is further from the middle */
} mesh;

void mesh_knot(mesh *m, int p, int q, int nu, int nv, double tube);
void mesh_torus(mesh *m, int nu, int nv, double tube);
void mesh_gem(mesh *m, double spike);

enum { M_FLAT, M_GOURAUD, M_ENV };

typedef struct material {
    int mode;
    const uint32_t *pal;    /* M_GOURAUD: 256 colours by brightness. M_ENV: the map's colours */
    const uint8_t *tex;     /* M_ENV: 256 x 256, looked up by the normal as the eye sees it */
    const uint16_t *shade;   /* M_GOURAUD: brightness (8.8, 1.0 to 254.0) by n.l, 256 steps from -1 to 1 */
    int lx, ly, lz;         /* M_GOURAUD: where the light is, Q14, as the eye sees it */
    /* M_FLAT: the colour of a face from its normal as the eye sees it (Q14) */
    uint32_t (*face)(int nx, int ny, int nz);
} material;

typedef struct pose {
    unsigned ax, ay, az;    /* turn about each axis, 65536 a turn */
    int x, y, z;            /* where its middle is, Q12; z is the distance from the eye */
    int focal;              /* pixels */
} pose;

/* Draw it. box gets what it covered (empty if nothing). */
void mesh_draw(t48_surface *s, const mesh *m, const pose *p, const material *mat, rect *box);

#endif
