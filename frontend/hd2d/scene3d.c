/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 3D diorama geometry (scene3d.h).
 *
 * Heights, which the scene does not have, are inferred per map column:
 *
 * - A cell is "raised" if it is blocked (solid or wall-ish) or has tiles
 *   drawn over sprites (overhead), and is not empty (no tile pixels in
 *   any layer: the void around a map).
 * - Each run of raised cells in a column, seen from the south, stands up
 *   from its south edge: its first hmax cells (fewer for a shorter run)
 *   become a wall facing the camera, cell k of the run from the bottom at
 *   height k..k+1 cells. The rest of the run lies flat at the wall's top,
 *   moved south by the wall's height, so its screen row (z - height) is
 *   where the original drew it: a roof, or the top of a hedge.
 * - A run blocked deeper than its wall is a box: where neighboring
 *   columns' boxes differ in height, side walls close it, textured with
 *   the box's top folded down over the edge (else the wall's own tiles).
 * - Blocked cells drawn only with the surrounding walkable ground's tiles
 *   stay flat (port: no blocks of grass). Other runs (posts, trunks, canopies, arches)
 *   stay thin cut-outs.
 * - Overhead tiles over walkable cells belong to the blocked run north of
 *   them (a canopy over its trunk, eaves behind a roof), and only those
 *   tiles rise: the ground under a canopy stays a floor. A long run of
 *   them with nothing blocked (railings, edges) stays on the floor.
 * - Floor the raised cells leave uncovered (behind a wall, north of its
 *   box) is filled with the ground of the cell north of the run.
 *
 * Every quad is an 8x8 tile, so a tile never stretches. */
#include "scene3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXC CT_SCENE_MAX_CELLS
#define HMAX_CAP 8

int s3d_cell_hooked(const ct_scene *s, int x, int y, s3d_mesh *m);   /* for scene3d_world.c */
static s3d_cell_hook cell_hook;
static void *cell_hook_user;

void s3d_set_cell_hook(s3d_cell_hook fn, void *user)
{
    cell_hook = fn;
    cell_hook_user = user;
}

/* scene3d_world.c's way to the hook. */
int s3d_cell_hooked(const ct_scene *s, int x, int y, s3d_mesh *m)
{
    return cell_hook && cell_hook(cell_hook_user, s, x, y, m);
}

static s3d_sprite_hook sprite_hook;
static void *sprite_hook_user;

void s3d_set_sprite_hook(s3d_sprite_hook fn, void *user)
{
    sprite_hook = fn;
    sprite_hook_user = user;
}

void s3d_mesh_free(s3d_mesh *m)
{
    free(m->v);
    free(m->idx);
    memset(m, 0, sizeof *m);
}

static int grow(s3d_mesh *m)
{
    int cap = m->cap ? m->cap * 2 : 4096;
    s3d_vertex *v = realloc(m->v, sizeof *v * 4 * (size_t)cap);
    if (!v)
        return -1;
    m->v = v;
    uint32_t *idx = realloc(m->idx, sizeof *idx * 6 * (size_t)cap);
    if (!idx)
        return -1;
    m->idx = idx;
    m->cap = cap;
    return 0;
}

static int oom;

void s3d_emit(s3d_mesh *m, const float tl[3], const float u[3], const float v[3], uint32_t tile,
              int hflip, int vflip)
{
    if (m->nv / 4 >= m->cap && grow(m)) {
        oom = 1;
        return;
    }
    s3d_vertex *q = &m->v[m->nv];
    for (int k = 0; k < 4; k++) {
        float a = (float)(k & 1), b = (float)(k >> 1);   /* tl tr bl br */
        q[k].x = tl[0] + 8 * (a * u[0] + b * v[0]);
        q[k].y = tl[1] + 8 * (a * u[1] + b * v[1]);
        q[k].z = tl[2] + 8 * (a * u[2] + b * v[2]);
        q[k].u = 8 * (hflip ? 1 - a : a);
        q[k].v = 8 * (vflip ? 1 - b : b);
        q[k].tile = tile;
    }
    static const uint32_t order[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 6; k++)
        m->idx[m->ni + k] = (uint32_t)m->nv + order[k];
    m->nv += 4;
    m->ni += 6;
}

int s3d_emit_corners(s3d_mesh *m, float c[4][3], uint32_t tile, int hflip, int vflip)
{
    if (m->nv / 4 >= m->cap && grow(m)) {
        oom = 1;
        return -1;
    }
    s3d_vertex *q = &m->v[m->nv];
    for (int k = 0; k < 4; k++) {
        float a = (float)(k & 1), b = (float)(k >> 1);   /* tl tr bl br */
        q[k].x = c[k][0];
        q[k].y = c[k][1];
        q[k].z = c[k][2];
        q[k].u = 8 * (hflip ? 1 - a : a);
        q[k].v = 8 * (vflip ? 1 - b : b);
        q[k].tile = tile;
    }
    static const uint32_t order[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 6; k++)
        m->idx[m->ni + k] = (uint32_t)m->nv + order[k];
    m->nv += 4;
    m->ni += 6;
    return 0;
}

/* ---- the map ---- */

typedef struct {
    const ct_scene *s;
    s3d_mesh *m;
    uint8_t empty[MAXC][MAXC];      /* [y][x]: no pixels in any layer */
    uint8_t raised[MAXC][MAXC];
    int8_t run_a[MAXC][MAXC], run_b[MAXC][MAXC], run_h[MAXC][MAXC];   /* -1: not in a run */
    uint8_t solid[MAXC][MAXC];
    uint8_t boxed[MAXC][MAXC];      /* its floor is inside a box */
    uint8_t hooked[MAXC][MAXC];     /* the cell hook drew it (or nothing) */
    float card_shift;               /* port: s3d_params.card_shift (cards) */
    uint8_t ground_ok[MAXC][MAXC];  /* port: per quarter (bit q): the back layer's tile there
                                       has pixels and stays down (cards' ground fill) */
} build_ctx;

static build_ctx ctx;   /* the last build's, also for placing sprites */
static int ctx_ok;

/* Per world cell (x, z), z up to MAXC + HMAX_CAP: the box top there (px)
   and the run's bottom row, for the side walls. */
static int16_t box_h[MAXC + HMAX_CAP][MAXC];
static int8_t box_b[MAXC + HMAX_CAP][MAXC];

/* Cells with pixels, from the last build: [x0, x1) x [z0, z1), px. */
static int bounds[4];

void s3d_map_bounds(int *x0, int *z0, int *x1, int *z1)
{
    *x0 = bounds[0];
    *z0 = bounds[1];
    *x1 = bounds[2];
    *z1 = bounds[3];
}

int s3d_height_at(int x, int z)
{
    return x < 0 || x >= MAXC || z < 0 || z >= MAXC + HMAX_CAP ? 0 : box_h[z][x];
}

static const ct_scene_layer *layer_of(const ct_scene *s, int l)
{
    return s->layer[l].present && s->layer[l].tiles ? &s->layer[l] : NULL;
}

static uint16_t entry_at(const ct_scene_layer *L, int x, int y, int q)
{
    return L->tiles[(2 * y + (q >> 1)) * CT_SCENE_STRIDE + 2 * x + (q & 1)];
}

static uint32_t tile_addr(const ct_scene_layer *L, uint16_t e)
{
    return (L->chr_base + (e & 0x3FFu) * (L->bpp == 2 ? 8u : 16u)) & S3D_ADDR_MASK;
}

static int tile_empty(const ct_scene *s, const ct_scene_layer *L, uint16_t e)
{
    uint32_t a = tile_addr(L, e);
    int n = L->bpp == 2 ? 8 : 16;
    for (int k = 0; k < n; k++)
        if (s->vram[(a + (uint32_t)k) & S3D_ADDR_MASK])
            return 0;
    return 1;
}

static uint32_t pack(const ct_scene_layer *L, uint16_t e, int normal, uint32_t flags)
{
    return tile_addr(L, e) | (uint32_t)((e >> 10) & 7) << S3D_PAL_SHIFT |
           (L->bpp == 2 ? S3D_2BPP : 0) | (uint32_t)normal << S3D_NORMAL_SHIFT |
           (e & 0x2000 ? S3D_OVERHEAD : 0) | flags;
}

/* The layers drawn as geometry, back to front: BG2, then BG1. (The third
   layer is mostly color-math effects in map views; it stays out.) */
static const int draw_layers[2] = {1, 0};

/* One quarter (q) of a 16x16 placement: origin O of the cell's top-left,
   U and V its image axes, N its normal for the layer lift. */
typedef struct {
    float o[3], u[3], v[3];
    int normal;
    uint32_t flags;
} place;

static void emit_quarter(build_ctx *c, const ct_scene_layer *L, uint16_t e, const place *p, int q,
                         int layer_rank, uint32_t extra)
{
    static const float nrm[6][3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 0, -1},
                                    {0, 0, 1}};
    float lift = 0.06f * (float)layer_rank;   /* the front layer a hair in front */
    float tl[3];
    for (int k = 0; k < 3; k++)
        tl[k] = p->o[k] + 8 * (float)(q & 1) * p->u[k] + 8 * (float)(q >> 1) * p->v[k] +
                lift * nrm[p->normal][k];
    /* port: a hair larger on every side (0.02 px), so neighbouring tiles
       overlap instead of leaving hairline cracks the background shows
       through */
    const float eps = 0.02f;
    float u2[3], v2[3];
    for (int k = 0; k < 3; k++) {
        tl[k] -= eps * (p->u[k] + p->v[k]);
        u2[k] = p->u[k] * (1.0f + eps / 4.0f);
        v2[k] = p->v[k] * (1.0f + eps / 4.0f);
    }
    s3d_emit(c->m, tl, u2, v2, pack(L, e, p->normal, p->flags | extra), e & 0x4000,
             e & 0x8000);
}

static void floor_place(place *p, int x, int y)
{
    *p = (place){{(float)x * 16, 0, (float)y * 16}, {1, 0, 0}, {0, 0, 1}, S3D_N_UP, 0};
}

/* Where cell (x, y), in a run, goes. */
static void raised_place(const build_ctx *c, place *p, int x, int y)
{
    int b = c->run_b[y][x], h = c->run_h[y][x], k = b - y;
    if (k < h)   /* the wall, facing south */
        *p = (place){{(float)x * 16, (float)(k + 1) * 16, (float)(b + 1) * 16},
                     {1, 0, 0}, {0, -1, 0}, S3D_N_SOUTH, S3D_RAISED};
    else         /* on top */
        *p = (place){{(float)x * 16, (float)h * 16, (float)(y + h) * 16},
                     {1, 0, 0}, {0, 0, 1}, S3D_N_UP, S3D_RAISED};
}

/* port: a blocked cell drawn with nothing but the tiles of the walkable
   ground around it (an invisible wall at a path's edge, the grass between
   trees) is ground: it stays flat instead of rising as a block of grass.
   Tiles are compared by number, palette and flips; priority aside. */
static int ground_entry(const build_ctx *c, int li, uint16_t e, int x, int y)
{
    const ct_scene *s = c->s;
    const ct_scene_layer *L = layer_of(s, draw_layers[li]);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= s->map_w || ny >= s->map_h ||
                c->raised[ny][nx] || c->empty[ny][nx] ||
                (s->cells[ny * CT_SCENE_MAX_CELLS + nx].flags & CT_CELL_OVERHEAD))
                continue;
            for (int q = 0; q < 4; q++)
                if ((entry_at(L, nx, ny, q) & ~0x2000) == (e & ~0x2000))
                    return 1;
        }
    return 0;
}

static void flatten_ground(build_ctx *c)
{
    const ct_scene *s = c->s;
    static uint8_t flat[MAXC][MAXC];
    memset(flat, 0, sizeof flat);
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++) {
            if (!c->raised[y][x] || (s->cells[y * CT_SCENE_MAX_CELLS + x].flags & CT_CELL_OVERHEAD))
                continue;
            int ground = 1, any = 0;
            for (int li = 0; li < 2 && ground; li++) {
                const ct_scene_layer *L = layer_of(s, draw_layers[li]);
                for (int q = 0; L && q < 4 && ground; q++) {
                    uint16_t e = entry_at(L, x, y, q);
                    if (tile_empty(s, L, e))
                        continue;
                    any = 1;
                    ground = ground_entry(c, li, e, x, y);
                }
            }
            flat[y][x] = ground && any;
        }
    for (int y = 0; y < s->map_h; y++)   /* all at once: decided on the original heights */
        for (int x = 0; x < s->map_w; x++)
            if (flat[y][x])
                c->raised[y][x] = 0;
}

static void find_runs(build_ctx *c)
{
    const ct_scene *s = c->s;
    bounds[0] = bounds[1] = 1 << 20;
    bounds[2] = bounds[3] = 0;
    memset(c->run_a, -1, sizeof c->run_a);
    memset(c->run_b, -1, sizeof c->run_b);
    memset(c->run_h, -1, sizeof c->run_h);
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++) {
            const ct_scene_cell *cl = &s->cells[y * CT_SCENE_MAX_CELLS + x];
            int empty = 1;
            for (int li = 0; li < 2 && empty; li++) {
                const ct_scene_layer *L = layer_of(s, draw_layers[li]);
                for (int q = 0; L && q < 4 && empty; q++)
                    empty = tile_empty(s, L, entry_at(L, x, y, q));
            }
            c->empty[y][x] = (uint8_t)empty;
            if (!empty) {
                bounds[0] = x * 16 < bounds[0] ? x * 16 : bounds[0];
                bounds[1] = y * 16 < bounds[1] ? y * 16 : bounds[1];
                bounds[2] = x * 16 + 16 > bounds[2] ? x * 16 + 16 : bounds[2];
                bounds[3] = y * 16 + 16 > bounds[3] ? y * 16 + 16 : bounds[3];
            }
            c->solid[y][x] = cl->cls == CT_CELL_SOLID || cl->cls == CT_CELL_WALLISH;
            c->raised[y][x] = !empty && (c->solid[y][x] || (cl->flags & CT_CELL_OVERHEAD));
        }
    flatten_ground(c);
}

static void mark_runs(build_ctx *c, int hmax)
{
    const ct_scene *s = c->s;
    memset(box_h, 0, sizeof box_h);
    memset(box_b, -1, sizeof box_b);
    for (int x = 0; x < s->map_w; x++)
        for (int y = s->map_h - 1; y >= 0; y--) {
            if (!c->raised[y][x])
                continue;
            /* Overhead tiles over walkable cells belong to a blocked run
               north of them (a canopy over its trunk, a roof's eaves), not
               south: a run that holds blocked cells starts with one. */
            int b = y;
            while (y > 0 && c->raised[y - 1][x] && (c->solid[y][x] || !c->solid[y - 1][x]))
                y--;
            int a = y, len = b - a + 1, h = len < hmax ? len : hmax, nsolid = 0;
            for (int r = a; r <= b; r++)
                nsolid += c->solid[r][x];
            /* Long runs of overhead tiles over walkable ground (railings,
               edges drawn over sprites) stay on the floor: standing them
               up would make walls the original never had. */
            if (!nsolid && len > hmax) {
                for (int r = a; r <= b; r++)
                    c->raised[r][x] = 0;
                continue;
            }
            for (int r = a; r <= b; r++) {
                c->run_a[r][x] = (int8_t)a;
                c->run_b[r][x] = (int8_t)b;
                c->run_h[r][x] = (int8_t)h;
            }
            /* The box under the top, closed by side walls: only where the
               run is solid deeper than its wall (a canopy over a short
               trunk is a cut-out with a lid, not a block). */
            for (int z = a + h; z <= b && nsolid > h; z++) {
                c->boxed[z][x] = 1;
                box_h[z][x] = (int16_t)(h * 16);
                box_b[z][x] = (int8_t)b;
            }
        }
}

/* The cell's own quads: floor, raised, or both (a walkable overhead cell). */
static void emit_cell(build_ctx *c, int x, int y)
{
    const ct_scene *s = c->s;
    if (c->empty[y][x])
        return;
    int in_run = c->run_b[y][x] >= 0, solid = c->solid[y][x];
    place fl, up;
    floor_place(&fl, x, y);
    if (in_run)
        raised_place(c, &up, x, y);
    int floor_drawn[4] = {0};
    for (int li = 0; li < 2; li++) {
        int l = draw_layers[li];
        const ct_scene_layer *L = layer_of(s, l);
        for (int q = 0; L && q < 4; q++) {
            uint16_t e = entry_at(L, x, y, q);
            if (tile_empty(s, L, e))
                continue;
            int rise = in_run && (solid || (e & 0x2000));
            /* The back layer's floor is opaque: its color 0 is the backdrop. */
            uint32_t base = !rise && li == 0 ? S3D_OPAQUE : 0;
            if (!rise)
                floor_drawn[q] = 1;
            emit_quarter(c, L, e, rise ? &up : &fl, q, li, base);
        }
    }
    /* Ground under raised tiles that no box covers: the ground north of the
       run, else south of it. */
    if (!in_run || c->boxed[y][x])
        return;
    int src = -1, sx = x;
    for (int k = 0; k < 2 && src < 0; k++) {
        int r = k == 0 ? c->run_a[y][x] - 1 : c->run_b[y][x] + 1;
        if (r >= 0 && r < s->map_h && !c->raised[r][x] && !c->empty[r][x])
            src = r;
    }
    /* Else the cell beside it, west or east (a post between two others). */
    for (int k = 0; k < 2 && src < 0; k++) {
        int nx = k == 0 ? x - 1 : x + 1;
        if (nx >= 0 && nx < s->map_w && !c->raised[y][nx] && !c->empty[y][nx]) {
            src = y;
            sx = nx;
        }
    }
    if (src < 0)
        return;
    for (int li = 0; li < 2; li++) {
        const ct_scene_layer *L = layer_of(s, draw_layers[li]);
        for (int q = 0; L && q < 4; q++) {
            if (floor_drawn[q])
                continue;
            uint16_t e = entry_at(L, sx, src, q);
            if (!tile_empty(s, L, e) && !(e & 0x2000))
                emit_quarter(c, L, e, &fl, q, li, li == 0 ? S3D_OPAQUE : 0);
        }
    }
}

/* port: layered cards (s3d_params.cards). Like the 3DS ports that give
   each 2D layer its own depth instead of building models: the back layer
   is the ground, the front layer a hair above it, and every tile the
   console drew over sprites (canopies, roofs, arches) one flat card
   S3D_CARD_OVER px up, moved south as far so that from the usual camera
   it sits where the original drew it. Nothing is stood up or stretched,
   so the art keeps its shapes; the depth shows as parallax when the
   camera moves, and the high cards cast their shadows on the ground. */
static void emit_card_cell(build_ctx *c, int x, int y)
{
    const ct_scene *s = c->s;
    if (c->empty[y][x])
        return;
    for (int li = 0; li < 2; li++) {
        const ct_scene_layer *L = layer_of(s, draw_layers[li]);
        for (int q = 0; L && q < 4; q++) {
            uint16_t e = entry_at(L, x, y, q);
            if (tile_empty(s, L, e))
                continue;
            int over = (e & 0x2000) != 0;
            float h = over ? S3D_CARD_OVER : li == 1 ? S3D_CARD_FRONT : 0.0f;
            place p = {{(float)x * 16, h, (float)y * 16 + h * c->card_shift}, {1, 0, 0},
                       {0, 0, 1}, S3D_N_UP, over ? S3D_RAISED : 0};
            emit_quarter(c, L, e, &p, q, li, !over && li == 0 ? S3D_OPAQUE : 0);
            if (over && li == 0) {
                /* The ground it lifted off: the back layer's commonest tile
                   (of this quarter) that stays down in the cells around it,
                   the paving or grass that repeats; a one-off neighbour
                   (another part of the same tent) showed as a ghost copy. */
                uint16_t g = 0, cand[49];
                int cnt[49], nc = 0, found = 0, best = 1;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= s->map_w || ny >= s->map_h ||
                            !(c->ground_ok[ny][nx] & (1u << q)))
                            continue;
                        uint16_t ne = entry_at(L, nx, ny, q);
                        int k = 0;
                        while (k < nc && cand[k] != ne)
                            k++;
                        if (k == nc) {
                            cand[nc] = ne;
                            cnt[nc++] = 0;
                        }
                        if (++cnt[k] > best) {
                            best = cnt[k];
                            g = ne;
                            found = 1;
                        }
                    }
                if (found) {
                    place f;
                    floor_place(&f, x, y);
                    emit_quarter(c, L, g, &f, q, li, S3D_OPAQUE);
                }
            }
        }
    }
}

/* Side walls where neighboring columns' boxes differ in height. */
static void emit_sides(build_ctx *c)
{
    const ct_scene *s = c->s;
    for (int z = 0; z < s->map_h + HMAX_CAP; z++)
        for (int x = -1; x < s->map_w; x++) {
            int h1 = x >= 0 ? box_h[z][x] : 0, h2 = x + 1 < s->map_w ? box_h[z][x + 1] : 0;
            if (h1 == h2)
                continue;
            int east = h1 > h2;               /* column x's east face, else x+1's west face */
            int src = east ? x : x + 1, lo = (east ? h2 : h1) / 16, hi = (east ? h1 : h2) / 16;
            int b = box_b[z][src], hc = (east ? h1 : h2) / 16, a = c->run_a[b][src];
            float X = (float)(x + 1) * 16;
            for (int k = lo; k < hi; k++) {
                /* port: the box's own top, folded down over its edge: the
                   top cell lying at this row (y = z - hc), a row further
                   south for each step down, so the side continues the top's
                   art instead of repeating the front wall's tiles along the
                   whole depth of the box (streaks down canopies). Else, or
                   off the run, the front wall's cell at that height. */
                int sy = z - hc - (hc - 1 - k);
                if (sy < a || sy > b - hc || c->empty[sy][src] || c->hooked[sy][src])
                    sy = b - k;   /* the wall cell at that height */
                if (sy < 0 || c->empty[sy][src] || c->hooked[sy][src])
                    continue;
                place p = east ? (place){{X, (float)(k + 1) * 16, (float)(z + 1) * 16},
                                         {0, 0, -1}, {0, -1, 0}, S3D_N_EAST, S3D_RAISED}
                               : (place){{X, (float)(k + 1) * 16, (float)z * 16},
                                         {0, 0, 1}, {0, -1, 0}, S3D_N_WEST, S3D_RAISED};
                for (int li = 0; li < 2; li++) {
                    const ct_scene_layer *L = layer_of(s, draw_layers[li]);
                    for (int q = 0; L && q < 4; q++) {
                        uint16_t e = entry_at(L, src, sy, q);
                        if (!tile_empty(s, L, e))
                            emit_quarter(c, L, e, &p, q, li, li == 0 ? S3D_OPAQUE : 0);
                    }
                }
            }
        }
}

int s3d_build_map(const ct_scene *s, const s3d_params *p, s3d_mesh *m)
{
    build_ctx *c = &ctx;
    memset(c, 0, sizeof *c);
    c->s = s;
    ctx_ok = 0;
    m->nv = m->ni = m->ni_floor = 0;
    oom = 0;
    if (s->mode != CT_SCENE_FIELD || !s->cells || !s->vram || s->map_w > MAXC ||
        s->map_h > MAXC)
        return 0;
    int hmax = p->hmax < 1 ? 1 : p->hmax > HMAX_CAP ? HMAX_CAP : p->hmax;
    find_runs(c);
    c->card_shift = p->card_shift > 0.0f ? p->card_shift : 1.0f;
    if (p->cards) {
        const ct_scene_layer *B = layer_of(s, draw_layers[0]);
        for (int y = 0; y < s->map_h; y++)
            for (int x = 0; x < s->map_w; x++)
                for (int q = 0; B && q < 4; q++) {
                    uint16_t e = entry_at(B, x, y, q);
                    if (!(e & 0x2000) && !tile_empty(s, B, e))
                        c->ground_ok[y][x] |= (uint8_t)(1u << q);
                }
        memset(box_h, 0, sizeof box_h);
        memset(box_b, -1, sizeof box_b);
    } else
        mark_runs(c, hmax);

    /* Two passes over the cells: floor quads first, then raised ones, so
       the shadow pass can draw only the second range. */
    static s3d_mesh tmp;
    tmp.nv = tmp.ni = 0;
    c->m = &tmp;
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++)
            if (!cell_hook || !cell_hook(cell_hook_user, s, x, y, &tmp)) {
                if (p->cards)
                    emit_card_cell(c, x, y);
                else
                    emit_cell(c, x, y);
            }
            else
                c->hooked[y][x] = 1;
    /* tmp holds every quad in cell order: copy the floor ones, then the
       raised ones (a hook's quads sort by their S3D_RAISED flag too). */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < tmp.nv; k += 4) {
            int raised = (tmp.v[k].tile & S3D_RAISED) != 0;
            if (raised != pass)
                continue;
            if (m->nv / 4 >= m->cap && grow(m)) {
                oom = 1;
                break;
            }
            memcpy(&m->v[m->nv], &tmp.v[k], sizeof *m->v * 4);
            static const uint32_t order[6] = {0, 1, 2, 2, 1, 3};
            for (int j = 0; j < 6; j++)
                m->idx[m->ni + j] = (uint32_t)m->nv + order[j];
            m->nv += 4;
            m->ni += 6;
        }
        if (pass == 0)
            m->ni_floor = m->ni;
    }
    c->m = m;
    if (!p->cards)
        emit_sides(c);
    ctx_ok = 1;
    return oom ? -1 : m->ni / 6;
}

/* ---- sprites ---- */

static int oam_visible(const ct_scene *s, int e, int *x, int *y, int *size)
{
    uint16_t w0 = s->oam[2 * e];
    unsigned hi = s->oam[0x100 + e / 8] >> (2 * (e % 8));
    *size = s->obj_size[(hi >> 1) & 1];
    *x = (w0 & 0xFF) | (hi & 1) << 8;
    if (*x >= 256)
        *x -= 512;
    *y = w0 >> 8;
    if (*y >= 224 && *y + *size <= 256)
        return 0;
    if (*y + *size > 256)
        *y -= 256;   /* wraps to the top */
    return *x > -*size && *x < 256;
}

/* One OAM entry as 8x8 quads standing upright at world z = foot_z: a
   screen row r is at height base_y + (foot_sy - r + lift) * stretch,
   foot_sy being the foot's screen row. */
/* port: the screen row under an OAM entry's lowest opaque pixel (its box
   bottom when it has none): sprites keep empty rows under the feet, and
   standing the box's bottom on the ground floated the character that far
   above its shadow. */
static int opaque_bottom(const ct_scene *s, int e, int y, int size)
{
    uint16_t w1 = s->oam[2 * e + 1];
    int vf = (w1 >> 15) & 1, n = size / 8;
    uint32_t base = s->obj_chr[(w1 >> 8) & 1];
    for (int r = size - 1; r >= 0; r--) {   /* screen rows, bottom up */
        int sr = vf ? size - 1 - r : r;       /* the row in the stored image */
        int ty = sr / 8, py = sr % 8;
        for (int tx = 0; tx < n; tx++) {
            int used = ((((w1 & 0xFF) >> 4) + ty) << 4) | (((w1 & 0xF) + tx) & 0xF);
            uint32_t a = (base + (uint32_t)used * 16) & S3D_ADDR_MASK;
            if (s->vram[(a + (uint32_t)py) & S3D_ADDR_MASK] ||
                s->vram[(a + 8u + (uint32_t)py) & S3D_ADDR_MASK])
                return y + r + 1;
        }
    }
    return y + size;
}

/* port: s3d_build_sprites runs twice: the entries under OAM priority 3
   (pass 0), then those over everything (pass 1); emit_piece skips the
   others. */
static int spr_pass = -1;
static int n_blobs;   /* port: contact shadows at the end of the last sprite mesh */
int s3d_sprite_blobs(void) { return n_blobs; }

static int prio3(const ct_scene *s, int e) { return ((s->oam[2 * (e & 127) + 1] >> 12) & 3) == 3; }

static void emit_piece(const ct_scene *s, s3d_mesh *m, int e, float foot_z, float base_y,
                       int foot_sy, float lift, float stretch)
{
    int x, y, size;
    if (spr_pass >= 0 && prio3(s, e) != spr_pass)
        return;
    if (!oam_visible(s, e, &x, &y, &size))
        return;
    uint16_t w1 = s->oam[2 * e + 1];
    int hf = (w1 >> 14) & 1, vf = (w1 >> 15) & 1, n = size / 8;
    uint32_t base = s->obj_chr[(w1 >> 8) & 1], pal = (w1 >> 9) & 7;
    const float U[3] = {1, 0, 0}, V[3] = {0, -stretch, 0};
    /* Lower OAM entries draw in front: a hair toward the camera. */
    float push = (float)(127 - e) * 0.004f;
    for (int ty = 0; ty < n; ty++)
        for (int tx = 0; tx < n; tx++) {
            int col = (hf ? n - 1 - tx : tx) * 8, row = (vf ? n - 1 - ty : ty) * 8;
            int used = ((((w1 & 0xFF) >> 4) + (row >> 3)) << 4) | (((w1 & 0xF) + (col >> 3)) & 0xF);
            uint32_t addr = (base + (uint32_t)used * 16) & S3D_ADDR_MASK;
            float h = (float)(foot_sy - (y + ty * 8)) + lift;   /* the tile's top, screen px */
            float tl[3] = {(float)(s->camera_x + x + tx * 8), base_y + h * stretch,
                           foot_z + push};
            s3d_emit(m, tl, U, V,
                     addr | pal << S3D_PAL_SHIFT | S3D_OBJ |
                         (uint32_t)S3D_N_SPRITE << S3D_NORMAL_SHIFT | S3D_RAISED,
                     hf, vf);
        }
}

static int build_sprites_pass(const ct_scene *s, const s3d_params *p, s3d_mesh *m)
{
    if (s->mode != CT_SCENE_FIELD || !s->oam || !s->vram)
        return 0;
    uint8_t owned[128] = {0};
    /* port: each object's screen box and foot, for the entries no object
       owns: one inside a box (a big character's separate arms) stands with
       that object instead of at its own bottom, which split it apart */
    int ob_n = 0, ob_box[CT_SCENE_MAX_OBJECTS][4], ob_foot[CT_SCENE_MAX_OBJECTS];
    float ob_z[CT_SCENE_MAX_OBJECTS], ob_base[CT_SCENE_MAX_OBJECTS];
    /* port: objects drawn as parts of one bigger one (a boss whose arms
       are objects of their own) stand at the biggest one's foot: each at
       its own bottom, the parts came out at other depths and heights, the
       figure disjointed. A part: its box mostly (60%) inside a bigger
       object's box, its bottom not below that one's. */
    int all_box[CT_SCENE_MAX_OBJECTS][4], all_ok[CT_SCENE_MAX_OBJECTS];
    for (int k = 0; k < s->n_objects && k < CT_SCENE_MAX_OBJECTS; k++) {
        const ct_scene_object *o = &s->object[k];
        int bx0 = 100000, by0 = 100000, bx1 = -100000, by1 = -100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (!oam_visible(s, o->piece[j], &x, &y, &size))
                continue;
            bx0 = x < bx0 ? x : bx0;
            by0 = y < by0 ? y : by0;
            bx1 = x + size > bx1 ? x + size : bx1;
            int ob = opaque_bottom(s, o->piece[j] & 127, y, size);
            by1 = ob > by1 ? ob : by1;
        }
        all_ok[k] = bx1 > bx0;
        all_box[k][0] = bx0;
        all_box[k][1] = by0;
        all_box[k][2] = bx1;
        all_box[k][3] = by1;
    }
    for (int k = 0; k < s->n_objects && k < CT_SCENE_MAX_OBJECTS; k++) {
        const ct_scene_object *o = &s->object[k];
        if (!o->n_pieces)
            continue;
        int group_bottom = -100000;
        if (all_ok[k]) {
            int *a = all_box[k], area = (a[2] - a[0]) * (a[3] - a[1]), best = 0;
            for (int j = 0; j < s->n_objects && j < CT_SCENE_MAX_OBJECTS; j++) {
                if (j == k || !all_ok[j])
                    continue;
                int *b = all_box[j], barea = (b[2] - b[0]) * (b[3] - b[1]);
                int ix = (a[2] < b[2] ? a[2] : b[2]) - (a[0] > b[0] ? a[0] : b[0]);
                int iy = (a[3] < b[3] ? a[3] : b[3]) - (a[1] > b[1] ? a[1] : b[1]);
                if (barea > area && barea > best && ix > 0 && iy > 0 && ix * iy * 10 >= area * 6 &&
                    b[3] >= a[3]) {
                    best = barea;
                    group_bottom = b[3];
                }
            }
        }
        int foot_sy = (int)(o->y - s->camera_y);
        /* The sprite's lowest row stands on the floor, never under it. */
        int bottom = -100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (oam_visible(s, o->piece[j], &x, &y, &size)) {
                int ob = opaque_bottom(s, o->piece[j] & 127, y, size);
                bottom = ob > bottom ? ob : bottom;
            }
        }
        if (bottom == -100000)
            continue;
        /* port: the foot from the sprite's own entries (the same frame's
           OAM), not the object's position: the game's copy of that can be
           a frame apart from what the console drew, and the difference
           made walkers bob up and down and split head from body. Only a
           jump's height (z) lifts it off the ground. */
        /* port: the object's height (z) isn't used: the cards' world is
           flat, so a character on a raised walkway (z > 0) stood above its
           own shadow; its feet stay on the row the console drew them. */
        int zj = 0;
        foot_sy = (group_bottom > bottom ? group_bottom : bottom) + zj;
        float lift = 0.0f;
        /* Standing in a raised cell: the console drew it over the wall
           there if its priority beats the wall's tiles (always for
           priority 3; for less, unless they are overhead tiles). Then it
           stands in front of the wall, or on top of it, raised so its
           screen row stays where the console had it. */
        float foot_z = (float)(s->camera_y + foot_sy), base_y = 0.0f;
        int cx = o->x / 16, cy = o->y / 16, prio = 0;
        for (int j = 0; j < o->n_pieces; j++) {
            int pr = (s->oam[2 * (o->piece[j] & 127) + 1] >> 12) & 3;
            prio = pr > prio ? pr : prio;
        }
        if (ctx_ok && cx >= 0 && cy >= 0 && cx < s->map_w && cy < s->map_h &&
            ctx.run_b[cy][cx] >= 0 &&
            (prio == 3 || !(s->cells[cy * CT_SCENE_MAX_CELLS + cx].flags & CT_CELL_OVERHEAD))) {
            int b = ctx.run_b[cy][cx], h = ctx.run_h[cy][cx];
            float up = b - cy < h ? (float)((b + 1) * 16 + 2 - o->y) : (float)(h * 16);
            foot_z += up;
            base_y = up;
        }
        s3d_sprite_image im;
        if (sprite_hook && (prio == 3) == spr_pass && sprite_hook(sprite_hook_user, s, k, &im)) {
            /* one quad for the whole picture, where its entries would be */
            float st = p->stretch, h = (float)(foot_sy - im.y) + lift;
            float tl[3] = {(float)(s->camera_x + im.x), base_y + h * st,
                           foot_z + (float)(127 - o->piece[0]) * 0.004f};
            const float U[3] = {(float)im.w / 8.0f, 0, 0}, V[3] = {0, -st * (float)im.h / 8.0f, 0};
            s3d_emit(m, tl, U, V,
                     (im.tile & (S3D_PACKED | S3D_ENTRY_MASK)) | S3D_OBJ |
                         (uint32_t)S3D_N_SPRITE << S3D_NORMAL_SHIFT | S3D_RAISED,
                     im.hflip, im.vflip);
            for (int j = 0; j < o->n_pieces; j++)
                owned[o->piece[j] & 127] = 1;
            continue;
        }
        for (int j = 0; j < o->n_pieces; j++) {
            emit_piece(s, m, o->piece[j], foot_z, base_y, foot_sy, lift, p->stretch);
            owned[o->piece[j] & 127] = 1;
        }
        int bx0 = 100000, by0 = 100000, bx1 = -100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (!oam_visible(s, o->piece[j], &x, &y, &size))
                continue;
            bx0 = x < bx0 ? x : bx0;
            by0 = y < by0 ? y : by0;
            bx1 = x + size > bx1 ? x + size : bx1;
        }
        ob_box[ob_n][0] = bx0;
        ob_box[ob_n][1] = by0;
        ob_box[ob_n][2] = bx1;
        ob_box[ob_n][3] = bottom;
        ob_foot[ob_n] = foot_sy;
        ob_z[ob_n] = foot_z;
        ob_base[ob_n++] = base_y;
    }
    /* The rest (effects, markers): each stands at its own bottom edge. */
    for (int e = 0; e < 128; e++) {
        int x, y, size;
        if (owned[e] || !oam_visible(s, e, &x, &y, &size))
            continue;
        int best = -1, best_area = 0;
        for (int k = 0; k < ob_n; k++) {
            int ix = (x + size < ob_box[k][2] + 8 ? x + size : ob_box[k][2] + 8) -
                     (x > ob_box[k][0] - 8 ? x : ob_box[k][0] - 8);
            int iy = (y + size < ob_box[k][3] ? y + size : ob_box[k][3]) -
                     (y > ob_box[k][1] - 8 ? y : ob_box[k][1] - 8);
            if (ix > 0 && iy > 0 && ix * iy > best_area) {
                best_area = ix * iy;
                best = k;
            }
        }
        if (best >= 0) {
            emit_piece(s, m, e, ob_z[best], ob_base[best], ob_foot[best], 0.0f, p->stretch);
            continue;
        }
        emit_piece(s, m, e, (float)(s->camera_y + y + size), 0.0f, y + size, 0.0f, p->stretch);
    }
    return 0;
}

/* The sprites, those under OAM priority 3 first: m->ni_floor indices are
   theirs (the renderer draws the map's overhead tiles between the two,
   as the console's priorities do). */
int s3d_build_sprites(const ct_scene *s, const s3d_params *p, s3d_mesh *m)
{
    m->nv = m->ni = m->ni_floor = 0;
    oom = 0;
    if (s->mode != CT_SCENE_FIELD || !s->oam || !s->vram)
        return 0;
    for (spr_pass = 0; spr_pass < 2; spr_pass++) {
        build_sprites_pass(s, p, m);
        if (spr_pass == 0)
            m->ni_floor = m->ni;
    }
    spr_pass = -1;
    /* port: a soft contact shadow under each character's feet (the
       renderer blends these in before the sprites, without depth writes),
       so walkers sit on the ground instead of floating over it. */
    n_blobs = 0;
    for (int k = 0; k < s->n_objects && k < CT_SCENE_MAX_OBJECTS; k++) {
        const ct_scene_object *o = &s->object[k];
        int x0 = 100000, x1 = -100000, bottom = -100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (!oam_visible(s, o->piece[j], &x, &y, &size))
                continue;
            x0 = x < x0 ? x : x0;
            x1 = x + size > x1 ? x + size : x1;
            int ob = opaque_bottom(s, o->piece[j] & 127, y, size);
            bottom = ob > bottom ? ob : bottom;
        }
        if (bottom == -100000 || x1 - x0 > 64)
            continue;
        /* a part of a bigger object (see build_sprites_pass): no shadow of its own */
        int part = 0, top = 100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (oam_visible(s, o->piece[j], &x, &y, &size) && y < top)
                top = y;
        }
        for (int k2 = 0; k2 < s->n_objects && k2 < CT_SCENE_MAX_OBJECTS && !part; k2++) {
            const ct_scene_object *b = &s->object[k2];
            if (k2 == k)
                continue;
            int bx0 = 100000, by0 = 100000, bx1 = -100000, by1 = -100000;
            for (int j = 0; j < b->n_pieces; j++) {
                int x, y, size;
                if (!oam_visible(s, b->piece[j], &x, &y, &size))
                    continue;
                bx0 = x < bx0 ? x : bx0;
                by0 = y < by0 ? y : by0;
                bx1 = x + size > bx1 ? x + size : bx1;
                int ob = opaque_bottom(s, b->piece[j] & 127, y, size);
                by1 = ob > by1 ? ob : by1;
            }
            int area = (x1 - x0) * (bottom - top), barea = (bx1 - bx0) * (by1 - by0);
            int ix = (x1 < bx1 ? x1 : bx1) - (x0 > bx0 ? x0 : bx0);
            int iy = (bottom < by1 ? bottom : by1) - (top > by0 ? top : by0);
            part = barea > area && ix > 0 && iy > 0 && ix * iy * 10 >= area * 6 && by1 >= bottom;
        }
        if (part)
            continue;
        int zj = 0;   /* (as above) */
        float w = (float)(x1 - x0) * 0.8f, d = w * 0.45f;
        float cx = (float)(s->camera_x + (x0 + x1) / 2), cz = (float)(s->camera_y + bottom + zj);
        float tl[3] = {cx - w * 0.5f, 0.05f, cz - d * 0.65f};
        const float U[3] = {w / 8.0f, 0, 0}, V[3] = {0, 0, d / 8.0f};
        s3d_emit(m, tl, U, V, S3D_OBJ | (uint32_t)S3D_N_WATER << S3D_NORMAL_SHIFT, 0, 0);
        n_blobs++;
    }
    return oom ? -1 : m->ni / 6;
}
