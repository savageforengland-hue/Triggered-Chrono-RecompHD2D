/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The world map scene (runtime/module.h CT_SCENE_WORLD, docs/SCENE.md
 * "World map" W1-W8): both BG layers' metatiles expanded to 8x8 tilemaps
 * over the whole 192 x 128 tile map, the terrain class of every 8 px
 * cell, the exits, the camera, the party and the BG3 cloud layer, read at
 * the frame edge. Called from scene.c; reads only.
 *
 * The terrain only says "blocked" for mountains, dense forest and the
 * shore band alike. The renderer tells them apart from the tile pixels
 * (frontend/hd2d/scene3d_world.c); relief[] below corrects the metatiles
 * that guess gets wrong, as a 3dSen profile would. */
#include <string.h>

#include "bus.h"
#include "module.h"

int ct_world_scene(ct_scene *out);

enum {
    NMI_VECTOR = 0x0500,
    F_EVENT = 0x0138,            /* nonzero while an event script holds the party */
    LOCATION = 0x0100,             /* $1F0-$1F7: the world maps */
    PX_X = 0x0283, PX_Y = 0x0285,  /* the party, world px (u16) */
    BG1_HOFS = 0x0020, BG1_VOFS = 0x0026,   /* the NMI's PPU shadows (D = 0) */
    BG3_HOFS = 0x0024, BG3_VOFS = 0x002A,
    DEFS_FG = 0x3000, DEFS_BG = 0x3800,     /* 256 metatiles x 4 tilemap words */
    MAP_FG = 0x4000, MAP_BG = 0x5800,       /* 96 x 64 metatile indices */
    PROPS = 0x7000,                /* 2 bytes per ground metatile: 4 nibbles */
    EXITS = 0x7E00, N_EXITS = 0x1B38,       /* 7-byte records */
    MW = 96, MH = 64,              /* metatiles */
    TW = 2 * MW, TH = 2 * MH,      /* tiles, cells */
    VRAM_BG3_MAP = 0x7800,         /* $2109 = $79: 64 x 32 */
    CHR_BG12 = 0x2000, CHR_BG3 = 0x7000,
    PARTY_OAM_DX = 8, PARTY_OAM_DY = 17,    /* OAM position = foot - camera - this */
    LEADER_NODE = 0x0D70,          /* +$28: facing (W5) */
    CAPTION_Y = 184,               /* OAM entries from here down: the era caption */
};
static const uint8_t world_nmi[4] = {0x5C, 0x5E, 0x01, 0xC2};   /* JML $C2015E */

/* Metatiles the pixel guess gets wrong: (layer, index) -> class. Layer 1
   is the ground (BG2, $5800), 0 the overlay (BG1, $4000); the overlay's
   entry wins where both have one. Read from the 1000 A.D. map ($1F0);
   map 0 = every world map. Keep in sync with worldmap_probe.py's
   RELIEF (the scene_decode test compares the two). */
static const struct { uint16_t map; uint8_t layer, index, cls; } relief[] = {
    /* Guardia Castle on $1F0: grey-brown stone reads as mountain and its
       green-lit walls as forest; it stands on flat ground, as drawn. The
       metatiles are the castle's own (each used once on the map). */
    {0x1F0, 0, 161, CT_CELL_FLAT}, {0x1F0, 0, 162, CT_CELL_FLAT}, {0x1F0, 0, 163, CT_CELL_FLAT},
    {0x1F0, 0, 177, CT_CELL_FLAT}, {0x1F0, 0, 178, CT_CELL_FLAT}, {0x1F0, 0, 193, CT_CELL_FLAT},
    {0x1F0, 0, 194, CT_CELL_FLAT}, {0x1F0, 0, 195, CT_CELL_FLAT}, {0x1F0, 0, 196, CT_CELL_FLAT},
    {0x1F0, 1, 176, CT_CELL_FLAT}, {0x1F0, 1, 177, CT_CELL_FLAT}, {0x1F0, 1, 178, CT_CELL_FLAT},
    {0x1F0, 1, 179, CT_CELL_FLAT}, {0x1F0, 1, 180, CT_CELL_FLAT}, {0x1F0, 1, 193, CT_CELL_FLAT},
    {0x1F0, 1, 194, CT_CELL_FLAT}, {0x1F0, 1, 195, CT_CELL_FLAT}, {0x1F0, 1, 196, CT_CELL_FLAT},
    {0x1F0, 1, 209, CT_CELL_FLAT}, {0x1F0, 1, 210, CT_CELL_FLAT}, {0x1F0, 1, 211, CT_CELL_FLAT},
};

static uint16_t tiles[CT_SCENE_LAYERS][CT_SCENE_WORLD_STRIDE * TH];
static ct_scene_cell cells[CT_SCENE_WORLD_STRIDE * TH];
static uint64_t last_hash;
static uint32_t generation;

static unsigned u16at(const uint8_t *w, unsigned a) { return w[a] | w[a + 1] << 8; }

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t k = 0; k < n; k++)
        h = (h ^ p[k]) * 0x100000001B3ull;
    return h;
}

static uint8_t relief_of(unsigned map, int layer, unsigned index)
{
    for (size_t k = 0; k < sizeof relief / sizeof *relief; k++)
        if (relief[k].cls && relief[k].layer == layer && relief[k].index == index &&
            (!relief[k].map || relief[k].map == map))
            return relief[k].cls;
    return 0;
}

static void build(const uint8_t *w, unsigned map)
{
    memset(tiles[0], 0, sizeof tiles[0]);
    memset(tiles[1], 0, sizeof tiles[1]);
    memset(cells, 0, sizeof cells);
    for (unsigned my = 0; my < MH; my++)
        for (unsigned mx = 0; mx < MW; mx++) {
            unsigned fg = w[MAP_FG + my * MW + mx], bg = w[MAP_BG + my * MW + mx];
            uint8_t cls_fg = fg ? relief_of(map, 0, fg) : 0, cls_bg = relief_of(map, 1, bg);
            for (unsigned q = 0; q < 4; q++) {   /* TL TR BL BR */
                unsigned x = 2 * mx + (q & 1), y = 2 * my + (q >> 1), at = y * CT_SCENE_WORLD_STRIDE + x;
                uint16_t ef = fg ? (uint16_t)u16at(w, DEFS_FG + fg * 8 + 2 * q) : 0;
                uint16_t eb = (uint16_t)u16at(w, DEFS_BG + bg * 8 + 2 * q);
                tiles[0][at] = ef;
                tiles[1][at] = eb;
                /* Terrain: from the ground metatile, a nibble per tile. */
                unsigned b = w[PROPS + bg * 2 + (y & 1)], n = x & 1 ? b & 15 : b >> 4;
                ct_scene_cell *c = &cells[at];
                c->raw = (uint8_t)n;
                if (n & 2)
                    c->cls = CT_CELL_WATER;
                else if (n & 1)
                    c->cls = cls_fg ? cls_fg : cls_bg ? cls_bg : CT_CELL_SOLID;
                else
                    c->cls = CT_CELL_FLOOR;
                c->flags = (uint8_t)((n & 4 ? CT_CELL_COVER : 0) |
                                     ((ef | eb) & 0x2000 ? CT_CELL_OVERHEAD : 0));
            }
        }
    /* Exits: the metatile under the party (W6). */
    unsigned n = w[N_EXITS];
    for (unsigned i = 0; i < n && i < 128; i++) {
        const uint8_t *r = w + EXITS + 7 * i;
        unsigned mx = r[0] & 0x7F, my = r[1] & 0x3F;
        if (!(r[0] & 0x80) || mx >= MW || my >= MH)
            continue;
        for (unsigned q = 0; q < 4; q++)
            cells[(2 * my + (q >> 1)) * CT_SCENE_WORLD_STRIDE + 2 * mx + (q & 1)].flags |=
                CT_CELL_EVENT;
    }
}

int ct_world_scene(ct_scene *out)
{
    const uint8_t *w = bus_wram();
    out->mode = CT_SCENE_OTHER;
    out->flags = 0;
    unsigned map = u16at(w, LOCATION) & 0x1FF;
    if (memcmp(w + NMI_VECTOR, world_nmi, sizeof world_nmi) || map < 0x1F0 || map > 0x1F7)
        return -1;

    uint64_t h = fnv(0xCBF29CE484222325ull, w + DEFS_FG, 0x4000);   /* defs and both maps */
    h = fnv(h, w + PROPS, 512);
    h = fnv(h, w + N_EXITS, 1);
    h = fnv(h, w + EXITS, 7 * (w[N_EXITS] < 128 ? w[N_EXITS] : 128));
    h = fnv(h, w + LOCATION, 2);
    if (h != last_hash || !generation) {
        last_hash = h;
        generation++;
        build(w, map);
    }

    out->mode = CT_SCENE_WORLD;
    out->cell_px = 8;
    out->map_w = TW;
    out->map_h = TH;
    out->map_id = map;
    out->map_generation = generation;
    int px = (int)u16at(w, PX_X), py = (int)u16at(w, PX_Y);
    /* Normally the party stays at (128,112), but the opening flyover pans
       independently. The BG1 shadows are the console's actual view; using
       the party here made the diorama show a different part of the map. */
    out->camera_x = (int32_t)(u16at(w, BG1_HOFS) % (16 * MW));
    out->camera_y = (int32_t)(u16at(w, BG1_VOFS) % (16 * MH));
    /* During free movement the party's foot is fixed at (128,112). A
       different relation means the event engine is panning the shot even
       if the party itself is stationary (the opening flyover). */
    int screen_x = (px - out->camera_x + 16 * MW) % (16 * MW);
    int screen_y = (py - out->camera_y + 16 * MH) % (16 * MH);
    if (w[F_EVENT] || screen_x != 128 || screen_y != 112)
        out->flags |= CT_SCENE_SCRIPTED;
    out->cells = cells;
    out->wrap = 1;

    /* The clouds: BG3's 64 x 32 tilemap (two 32 x 32 screens side by
       side), from VRAM as the console has it, half-added (W1: $2131 =
       $44), scrolled on their own. */
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 64; x++)
            tiles[2][y * CT_SCENE_WORLD_STRIDE + x] =
                out->vram ? out->vram[(VRAM_BG3_MAP + (x >> 5) * 1024 + y * 32 + (x & 31)) & 0x7FFF]
                          : 0;
    out->overlay_blend = 8;
    out->overlay_w = 64;
    out->overlay_h = 32;
    out->overlay_x = (int32_t)(u16at(w, BG3_HOFS) & 511);
    out->overlay_y = (int32_t)(u16at(w, BG3_VOFS) & 255);
    for (int l = 0; l < CT_SCENE_LAYERS; l++)
        out->layer[l] = (ct_scene_layer){
            .present = 1,
            .bpp = l < 2 ? 4 : 2,
            .chr_base = l < 2 ? CHR_BG12 : CHR_BG3,
            .pal_base = 0,
            .tiles = tiles[l],
        };

    /* The party: the OAM entries at its sprite's place (W5: the leader's
       16x16 piece is at its foot - camera - (8, 17)). */
    out->n_objects = 1;
    out->player = 0;
    ct_scene_object *ob = &out->object[0];
    memset(ob, 0, sizeof *ob);
    ob->x = px % (16 * MW);
    ob->y = py % (16 * MH);
    ob->facing = w[LEADER_NODE + 0x28] <= 3 ? w[LEADER_NODE + 0x28] : 0xFF;
    if (!out->oam)
        return 0;
    int ex[128], ey[128], vis[128];
    for (int e = 0; e < 128; e++) {
        unsigned w0 = out->oam[2 * e];
        ex[e] = (int)(w0 & 0xFF) | ((out->oam[0x100 + e / 8] >> (2 * (e % 8))) & 1) << 8;
        if (ex[e] >= 256)
            ex[e] -= 512;
        ey[e] = (int)(w0 >> 8);
        vis[e] = ey[e] < 224 && ex[e] > -16;   /* all pieces 16 x 16 here (OBSEL $60) */
    }
    int sx = (ob->x - out->camera_x + 16 * MW) % (16 * MW) - PARTY_OAM_DX;
    int sy = (ob->y - out->camera_y + 16 * MH) % (16 * MH) - PARTY_OAM_DY;
    if (sx >= 16 * MW - 16)
        sx -= 16 * MW;
    if (sy >= 16 * MH - 32)
        sy -= 16 * MH;
    for (int e = 0; e < 128 && ob->n_pieces < CT_SCENE_MAX_PIECES; e++)
        /* The 16 x 16 piece, and any piece on top of it (a 32 px tall pose). */
        if (vis[e] && ex[e] == sx && (ey[e] == sy || ey[e] == sy - 16)) {
            ob->piece[ob->n_pieces++] = (uint8_t)e;
            vis[e] = 0;
        }
    /* The other sprites in the play area (the ferry, people): one object
       per group of touching pieces, standing at the group's bottom middle.
       The era caption (y = 192, W5) and anything below the play area
       belong to no object: the renderer lays them flat over the picture. */
    for (int e = 0; e < 128 && out->n_objects < CT_SCENE_MAX_OBJECTS; e++) {
        if (!vis[e] || ey[e] >= CAPTION_Y)
            continue;
        ct_scene_object *o = &out->object[out->n_objects++];
        memset(o, 0, sizeof *o);
        o->facing = 0xFF;
        o->piece[o->n_pieces++] = (uint8_t)e;
        vis[e] = 0;
        int x0 = ex[e], x1 = ex[e] + 16, y1 = ey[e] + 16;
        for (int grew = 1; grew;) {
            grew = 0;
            for (int k = 0; k < 128 && o->n_pieces < CT_SCENE_MAX_PIECES; k++) {
                if (!vis[k] || ey[k] >= CAPTION_Y)
                    continue;
                for (int j = 0; j < o->n_pieces; j++) {
                    int p = o->piece[j];
                    if (ex[k] <= ex[p] + 16 && ex[k] + 16 >= ex[p] && ey[k] <= ey[p] + 16 &&
                        ey[k] + 16 >= ey[p]) {
                        o->piece[o->n_pieces++] = (uint8_t)k;
                        vis[k] = 0;
                        x0 = ex[k] < x0 ? ex[k] : x0;
                        x1 = ex[k] + 16 > x1 ? ex[k] + 16 : x1;
                        y1 = ey[k] + 16 > y1 ? ey[k] + 16 : y1;
                        grew = 1;
                        break;
                    }
                }
            }
        }
        o->x = ((out->camera_x + (x0 + x1) / 2) % (16 * MW) + 16 * MW) % (16 * MW);
        o->y = ((out->camera_y + y1) % (16 * MH) + 16 * MH) % (16 * MH);
    }
    return 0;
}
