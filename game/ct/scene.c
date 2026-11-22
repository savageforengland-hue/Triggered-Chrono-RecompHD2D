/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The field scene (runtime/module.h, docs/SCENE.md): the map, its tile
 * layers, cell properties, camera and objects, read from WRAM at the frame
 * edge for the engine's 3D renderer; the world map's is world_scene.c. Linked into the game module (the
 * ct_game_scene export) and into the static programs. Reads only. */
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "module.h"
#include "sprites.h"

int ct_game_scene(ct_scene *out);
int ct_game_pose(int obj, ct_scene_pose *out, uint8_t *rgba, int max);
int ct_world_scene(ct_scene *out);   /* world_scene.c */

/* WRAM offsets (bank $7E unless noted; docs/SCENE.md A0). */
enum {
    NMI_VECTOR = 0x0500,         /* JML here: the running main loop's NMI handler */
    LOCATION = 0x0100,           /* u16, 9 bits */
    LEADER = 0x0197,             /* 2 * slot, $80 none */
    L1_W = 0x0BCB, L1_H = 0x0BCD, L2_W = 0x0BCF, L2_H = 0x0BD1, L3_W = 0x0BD3, L3_H = 0x0BD5,
    L3_ON = 0x0BCA,              /* bit 0 */
    CAM_TX = 0x1D0A, CAM_TY = 0x1D0E,   /* camera left / top, 8 px units */
    BG1_HOFS = 0x1D87, BG1_VOFS = 0x1D89,
    GRID = 0x3000,               /* rows of 256: L1 +$00, L2 +$40, L3 +$80, props +$C0 */
    PROPS = 0x7000,              /* rows of 256: A +$00, B +$40, exits +$80, zones +$C0 */
    META12 = 0xB000,             /* 512 x 4 tilemap words: L1 and L2 metatiles */
    META3 = 0xC000,              /* 256 x 4: L3 metatiles */
    OBJ_COUNT = 0x12000,         /* $7F2000 */
    OBJ_TYPE = 0x1100, OBJ_X = 0x1800, OBJ_Y = 0x1880, OBJ_FACING = 0x1600,
    OBJ_ACTIVE = 0x0F00, OBJ_SLOT_TILE = 0x0D00,
};
/* The field loop's NMI handler (JML $C0EA63) is installed while it runs;
   battles, menus, the world map and the title install their own. */
static const uint8_t field_nmi[4] = {0x5C, 0x63, 0xEA, 0xC0};
/* Menu_InstallInterruptVectors: name entry, pause/save, shops and the
   other full-screen menu views all use JML $C2840E. */
static const uint8_t ui_nmi[4] = {0x5C, 0x0E, 0x84, 0xC2};

/* BG character bases while the field runs ($210B = $11, $210C = $05). */
enum { CHR_BG12 = 0x1000, CHR_BG3 = 0x5000 };

static uint16_t tiles[CT_SCENE_LAYERS][CT_SCENE_STRIDE * 2 * CT_SCENE_MAX_CELLS];
static ct_scene_cell cells[CT_SCENE_MAX_CELLS * CT_SCENE_MAX_CELLS];
static uint64_t last_hash;
static uint32_t generation;
/* The object slot of each object[] entry of the last field scene (poses). */
static int8_t slot_of[CT_SCENE_MAX_OBJECTS];
static int n_slots;

static unsigned u16at(const uint8_t *w, unsigned a) { return w[a] | w[a + 1] << 8; }

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t k = 0; k < n; k++)
        h = (h ^ p[k]) * 0x100000001B3ull;
    return h;
}

static uint8_t cell_class(unsigned c)
{
    if (c == 0)
        return CT_CELL_FLOOR;
    if (c == 1)
        return CT_CELL_SOLID;
    if (c <= 0x15)
        return CT_CELL_WALLISH;   /* shaped: corners and diagonals */
    if (c == 0x18 || c == 0x19 || c == 0x1D)
        return CT_CELL_STAIRS;    /* seen under the leader on stairs (SCENE.md A0.3) */
    return CT_CELL_UNKNOWN;
}

/* Expand the metatile grids into 8x8 tilemaps and decode the cells. */
static void build(const uint8_t *w, unsigned mw, unsigned mh)
{
    memset(tiles, 0, sizeof tiles);
    memset(cells, 0, sizeof cells);
    for (unsigned y = 0; y < mh; y++)
        for (unsigned x = 0; x < mw; x++) {
            unsigned g = GRID + y * 256 + x, pb = w[g + 0xC0];
            unsigned over = 0;
            for (int l = 0; l < CT_SCENE_LAYERS; l++) {
                unsigned base;
                if (l < 2)   /* L1, L2: 9-bit index, bit 8 in the property byte */
                    base = META12 + (w[g + 0x40 * l] | ((pb >> l) & 1) << 8) * 8;
                else
                    base = META3 + w[g + 0x80] * 8;
                uint16_t *t = &tiles[l][(2 * y) * CT_SCENE_STRIDE + 2 * x];
                for (int q = 0; q < 4; q++) {   /* TL TR BL BR */
                    uint16_t e = (uint16_t)u16at(w, base + 2 * q);
                    t[(q >> 1) * CT_SCENE_STRIDE + (q & 1)] = e;
                    if (l < 2 && (e & 0x2000))
                        over = 1;
                }
            }
            unsigned cls = pb >> 2, a = w[PROPS + y * 256 + x], b = w[PROPS + 0x40 + y * 256 + x];
            ct_scene_cell *c = &cells[y * CT_SCENE_MAX_CELLS + x];
            c->cls = cell_class(cls);
            c->raw = (uint8_t)cls;
            c->plane = b & 3;
            c->flags = (over ? CT_CELL_OVERHEAD : 0) |
                       ((a & 0x10) || w[PROPS + 0x80 + y * 256 + x] != 0x80 ? CT_CELL_EVENT : 0);
        }
}

int ct_game_scene(ct_scene *out)
{
    const uint8_t *w = bus_wram();
    out->mode = CT_SCENE_OTHER;
    n_slots = 0;
    if (!memcmp(w + NMI_VECTOR, ui_nmi, sizeof ui_nmi)) {
        out->mode = CT_SCENE_UI;
        return 0;
    }
    if (memcmp(w + NMI_VECTOR, field_nmi, sizeof field_nmi))
        return ct_world_scene(out);   /* the world map, or nothing */
    unsigned mw = w[L1_W] > w[L2_W] ? w[L1_W] : w[L2_W];
    unsigned mh = w[L1_H] > w[L2_H] ? w[L1_H] : w[L2_H];
    if (!mw || !mh || mw > CT_SCENE_MAX_CELLS || mh > CT_SCENE_MAX_CELLS)
        return -1;

    /* The map changes on a load and in place (event tile copies, opened
       chests): the grids and metatile tables decide. */
    uint64_t h = fnv(0xCBF29CE484222325ull, w + GRID, 64 * 256);
    h = fnv(h, w + META12, 512 * 8);
    h = fnv(h, w + META3, 256 * 8);
    h = fnv(h, w + PROPS, 64 * 256);
    h = fnv(h, w + LOCATION, 2);
    h = fnv(h, w + L1_W, 12);
    if (h != last_hash || !generation) {
        last_hash = h;
        generation++;
        build(w, mw, mh);
    }

    out->mode = CT_SCENE_FIELD;
    out->cell_px = 16;
    out->map_w = (uint16_t)mw;
    out->map_h = (uint16_t)mh;
    out->map_id = u16at(w, LOCATION) & 0x1FF;
    out->map_generation = generation;
    out->camera_x = w[CAM_TX] * 8 + (u16at(w, BG1_HOFS) & 7);
    out->camera_y = w[CAM_TY] * 8 + (u16at(w, BG1_VOFS) & 7);
    for (int l = 0; l < CT_SCENE_LAYERS; l++) {
        out->layer[l] = (ct_scene_layer){
            .present = l < 2 || (w[L3_ON] & 1),
            .bpp = l < 2 ? 4 : 2,
            .chr_base = l < 2 ? CHR_BG12 : CHR_BG3,
            .pal_base = 0,
            .tiles = tiles[l],
        };
    }
    out->cells = cells;

    /* Objects, and the OAM entries each draws: a field sprite's pieces
       use tiles from its VRAM slot ($0D00: the slot's OAM tile number). */
    unsigned n = w[OBJ_COUNT] < CT_SCENE_MAX_OBJECTS ? w[OBJ_COUNT] : CT_SCENE_MAX_OBJECTS;
    unsigned leader = w[LEADER];
    out->player = 0xFF;
    out->n_objects = 0;
    int owner[64];   /* object index per slot tile group (tile9 >> 3), -1 none */
    memset(owner, -1, sizeof owner);
    for (unsigned i = 0; i < n; i++) {
        unsigned o = 2 * i, type = w[OBJ_TYPE + o];
        if ((type & 0x80) || type == 7)
            continue;
        ct_scene_object *ob = &out->object[out->n_objects];
        memset(ob, 0, sizeof *ob);
        ob->x = (int32_t)(u16at(w, OBJ_X + o) >> 4);
        ob->y = (int32_t)(u16at(w, OBJ_Y + o) >> 4);
        ob->facing = w[OBJ_FACING + o] <= 3 ? w[OBJ_FACING + o] : 0xFF;
        if (w[OBJ_ACTIVE + o] & 0x80) {
            unsigned grp = (u16at(w, OBJ_SLOT_TILE + o) & 0x1F8) >> 3;
            if (owner[grp] < 0)
                owner[grp] = out->n_objects;
        }
        if (!(leader & 0x80) && leader == o)
            out->player = out->n_objects;
        slot_of[out->n_objects] = (int8_t)i;
        out->n_objects++;
    }
    n_slots = out->n_objects;
    if (out->oam)
        for (int e = 0; e < 128; e++) {
            unsigned w1 = out->oam[2 * e + 1], tile9 = w1 & 0x1FF;
            int k = owner[tile9 >> 3];
            if (k < 0)
                continue;
            /* Hidden entries (parked below the screen) draw nothing. */
            unsigned y = out->oam[2 * e] >> 8;
            unsigned size = out->obj_size[(out->oam[0x100 + e / 8] >> (2 * (e % 8) + 1)) & 1];
            if (y >= 224 && y + size <= 256)
                continue;
            ct_scene_object *ob = &out->object[k];
            if (ob->n_pieces < CT_SCENE_MAX_PIECES)
                ob->piece[ob->n_pieces++] = (uint8_t)e;
        }
    return 0;
}

/* Poses (module.h ct_module_pose_fn) from the sprite decoder (sprites.h):
   the key is kind << 16 | id << 8 | frame, the same pose on any map and
   in any slot; one sheet per sprite record (kind and id). The game
   mirrors 8x8 tiles inside a frame, never a whole frame, so a pose is
   never drawn flipped. */
int ct_game_pose(int obj, ct_scene_pose *out, uint8_t *rgba, int max)
{
    static const char *const kinds[4] = {"pc", "npc", "enemy", "other"};
    if (obj < 0 || obj >= n_slots)
        return -1;
    ct_sprite_info in;
    int r = rgba ? ct_sprite_frame(slot_of[obj], rgba, max, &in)
                 : ct_sprite_describe(slot_of[obj], &in);
    if (r || in.kind == CT_SPRITE_OTHER)
        return -1;
    memset(out, 0, sizeof *out);
    out->key = in.key | 1u << 24;   /* never 0 */
    out->sheet = (uint32_t)in.kind << 8 | in.id;
    out->w = in.w;
    out->h = in.h;
    out->origin_x = in.origin_x;
    out->origin_y = in.origin_y;
    snprintf(out->name, sizeof out->name, "%s%02x", kinds[in.kind & 3], in.id);
    return 0;
}
