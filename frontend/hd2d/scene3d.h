/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 3D diorama geometry from a scene description (runtime/module.h): the
 * map's tile layers stood up into floors, walls and roofs, and the
 * frame's sprites as billboards. CPU only; hd2d.c draws the result. Every
 * quad is one 8x8 tile of the console's own VRAM, which the renderer's
 * shaders decode as they draw, so nothing here holds pixels.
 *
 * World space: x is the map's x and z its y, in map pixels; y is height
 * above the floor. The game stores no heights, so they are inferred from
 * the cells (docs/HD2D.md, "Heights"): each column's runs of blocked or
 * overhead cells stand up from their south edge, as a wall up to hmax
 * cells high, and whatever is left of a run north of that lies flat on
 * top of it, as a roof. That keeps every tile where the original's
 * three-quarter view drew it (screen row = z - height). */
#ifndef CT_SCENE3D_H
#define CT_SCENE3D_H

#include <stdint.h>

#include "module.h"

/* s3d_vertex.tile: what a quad shows, packed for the shader. */
#define S3D_ADDR_MASK   0x7FFFu      /* VRAM word address of the 8x8 tile */
#define S3D_PAL_SHIFT   15           /* palette, 3 bits */
#define S3D_2BPP        (1u << 18)
#define S3D_OBJ         (1u << 19)   /* sprite palettes (CGRAM 128-255) */
#define S3D_NORMAL_SHIFT 20          /* 3 bits: S3D_N_* */
#define S3D_OPAQUE      (1u << 23)   /* color 0 shows the backdrop instead of nothing */
#define S3D_RAISED      (1u << 24)   /* stood up or lifted (not floor): casts shadows */
#define S3D_OVERHEAD    (1u << 25)   /* the original drew it over sprites */
#define S3D_PACKED      (1u << 26)   /* asset packs: bits 0-17 are an atlas entry (a whole
                                        picture: uv 0-8 spans it), not a VRAM tile */
#define S3D_ENTRY_MASK  0x3FFFFu
/* Bits 27-31 are free, for asset packs: a pack's sampling hook (the
   shader's tileTexel(), shaders/scene3d.frag) gets the whole value, so it
   can look a tile up by (VRAM address = character base + tile id * words
   per tile, palette) or by a flag of its own set from s3d_set_cell_hook. */
enum { S3D_N_UP, S3D_N_SOUTH, S3D_N_EAST, S3D_N_WEST, S3D_N_NORTH, S3D_N_SPRITE,
       S3D_N_TERRAIN,   /* the world map's ground: the normal from its height map */
       S3D_N_WATER };   /* the world map's water: up, with a shimmer (with S3D_OBJ: an
                           entrance marker, drawn by the shader alone) */

typedef struct {
    float x, y, z;   /* world, map px */
    float u, v;      /* in the tile, 0-8 px, flips applied */
    uint32_t tile;
} s3d_vertex;

typedef struct {
    s3d_vertex *v;
    uint32_t *idx;   /* 6 per quad */
    int nv, ni, cap;   /* cap: quads */
    int ni_floor;    /* indices before this are floor quads (no shadow casters) */
} s3d_mesh;

typedef struct {
    int hmax;        /* wall height cap, cells */
    float stretch;   /* sprite billboards' height per screen px (upright things
                        look shorter from above than the floor does) */
    int cards;       /* port: layered cards (the 3DS ports' trick): no walls or
                        boxes; each layer lies flat at its own height, the
                        tiles drawn over sprites highest (S3D_CARD_*) */
    float card_shift;  /* port: cards: how far south a card moves per px it is lifted,
                          so it sits where the original drew it from the camera's
                          tilt (tan of the tilt from straight down; 0: 1) */
} s3d_params;

#define S3D_CARD_FRONT 3    /* the front layer over the back one, px */
#define S3D_CARD_OVER  16   /* tiles drawn over sprites (canopies, roofs), px */

void s3d_mesh_free(s3d_mesh *m);

/* The map's geometry: floor quads first (m->ni_floor), then the raised
   ones. Rebuild when the scene's map_generation changes. Returns the
   number of quads, or -1 out of memory. */
int s3d_build_map(const ct_scene *s, const s3d_params *p, s3d_mesh *m);

/* This frame's sprites: one quad per 8x8 piece of every visible OAM entry,
   standing at its object's foot point (entries no object owns stand at
   their own bottom edge). Returns the number of quads. */
int s3d_build_sprites(const ct_scene *s, const s3d_params *p, s3d_mesh *m);
/* port: how many quads at the end of that mesh are contact shadows (flat
   under each character's feet, S3D_OBJ | S3D_N_WATER; the shader draws
   them as a soft dark ellipse, to be blended by multiplying). */
int s3d_sprite_blobs(void);

/* One textured quad: corner tl, then a step of 8 px along u (the tile's
   +x) and along v (its +y, downward in the image); tile is the packed
   S3D_* value (VRAM address, palette, normal, flags), hflip and vflip
   mirror the image. For the cell hook below and for the builders. */
void s3d_emit(s3d_mesh *m, const float tl[3], const float u[3], const float v[3],
              uint32_t tile, int hflip, int vflip);
/* The same with the four corners given (tl, tr, bl, br): a tile draped
   over uneven ground. Returns 0, or -1 out of memory. */
int s3d_emit_corners(s3d_mesh *m, float c[4][3], uint32_t tile, int hflip, int vflip);

/* Asset packs: a hook asked for every cell before the builder makes its
   quads (the field's and the world map's). It may emit its own with
   s3d_emit and return nonzero, and the cell's generated quads are skipped
   (on the field, the side walls it would give too). NULL (the default):
   none. */
typedef int (*s3d_cell_hook)(void *user, const ct_scene *s, int x, int y, s3d_mesh *m);
void s3d_set_cell_hook(s3d_cell_hook fn, void *user);

/* Asset packs: a whole picture for an object (a pose), asked for every
   object s3d_build_sprites draws. Returns nonzero with *out filled to draw
   the object as that one quad, standing where its OAM entries would; 0
   for its OAM entries as usual. */
typedef struct {
    uint32_t tile;     /* S3D_PACKED | atlas entry (the flags are added) */
    int x, y, w, h;    /* its rectangle on the console's screen, px */
    int hflip, vflip;
} s3d_sprite_image;
typedef int (*s3d_sprite_hook)(void *user, const ct_scene *s, int obj, s3d_sprite_image *out);
void s3d_set_sprite_hook(s3d_sprite_hook fn, void *user);

/* The part of the map with pixels (map px), from the last s3d_build_map:
   the renderer keeps its camera from showing much beyond it. */
void s3d_map_bounds(int *x0, int *z0, int *x1, int *z1);

/* For tests and the renderer: the inferred height (px) of the top of the
   raised geometry over cell (x, z) of the world grid (16 px cells), from
   the last s3d_build_map. */
int s3d_height_at(int x, int z);

/* ---- the world map (CT_SCENE_WORLD), scene3d_world.c ----
   One copy of the (wrapping) map as a heightfield of 8 px tiles: water
   flat and lowest, walkable ground at 0, blocked ground raised by what
   its pixels look like (docs/HD2D.md, "The world map"). Floor quads
   first (m->ni_floor), then the raised ones. Rebuild when the scene's
   map_generation changes. Returns the number of quads, or -1. */
int s3d_build_world(const ct_scene *s, s3d_mesh *m);

/* What s3d_build_world made of each cell, for tests and the docs. */
enum { S3D_W_FLOOR, S3D_W_WATER, S3D_W_SHORE, S3D_W_FOREST, S3D_W_MOUNTAIN, S3D_W_FLAT };
int s3d_world_class(int x, int y);
/* The ground's height (px) at world point (x, z), wrapping. */
float s3d_world_height(float x, float z);
/* Its normals: w x h RGBA8 texels, one per tile corner (x, z) / 8,
   wrapping; rgb = normal * 0.5 + 0.5, a = 255. */
const uint8_t *s3d_world_normals(int *w, int *h);

/* The camera, for sprites that keep the console's pixel grid: eye, the
   point it looks at, unit right / up / forward, focal length in target
   px, the near plane's distance, the target size. */
typedef struct {
    float eye[3], at[3], right[3], up[3], fwd[3];
    float focal, near;
    int w, h;
} s3d_view;

/* This frame's world sprites, as billboards facing the camera: each
   object's pieces stand at its foot point on the ground, and are snapped
   so that one sprite pixel is a whole number of target pixels, on the
   target's pixel grid. Entries no object owns (captions) lie flat over
   the picture where the console drew them, at that same scale. With
   marks, a quad in the middle of each entrance (S3D_N_WATER | S3D_OBJ). Returns
   the number of quads. */
int s3d_build_world_sprites(const ct_scene *s, const s3d_view *v, int marks, s3d_mesh *m);

#endif
