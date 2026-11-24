/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: asset packs in the scene (pack_scene.h, docs/ASSET-PACKS.md). */
#include "pack_scene.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif

#include "hd2d/scene3d.h"
#include "packs.h"
#include "pngio.h"

#define WS CT_SCENE_WORLD_STRIDE   /* the largest map side, cells */

/* ---- models ---- */

static struct {
    uint8_t hide[WS][WS];   /* [y][x]: a model's footprint with hide_cells */
    int any_hidden;
    hd2d_mvertex *v;
    uint32_t *idx;
    hd2d_mpart *part;
    hd2d_mtex *tex;
    int capv, capi, capp, capt;
    int warned;
} M;

typedef struct {
    int model;
    int x, y;   /* the footprint's top-left cell */
} instance;

static int grow(void **p, int *cap, long want, size_t size)
{
    if (want <= *cap)
        return 0;
    if (want > (1L << 26))
        return -1;
    long c = *cap ? *cap : 256;
    while (c < want)
        c *= 2;
    void *n = realloc(*p, (size_t)c * size);
    if (!n)
        return -1;
    *p = n;
    *cap = (int)c;
    return 0;
}

static uint16_t tile_word(const ct_scene *sc, int layer, int x, int y, int q)
{
    const ct_scene_layer *L = &sc->layer[layer];
    if (sc->mode == CT_SCENE_WORLD)
        return L->tiles[y * CT_SCENE_WORLD_STRIDE + x];
    return L->tiles[(2 * y + (q >> 1)) * CT_SCENE_STRIDE + 2 * x + (q & 1)];
}

/* The models on sc's map: by map id and cells, or every cell showing
   the tiles. At most max. */
static int find_instances(const ct_scene *sc, instance *out, int max)
{
    int nm, n = 0;
    const packs_model *pm = packs_models(&nm);
    int world = sc->mode == CT_SCENE_WORLD;
    for (int k = 0; k < nm && n < max; k++) {
        const packs_model *m = &pm[k];
        if (!m->primitives || (m->map >= 0 && (uint32_t)m->map != sc->map_id))
            continue;
        if (!m->n_tiles) {
            out[n++] = (instance){k, m->cell_x, m->cell_y};
            continue;
        }
        int l = m->tiles_layer;
        if (l < 0 || l >= CT_SCENE_LAYERS || !sc->layer[l].present || !sc->layer[l].tiles)
            continue;
        int words = world ? 1 : m->n_tiles;
        for (int y = 0; y < sc->map_h && n < max; y++)
            for (int x = 0; x < sc->map_w && n < max; x++) {
                int same = 1;
                for (int q = 0; q < words && same; q++)
                    same = tile_word(sc, l, x, y, q) == m->tiles[q];
                if (same)
                    out[n++] = (instance){k, x, y};
            }
    }
    return n;
}

static int hide_hook(void *user, const ct_scene *s, int x, int y, s3d_mesh *m)
{
    (void)user;
    (void)s;
    (void)m;
    return x >= 0 && y >= 0 && x < WS && y < WS && M.hide[y][x];
}

/* A model's placement: scale, rotate about x, y, then z (degrees), move. */
typedef struct {
    float r[9];   /* rotation, column-major */
    float s[3], t[3];
} xform;

static void xform_make(xform *f, const packs_model *m, const float at[3])
{
    float c[3], s[3];
    for (int k = 0; k < 3; k++) {
        float a = m->rotate[k] * 3.14159265f / 180.0f;
        c[k] = cosf(a);
        s[k] = sinf(a);
    }
    /* R = Rz * Ry * Rx */
    float rx[9] = {1, 0, 0, 0, c[0], s[0], 0, -s[0], c[0]};
    float ry[9] = {c[1], 0, -s[1], 0, 1, 0, s[1], 0, c[1]};
    float rz[9] = {c[2], s[2], 0, -s[2], c[2], 0, 0, 0, 1};
    float t[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            t[j * 3 + i] = 0;
            for (int k = 0; k < 3; k++)
                t[j * 3 + i] += ry[k * 3 + i] * rx[j * 3 + k];
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            f->r[j * 3 + i] = 0;
            for (int k = 0; k < 3; k++)
                f->r[j * 3 + i] += rz[k * 3 + i] * t[j * 3 + k];
        }
    for (int k = 0; k < 3; k++) {
        f->s[k] = m->scale[k];
        f->t[k] = at[k] + m->offset[k];
    }
}

static void xform_vertex(const xform *f, const packs_mvertex *in, hd2d_mvertex *o)
{
    float p[3], n[3], q[3] = {0, 0, 0}, nn[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        p[k] = in->pos[k] * f->s[k];
        n[k] = f->s[k] != 0 ? in->nrm[k] / f->s[k] : 0;   /* the inverse transpose */
    }
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            q[i] += f->r[k * 3 + i] * p[k];
            nn[i] += f->r[k * 3 + i] * n[k];
        }
    float l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    l = l > 1e-20f ? 1.0f / l : 0.0f;
    o->x = q[0] + f->t[0];
    o->y = q[1] + f->t[1];
    o->z = q[2] + f->t[2];
    o->nx = nn[0] * l;
    o->ny = nn[1] * l;
    o->nz = nn[2] * l;
    o->u = in->uv[0];
    o->v = in->uv[1];
    memcpy(o->rgba, in->rgba, 4);
}

int pack_scene_models(void *user, const ct_scene *sc, hd2d_models *out)
{
    (void)user;
    enum { MAX_INST = 4096 };
    static instance inst[MAX_INST];
    if (!sc || (sc->mode != CT_SCENE_FIELD && sc->mode != CT_SCENE_WORLD))
        return 0;
    int n = find_instances(sc, inst, MAX_INST);
    int cp = sc->cell_px ? sc->cell_px : 16;
    if (!out) {   /* before the map is built: the cells to hide */
        if (M.any_hidden)
            memset(M.hide, 0, sizeof M.hide);
        M.any_hidden = 0;
        for (int i = 0; i < n; i++) {
            const packs_model *m = &packs_models(&(int){0})[inst[i].model];
            if (!m->hide_cells)
                continue;
            for (int y = inst[i].y; y < inst[i].y + m->cells_h; y++)
                for (int x = inst[i].x; x < inst[i].x + m->cells_w; x++)
                    if (x >= 0 && y >= 0 && x < WS && y < WS)
                        M.hide[y][x] = 1, M.any_hidden = 1;
        }
        return 0;
    }
    memset(out, 0, sizeof *out);
    int nm;
    const packs_model *pm = packs_models(&nm);
    long nv = 0, ni = 0;
    int np = 0, nt = 0;
    /* model by model: its textures once, its vertices once per instance,
       then each of its parts' indices for every instance (one draw each) */
    for (int k = 0; k < nm; k++) {
        int first = -1, count = 0;
        for (int i = 0; i < n; i++)
            if (inst[i].model == k) {
                first = first < 0 ? i : first;
                count++;
            }
        if (!count)
            continue;
        const packs_mesh *mesh = packs_model_mesh(k);
        if (!mesh)
            continue;
        const packs_model *m = &pm[k];
        if (nv + (long)mesh->nv * count > (1L << 24) || ni + (long)mesh->ni * count > (1L << 26)) {
            if (!M.warned++)
                fprintf(stderr, "pack_scene: too many model triangles on map %u: some left out\n",
                        sc->map_id);
            continue;
        }
        if (grow((void **)&M.v, &M.capv, nv + (long)mesh->nv * count, sizeof *M.v) ||
            grow((void **)&M.idx, &M.capi, ni + (long)mesh->ni * count, sizeof *M.idx) ||
            grow((void **)&M.part, &M.capp, np + mesh->nparts, sizeof *M.part) ||
            grow((void **)&M.tex, &M.capt, nt + mesh->ntex, sizeof *M.tex))
            break;
        int tex0 = nt;
        for (int t = 0; t < mesh->ntex; t++)
            M.tex[nt++] = (hd2d_mtex){mesh->tex[t].rgba, mesh->tex[t].w, mesh->tex[t].h, m->linear};
        long v0 = nv;
        for (int i = first; i < n; i++) {
            if (inst[i].model != k)
                continue;
            /* the footprint's middle, on the ground */
            float at[3] = {((float)inst[i].x + 0.5f * (float)m->cells_w) * (float)cp, 0.0f,
                           ((float)inst[i].y + 0.5f * (float)m->cells_h) * (float)cp};
            if (sc->mode == CT_SCENE_WORLD)
                at[1] = s3d_world_height(at[0], at[2]);
            xform f;
            xform_make(&f, m, at);
            for (int j = 0; j < mesh->nv; j++)
                xform_vertex(&f, &mesh->v[j], &M.v[nv + j]);
            nv += mesh->nv;
        }
        for (int p = 0; p < mesh->nparts; p++) {
            const packs_mpart *mp = &mesh->part[p];
            hd2d_mpart *o = &M.part[np++];
            o->first = (int)ni;
            o->tex = mp->tex >= 0 ? tex0 + mp->tex : -1;
            o->cutout = mp->cutout;
            for (int c = 0; c < 4; c++)
                o->color[c] = mp->color[c] * m->color[c];
            long base = v0;
            for (int i = first; i < n; i++) {
                if (inst[i].model != k)
                    continue;
                for (int j = 0; j < mp->count; j++)
                    M.idx[ni + j] = (uint32_t)(base + mesh->idx[mp->first + j]);
                ni += mp->count;
                base += mesh->nv;
            }
            o->count = (int)(ni - o->first);
        }
    }
    out->v = M.v;
    out->idx = M.idx;
    out->part = M.part;
    out->tex = M.tex;
    out->nv = (int)nv;
    out->ni = (int)ni;
    out->nparts = np;
    out->ntex = nt;
    return (int)(ni / 3);
}

/* ---- poses ---- */

typedef struct {
    int obj, entry;
    uint32_t key;
    int w, h, hflip, vflip;
    int n;
    uint8_t oam[16];
} pose_use;

static struct {
    pose_use cur[CT_SCENE_MAX_OBJECTS], prev[CT_SCENE_MAX_OBJECTS];
    int ncur, nprev;
    uint8_t obj_size[2], prev_size[2];
} P;

static int export_on;
static void export_pose(const ct_scene *sc, int obj, const ct_scene_pose *p);
static void export_map(const ct_scene *sc);

static int where_on = -1;   /* CT_PACK_WHERE: say where the player is */

int pack_scene_wants_poses(void)
{
    if (where_on < 0)
        where_on = getenv("CT_PACK_WHERE") && *getenv("CT_PACK_WHERE") != '0';
    return packs_have_poses() || export_on || where_on;
}

void pack_scene_frame(const ct_scene *sc)
{
    memcpy(P.prev, P.cur, sizeof P.cur);
    P.nprev = P.ncur;
    memcpy(P.prev_size, P.obj_size, 2);
    P.ncur = 0;
    if (!sc || !pack_scene_wants_poses())
        return;
    int map = sc->mode == CT_SCENE_FIELD || sc->mode == CT_SCENE_WORLD;
    if (where_on > 0 && map && sc->player < sc->n_objects) {
        static unsigned t;   /* for placing models: once a second */
        const ct_scene_object *o = &sc->object[sc->player];
        int cp = sc->cell_px ? sc->cell_px : 16;
        if (t++ % 60 == 0)
            fprintf(stderr, "pack_scene: map 0x%03x, player at %d,%d px: cell %d,%d\n", sc->map_id,
                    o->x, o->y, o->x / cp, o->y / cp);
    }
    if (export_on && map)
        export_map(sc);
    if (sc->mode != CT_SCENE_FIELD)
        return;
    memcpy(P.obj_size, sc->obj_size, 2);
    int n;
    const packs_rect *e = packs_entries(&n);
    for (int k = 0; k < sc->n_objects && k < CT_SCENE_MAX_OBJECTS; k++) {
        ct_scene_pose p;
        if (ct_scene_pose_of(k, &p, NULL, 0))
            continue;
        if (export_on)
            export_pose(sc, k, &p);
        const packs_rect *r = packs_lookup_pose(p.key);
        const ct_scene_object *o = &sc->object[k];
        if (!r || !o->n_pieces)
            continue;
        pose_use *u = &P.cur[P.ncur++];
        u->obj = k;
        u->entry = (int)(r - e);
        u->key = p.key;
        u->w = p.w;
        u->h = p.h;
        u->hflip = p.hflip;
        u->vflip = p.vflip;
        u->n = o->n_pieces < 16 ? o->n_pieces : 16;
        memcpy(u->oam, o->piece, (size_t)u->n);
    }
}

/* The frame drawn now is the one the previous edge's scene described
   (its OAM was uploaded after that edge). */
void pack_scene_map_poses(void)
{
    packs_pose_place pl[CT_SCENE_MAX_OBJECTS];
    for (int k = 0; k < P.nprev; k++) {
        const pose_use *u = &P.prev[k];
        pl[k] = (packs_pose_place){u->entry, u->w, u->h, u->hflip, u->vflip, u->n, {0}};
        memcpy(pl[k].oam, u->oam, (size_t)u->n);
    }
    packs_set_poses(pl, P.nprev, P.prev_size);
}

static int sprite_hook(void *user, const ct_scene *s, int obj, s3d_sprite_image *out)
{
    (void)user;
    for (int k = 0; k < P.ncur; k++) {
        const pose_use *u = &P.cur[k];
        if (u->obj != obj)
            continue;
        int n, x, y, w, h;
        const packs_rect *e = packs_entries(&n);
        if (u->entry >= n || u->entry > (int)S3D_ENTRY_MASK ||
            packs_oam_box(s->oam, s->obj_size, u->oam, u->n, &x, &y, &w, &h) || w != u->w ||
            h != u->h)
            return 0;
        int fl = e[u->entry].flags;
        *out = (s3d_sprite_image){S3D_PACKED | (uint32_t)u->entry, x, y, w, h,
                                  (u->hflip != 0) ^ ((fl & PACKS_FLIP_H) != 0),
                                  (u->vflip != 0) ^ ((fl & PACKS_FLIP_V) != 0)};
        return 1;
    }
    return 0;
}

void pack_scene_attach(void)
{
    int nm;
    packs_models(&nm);
    hd2d_pack_models(nm ? pack_scene_models : NULL, NULL);
    s3d_set_cell_hook(nm ? hide_hook : NULL, NULL);
    s3d_set_sprite_hook(packs_have_poses() ? sprite_hook : NULL, NULL);
    P.ncur = P.nprev = 0;
}

/* ---- the sheet export ---- */

#define POSE_MAX 256   /* a pose's side, px (the 2D map's texels are 8 bits) */

typedef struct {
    uint32_t key, sheet;
    char name[24];
    int w, h, ox, oy;
    uint8_t *rgba;
} kept_pose;

static struct {
    char dir[3072];
    int scale;
    kept_pose *p;
    int n, cap;
    FILE *list;   /* poses.txt */
    uint32_t maps_seen[64];   /* map ids written: id + 1 */
    uint32_t map_gen[64];
} X;

static int mkdirs(const char *path)
{
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir_one(p) && errno != EEXIST)
                return -1;
            *s = '/';
        }
    return mkdir_one(p) && errno != EEXIST ? -1 : 0;
}

static int kept_find(uint32_t key)
{
    for (int k = 0; k < X.n; k++)
        if (X.p[k].key == key)
            return k;
    return -1;
}

static int keep(uint32_t key, uint32_t sheet, const char *name, int w, int h, int ox, int oy,
                uint8_t *rgba)
{
    if (X.n == X.cap) {
        int c = X.cap ? X.cap * 2 : 256;
        kept_pose *n = realloc(X.p, sizeof *n * (size_t)c);
        if (!n)
            return -1;
        X.p = n;
        X.cap = c;
    }
    kept_pose *q = &X.p[X.n++];
    q->key = key;
    q->sheet = sheet;
    snprintf(q->name, sizeof q->name, "%s", name);
    for (char *c = q->name; *c; c++)   /* file names: [a-z0-9_] only */
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')))
            *c = '_';
    q->w = w;
    q->h = h;
    q->ox = ox;
    q->oy = oy;
    q->rgba = rgba;
    return 0;
}

int pack_scene_export_open(const char *dir, int scale)
{
    snprintf(X.dir, sizeof X.dir, "%s", dir);
    X.scale = scale > 1 && scale <= 16 ? scale : 1;
    char path[4096];
    snprintf(path, sizeof path, "%s/poses", dir);
    if (mkdirs(path)) {
        fprintf(stderr, "pack_scene: cannot make %s: %s\n", path, strerror(errno));
        return -1;
    }
    snprintf(path, sizeof path, "%s/maps", dir);
    mkdirs(path);
    /* what an earlier export collected */
    snprintf(path, sizeof path, "%s/poses.txt", dir);
    FILE *f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        unsigned key, sheet;
        char name[24];
        int w, h, ox, oy;
        if (line[0] == '#' ||
            sscanf(line, "%x %x %23s %d %d %d %d", &key, &sheet, name, &w, &h, &ox, &oy) != 7 ||
            kept_find(key) >= 0)
            continue;
        char png[4096];
        snprintf(png, sizeof png, "%s/poses/%08x.png", dir, key);
        int iw, ih;
        uint8_t *img = pngio_read_rgba(png, &iw, &ih, NULL);
        if (img && iw == w && ih == h)
            keep(key, sheet, name, w, h, ox, oy, img);
        else
            free(img);
    }
    int fresh = !f;
    if (f)
        fclose(f);
    X.list = fopen(path, "a");
    if (!X.list) {
        fprintf(stderr, "pack_scene: cannot write %s\n", path);
        return -1;
    }
    if (fresh)
        fprintf(X.list, "# Poses seen by ct_sdl --pack-export-sheets, from your own ROM:\n"
                        "# the game's own art, for your personal use only. Never share it.\n"
                        "# key sheet name w h origin_x origin_y\n");
    snprintf(path, sizeof path, "%s/README.txt", dir);
    FILE *r = fopen(path, "w");
    if (r) {
        fprintf(r,
                "Sprite sheets exported by ct_sdl --pack-export-sheets from your own ROM.\n\n"
                "These pictures are the game's own art. They are for your personal use only:\n"
                "never share, upload or distribute them or anything made from them.\n\n"
                "sheet_NAME_1x.png (and _Nx.png): every pose of one character seen while\n"
                "playing, one per cell, lined up on the foot point. Grid lines and the\n"
                "foot-point ticks are in the gutters; draw only inside the cells.\n"
                "sheet_NAME.json: which pose is in which cell.\n"
                "Edit a sheet, then: tools/pack_import_sheet.py sheet_NAME_4x.png --pack MYPACK\n"
                "maps/map_ID.txt: each cell's tile words, for [[model]] tiles = [...].\n"
                "Format and workflow: docs/ASSET-PACKS.md.\n");
        fclose(r);
    }
    export_on = 1;
    fprintf(stderr, "pack_scene: exporting sheets into %s (%d poses already there)\n", dir, X.n);
    return 0;
}

static void export_pose(const ct_scene *sc, int obj, const ct_scene_pose *p)
{
    (void)sc;
    if (kept_find(p->key) >= 0 || p->w > POSE_MAX || p->h > POSE_MAX)
        return;
    uint8_t *img = malloc((size_t)p->w * p->h * 4);
    ct_scene_pose q;
    if (!img || ct_scene_pose_of(obj, &q, img, p->w * p->h) || q.key != p->key || q.w != p->w ||
        q.h != p->h || keep(q.key, q.sheet, q.name, q.w, q.h, q.origin_x, q.origin_y, img)) {
        free(img);
        return;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/poses/%08x.png", X.dir, q.key);
    pngio_write_rgba(path, img, q.w, q.h);
    const kept_pose *k = &X.p[X.n - 1];
    fprintf(X.list, "%08x %x %s %d %d %d %d\n", k->key, k->sheet, k->name, k->w, k->h, k->ox, k->oy);
    fflush(X.list);
}

/* Each map once (again when it changes): its cells' tile words. */
static void export_map(const ct_scene *sc)
{
    int slot = -1;
    for (int k = 0; k < 64; k++)
        if (X.maps_seen[k] == sc->map_id + 1) {
            if (X.map_gen[k] == sc->map_generation)
                return;
            slot = k;
            break;
        }
    for (int k = 0; k < 64 && slot < 0; k++)
        if (!X.maps_seen[k])
            slot = k;
    if (slot < 0)
        slot = (int)(sc->map_id % 64);
    X.maps_seen[slot] = sc->map_id + 1;
    X.map_gen[slot] = sc->map_generation;
    char path[4096];
    snprintf(path, sizeof path, "%s/maps/map_%03x.txt", X.dir, sc->map_id);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# map 0x%03x, %dx%d cells of %d px. One line per cell:\n"
               "# x y class flags, then per layer L: its tile words TL TR BL BR (hex)\n"
               "# [[model]] tiles = [L, 0xTL, 0xTR, 0xBL, 0xBR] places a model on every cell\n"
               "# showing them (a world map's cells are one tile: [L, 0xW]); cells = [x0, y0,\n"
               "# x1, y1] with map = 0x%03x one place.\n",
            sc->map_id, sc->map_w, sc->map_h, sc->cell_px, sc->map_id);
    int world = sc->mode == CT_SCENE_WORLD;
    int stride = world ? CT_SCENE_WORLD_STRIDE : CT_SCENE_MAX_CELLS;
    for (int y = 0; y < sc->map_h; y++)
        for (int x = 0; x < sc->map_w; x++) {
            const ct_scene_cell *c = &sc->cells[y * stride + x];
            fprintf(f, "%d %d %d %d", x, y, c->cls, c->flags);
            for (int l = 0; l < (world ? 2 : CT_SCENE_LAYERS); l++) {
                if (!sc->layer[l].present || !sc->layer[l].tiles)
                    continue;
                fprintf(f, "  %d:", l);
                for (int q = 0; q < (world ? 1 : 4); q++)
                    fprintf(f, " %04x", tile_word(sc, l, x, y, q));
            }
            fputc('\n', f);
        }
    fclose(f);
}

static int cmp_kept(const void *a, const void *b)
{
    const kept_pose *x = a, *y = b;
    if (x->sheet != y->sheet)
        return x->sheet < y->sheet ? -1 : 1;
    return x->key < y->key ? -1 : x->key > y->key;
}

/* The picture of pose a is pose b's mirrored left to right. */
static int mirrors(const kept_pose *a, const kept_pose *b)
{
    if (a->w != b->w || a->h != b->h || a == b)
        return 0;
    int any = 0;   /* (two empty pictures are no pair) */
    for (int k = 0; k < a->w * a->h && !any; k++)
        any = a->rgba[k * 4 + 3];
    if (!any)
        return 0;
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            const uint8_t *p = a->rgba + ((size_t)y * a->w + x) * 4;
            const uint8_t *q = b->rgba + ((size_t)y * b->w + (b->w - 1 - x)) * 4;
            if (p[3] != q[3] || (p[3] && memcmp(p, q, 3)))
                return 0;
        }
    return 1;
}

#define GUTTER 4   /* px at 1x between cells: a grid line in the middle, the ticks */

static void put(uint8_t *img, int iw, int ih, int x, int y, const uint8_t c[4])
{
    if (x >= 0 && y >= 0 && x < iw && y < ih)
        memcpy(img + ((size_t)y * iw + x) * 4, c, 4);
}

/* One sheet (poses p[0..n-1], one character) at scale s. */
static void write_sheet(const kept_pose *p, int n, int s, int cols, int cw, int ch, int left, int up)
{
    int rows = (n + cols - 1) / cols;
    int iw = (GUTTER + cols * (cw + GUTTER)) * s, ih = (GUTTER + rows * (ch + GUTTER)) * s;
    uint8_t *img = calloc((size_t)iw * ih, 4);
    if (!img)
        return;
    static const uint8_t grid[4] = {255, 0, 255, 255}, tick[4] = {0, 224, 255, 255};
    /* grid lines through the gutters' middle */
    for (int c = 0; c <= cols; c++) {
        int x = (c * (cw + GUTTER) + GUTTER / 2) * s;
        for (int y = 0; y < ih; y++)
            put(img, iw, ih, x, y, grid);
    }
    for (int r = 0; r <= rows; r++) {
        int y = (r * (ch + GUTTER) + GUTTER / 2) * s;
        for (int x = 0; x < iw; x++)
            put(img, iw, ih, x, y, grid);
    }
    for (int k = 0; k < n; k++) {
        int cx = GUTTER + (k % cols) * (cw + GUTTER), cy = GUTTER + (k / cols) * (ch + GUTTER);
        int px = cx + left - p[k].ox, py = cy + up - p[k].oy;
        for (int y = 0; y < p[k].h * s; y++)
            for (int x = 0; x < p[k].w * s; x++) {
                const uint8_t *c = p[k].rgba + ((size_t)(y / s) * p[k].w + x / s) * 4;
                if (c[3])
                    put(img, iw, ih, px * s + x, py * s + y, c);
            }
        /* the foot point: ticks in the gutters below and left of the cell */
        int fx = (cx + left) * s, fy = (cy + up) * s;
        for (int t = 0; t < s; t++)
            for (int u = 0; u < s; u++) {
                put(img, iw, ih, fx + t, (cy + ch) * s + u, tick);   /* its column */
                put(img, iw, ih, (cx - 1) * s + t, fy + u, tick);    /* its row */
            }
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/sheet_%s_%dx.png", X.dir, p[0].name, s);
    if (pngio_write_rgba(path, img, iw, ih))
        fprintf(stderr, "pack_scene: cannot write %s\n", path);
    free(img);
}

int pack_scene_export_close(void)
{
    if (!export_on)
        return 0;
    export_on = 0;
    if (X.list)
        fclose(X.list);
    X.list = NULL;
    qsort(X.p, (size_t)X.n, sizeof *X.p, cmp_kept);
    int sheets = 0;
    for (int a = 0; a < X.n;) {
        int b = a;
        while (b < X.n && X.p[b].sheet == X.p[a].sheet)
            b++;
        const kept_pose *p = &X.p[a];
        int n = b - a;
        /* cells big enough for every pose, lined up on the foot point */
        int left = 0, right = 0, up = 0, down = 0;
        for (int k = 0; k < n; k++) {
            left = p[k].ox > left ? p[k].ox : left;
            up = p[k].oy > up ? p[k].oy : up;
            right = p[k].w - p[k].ox > right ? p[k].w - p[k].ox : right;
            down = p[k].h - p[k].oy > down ? p[k].h - p[k].oy : down;
        }
        int cw = left + right, ch = up + down, cols = n < 8 ? n : 8;
        write_sheet(p, n, 1, cols, cw, ch, left, up);
        if (X.scale > 1)
            write_sheet(p, n, X.scale, cols, cw, ch, left, up);
        char path[4096];
        snprintf(path, sizeof path, "%s/sheet_%s.json", X.dir, p[0].name);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\n  \"note\": \"The game's own art, from your own ROM: for your personal "
                       "use only. Never share it.\",\n"
                       "  \"format\": \"ct-recomp pose sheet 1\",\n"
                       "  \"name\": \"%s\",\n  \"sheet\": %u,\n  \"scales\": [1%s",
                    p[0].name, (unsigned)p[0].sheet, X.scale > 1 ? ", " : "");
            if (X.scale > 1)
                fprintf(f, "%d", X.scale);
            fprintf(f, "],\n  \"cell\": [%d, %d],\n  \"gutter\": %d,\n  \"columns\": %d,\n"
                       "  \"size\": [%d, %d],\n  \"poses\": [\n",
                    cw, ch, GUTTER, cols, GUTTER + cols * (cw + GUTTER),
                    GUTTER + ((n + cols - 1) / cols) * (ch + GUTTER));
            for (int k = 0; k < n; k++) {
                int cx = GUTTER + (k % cols) * (cw + GUTTER), cy = GUTTER + (k / cols) * (ch + GUTTER);
                fprintf(f, "    {\"key\": \"%08x\", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d, "
                           "\"origin\": [%d, %d]",
                        p[k].key, cx + left - p[k].ox, cy + up - p[k].oy, p[k].w, p[k].h, p[k].ox,
                        p[k].oy);
                for (int j = 0; j < n; j++)
                    if (j != k && mirrors(&p[k], &p[j])) {
                        fprintf(f, ", \"mirror_of\": \"%08x\"", p[j].key);
                        break;
                    }
                fprintf(f, "}%s\n", k + 1 < n ? "," : "");
            }
            fprintf(f, "  ]\n}\n");
            fclose(f);
        }
        sheets++;
        a = b;
    }
    fprintf(stderr, "pack_scene: %d poses on %d sheets in %s\n", X.n, sheets, X.dir);
    int n = X.n;
    for (int k = 0; k < X.n; k++)
        free(X.p[k].rgba);
    free(X.p);
    X.p = NULL;
    X.n = X.cap = 0;
    return n;
}
