/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The 3D diorama's geometry (frontend/hd2d/scene3d.c) on a synthetic
 * scene: a floor of one tile with one column of blocked cells, which must
 * stand up as a wall of hmax cells with the rest of the run on top, side
 * walls where the box ends, and the ground behind it filled; and sprites
 * standing at their object's foot point. And a small world map
 * (scene3d_world.c): water flat and lowest, a red block raised as a
 * mountain, a green one as a forest canopy, the shore band, wrapping, and
 * the party's billboard on the target's pixel grid. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hd2d/scene3d.h"

static int fails;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (!(c)) {                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            fails++;                                      \
        }                                                 \
    } while (0)

static uint16_t vram[32768], cgram[256], oam[272];
static uint16_t tiles[CT_SCENE_LAYERS][CT_SCENE_STRIDE * 2 * CT_SCENE_MAX_CELLS];
static ct_scene_cell cells[CT_SCENE_MAX_CELLS * CT_SCENE_MAX_CELLS];

enum { MW = 6, MH = 8, WALL_X = 2, RUN_A = 1, RUN_B = 4 };

static void make_scene(ct_scene *s)
{
    memset(s, 0, sizeof *s);
    /* Tile 1 at character base $1000 has pixels; tile 0 is empty. */
    for (int k = 0; k < 16; k++)
        vram[0x1000 + 16 + k] = 0x8181;
    for (int y = 0; y < 2 * MH; y++)
        for (int x = 0; x < 2 * MW; x++)
            tiles[1][y * CT_SCENE_STRIDE + x] = 0x0001;   /* BG2: tile 1; BG1: empty */
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            int solid = x == WALL_X && y >= RUN_A && y <= RUN_B;
            cells[y * CT_SCENE_MAX_CELLS + x].cls = solid ? CT_CELL_SOLID : CT_CELL_FLOOR;
        }
    s->mode = CT_SCENE_FIELD;
    s->cell_px = 16;
    s->map_w = MW;
    s->map_h = MH;
    for (int l = 0; l < CT_SCENE_LAYERS; l++)
        s->layer[l] = (ct_scene_layer){l < 2, 4, 0x1000, 0, tiles[l]};
    s->cells = cells;
    s->vram = vram;
    s->cgram = cgram;
    s->oam = oam;
    s->obj_chr[0] = 0x1000;
    s->obj_chr[1] = 0x2000;
    s->obj_size[0] = 8;
    s->obj_size[1] = 16;
}

static int has_vertex(const s3d_mesh *m, float x, float y, float z)
{
    for (int k = 0; k < m->nv; k++)
        if (fabsf(m->v[k].x - x) < 0.2f && fabsf(m->v[k].y - y) < 0.2f &&
            fabsf(m->v[k].z - z) < 0.2f)
            return 1;
    return 0;
}

static void test_map(void)
{
    static ct_scene s;
    make_scene(&s);
    s3d_mesh m = {0};
    s3d_params p = {3, 1.0f};
    int quads = s3d_build_map(&s, &p, &m);
    /* Floors: 44 free cells, plus 3 run rows not under the box, filled
       from the ground north of the run; raised: the 4 run cells; sides: 3
       bands on both sides of the one-cell box. 4 quarters each. */
    CHECK(m.ni_floor / 6 == (44 + 3) * 4, "floor quads %d", m.ni_floor / 6);
    CHECK(quads == (44 + 3 + 4 + 2 * 3) * 4, "quads %d", quads);
    /* The wall stands at the run's south edge, 3 cells high. */
    CHECK(has_vertex(&m, WALL_X * 16, 48, (RUN_B + 1) * 16), "wall top corner");
    CHECK(has_vertex(&m, WALL_X * 16, 0, (RUN_B + 1) * 16), "wall foot");
    /* The rest of the run (row 1) lies on top, moved south by the wall's height. */
    CHECK(has_vertex(&m, WALL_X * 16, 48, (RUN_A + 3) * 16), "roof");
    CHECK(s3d_height_at(WALL_X, RUN_B) == 48 && s3d_height_at(WALL_X, RUN_A) == 0 &&
              s3d_height_at(WALL_X + 1, RUN_B) == 0,
          "box heights");
    /* Side walls on the box's east and west faces. */
    CHECK(has_vertex(&m, (WALL_X + 1) * 16, 48, RUN_B * 16), "east side");
    CHECK(has_vertex(&m, WALL_X * 16, 48, (RUN_B + 1) * 16), "west side");
    int opaque = 0, raised = 0;
    for (int k = 0; k < m.nv; k += 4) {
        opaque += (m.v[k].tile & S3D_OPAQUE) != 0;
        raised += (m.v[k].tile & S3D_RAISED) != 0;
        CHECK((m.v[k].tile & S3D_ADDR_MASK) == 0x1010, "tile address %x", m.v[k].tile);
    }
    CHECK(raised == (4 + 6) * 4, "raised %d", raised);
    /* The back layer is opaque (color 0: the backdrop) except where it
       rises as a cut-out: posts, trunks and canopies show what is behind. */
    CHECK(opaque == quads - 4 * 4, "opaque %d of %d", opaque, quads);
    s3d_mesh_free(&m);
}

static void test_disconnected_rooms(void)
{
    enum { RW = 32, RH = 8, ROOM_W = 6, ROOM_H = 6, ROOM_B_X = 24 };
    static ct_scene s;
    memset(&s, 0, sizeof s);
    memset(vram, 0, sizeof vram);
    memset(tiles, 0, sizeof tiles);
    memset(cells, 0, sizeof cells);
    for (int k = 0; k < 16; k++)
        vram[0x1000 + 16 + k] = 0x8181;
    for (int room = 0; room < 2; room++) {
        int rx = room ? ROOM_B_X : 1;
        for (int y = 1; y <= ROOM_H; y++)
            for (int x = rx; x < rx + ROOM_W; x++) {
                for (int qy = 0; qy < 2; qy++)
                    for (int qx = 0; qx < 2; qx++)
                        tiles[1][(2 * y + qy) * CT_SCENE_STRIDE + 2 * x + qx] = 1;
                cells[y * CT_SCENE_MAX_CELLS + x].cls = CT_CELL_FLOOR;
            }
    }
    s.mode = CT_SCENE_FIELD;
    s.cell_px = 16;
    s.map_w = RW;
    s.map_h = RH;
    for (int l = 0; l < CT_SCENE_LAYERS; l++)
        s.layer[l] = (ct_scene_layer){l < 2, 4, 0x1000, 0, tiles[l]};
    s.cells = cells;
    s.vram = vram;
    s.cgram = cgram;
    s.oam = oam;

    s3d_mesh m = {0};
    s3d_params p = {3, 1.0f};
    int quads = s3d_build_map(&s, &p, &m);
    int x0, y0, x1, y1;
    s3d_map_bounds(&x0, &y0, &x1, &y1);
    CHECK(quads == ROOM_W * ROOM_H * 4, "first-room quads %d", quads);
    CHECK(x0 == 16 && y0 == 16 && x1 == 112 && y1 == 112, "first bounds %d %d %d %d",
          x0, y0, x1, y1);
    CHECK(s3d_room_matches(&s), "first room should still match");
    CHECK(fabsf(s3d_room_zoom(0.75f) - 1.25f) < 0.001f, "small-room zoom %.2f",
          s3d_room_zoom(0.75f));

    s.camera_x = 1;
    CHECK(!s3d_room_matches(&s), "right edge entering a cell must invalidate the mesh");
    s.camera_x = 128;   /* the transition gap contains no walkable cells */
    CHECK(!s3d_room_matches(&s), "camera movement must invalidate the room");
    s.camera_x = 256;
    CHECK(!s3d_room_matches(&s), "second room should request a rebuild");
    quads = s3d_build_map(&s, &p, &m);
    s3d_map_bounds(&x0, &y0, &x1, &y1);
    CHECK(quads == ROOM_W * ROOM_H * 4, "second-room quads %d", quads);
    CHECK(x0 == ROOM_B_X * 16 && y0 == 16 && x1 == (ROOM_B_X + ROOM_W) * 16 && y1 == 112,
          "second bounds %d %d %d %d", x0, y0, x1, y1);
    CHECK(s3d_room_matches(&s), "second room should match after rebuild");
    s3d_mesh_free(&m);
}

static void test_sprites(void)
{
    static ct_scene s;
    make_scene(&s);
    s.camera_x = 100;
    s.camera_y = 200;
    for (int e = 0; e < 128; e++)
        oam[2 * e] = 0xF0 << 8;   /* hidden */
    /* Entry 3: 16x16 at screen (40, 60); its object's foot is at screen
       (48, 74): 2 px above the sprite's bottom, so it is lifted 2 px. */
    oam[2 * 3] = (uint16_t)(60 << 8 | 40);
    oam[2 * 3 + 1] = 0x0002;
    oam[0x100 + 0] = 2 << 6;      /* entry 3: large */
    /* Entry 9: 8x8 at (10, 20), owned by nobody. */
    oam[2 * 9] = (uint16_t)(20 << 8 | 10);
    s.n_objects = 1;
    s.object[0] = (ct_scene_object){.x = 148, .y = 274, .facing = 1, .n_pieces = 1, .piece = {3}};
    s3d_mesh m = {0};
    s3d_params p = {3, 1.0f};
    int quads = s3d_build_sprites(&s, &p, &m);
    CHECK(quads == 4 + 1, "sprite quads %d", quads);
    /* Its top-left tile: x = camera + 40, top at 74 + 2 - 60 = 16 px up, at the foot's z. */
    CHECK(has_vertex(&m, 140, 16, 274.0f + 124 * 0.004f), "billboard top-left");
    CHECK(has_vertex(&m, 156, 0, 274.0f + 124 * 0.004f), "billboard foot");
    /* Unowned: stands at its own bottom (screen row 28 -> z = 228). */
    CHECK(has_vertex(&m, 110, 8, 228.0f + 118 * 0.004f), "unowned piece");
    CHECK((m.v[0].tile & S3D_OBJ) && ((m.v[0].tile >> S3D_NORMAL_SHIFT) & 7) == S3D_N_SPRITE,
          "sprite flags");
    s3d_mesh_free(&m);
}

/* ---- the world map ---- */

enum { WW = 48, WH = 32 };
static uint16_t wtiles[CT_SCENE_LAYERS][CT_SCENE_WORLD_STRIDE * WH];
static ct_scene_cell wcells[CT_SCENE_WORLD_STRIDE * WH];

static void make_world(ct_scene *s)
{
    memset(s, 0, sizeof *s);
    memset(vram, 0, sizeof vram);
    memset(wtiles, 0, sizeof wtiles);
    memset(wcells, 0, sizeof wcells);
    /* Tile 1 at $2000: color 1 everywhere. Palettes: 0 blue, 1 red-brown,
       2 green, 3 sand. */
    for (int k = 0; k < 8; k++)
        vram[0x2000 + 16 + k] = 0x00FF;
    cgram[1] = 31 << 10 | 6 << 5 | 4;
    cgram[17] = 4 << 10 | 10 << 5 | 24;
    cgram[33] = 4 << 10 | 22 << 5 | 6;
    cgram[49] = 16 << 10 | 22 << 5 | 26;
    for (int y = 0; y < WH; y++)
        for (int x = 0; x < WW; x++) {
            int water = x < 8, mtn = x >= 16 && x < 28 && y >= 8 && y < 22,
                forest = x >= 32 && x < 42 && y >= 10 && y < 20, shore = x == 8 || x == 9;
            int pal = water ? 0 : mtn ? 1 : forest ? 2 : shore ? 3 : 2;
            wtiles[1][y * CT_SCENE_WORLD_STRIDE + x] = (uint16_t)(pal << 10 | 1);
            wcells[y * CT_SCENE_WORLD_STRIDE + x].cls =
                water ? CT_CELL_WATER : mtn || forest || shore ? CT_CELL_SOLID : CT_CELL_FLOOR;
        }
    s->mode = CT_SCENE_WORLD;
    s->cell_px = 8;
    s->map_w = WW;
    s->map_h = WH;
    s->wrap = 1;
    for (int l = 0; l < CT_SCENE_LAYERS; l++)
        s->layer[l] = (ct_scene_layer){l < 2, l < 2 ? 4 : 2, 0x2000, 0, wtiles[l]};
    s->cells = wcells;
    s->vram = vram;
    s->cgram = cgram;
    s->oam = oam;
    s->obj_chr[0] = 0x1000;
    s->obj_chr[1] = 0x2000;
    s->obj_size[0] = 16;
    s->obj_size[1] = 32;
}

static void test_world(void)
{
    static ct_scene s;
    make_world(&s);
    s3d_mesh m = {0};
    int quads = s3d_build_world(&s, &m);
    CHECK(quads == WW * WH, "world quads %d", quads);   /* one per cell; BG1 is empty */
    CHECK(s3d_world_class(2, 5) == S3D_W_WATER && s3d_world_class(22, 15) == S3D_W_MOUNTAIN &&
              s3d_world_class(36, 15) == S3D_W_FOREST && s3d_world_class(9, 5) == S3D_W_SHORE &&
              s3d_world_class(12, 5) == S3D_W_FLOOR,
          "classes %d %d %d %d %d", s3d_world_class(2, 5), s3d_world_class(22, 15),
          s3d_world_class(36, 15), s3d_world_class(9, 5), s3d_world_class(12, 5));
    /* Water flat and lowest; the floor at 0; the mountain's middle high,
       its edge lower; the canopy a few px up. */
    float hw = s3d_world_height(20, 40), hf = s3d_world_height(100, 40);
    float hm = s3d_world_height(22 * 8, 15 * 8), he = s3d_world_height(16 * 8 + 2, 15 * 8);
    float hc = s3d_world_height(37 * 8, 15 * 8);
    CHECK(hw < -4 && fabsf(hf) < 0.01f && hm > 30 && he < hm - 10 && hc > 3 && hc < 12,
          "heights water %.1f floor %.1f mountain %.1f edge %.1f canopy %.1f", hw, hf, hm, he, hc);
    /* It wraps: west of x = 0 is the east edge (floor), and the water
       column next to it is still flat. */
    CHECK(fabsf(s3d_world_height(-4, 40) - s3d_world_height(WW * 8 - 4, 40)) < 0.01f, "wrap");
    int water = 0, raised = 0;
    for (int k = 0; k < m.nv; k += 4) {
        int n = (m.v[k].tile >> S3D_NORMAL_SHIFT) & 7;
        if (n == S3D_N_WATER) {
            water++;
            for (int j = 0; j < 4; j++)
                CHECK(fabsf(m.v[k + j].y - hw) < 0.01f, "water not flat");
        }
        raised += (m.v[k].tile & S3D_RAISED) != 0;
    }
    CHECK(water == 8 * WH, "water quads %d", water);
    CHECK(m.ni_floor / 6 + raised == quads && raised > 12 * 14, "raised %d", raised);

    /* The party: one 16x16 entry at its console place; the billboard lands
       on whole target pixels, 16 sprite px = 16 k target px. */
    for (int e = 0; e < 128; e++)
        oam[2 * e] = 0xF0 << 8;
    memset(&oam[0x100], 0, 16 * sizeof oam[0]);
    oam[2 * 7] = (uint16_t)(95 << 8 | 120);
    oam[2 * 7 + 1] = 0x010E;
    s.camera_x = 100;
    s.camera_y = 50;
    s.n_objects = 1;
    s.object[0] = (ct_scene_object){.x = 228, .y = 162, .n_pieces = 1, .piece = {7}};
    s3d_view v = {.eye = {228, 300, 162 + 250}, .at = {228, 0, 170}, .w = 1280, .h = 960,
                  .near = 50};
    float f[3] = {v.at[0] - v.eye[0], v.at[1] - v.eye[1], v.at[2] - v.eye[2]};
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int k = 0; k < 3; k++)
        v.fwd[k] = f[k] / l;
    v.right[0] = 1;   /* fwd has no x */
    v.up[1] = -v.fwd[2];
    v.up[2] = v.fwd[1];
    v.focal = 0.5f * 960 / tanf(15.0f * 3.14159265f / 180.0f);
    s3d_mesh sp = {0};
    int n = s3d_build_world_sprites(&s, &v, 0, &sp);
    CHECK(n == 4, "sprite quads %d", n);
    float xs[16], ys[16];
    for (int k = 0; k < sp.nv && k < 16; k++) {
        float r[3] = {sp.v[k].x - v.eye[0], sp.v[k].y - v.eye[1], sp.v[k].z - v.eye[2]};
        float d = r[0] * v.fwd[0] + r[1] * v.fwd[1] + r[2] * v.fwd[2];
        xs[k] = 0.5f * v.w + v.focal * (r[0] * v.right[0] + r[1] * v.right[1] + r[2] * v.right[2]) / d;
        ys[k] = 0.5f * v.h - v.focal * (r[0] * v.up[0] + r[1] * v.up[1] + r[2] * v.up[2]) / d;
        CHECK(fabsf(xs[k] - roundf(xs[k])) < 0.01f && fabsf(ys[k] - roundf(ys[k])) < 0.01f,
              "corner %d off the pixel grid: %.3f %.3f", k, xs[k], ys[k]);
    }
    float w8 = xs[1] - xs[0], h8 = ys[2] - ys[0];
    CHECK(w8 >= 8 && fabsf(w8 - h8) < 0.01f && fmodf(w8, 8.0f) < 0.01f, "texel size %.2f x %.2f",
          w8, h8);
    s3d_mesh_free(&sp);
    s3d_mesh_free(&m);
}

static void test_composition_and_camera(void)
{
    static ct_scene s;
    make_scene(&s);
    memset(oam, 0, sizeof oam);
    s.n_objects = 1;
    s.object[0] = (ct_scene_object){.x = WALL_X * 16, .y = RUN_A * 16,
                                   .n_pieces = 1, .piece = {0}};
    oam[0] = 16 << 8 | 32;
    CHECK(s3d_field_needs_2d(&s), "blocked furniture must preserve console composition");
    s.object[0].x = 16;
    CHECK(!s3d_field_needs_2d(&s), "ordinary floor actor stays 3D");
    s.object[0].x = WALL_X * 16;
    oam[0] = 240 << 8;
    CHECK(!s3d_field_needs_2d(&s), "offscreen furniture actor must not force 2D");
    oam[0] = 16 << 8 | 32;
    s.mode = CT_SCENE_WORLD;
    CHECK(!s3d_field_needs_2d(&s), "world uses its own geometry");
    s.player = 0;
    s.object[0].x = 999;
    s.object[0].y = 999;
    s.camera_x = 300;
    s.camera_y = 500;
    float at[3];
    s3d_camera_target(&s, at);
    CHECK(at[0] == 428 && at[1] == 0 && at[2] == 620,
          "flyover follows console scroll, not the party");
    s.camera_x += 8;
    s3d_camera_target(&s, at);
    CHECK(at[0] == 436, "scripted pan advances with a stationary player");
}

int main(void)
{
    test_map();
    test_disconnected_rooms();
    test_sprites();
    test_world();
    test_composition_and_camera();
    if (fails)
        return 1;
    printf("scene3d: ok\n");
    return 0;
}
