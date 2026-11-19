/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Asset packs (packs.h): content keys, the dump, loading and the atlas,
 * and the per-pixel replacement map. */
#include "packs.h"

#include <errno.h>
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

#include "pngio.h"

/* ---- a uint64 -> int map (open addressing; key 0 is "empty") ---- */

typedef struct {
    uint64_t *keys;
    int *vals;
    size_t cap, n;
} kmap;

static void kmap_free(kmap *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static int *kmap_find(const kmap *m, uint64_t key)
{
    if (!m->cap)
        return NULL;
    for (size_t h = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (m->cap - 1);; h = (h + 1) & (m->cap - 1)) {
        if (m->keys[h] == key)
            return &m->vals[h];
        if (!m->keys[h])
            return NULL;
    }
}

static int kmap_put(kmap *m, uint64_t key, int val)
{
    if ((m->n + 1) * 2 > m->cap) {
        kmap old = *m;
        size_t cap = old.cap ? old.cap * 2 : 1024;
        m->keys = calloc(cap, sizeof *m->keys);
        m->vals = calloc(cap, sizeof *m->vals);
        if (!m->keys || !m->vals) {
            free(m->keys);
            free(m->vals);
            *m = old;
            return -1;
        }
        m->cap = cap;
        m->n = 0;
        for (size_t k = 0; k < old.cap; k++)
            if (old.keys[k])
                kmap_put(m, old.keys[k], old.vals[k]);
        kmap_free(&old);
    }
    size_t h = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (m->cap - 1);
    while (m->keys[h] && m->keys[h] != key)
        h = (h + 1) & (m->cap - 1);
    if (!m->keys[h])
        m->n++;
    m->keys[h] = key;
    m->vals[h] = val;
    return 0;
}

/* ---- keys ---- */

#define FNV_BASIS 0xCBF29CE484222325ull
#define FNV_PRIME 0x100000001B3ull

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t k = 0; k < n; k++)
        h = (h ^ b[k]) * FNV_PRIME;
    return h;
}

/* Color indices of the piece p names, stored orientation, row by row. */
static int decode_indices(const uint16_t *vram, const sched_prov_px *p, uint8_t *idx, int *w, int *h,
                          int *bpp)
{
    int code = p->flags & SCHED_PROV_BPP;
    if (!code)
        return -1;
    int depth = code == 1 ? 2 : code == 2 ? 4 : 8;
    *bpp = depth;
    if (!(p->flags & SCHED_PROV_OBJ)) {
        *w = *h = 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = 0;
                for (int pl = 0; pl < depth; pl += 2) {
                    uint16_t word = vram[(p->chr + pl / 2 * 8 + y) & 0x7fff];
                    v |= ((word >> (7 - x)) & 1) << pl | ((word >> (15 - x)) & 1) << (pl + 1);
                }
                idx[y * 8 + x] = (uint8_t)v;
            }
        return 0;
    }
    int size = p->size >= 8 && p->size <= 64 ? p->size : 8;
    *w = *h = size;
    for (int cy = 0; cy < size / 8; cy++)
        for (int cx = 0; cx < size / 8; cx++) {
            int n = ((((p->name >> 4) + cy) << 4) | (((p->name & 15) + cx) & 15)) & 0xff;
            unsigned chr = (p->chr + n * 16u) & 0x7fff;
            for (int y = 0; y < 8; y++) {
                uint16_t lo = vram[(chr + y) & 0x7fff], hi = vram[(chr + 8 + y) & 0x7fff];
                for (int x = 0; x < 8; x++) {
                    int s = 7 - x;
                    int v = ((lo >> s) & 1) | ((lo >> (8 + s)) & 1) << 1 | ((hi >> s) & 1) << 2 |
                            ((hi >> (8 + s)) & 1) << 3;
                    idx[(cy * 8 + y) * size + cx * 8 + x] = (uint8_t)v;
                }
            }
        }
    return 0;
}

static int palette_size(int bpp) { return 1 << bpp; }

static uint64_t key_from(const uint16_t *vram, const uint16_t *cgram, const sched_prov_px *p)
{
    uint8_t idx[64 * 64];
    int w, h, bpp;
    if (decode_indices(vram, p, idx, &w, &h, &bpp))
        return 0;
    uint8_t head[4] = {(uint8_t)(p->flags & SCHED_PROV_OBJ ? 'S' : 'B'), (uint8_t)w, (uint8_t)h,
                       (uint8_t)bpp};
    uint64_t k = fnv(FNV_BASIS, head, sizeof head);
    k = fnv(k, idx, (size_t)w * h);
    int n = palette_size(bpp);
    for (int c = 1; c < n; c++) {
        uint16_t col = cgram[(p->pal + c) & 0xff] & 0x7fff;
        uint8_t b[2] = {(uint8_t)col, (uint8_t)(col >> 8)};
        k = fnv(k, b, 2);
    }
    return k ? k : 1;
}

uint64_t packs_key(const sched_vmem *m, const sched_prov_px *p)
{
    return key_from(m->vram, m->cgram, p);
}

uint64_t packs_key_tile(const uint16_t *vram, const uint16_t *cgram, unsigned chr, int pal, int bpp)
{
    sched_prov_px p;
    memset(&p, 0, sizeof p);
    p.chr = (uint16_t)(chr & 0x7fff);
    p.pal = (uint8_t)pal;
    p.flags = (uint8_t)(bpp == 2 ? 1 : bpp == 4 ? 2 : 3);
    p.size = 8;
    return key_from(vram, cgram, &p);
}

int packs_decode(const sched_vmem *m, const sched_prov_px *p, uint8_t *rgba, int *w, int *h)
{
    uint8_t idx[64 * 64];
    int bpp;
    if (decode_indices(m->vram, p, idx, w, h, &bpp))
        return -1;
    for (int k = 0; k < *w * *h; k++) {
        uint16_t c = m->cgram[(p->pal + idx[k]) & 0xff];
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        rgba[k * 4 + 0] = (uint8_t)(r << 3 | r >> 2);
        rgba[k * 4 + 1] = (uint8_t)(g << 3 | g >> 2);
        rgba[k * 4 + 2] = (uint8_t)(b << 3 | b >> 2);
        rgba[k * 4 + 3] = idx[k] ? 255 : 0;
    }
    return 0;
}

void packs_key_text(uint64_t key, char *buf)
{
    snprintf(buf, 17, "%016llx", (unsigned long long)key);
}

/* The provenance entry's identity within one frame (the VRAM and CGRAM it
   is read from don't change during the frame): same id, same key. */
static uint64_t raw_id(const sched_prov_px *p)
{
    uint64_t id = (uint64_t)p->chr | (uint64_t)p->pal << 16 | (uint64_t)(p->flags & 0x13) << 24;
    if (p->flags & SCHED_PROV_OBJ)
        id |= (uint64_t)p->name << 32 | (uint64_t)p->size << 40;
    return id + 1;
}

/* ---- a small pack.toml reader: [table] / [[array]] headers and
   key = value lines (strings, numbers, [number, ...]); enough for the
   files this writes and people edit. ---- */

int packs_toml_parse(char *s, packs_toml_line *t)
{
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s || *s == '#' || *s == '\n' || *s == '\r')
        return 0;
    if (*s == '[') {
        int arr = s[1] == '[';
        char *b = s + 1 + arr, *e = strchr(b, ']');
        if (!e)
            return 0;
        *e = 0;
        snprintf(t->table, sizeof t->table, "%s", b);
        t->is_array = arr;
        return 1;
    }
    char *eq = strchr(s, '=');
    if (!eq)
        return 0;
    char *ke = eq;
    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
    snprintf(t->key, sizeof t->key, "%.*s", (int)(ke - s), s);
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    if (*v == '"') {
        char *e = strchr(v + 1, '"');
        snprintf(t->val, sizeof t->val, "%.*s", e ? (int)(e - v - 1) : 0, v + 1);
    } else {
        size_t n = strcspn(v, "#\r\n");
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
            n--;
        snprintf(t->val, sizeof t->val, "%.*s", (int)n, v);
    }
    return 2;
}

static int parse_numbers(const char *s, float *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && !(*s == '-' || *s == '.' || (*s >= '0' && *s <= '9')))
            s++;
        if (!*s)
            break;
        char *e;
        out[n++] = strtof(s, &e);
        if (e == s)
            break;
        s = e;
    }
    return n;
}

/* ---- dumping ---- */

static struct {
    FILE *manifest;
    char dir[3072];
    kmap seen;      /* piece keys written (or already in the pack) */
    kmap objects;   /* object keys written */
    long pieces, objs;
    /* per-frame: raw id -> key */
    kmap frame_ids;
} dump;

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

int packs_dump_open(const char *dir, const char *name)
{
    packs_dump_close();
    snprintf(dump.dir, sizeof dump.dir, "%s", dir);
    char sub[4096];
    static const char *const subs[] = {"", "/tiles", "/sprites", "/objects"};
    for (int k = 0; k < 4; k++) {
        snprintf(sub, sizeof sub, "%s%s", dir, subs[k]);
        if (mkdirs(sub)) {
            fprintf(stderr, "packs: cannot make %s: %s\n", sub, strerror(errno));
            return -1;
        }
    }
    snprintf(sub, sizeof sub, "%s/pack.toml", dir);
    FILE *old = fopen(sub, "r");
    int fresh = !old;
    if (old) {   /* keep extending it: its keys are not written again */
        char line[2048];
        packs_toml_line t = {0};
        while (fgets(line, sizeof line, old)) {
            int r = packs_toml_parse(line, &t);
            if (r == 2 && !strcmp(t.key, "key") && t.is_array) {
                uint64_t key = strtoull(t.val, NULL, 16);
                if (!strcmp(t.table, "tile"))
                    kmap_put(&dump.seen, key, 1);
                else if (!strcmp(t.table, "object"))
                    kmap_put(&dump.objects, key, 1);
            }
        }
        fclose(old);
    }
    dump.manifest = fopen(sub, "a");
    if (!dump.manifest) {
        fprintf(stderr, "packs: cannot write %s: %s\n", sub, strerror(errno));
        return -1;
    }
    if (fresh)
        fprintf(dump.manifest,
                "# Asset pack written by ct_sdl --pack-dump from your own ROM.\n"
                "# The pictures in it are the game's own art: for your personal use only.\n"
                "# Never share or distribute this folder or anything made from it.\n"
                "# Format: docs/ASSET-PACKS.md.\n"
                "[pack]\nversion = 1\nname = \"%s\"\nscale = 1\n",
                name);
    fflush(dump.manifest);
    fprintf(stderr, "packs: dumping into %s (%zu pieces already there)\n", dir, dump.seen.n);
    return 0;
}

void packs_dump_counts(long *pieces, long *objects)
{
    *pieces = dump.pieces;
    *objects = dump.objs;
}

void packs_dump_close(void)
{
    if (dump.manifest)
        fclose(dump.manifest);
    kmap_free(&dump.seen);
    kmap_free(&dump.objects);
    kmap_free(&dump.frame_ids);
    memset(&dump, 0, sizeof dump);
}

static const char *layer_name(const sched_prov_px *p)
{
    static const char *const n[8] = {"BG1", "BG2", "BG3", "BG4", "OBJ", "backdrop", "OBJ", "?"};
    return p->flags & SCHED_PROV_OBJ ? "OBJ" : n[p->index & 7];
}

static void dump_piece(long frame, const sched_vmem *m, const sched_prov_px *p, uint64_t key)
{
    uint8_t rgba[64 * 64 * 4];
    int w, h;
    if (packs_decode(m, p, rgba, &w, &h))
        return;
    char kt[17], path[4096];
    packs_key_text(key, kt);
    int obj = (p->flags & SCHED_PROV_OBJ) != 0;
    const char *sub = obj ? "sprites" : "tiles";
    snprintf(path, sizeof path, "%s/%s/%s.png", dump.dir, sub, kt);
    if (pngio_write_rgba(path, rgba, w, h)) {
        fprintf(stderr, "packs: cannot write %s\n", path);
        return;
    }
    int code = p->flags & SCHED_PROV_BPP;
    fprintf(dump.manifest,
            "\n[[tile]]\nkey = \"%s\"\nkind = \"%s\"\nfile = \"%s/%s.png\"\nw = %d\nh = %d\n"
            "bpp = %d\nlayer = \"%s\"\n",
            kt, obj ? "sprite" : "bg", sub, kt, w, h, code == 1 ? 2 : code == 2 ? 4 : 8,
            layer_name(p));
    if (obj)
        fprintf(dump.manifest, "palette = %d\noam = %d\n", (p->pal >> 4) & 7, p->index);
    fprintf(dump.manifest, "first_frame = %ld\n", frame);
    dump.pieces++;
}

/* Sprites drawn together: OAM entries whose boxes overlap or touch are
   one object. A new combination (pieces and their offsets) gets a
   picture of the assembled object and an [[object]] entry naming its
   pieces. */
typedef struct {
    int on, x, y, size, hflip, vflip;
    uint64_t key;
    sched_prov_px p;
} oam_piece;

static int uf_find(int *parent, int k)
{
    while (parent[k] != k)
        k = parent[k] = parent[parent[k]];
    return k;
}

static void dump_objects(long frame, const sched_vmem *m, oam_piece *pc)
{
    int parent[128];
    for (int k = 0; k < 128; k++) {
        parent[k] = k;
        if (!pc[k].on)
            continue;
        uint16_t lo = m->oam[k * 2], hiw = m->oam[0x100 + (k >> 3)];
        int high = hiw >> ((k * 2) & 15);
        int x = (lo & 0xff) + (high & 1) * 256;
        if (x >= 256)
            x -= 512;
        pc[k].x = x;
        pc[k].y = lo >> 8;
        pc[k].hflip = (pc[k].p.flags & SCHED_PROV_HFLIP) != 0;
        pc[k].vflip = (pc[k].p.flags & SCHED_PROV_VFLIP) != 0;
        pc[k].size = pc[k].p.size;
    }
    for (int a = 0; a < 128; a++)
        for (int b = a + 1; b < 128 && pc[a].on; b++) {
            if (!pc[b].on)
                continue;
            /* y wraps at 256 (a sprite partly above the top) */
            int dy = ((pc[b].y - pc[a].y + 128) & 255) - 128;
            if (pc[b].x <= pc[a].x + pc[a].size && pc[a].x <= pc[b].x + pc[b].size &&
                dy <= pc[a].size && -dy <= pc[b].size) {
                int ra = uf_find(parent, a), rb = uf_find(parent, b);
                if (ra != rb)
                    parent[rb] = ra;
            }
        }
    for (int root = 0; root < 128; root++) {
        if (!pc[root].on || uf_find(parent, root) != root)
            continue;
        int members[128], n = 0, minx = 1 << 20, miny = 1 << 20, maxx = -(1 << 20), maxy = -(1 << 20);
        for (int k = 0; k < 128; k++)
            if (pc[k].on && uf_find(parent, k) == root) {
                members[n++] = k;
                int y = pc[root].y + (((pc[k].y - pc[root].y + 128) & 255) - 128);
                pc[k].y = y;
                if (pc[k].x < minx) minx = pc[k].x;
                if (y < miny) miny = y;
                if (pc[k].x + pc[k].size > maxx) maxx = pc[k].x + pc[k].size;
                if (y + pc[k].size > maxy) maxy = y + pc[k].size;
            }
        if (n < 2 || maxx - minx > 256 || maxy - miny > 256)
            continue;
        uint64_t gk = fnv(FNV_BASIS, "O", 1);
        for (int i = 0; i < n; i++) {   /* OAM order: also the drawing order */
            const oam_piece *q = &pc[members[i]];
            int v[5] = {q->x - minx, q->y - miny, q->hflip, q->vflip, 0};
            gk = fnv(fnv(gk, &q->key, sizeof q->key), v, sizeof v);
        }
        gk = gk ? gk : 1;
        if (kmap_find(&dump.objects, gk))
            continue;
        kmap_put(&dump.objects, gk, 1);
        int w = maxx - minx, h = maxy - miny;
        uint8_t *img = calloc((size_t)w * h * 4, 1);
        if (!img)
            continue;
        /* Lower OAM indices are in front: paint from the back. */
        for (int i = n - 1; i >= 0; i--) {
            const oam_piece *q = &pc[members[i]];
            uint8_t rgba[64 * 64 * 4];
            int pw, ph;
            if (packs_decode(m, &q->p, rgba, &pw, &ph))
                continue;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) {
                    int sx = q->hflip ? pw - 1 - x : x, sy = q->vflip ? ph - 1 - y : y;
                    const uint8_t *s = &rgba[(sy * pw + sx) * 4];
                    if (s[3])
                        memcpy(&img[((size_t)(q->y - miny + y) * w + q->x - minx + x) * 4], s, 4);
                }
        }
        char kt[17], path[4096];
        packs_key_text(gk, kt);
        snprintf(path, sizeof path, "%s/objects/%s.png", dump.dir, kt);
        int bad = pngio_write_rgba(path, img, w, h);
        free(img);
        if (bad)
            continue;
        fprintf(dump.manifest, "\n[[object]]\nkey = \"%s\"\nfile = \"objects/%s.png\"\nw = %d\nh = %d\n"
                               "first_frame = %ld\npieces = [\n", kt, kt, w, h, frame);
        for (int i = 0; i < n; i++) {
            const oam_piece *q = &pc[members[i]];
            char pk[17];
            packs_key_text(q->key, pk);
            fprintf(dump.manifest, "  { key = \"%s\", x = %d, y = %d, hflip = %d, vflip = %d, oam = %d },\n",
                    pk, q->x - minx, q->y - miny, q->hflip, q->vflip, members[i]);
        }
        fprintf(dump.manifest, "]\n");
        dump.objs++;
    }
}

void packs_dump_frame(long frame, const sched_prov_px *prov, const sched_vmem *m)
{
    if (!dump.manifest || !prov || !m)
        return;
    long before = dump.pieces + dump.objs;
    int written = 0;
    kmap_free(&dump.frame_ids);
    static oam_piece pc[128];
    memset(pc, 0, sizeof pc);
    uint64_t last_id = 0, last_key = 0;
    for (int k = 0; k < SCHED_WIDTH * SCHED_HEIGHT; k++) {
        const sched_prov_px *p = &prov[k];
        if (!(p->flags & SCHED_PROV_BPP))
            continue;
        uint64_t id = raw_id(p), key;
        if (id == last_id) {
            key = last_key;
        } else {
            static uint64_t fkeys[16384];
            static int nf;
            if (!dump.frame_ids.n)
                nf = 0;
            int *v = kmap_find(&dump.frame_ids, id);
            if (v) {
                key = fkeys[*v];   /* seen this frame */
            } else {
                key = packs_key(m, p);
                if (nf < 16384) {
                    fkeys[nf] = key;
                    kmap_put(&dump.frame_ids, id, nf++);
                }
                /* port: at most 6 new pictures written a frame (each is a PNG
                   file): the rest wait for the next frames, so dumping no
                   longer stalls the game on a screen full of new tiles */
                if (key && written < 6 && !kmap_find(&dump.seen, key)) {
                    kmap_put(&dump.seen, key, 1);
                    dump_piece(frame, m, p, key);
                    written++;
                }
            }
            last_id = id;
            last_key = key;
        }
        if ((p->flags & SCHED_PROV_OBJ) && !pc[p->index & 127].on && key) {
            oam_piece *q = &pc[p->index & 127];
            q->on = 1;
            q->key = key;
            q->p = *p;
        }
    }
    dump_objects(frame, m, pc);
    if (dump.pieces + dump.objs != before)
        fflush(dump.manifest);
}

/* ---- loading ---- */

#define MAX_PACKS 16

static struct {
    char dirs[MAX_PACKS][3072];
    int ndirs;
    packs_rect *entries;
    int n, cap;
    kmap by_key;   /* key -> entry index */
    uint8_t *atlas;
    int page_size, pages;
    unsigned generation;
    packs_model *models;
    int nmodels;
    packs_mesh *meshes;     /* per model, read on first use */
    int8_t *mesh_state;     /* 0 not read yet, 1 read, -1 unreadable, 2 shares mesh_of */
    int *mesh_of;
    kmap by_pose;   /* pose key -> entry index */
    /* poses this frame (packs_set_poses); OAM entry -> place + 1 */
    packs_pose_place places[64];
    int nplaces;
    uint8_t obj_size[2];
    /* per frame: raw id -> entry index + 1 (0: none), by generation */
    struct { uint64_t id; int val; unsigned gen; } cache[8192];
    unsigned frame_gen;
} L;

typedef struct {
    uint64_t key;
    uint8_t *img;
    int w, h, src_w, src_h;
    int pose, flags;   /* a [[pose]] (key: the pose key), its flips */
} pending;

static int cmp_height(const void *a, const void *b)
{
    const pending *x = a, *y = b;
    return y->h != x->h ? y->h - x->h : y->w - x->w;
}

void packs_unload(void)
{
    for (int k = 0; k < L.nmodels && L.meshes; k++)
        if (L.mesh_state[k] == 1)
            packs_mesh_free(&L.meshes[k]);
    free(L.meshes);
    free(L.mesh_state);
    free(L.mesh_of);
    L.meshes = NULL;
    L.mesh_state = NULL;
    L.mesh_of = NULL;
    free(L.entries);
    free(L.atlas);
    free(L.models);
    kmap_free(&L.by_key);
    kmap_free(&L.by_pose);
    L.nplaces = 0;
    L.entries = NULL;
    L.atlas = NULL;
    L.models = NULL;
    L.n = L.cap = L.page_size = L.pages = L.nmodels = 0;
    L.generation++;
    memset(L.cache, 0, sizeof L.cache);
}

int packs_active(void) { return L.n > 0 || L.nmodels > 0; }
int packs_have_poses(void) { return L.by_pose.n > 0; }
unsigned packs_generation(void) { return L.generation; }

static void model_defaults(packs_model *m)
{
    memset(m, 0, sizeof *m);
    m->map = -1;
    m->cells_w = m->cells_h = 1;
    for (int k = 0; k < 3; k++)
        m->scale[k] = 1;
    for (int k = 0; k < 4; k++)
        m->color[k] = 1;
}

/* Numbers with hex (0x...) allowed: tile words and map ids are hex. */
static int parse_ints(const char *s, long *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && !(*s == '-' || (*s >= '0' && *s <= '9')))
            s++;
        if (!*s)
            break;
        char *e;
        out[n++] = strtol(s, &e, 0);
        if (e == s)
            break;
        s = e;
    }
    return n;
}

static void model_key(packs_model *m, const char *key, const char *val)
{
    float f[4] = {0, 0, 0, 0};
    long v[8];
    if (!strcmp(key, "map")) {
        m->map = (int)strtol(val, NULL, 0);
    } else if (!strcmp(key, "cell")) {   /* one cell */
        if (parse_ints(val, v, 2) == 2)
            m->cell_x = (int)v[0], m->cell_y = (int)v[1], m->cells_w = m->cells_h = 1;
    } else if (!strcmp(key, "cells")) {   /* [x0, y0, x1, y1], inclusive */
        if (parse_ints(val, v, 4) == 4 && v[2] >= v[0] && v[3] >= v[1]) {
            m->cell_x = (int)v[0];
            m->cell_y = (int)v[1];
            m->cells_w = (int)(v[2] - v[0] + 1);
            m->cells_h = (int)(v[3] - v[1] + 1);
        }
    } else if (!strcmp(key, "size")) {   /* with tiles: the footprint from the matching cell */
        if (parse_ints(val, v, 2) == 2 && v[0] > 0 && v[1] > 0)
            m->cells_w = (int)v[0], m->cells_h = (int)v[1];
    } else if (!strcmp(key, "tiles")) {   /* [layer, TL, TR, BL, BR] (world map: [layer, w]) */
        int n = parse_ints(val, v, 1 + PACKS_MODEL_MAX_TILES);
        if (n >= 2) {
            m->tiles_layer = (int)v[0];
            m->n_tiles = n - 1;
            for (int k = 1; k < n; k++)
                m->tiles[k - 1] = (uint16_t)v[k];
        }
    } else if (!strcmp(key, "scale")) {
        int n = parse_numbers(val, f, 3);
        for (int k = 0; k < 3 && n; k++)
            m->scale[k] = n == 3 ? f[k] : f[0];
    } else if (!strcmp(key, "rotate")) {
        parse_numbers(val, m->rotate, 3);
    } else if (!strcmp(key, "offset")) {
        parse_numbers(val, m->offset, 3);
    } else if (!strcmp(key, "color")) {
        int n = parse_numbers(val, f, 4);
        for (int k = 0; k < n; k++)
            m->color[k] = f[k];
    } else if (!strcmp(key, "filter")) {
        m->linear = !strcmp(val, "linear");
    } else if (!strcmp(key, "hide_cells")) {
        m->hide_cells = !strcmp(val, "true") || !strcmp(val, "1");
    }
}

static void load_one(const char *dir, pending **list, int *n, int *cap, kmap *have)
{
    char path[4096], line[2048];
    snprintf(path, sizeof path, "%s/pack.toml", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "packs: no %s\n", path);
        return;
    }
    packs_toml_line t = {0};
    char key[64] = "", file[1024] = "";
    int w = 0, h = 0, loaded = 0, missing = 0, flip = 0, models = 0;
    packs_model mdl;
    model_defaults(&mdl);
    char table[32] = "";
    /* An entry is complete at the next header or the end of the file. */
    for (int eof = 0; !eof;) {
        int r = fgets(line, sizeof line, f) ? packs_toml_parse(line, &t) : (eof = 1, 1);
        if (r == 1) {
            int pose = !strcmp(table, "pose");
            if ((pose || !strcmp(table, "tile")) && key[0] && file[0] && w > 0 && h > 0) {
                uint64_t k = strtoull(key, NULL, 16);
                /* poses apart from tiles: their own key space */
                uint64_t hk = pose ? (k & 0xFFFFFFFFull) | 0xB05E000000000000ull : k;
                if (pose && (k == 0 || k > 0xFFFFFFFFull))
                    fprintf(stderr, "packs: %s: [[pose]] key %s: 8 hex digits, not 0\n", dir, key);
                else if (!kmap_find(have, hk)) {
                    snprintf(path, sizeof path, "%s/%s", dir, file);
                    int iw, ih;
                    const char *err = NULL;
                    uint8_t *img = pngio_read_rgba(path, &iw, &ih, &err);
                    if (!img) {
                        if (missing++ < 5)
                            fprintf(stderr, "packs: %s: %s\n", path, err ? err : "unreadable");
                    } else if (iw > 2048 || ih > 2048) {
                        fprintf(stderr, "packs: %s: over 2048 pixels: skipped\n", path);
                        free(img);
                    } else {
                        if (*n == *cap) {
                            *cap = *cap ? *cap * 2 : 256;
                            pending *nl = realloc(*list, (size_t)*cap * sizeof **list);
                            if (!nl) {
                                free(img);
                                break;
                            }
                            *list = nl;
                        }
                        if (pose && (long)iw * h != (long)ih * w)
                            fprintf(stderr, "packs: %s: %dx%d, not the pose's %d:%d shape: "
                                            "stretched\n", path, iw, ih, w, h);
                        (*list)[(*n)++] = (pending){k, img, iw, ih, w, h, pose, flip};
                        kmap_put(have, hk, 1);
                        loaded++;
                    }
                }
            } else if (!strcmp(table, "model") && file[0]) {
                packs_model *nm = realloc(L.models, (size_t)(L.nmodels + 1) * sizeof *nm);
                if (mdl.map < 0 && !mdl.n_tiles) {
                    fprintf(stderr, "packs: %s: [[model]] %s: needs map (with cells) or tiles\n",
                            dir, file);
                } else if (nm) {
                    L.models = nm;
                    if (snprintf(mdl.file, sizeof mdl.file, "%s/%s", dir, file) >= (int)sizeof mdl.file)
                        mdl.file[0] = 0;   /* too long a path */
                    L.models[L.nmodels++] = mdl;
                    models++;
                }
            }
            if (!eof)
                snprintf(table, sizeof table, "%s", t.table);
            key[0] = file[0] = 0;
            w = h = flip = 0;
            model_defaults(&mdl);
        } else if (r == 2) {
            if (!strcmp(t.key, "key"))
                snprintf(key, sizeof key, "%.20s", t.val);
            else if (!strcmp(t.key, "file"))
                snprintf(file, sizeof file, "%s", t.val);
            else if (!strcmp(t.key, "w"))
                w = atoi(t.val);
            else if (!strcmp(t.key, "h"))
                h = atoi(t.val);
            else if (!strcmp(t.key, "flip"))
                flip = (strchr(t.val, 'h') ? PACKS_FLIP_H : 0) | (strchr(t.val, 'v') ? PACKS_FLIP_V : 0);
            else if (!strcmp(table, "model"))
                model_key(&mdl, t.key, t.val);
        }
    }
    fclose(f);
    fprintf(stderr, "packs: %s: %d replacements%s", dir, loaded,
            missing ? " (some files missing or unreadable)" : "");
    if (models)
        fprintf(stderr, ", %d models", models);
    fputc('\n', stderr);
}

static int build_atlas(pending *list, int n)
{
    qsort(list, (size_t)n, sizeof *list, cmp_height);
    double area = 0;
    int maxdim = 0;
    for (int k = 0; k < n; k++) {
        area += (double)list[k].w * list[k].h;
        if (list[k].w > maxdim) maxdim = list[k].w;
        if (list[k].h > maxdim) maxdim = list[k].h;
    }
    int size = 256;
    while (size < 4096 && ((double)size * size * 0.8 < area || size < maxdim))
        size *= 2;
    /* Shelf packing, page by page. */
    L.entries = calloc((size_t)(n ? n : 1), sizeof *L.entries);
    if (!L.entries)
        return -1;
    int page = 0, x = 0, y = 0, shelf = 0;
    for (int k = 0; k < n; k++) {
        if (x + list[k].w > size) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + list[k].h > size) {
            page++;
            x = y = shelf = 0;
        }
        L.entries[k] = (packs_rect){list[k].key, page, x, y, list[k].w, list[k].h, list[k].src_w,
                                    list[k].src_h, list[k].flags};
        x += list[k].w;
        if (list[k].h > shelf)
            shelf = list[k].h;
    }
    L.pages = n ? page + 1 : 0;
    L.page_size = size;
    L.atlas = calloc((size_t)L.pages * size * size * 4 + 4, 1);
    if (!L.atlas)
        return -1;
    for (int k = 0; k < n; k++) {
        const packs_rect *r = &L.entries[k];
        for (int row = 0; row < r->h; row++)
            memcpy(L.atlas + (((size_t)r->page * size + r->y + row) * size + r->x) * 4,
                   list[k].img + (size_t)row * r->w * 4, (size_t)r->w * 4);
        kmap_put(list[k].pose ? &L.by_pose : &L.by_key, r->key, k);
    }
    L.n = n;
    return 0;
}

static int load_dirs(void)
{
    packs_unload();
    pending *list = NULL;
    int n = 0, cap = 0, readable = 0;
    kmap have = {0};
    for (int k = 0; k < L.ndirs; k++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/pack.toml", L.dirs[k]);
        FILE *f = fopen(path, "r");
        if (f) {
            fclose(f);
            readable++;
        }
        load_one(L.dirs[k], &list, &n, &cap, &have);
    }
    kmap_free(&have);
    for (int k = 0; k < L.nmodels; k++) {   /* [[model]]: check each file */
        char err[512];
        if (packs_model_parse(L.models[k].file, &L.models[k], err, sizeof err)) {
            fprintf(stderr, "packs: model: %s\n", err);
            L.models[k].meshes = L.models[k].primitives = 0;
        }
    }
    if (L.nmodels) {   /* meshes are read when a map first shows them */
        L.meshes = calloc((size_t)L.nmodels, sizeof *L.meshes);
        L.mesh_state = calloc((size_t)L.nmodels, sizeof *L.mesh_state);
        L.mesh_of = calloc((size_t)L.nmodels, sizeof *L.mesh_of);
        if (!L.meshes || !L.mesh_state || !L.mesh_of) {
            free(L.models);
            L.models = NULL;
            L.nmodels = 0;
        }
    }
    int rc = build_atlas(list, n);
    for (int k = 0; k < n; k++)
        free(list[k].img);
    free(list);
    if (rc) {
        fprintf(stderr, "packs: out of memory for the atlas\n");
        packs_unload();
        return -1;
    }
    if (n)
        fprintf(stderr, "packs: %d replacements in %d atlas page(s) of %dx%d\n", n, L.pages,
                L.page_size, L.page_size);
    return readable ? n + L.nmodels : -1;
}

int packs_load(const char *const *dirs, int n)
{
    L.ndirs = 0;
    for (int k = 0; k < n && k < MAX_PACKS; k++)
        snprintf(L.dirs[L.ndirs++], sizeof L.dirs[0], "%s", dirs[k]);
    return load_dirs();
}

int packs_reload(void) { return load_dirs(); }

const packs_rect *packs_lookup_tile(uint64_t key)
{
    int *v = kmap_find(&L.by_key, key);
    return v ? &L.entries[*v] : NULL;
}

const uint8_t *packs_atlas(int *size, int *pages)
{
    *size = L.page_size;
    *pages = L.pages;
    return L.atlas;
}

const packs_rect *packs_entries(int *n)
{
    *n = L.n;
    return L.entries;
}

const packs_model *packs_models(int *n)
{
    *n = L.nmodels;
    return L.models;
}

const packs_mesh *packs_model_mesh(int k)
{
    if (k < 0 || k >= L.nmodels || !L.meshes)
        return NULL;
    if (!L.mesh_state[k]) {
        for (int j = 0; j < k; j++)   /* the same file: one mesh */
            if (L.mesh_state[j] && !strcmp(L.models[j].file, L.models[k].file)) {
                L.mesh_state[k] = 2;
                L.mesh_of[k] = L.mesh_state[j] == 2 ? L.mesh_of[j] : j;
                break;
            }
    }
    if (!L.mesh_state[k]) {
        char err[512];
        if (L.models[k].primitives && !packs_mesh_load(L.models[k].file, &L.meshes[k], err, sizeof err)) {
            L.mesh_state[k] = 1;
        } else {
            if (L.models[k].primitives)   /* (else said when the pack loaded) */
                fprintf(stderr, "packs: model: %s\n", err);
            L.mesh_state[k] = -1;
        }
    }
    int src = L.mesh_state[k] == 2 ? L.mesh_of[k] : k;
    return L.mesh_state[src] == 1 ? &L.meshes[src] : NULL;
}

const packs_rect *packs_lookup_pose(uint32_t key)
{
    int *v = key ? kmap_find(&L.by_pose, key) : NULL;
    return v ? &L.entries[*v] : NULL;
}

void packs_set_poses(const packs_pose_place *p, int n, const uint8_t obj_size[2])
{
    L.nplaces = 0;
    for (int k = 0; k < n && L.nplaces < (int)(sizeof L.places / sizeof *L.places); k++)
        L.places[L.nplaces++] = p[k];
    L.obj_size[0] = obj_size ? obj_size[0] : 8;
    L.obj_size[1] = obj_size ? obj_size[1] : 16;
}

int packs_oam_box(const uint16_t *oam, const uint8_t obj_size[2], const uint8_t *idx, int n,
                  int *x, int *y, int *w, int *h)
{
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (int k = 0; k < n; k++) {
        int e = idx[k] & 127;
        uint16_t w0 = oam[2 * e];
        unsigned hi = oam[0x100 + e / 8] >> (2 * (e % 8));
        int size = obj_size[(hi >> 1) & 1];
        int ex = (w0 & 0xFF) | (hi & 1) << 8, ey = w0 >> 8;
        if (ex >= 256)
            ex -= 512;
        if (ey >= 224 && ey + size <= 256)
            continue;   /* parked below the picture */
        if (ey + size > 256)
            ey -= 256;   /* wraps to the top */
        x0 = ex < x0 ? ex : x0;
        y0 = ey < y0 ? ey : y0;
        x1 = ex + size > x1 ? ex + size : x1;
        y1 = ey + size > y1 ? ey + size : y1;
    }
    if (x1 < x0)
        return -1;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 0;
}

/* A pose's texel (u, v: console px, stored orientation) has some cover. */
static int pose_opaque(const packs_pose_place *q, int u, int v)
{
    const packs_rect *r = &L.entries[q->entry];
    int x = r->x + (int)(((long)u * 2 + 1) * r->w / (2L * q->w));
    int y = r->y + (int)(((long)v * 2 + 1) * r->h / (2L * q->h));
    return L.atlas[(((size_t)r->page * L.page_size + y) * L.page_size + x) * 4 + 3] >= 128;
}

long packs_map_frame(const sched_prov_px *prov, const sched_vmem *m, uint16_t *map)
{
    long replaced = 0;
    enum { CACHE = sizeof L.cache / sizeof *L.cache };
    if (!prov || !m || !L.n) {
        memset(map, 0, sizeof(uint16_t) * 4 * SCHED_WIDTH * SCHED_HEIGHT);
        return 0;
    }
    if (++L.frame_gen == 0)
        L.frame_gen = 1;
    /* Poses: OAM entry -> the place drawing it (+ 1); where each is, its
       front OAM entry, the z level its pixels won at (-1: none). */
    uint8_t by_oam[128] = {0};
    int px[64], py[64], front[64], z[64], hf[64], vf[64];
    for (int i = 0; i < L.nplaces; i++) {
        packs_pose_place *q = &L.places[i];
        int bw, bh;
        z[i] = -1;
        if (q->entry < 0 || q->entry >= L.n ||
            packs_oam_box(m->oam, L.obj_size, q->oam, q->n, &px[i], &py[i], &bw, &bh) ||
            bw != q->w || bh != q->h || q->w > 256 || q->h > 256)
            continue;   /* not as described: the entries' own replacements */
        front[i] = 127;
        for (int j = 0; j < q->n; j++) {
            by_oam[q->oam[j] & 127] = (uint8_t)(i + 1);
            front[i] = (q->oam[j] & 127) < front[i] ? q->oam[j] & 127 : front[i];
        }
        int fl = L.entries[q->entry].flags;
        hf[i] = (q->hflip != 0) ^ ((fl & PACKS_FLIP_H) != 0);
        vf[i] = (q->vflip != 0) ^ ((fl & PACKS_FLIP_V) != 0);
    }
    uint16_t *map0 = map;
    uint64_t last_id = 0;
    int last_val = 0;
    for (int k = 0; k < SCHED_WIDTH * SCHED_HEIGHT; k++, map += 4) {
        const sched_prov_px *p = &prov[k];
        if (!(p->flags & SCHED_PROV_BPP)) {
            map[0] = map[1] = map[2] = map[3] = 0;
            continue;
        }
        int pl = p->flags & SCHED_PROV_OBJ ? by_oam[p->index & 127] : 0;
        if (pl) {   /* a pose: the texel of the whole picture at this screen pixel */
            int i = pl - 1;
            const packs_pose_place *q = &L.places[i];
            int u = k % SCHED_WIDTH - px[i], v = k / SCHED_WIDTH - py[i];
            if (u >= 0 && v >= 0 && u < q->w && v < q->h) {
                if (hf[i])
                    u = q->w - 1 - u;
                if (vf[i])
                    v = q->h - 1 - v;
                map[0] = (uint16_t)(q->entry + 1);
                map[1] = (uint16_t)(u | v << 8);
                map[2] = (uint16_t)((p->flags & ~(SCHED_PROV_HFLIP | SCHED_PROV_VFLIP)) |
                                    (hf[i] ? SCHED_PROV_HFLIP : 0) | (vf[i] ? SCHED_PROV_VFLIP : 0) |
                                    (p->layer & 8 ? 32 : 0));
                map[3] = m->cgram[p->cidx] & 0x7fff;
                replaced++;
                last_id = 0;
                z[i] = p->layer >> 4;
                continue;
            }
        }
        uint64_t id = raw_id(p);
        int val;
        if (id == last_id) {
            val = last_val;
        } else {
            size_t h = (size_t)(id * 0x9E3779B97F4A7C15ull >> 40) & (CACHE - 1);
            int probes = 0;
            while (L.cache[h].gen == L.frame_gen && L.cache[h].id != id && probes < 16)
                h = (h + 1) & (CACHE - 1), probes++;
            if (L.cache[h].gen == L.frame_gen && L.cache[h].id == id) {
                val = L.cache[h].val;
            } else {
                int *e = kmap_find(&L.by_key, packs_key(m, p));
                val = e ? *e + 1 : 0;
                if (probes < 16) {
                    L.cache[h].id = id;
                    L.cache[h].val = val;
                    L.cache[h].gen = L.frame_gen;
                }
            }
            last_id = id;
            last_val = val;
        }
        map[0] = (uint16_t)val;
        map[1] = (uint16_t)(p->u | p->v << 8);
        map[2] = (uint16_t)(p->flags | (p->layer & 8 ? 32 : 0));   /* 32: color math */
        map[3] = m->cgram[p->cidx] & 0x7fff;
        replaced += val != 0;
    }
    /* A pose is a picture of its own: where it has cover beyond the
       original's pixels, it goes over what is behind the object (by the
       PPU's z level and, among sprites, OAM order), with that pixel under
       its transparent texels (64) instead of the behind G-buffer. */
    for (int i = 0; i < L.nplaces; i++) {
        const packs_pose_place *q = &L.places[i];
        if (z[i] < 0)
            continue;   /* not drawn this frame, or nothing of it won */
        for (int v0 = 0; v0 < q->h; v0++) {
            int y = py[i] + v0;
            if (y < 0 || y >= SCHED_HEIGHT)
                continue;
            for (int u0 = 0; u0 < q->w; u0++) {
                int x = px[i] + u0;
                if (x < 0 || x >= SCHED_WIDTH)
                    continue;
                const sched_prov_px *p = &prov[y * SCHED_WIDTH + x];
                int obj = (p->flags & SCHED_PROV_OBJ) != 0, wz = p->layer >> 4;
                if ((obj && by_oam[p->index & 127] == i + 1) || wz > z[i] ||
                    (obj && wz == z[i] && (p->index & 127) < front[i]))
                    continue;   /* its own pixel, or something in front of it */
                int u = hf[i] ? q->w - 1 - u0 : u0, v = vf[i] ? q->h - 1 - v0 : v0;
                if (!pose_opaque(q, u, v))
                    continue;
                uint16_t *e = map0 + 4 * (y * SCHED_WIDTH + x);
                replaced += e[0] == 0;
                e[0] = (uint16_t)(q->entry + 1);
                e[1] = (uint16_t)(u | v << 8);
                e[2] = (uint16_t)(SCHED_PROV_OBJ | 2 | (hf[i] ? SCHED_PROV_HFLIP : 0) |
                                  (vf[i] ? SCHED_PROV_VFLIP : 0) | (p->layer & 8 ? 32 : 0) | 64);
                e[3] = (p->flags & SCHED_PROV_BPP ? m->cgram[p->cidx] : m->cgram[0]) & 0x7fff;
            }
        }
    }
    return replaced;
}
