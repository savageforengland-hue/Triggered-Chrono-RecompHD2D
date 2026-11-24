/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Game modules: a game's generated code built as a shared library on the
 * user's machine from their own ROM, loaded by an engine that ships with
 * no game code at all. The module exports one function, ct_module_get,
 * describing its tables; everything else it calls (bus, scheduler,
 * interpreter) it takes from the engine. */
#ifndef CT_MODULE_H
#define CT_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "func_table.h"
#include "rumble.h"

/* Bump when CPU, the table entry layouts, or what generated code expects
   of the engine change. The ops.h helpers are compiled into the module, so
   a change to what they do counts too: 2 is decimal-mode ADC/SBC (a module
   built for 1 stops on the first ADC/SBC run with D=1); 3 is a compiled
   callee that comes back elsewhere handing the caller's rest to the
   executor (ops.h cpu_return_ok) instead of stopping, and a jump table
   index past the recorded entries read from the bus and dispatched; 4 is
   the scene export (ct_module_info.scene and scene_size, ct_scene); 5 is
   game patches (funcs.toml `patch`) and the players API they call
   (sched.h); 6 is the preferences API patches call (sched_pref: a module
   with the always-run patches needs it from the engine), and save
   states record it (savestate.h); 7 is the world map scene
   (CT_SCENE_WORLD and ct_scene's overlay fields); 8 is simulated rumble
   (rumble.h: the rumble_event API patches call, and the frame hook
   ct_module_info.rumble); 9 is the music and text exports
   (ct_music_hooks, ct_text: audio packs and HD text); 10 distinguishes
   full-screen UI from other flat scenes so renderers never blur menus;
   11 adds scene flags for scripted camera control and exact-console
   presentation. */
#define CT_MODULE_ABI 11

/* ---- the scene: what the game is showing, for renderers (docs/SCENE.md) ----
   A module may describe its current view as a map of cells, tile layers
   and objects, so that a renderer can rebuild it in 3D (frontend/hd2d).
   The module fills a ct_scene from the game's memory once per frame, at
   the frame edge; the engine adds the console's state (VRAM, CGRAM, OAM)
   and never reads game memory itself. Everything is in the console's own
   formats, so nothing here belongs to one game. */
#define CT_SCENE_LAYERS      3
#define CT_SCENE_MAX_CELLS   64                        /* map side, in cells */
#define CT_SCENE_STRIDE      (2 * CT_SCENE_MAX_CELLS)  /* tilemap row, in 8x8 tiles */
/* CT_SCENE_WORLD: cells of 8 px (one tile each), map side at most this,
   which is also the row stride of its cells and tilemaps. */
#define CT_SCENE_WORLD_STRIDE 256
#define CT_SCENE_MAX_OBJECTS 64
#define CT_SCENE_MAX_PIECES  16

enum {                       /* ct_scene.mode */
    CT_SCENE_OTHER = 0,      /* not a map view (battle, title...): draw it as a 2D scene */
    CT_SCENE_FIELD = 1,      /* a map view, as the cells, layers and objects describe it */
    CT_SCENE_WORLD = 2,      /* an overworld: a big map of 8 px cells, maybe wrapping, seen
                                from above; layers 0 and 1 are its tiles, layer 2 an overlay
                                (clouds) with its own scroll (overlay_*) */
    CT_SCENE_UI = 3,         /* full-screen UI (menu, name entry, shop...): plain and sharp */
};

enum {                       /* ct_scene.flags */
    CT_SCENE_SCRIPTED = 1 << 0,  /* the game, not the player, controls this shot: renderers
                                    must not add live camera input on top of it */
    CT_SCENE_EXACT_2D = 1 << 1,  /* staging is not represented by ct_scene (Mode 7, HDMA,
                                    mixed video modes...): preserve the console frame */
};

enum {                       /* ct_scene_cell.cls: what the cell is to walk on */
    CT_CELL_UNKNOWN = 0,
    CT_CELL_FLOOR,           /* walkable */
    CT_CELL_SOLID,           /* blocked */
    CT_CELL_WALLISH,         /* partly blocked (corners, diagonal edges) */
    CT_CELL_WATER,
    CT_CELL_STAIRS,          /* stairs and ramps between levels */
    /* Blocked, and what stands there (CT_SCENE_WORLD; a module that can't
       tell leaves CT_CELL_SOLID and the renderer guesses from the pixels). */
    CT_CELL_MOUNTAIN,
    CT_CELL_FOREST,
    CT_CELL_SHORE,           /* the band between land and water, cliffs */
    CT_CELL_FLAT,            /* blocked, but lies flat (fences, signs, a town's floor) */
};
enum {                       /* ct_scene_cell.flags */
    CT_CELL_OVERHEAD = 1 << 0,   /* some of its tiles draw over sprites (roofs, canopies) */
    CT_CELL_EVENT    = 1 << 1,   /* an exit, or stepping on it starts an event */
    CT_CELL_COVER    = 1 << 2,   /* walkable; the walker draws partly hidden (tall grass, woods) */
};
typedef struct {
    uint8_t cls;             /* CT_CELL_* class */
    uint8_t flags;           /* CT_CELL_OVERHEAD ... */
    uint8_t plane;           /* walk level the game keeps for it (0: unknown) */
    uint8_t raw;             /* the game's own class value, for debugging */
} ct_scene_cell;

typedef struct {
    uint8_t present;
    uint8_t bpp;             /* bits per pixel of its tiles: 2 or 4 */
    uint16_t chr_base;       /* VRAM word address of tile 0 */
    uint16_t pal_base;       /* CGRAM index of palette 0 */
    /* (2 map_w) x (2 map_h) BG tilemap words, row stride CT_SCENE_STRIDE:
       tile 0-9, palette 10-12, priority 13 (over sprites), h flip 14,
       v flip 15. The module's memory, valid until its next fill.
       CT_SCENE_WORLD: map_w x map_h words (cell = tile), row stride
       CT_SCENE_WORLD_STRIDE; the overlay (layer 2): overlay_w x
       overlay_h, the same stride. */
    const uint16_t *tiles;
} ct_scene_layer;

typedef struct {
    int32_t x, y;            /* map position of its foot point, px */
    int16_t z;               /* height above its floor, px (0: unknown) */
    uint8_t facing;          /* 0 up, 1 down, 2 left, 3 right, 0xFF unknown */
    uint8_t n_pieces;
    uint8_t piece[CT_SCENE_MAX_PIECES];   /* the OAM entries (0-127) drawing it */
} ct_scene_object;

typedef struct {
    /* Filled by the module. With mode OTHER or UI nothing else need be. */
    uint8_t mode;            /* CT_SCENE_* */
    uint8_t player;          /* index in object[] of the character the player moves, 0xFF none */
    uint8_t cell_px;         /* cell size in px (16: 2x2 tiles) */
    uint8_t n_objects;
    uint16_t map_w, map_h;   /* in cells, at most CT_SCENE_MAX_CELLS (world: _WORLD_STRIDE) */
    uint32_t map_id;         /* the game's own id for the map */
    uint32_t map_generation; /* changes whenever the map's cells or tiles do */
    int32_t camera_x, camera_y;           /* map px at the screen's top-left */
    ct_scene_layer layer[CT_SCENE_LAYERS];   /* front to back: 0 draws over 1 */
    const ct_scene_cell *cells;           /* map_w x map_h, row stride CT_SCENE_MAX_CELLS
                                             (world: CT_SCENE_WORLD_STRIDE) */
    ct_scene_object object[CT_SCENE_MAX_OBJECTS];
    /* Filled by the engine (ct_scene_capture): the console's state at the
       frame edge, which the module's description refers to. */
    const uint16_t *vram;    /* 32768 words */
    const uint16_t *cgram;   /* 256 BGR555 colors */
    const uint16_t *oam;     /* 272 words, the console's layout: 2 per entry, then the
                                16 words of size and x bit 8 */
    uint16_t obj_chr[2];     /* VRAM word address of OBJ tiles 0 and 256 */
    uint8_t obj_size[2];     /* small and large OBJ size, px */
    uint8_t brightness;      /* 0-15; 0 also in forced blank */
    /* Filled by the module, all modes (ABI 11). */
    uint8_t flags;           /* CT_SCENE_SCRIPTED, CT_SCENE_EXACT_2D */
    /* Filled by the module, CT_SCENE_WORLD only (ABI 7). */
    uint8_t wrap;            /* the map wraps at both edges */
    uint8_t overlay_blend;   /* layer 2 is added at this many sixteenths over the rest
                                (8: half-add); 0: no overlay */
    uint16_t overlay_w, overlay_h;     /* layer 2's tilemap, in tiles; it repeats */
    int32_t overlay_x, overlay_y;      /* its scroll: overlay px at the screen's top-left */
} ct_scene;

/* Module export (optional): describe this frame's scene. Returns 0 with
   out filled, or -1 when the module can't say. */
typedef int (*ct_module_scene_fn)(ct_scene *out);

/* ---- music: what the game asks its sound program to play (audio packs,
   docs/ASSET-PACKS.md) ----
   The module watches the commands the CPU sends through the APU ports
   and says, in its game's own song ids, when a song starts, fades or
   stops. The engine only passes the port writes on and never reads
   game memory for this. */
enum {                       /* ct_music_event.kind */
    CT_MUSIC_PLAY = 1,       /* song starts (or restarts) */
    CT_MUSIC_STOP = 2,       /* no music: the song stops */
    CT_MUSIC_FADE = 3,       /* the music's volume moves to `volume` over `frames` */
};
typedef struct {
    uint8_t kind;            /* CT_MUSIC_* */
    uint8_t song;            /* PLAY: the game's song id */
    uint8_t volume;          /* FADE: the target, 0 (silent) - 255 (the song's full volume) */
    uint8_t reserved;
    uint16_t frames;         /* FADE: how long, in frames (0: at once) */
} ct_music_event;

typedef struct {
    /* Every CPU write to an APU port (0-3), with what the CPU has last
       written to each port, this write included. Called mid-frame, from
       the CPU's write: it may read memory, never change it. */
    void (*port_write)(unsigned port, uint8_t value, const uint8_t ports[4]);
    /* At the frame edge: the events since the last call, oldest first;
       returns how many it wrote (at most max). */
    int (*events)(ct_music_event *out, int max);
    /* The sound program's APU RAM byte whose bit n is set while DSP voice
       n plays a sound effect, not music; -1 when the module can't tell
       them apart (then a replaced song silences all of the console's
       sound). */
    int32_t sfx_voices_aram;
} ct_music_hooks;

/* ---- text: what the game's text boxes show, as characters (HD text) ----
   For a renderer that draws a box's text with a font of the player's at
   the screen's resolution, over the console's glyphs. The module gives
   the characters it has drawn into each line of a box and where the
   line's 8x8 tiles are in VRAM; the engine finds them on the screen from
   the provenance G-buffer (sched_prov), so wherever the game puts the
   box, the text follows, and a line not on the screen is not drawn. */
#define CT_TEXT_LINES   4
#define CT_TEXT_GLYPHS  64
#define CT_TEXT_COLS    32
#define CT_TEXT_ROWS    2
typedef struct {
    uint32_t ch;             /* Unicode code point (0: a glyph with no character) */
    int16_t x;               /* left edge, px from the line's left */
    uint8_t w;               /* the console font's advance, px */
    uint8_t reserved;
} ct_text_glyph;
typedef struct {
    uint8_t n;               /* glyphs in g[] */
    uint8_t cols, rows;      /* the line's tiles: cols x rows (at most CT_TEXT_COLS x CT_TEXT_ROWS) */
    uint8_t top;             /* glyph cell top, px below the line's top */
    uint8_t height;          /* glyph cell height, px (the console font's) */
    uint8_t reserved[3];
    /* VRAM word address of each 8x8 character of the line, rows x cols:
       the tile drawn at (col, row) from the line's top-left. */
    uint16_t chr[CT_TEXT_ROWS][CT_TEXT_COLS];
    ct_text_glyph g[CT_TEXT_GLYPHS];
} ct_text_line;
typedef struct {
    uint8_t layer;           /* the BG layer (0-3) the lines are drawn on */
    uint8_t n_lines;
    uint8_t fg, shadow;      /* the glyphs' color and shadow: color index within their palette */
    ct_text_line line[CT_TEXT_LINES];
} ct_text;
/* Module export (optional): this frame's text, at the frame edge.
   Returns 0 with out filled (n_lines may be 0), or -1 when the module
   can't say. */
typedef int (*ct_module_text_fn)(ct_text *out);
/* Poses (optional, no ABI change: a module without it has none). A pose
   is one object's whole animation frame as the game assembles it, keyed
   so that the same picture has the same key on any map and in any object
   slot: what asset packs replace as one picture, and what an artist's
   sheet holds (docs/ASSET-PACKS.md, "Poses"). */
typedef struct {
    uint32_t key;            /* the pose: the module's stable id, never 0 */
    uint32_t sheet;          /* which poses go on one sheet (one character) */
    uint16_t w, h;           /* the picture, px: the bounding box of its OAM entries */
    int16_t origin_x, origin_y;   /* the object's foot point in the picture, px */
    uint8_t hflip, vflip;    /* the console draws the picture mirrored */
    char name[22];           /* the sheet's name for files ("pc00", ...): [a-z0-9_] */
} ct_scene_pose;

/* Module export ct_module_pose (optional): the pose of object[obj] of the
   ct_scene the module filled last. With rgba (max pixels) it also draws
   the original picture there, w x h RGBA8, color 0 transparent. Returns
   0, or -1 when the object has no pose this can describe. */
typedef int (*ct_module_pose_fn)(int obj, ct_scene_pose *out, uint8_t *rgba, int max);

typedef struct {
    uint32_t abi;            /* CT_MODULE_ABI the module was built with */
    uint32_t cpu_size;       /* sizeof(CPU) */
    uint32_t func_size;      /* sizeof(ct_func) */
    uint32_t overlay_size;   /* sizeof(ct_overlay_func) */
    ct_tables tables;
    /* The scene export (ABI 4: these two fields and ct_scene's layout). */
    uint32_t scene_size;     /* sizeof(ct_scene) */
    ct_module_scene_fn scene;   /* NULL: no scene description */
    /* The rumble frame hook (ABI 8, rumble.h). */
    ct_module_rumble_fn rumble;   /* NULL: none */
    /* ABI 9: music and text (NULL: none). */
    uint32_t music_size;     /* sizeof(ct_music_hooks) */
    const ct_music_hooks *music;
    uint32_t text_size;      /* sizeof(ct_text) */
    ct_module_text_fn text;
} ct_module_info;

typedef const ct_module_info *(*ct_module_get_fn)(void);

/* Load the module at path and install its tables. Call after bus_init and
   before sched_init. Returns 0, or -1 with the reason in *err. */
int ct_module_load(const char *path, const char **err);

/* The scene source: set by ct_module_load from the module, or at startup
   by a program the game is linked into (func_table_static.c). */
void ct_scene_set_source(ct_module_scene_fn fn);
int ct_scene_available(void);
/* This frame's scene: the module's description plus the console's state.
   Call at the frame edge (the frame hook), while the CPU is stopped.
   Returns 0, or -1 when there is none (no source, or the module has none
   to give); out->mode is then CT_SCENE_OTHER. */
int ct_scene_capture(ct_scene *out);

/* Music and text sources: set like the scene's. ct_music_watch(1) passes
   the CPU's APU port writes to the module (snes_apu_port_hook); 0 stops.
   ct_music_events and ct_text_capture return -1 when there is no source. */
void ct_music_set_source(const ct_music_hooks *hooks);
const ct_music_hooks *ct_music_source(void);
void ct_music_watch(int on);
int ct_music_events(ct_music_event *out, int max);
void ct_text_set_source(ct_module_text_fn fn);
int ct_text_available(void);
int ct_text_capture(ct_text *out);
/* The pose source: ct_module_load finds the module's ct_module_pose, a
   static build sets the game's ct_game_pose (NULL: none). ct_scene_pose
   asks it about object obj of the last ct_scene_capture; -1 without one. */
void ct_pose_set_source(ct_module_pose_fn fn);
int ct_pose_available(void);
int ct_scene_pose_of(int obj, ct_scene_pose *out, uint8_t *rgba, int max);

#endif
