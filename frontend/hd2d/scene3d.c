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
 *   the wall's own tiles. Other runs (posts, trunks, canopies, arches)
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
    uint8_t room[MAXC][MAXC];       /* components visible to the console camera */
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

float s3d_room_zoom(float zoom)
{
    int w = bounds[2] - bounds[0], h = bounds[3] - bounds[1];
    if (w <= 0 || h <= 0)
        return zoom;
    /* Leave one cell around the room. At most 1.25x avoids a dramatic
       camera jump while making the small-room preset stop looking shrunk. */
    float zx = 256.0f / (float)(w + 32), zy = 224.0f / (float)(h + 32);
    float fit = zx < zy ? zx : zy;
    if (fit > 1.25f)
        fit = 1.25f;
    return fit > zoom ? fit : zoom;
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

static int div_floor16(int v)
{
    return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

static int room_walkable(const build_ctx *c, int x, int y)
{
    return x >= 0 && y >= 0 && x < c->s->map_w && y < c->s->map_h && !c->empty[y][x] &&
           !c->solid[y][x];
}

/* Flood every walkable component the console camera actually shows. Then
   grow it by two cells through non-empty geometry, enough to retain walls,
   furniture and roof runs without pulling a separate room across the void. */
static void find_room(build_ctx *c)
{
    const ct_scene *s = c->s;
    int qx[MAXC * MAXC], qy[MAXC * MAXC], head = 0, tail = 0;
    int x0 = div_floor16(s->camera_x), y0 = div_floor16(s->camera_y);
    int x1 = div_floor16(s->camera_x + 255), y1 = div_floor16(s->camera_y + 223);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= s->map_w)
        x1 = s->map_w - 1;
    if (y1 >= s->map_h)
        y1 = s->map_h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (room_walkable(c, x, y) && !c->room[y][x]) {
                c->room[y][x] = 1;
                qx[tail] = x;
                qy[tail++] = y;
            }
    while (head < tail) {
        int x = qx[head], y = qy[head++];
        static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (!room_walkable(c, nx, ny) || c->room[ny][nx])
                continue;
            c->room[ny][nx] = 1;
            qx[tail] = nx;
            qy[tail++] = ny;
        }
    }
    if (!tail) {   /* An unusual map with no classified floor: preserve the old behaviour. */
        for (int y = 0; y < s->map_h; y++)
            for (int x = 0; x < s->map_w; x++)
                c->room[y][x] = (uint8_t)!c->empty[y][x];
        return;
    }
    for (int step = 0; step < 2; step++) {
        uint8_t next[MAXC][MAXC];
        memcpy(next, c->room, sizeof next);
        for (int y = 0; y < s->map_h; y++)
            for (int x = 0; x < s->map_w; x++) {
                if (!c->room[y][x])
                    continue;
                for (int yy = y - 1; yy <= y + 1; yy++)
                    for (int xx = x - 1; xx <= x + 1; xx++)
                        if (xx >= 0 && yy >= 0 && xx < s->map_w && yy < s->map_h &&
                            !c->empty[yy][xx])
                            next[yy][xx] = 1;
            }
        memcpy(c->room, next, sizeof c->room);
    }
}

int s3d_room_matches(const ct_scene *s)
{
    if (!ctx_ok || ctx.s != s || s->mode != CT_SCENE_FIELD)
        return 0;
    int x0 = div_floor16(s->camera_x), y0 = div_floor16(s->camera_y);
    int x1 = div_floor16(s->camera_x + 255), y1 = div_floor16(s->camera_y + 223);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= s->map_w)
        x1 = s->map_w - 1;
    if (y1 >= s->map_h)
        y1 = s->map_h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (room_walkable(&ctx, x, y)) {
                if (!ctx.room[y][x])
                    return 0;
            }
    /* During a transition the console camera can briefly show only void.
       Keep the last room until a walkable cell of the next one appears. */
    return 1;
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
    s3d_emit(c->m, tl, p->u, p->v, pack(L, e, p->normal, p->flags | extra), e & 0x4000,
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

static void find_runs(build_ctx *c)
{
    const ct_scene *s = c->s;
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
            c->solid[y][x] = cl->cls == CT_CELL_SOLID || cl->cls == CT_CELL_WALLISH;
            c->raised[y][x] = !empty && (c->solid[y][x] || (cl->flags & CT_CELL_OVERHEAD));
        }
    find_room(c);
    bounds[0] = bounds[1] = 1 << 20;
    bounds[2] = bounds[3] = 0;
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++)
            if (c->room[y][x] && !c->empty[y][x]) {
                if (x * 16 < bounds[0])
                    bounds[0] = x * 16;
                if (y * 16 < bounds[1])
                    bounds[1] = y * 16;
                if (x * 16 + 16 > bounds[2])
                    bounds[2] = x * 16 + 16;
                if (y * 16 + 16 > bounds[3])
                    bounds[3] = y * 16 + 16;
            }
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
    if (c->empty[y][x] || !c->room[y][x])
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
        if (r >= 0 && r < s->map_h && c->room[r][x] && !c->raised[r][x] && !c->empty[r][x])
            src = r;
    }
    /* Else the cell beside it, west or east (a post between two others). */
    for (int k = 0; k < 2 && src < 0; k++) {
        int nx = k == 0 ? x - 1 : x + 1;
        if (nx >= 0 && nx < s->map_w && c->room[y][nx] && !c->raised[y][nx] &&
            !c->empty[y][nx]) {
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
            int b = box_b[z][src];
            float X = (float)(x + 1) * 16;
            for (int k = lo; k < hi; k++) {
                int sy = b - k;   /* the wall cell at that height */
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
    mark_runs(c, hmax);
    /* Hidden rooms must not affect side-wall heights at the active room's
       boundary. The visible neighbor remains, so its closing face is made. */
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++)
            if (!c->room[y][x]) {
                box_h[y][x] = 0;
                box_b[y][x] = -1;
            }

    /* Two passes over the cells: floor quads first, then raised ones, so
       the shadow pass can draw only the second range. */
    static s3d_mesh tmp;
    tmp.nv = tmp.ni = 0;
    c->m = &tmp;
    for (int y = 0; y < s->map_h; y++)
        for (int x = 0; x < s->map_w; x++)
            if (!c->room[y][x])
                continue;
            else if (!cell_hook || !cell_hook(cell_hook_user, s, x, y, &tmp))
                emit_cell(c, x, y);
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
static void emit_piece(const ct_scene *s, s3d_mesh *m, int e, float foot_z, float base_y,
                       int foot_sy, float lift, float stretch)
{
    int x, y, size;
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

int s3d_build_sprites(const ct_scene *s, const s3d_params *p, s3d_mesh *m)
{
    m->nv = m->ni = m->ni_floor = 0;
    oom = 0;
    if (s->mode != CT_SCENE_FIELD || !s->oam || !s->vram)
        return 0;
    uint8_t owned[128] = {0};
    for (int k = 0; k < s->n_objects && k < CT_SCENE_MAX_OBJECTS; k++) {
        const ct_scene_object *o = &s->object[k];
        if (!o->n_pieces)
            continue;
        int foot_sy = (int)(o->y - s->camera_y);
        /* The sprite's lowest row stands on the floor, never under it. */
        int bottom = -100000;
        for (int j = 0; j < o->n_pieces; j++) {
            int x, y, size;
            if (oam_visible(s, o->piece[j], &x, &y, &size) && y + size > bottom)
                bottom = y + size;
        }
        if (bottom == -100000)
            continue;
        float lift = bottom > foot_sy ? (float)(bottom - foot_sy) : 0.0f;
        lift -= (float)o->z;
        if (lift < 0)
            lift = 0;
        /* Standing in a raised cell: the console drew it over the wall
           there if its priority beats the wall's tiles (always for
           priority 3; for less, unless they are overhead tiles). Then it
           stands in front of the wall, or on top of it, raised so its
           screen row stays where the console had it. */
        float foot_z = (float)o->y, base_y = 0.0f;
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
        if (sprite_hook && sprite_hook(sprite_hook_user, s, k, &im)) {
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
    }
    /* The rest (effects, markers): each stands at its own bottom edge. */
    for (int e = 0; e < 128; e++) {
        int x, y, size;
        if (owned[e] || !oam_visible(s, e, &x, &y, &size))
            continue;
        emit_piece(s, m, e, (float)(s->camera_y + y + size), 0.0f, y + size, 0.0f, p->stretch);
    }
    return oom ? -1 : m->ni / 6;
}
