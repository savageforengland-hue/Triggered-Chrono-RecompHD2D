# R1: Geometry hints and the 3D-scene correctness items

Extends VISUAL-DESIGN.md (P0 items 1-5, P1 item 8), HD2D.md ("The 3D diorama",
"Heights"), SCENE.md A0 and PRIOR-ART.md A/A2. It does not repeat them.

Marks: **[V]** = verified by reading source in this repo (file named);
**[U]** = unverified (web claim, inference, or needs a probe). Read-only research;
no build or test was run.

## 0. What the repo has today (all [V])
- `ct_scene_cell` = `{cls, flags, plane, raw}` per 16 px cell (`runtime/module.h`).
  `game/ct/scene.c build()` fills `plane = byteB & 3`, `raw` = the 6-bit solidity
  class, `flags` OVERHEAD (any BG1/BG2 quarter with priority bit) and EVENT
  (touch bit or an exit). `cell_class()`: 0 floor, 1 solid, 2..0x15 wall-ish,
  0x18/0x19/0x1D stairs, else unknown.
- The engine only uses `cls` and `OVERHEAD` for heights (`scene3d.c find_runs`,
  `mark_runs`: runs of raised cells, wall height = min(run length, `hmax`=3)).
  `plane` and `CT_CELL_STAIRS` are never read by `scene3d.c` (grep: `plane` is
  only in the header and `scene.c`). The floor is one plane at y=0.
- `s3d_height_at(x,z)` returns `box_h` only, so sprites never stand on anything but y=0.
- A cell hook exists (`s3d_set_cell_hook`, `scene3d.h:91`) so packs can replace a
  cell's geometry. It is per cell, no heights.
- The frame has a G-buffer with a `math` bit (`z<<4 | math<<3 | layer`, HD2D.md),
  an optional runner-up buffer, and an optional provenance buffer
  `sched_prov()` (per pixel: VRAM char address, palette, colour index, flips,
  OBJ flag, layer; `runtime/sched.h:110-132`). `sched_ppu_state` carries
  `math_enabled`, `add_subscreen`, `subtract_color`, `half_color`, `main/sub_enabled`.

## 1. Walls, heights, stairs and roofs

### 1.1 What the game's own data gives for free

| Datum (in `ct_scene` already) | Gives | Used now |
| --- | --- | --- |
| solidity class 1 next to class 0 | wall footprint (where a block stands) | yes |
| BG priority bit on metatile quarters (OVERHEAD) | which tiles the art draws over actors: roofs, eaves, canopies, lintels | yes, as "raised" |
| **facade vs roof split inside a run**: in a column, the rows with SOLID and not OVERHEAD are facade; OVERHEAD rows are roof/eave art | wall height in cells = number of facade rows below the roof rows; today the run is capped at `hmax` and the roof rows are laid flat on top | no (new) [U: needs a survey, 1.5] |
| `plane` (byteB bits 0-1) | the two walk levels. Community editor docs: plane 1, plane 2, a walkable transition tile and an always-solid tile; the plane you are not on is solid; stairs and ladders are transition tiles | no |
| byteB bit 2 "plane transition" (SCENE.md A0.3, `$6C`) | exactly the cells where level changes | no |
| `CT_CELL_STAIRS` raw 0x18/0x19/0x1D | stair or ramp cells (SCENE.md says "probably", seen under the leader) | no |
| byteA bits 0-3 auto-move direction + speed | slope direction on conveyors, wind, slopes (SCENE.md A0.3) | not exported to `ct_scene` |
| byteA6 / byteB6 per-tile sprite priority (top/bottom half) | where an actor's upper body goes behind art: a free "this tile is taller than a person" hint | not exported |
| exits and camera zones (`$7E7080`, `$7E70C0`) | door cells: place a doorway in a wall, bound the visible room (1.4, 2.4) | EVENT flag only |

Sources: repo `runtime/module.h`, `game/ct/scene.c`, `docs/SCENE.md` A0.2-A0.3 [V].
Community plane semantics: Data Crystal "Temporal Flux Event Tricks"
(https://datacrystal.tcrf.net/wiki/Chrono_Trigger_(SNES)/Temporal_Flux_Event_Tricks)
[U: a wiki, not checked against our own dump].

Note the plane value is only trustworthy as "plane id" if the bit pattern matches the
editor's four types. Probe needed (1.5): histogram `plane`, bit 2 and `raw` over all
512 locations from a WRAM dump per map; confirm that plane-2 floors appear exactly on
maps with balconies, bridges and upper floors [U].

### 1.2 Generic derivation in the engine (no hints needed), a "level solver"

All inputs are already engine-visible generic fields, so this lives in
`frontend/hd2d/scene3d.c` and passes `check_agnostic.py`.

1. **Floor level per walkable cell** `L(c)` in quarter-cell units:
   plane id 1 (or unknown 0) = 0; plane id 2 = `plane_step` (default 1 cell = 16 px,
   `CT_HD2D_3D plane=`). Cells flagged transition or STAIRS are *ramps*.
2. **Ramp resolution.** For each ramp cell, take its 4-neighbours that are floors with
   a known level. If the lowest and highest neighbours lie on opposite sides along one
   axis, the cell is a linear ramp along that axis between them (the slope direction
   falls out of connectivity; no need to decode which stair class means which way).
   A run of ramp cells along the axis shares one slope across the run (L interpolates
   per cell edge, not per cell, so a 3-cell stair is one smooth surface). If no
   unambiguous pair exists, leave it flat. [U: stair classes might also carry the
   direction; the connectivity rule needs no knowledge of them.]
3. **Wall height from level difference.** A solid cell (or any wall-ish cell) whose
   south neighbour is a floor at level A and whose north neighbour is a floor at B > A
   is a **retaining wall** of height B-A (a balcony front, a cliff). This replaces
   the fixed `hmax` guess exactly where the game states a level change, and it is
   self-consistent with the plane-2 floor behind it (fixes the HD2D.md known-wrong
   "upper floor is at floor level behind the wall that should hold it up").
4. **Standing blocks (buildings).** Keep the current run logic, but set a run's wall
   height to the *facade row count* (SOLID and not OVERHEAD rows from the south
   end), min 1, max 6; the OVERHEAD rows above are the roof. Roof = a flat lid at
   wall height (current behaviour) *plus* an eave: lift the lid's south edge by
   4 px and tilt the first OVERHEAD row (slope 12 degrees) so the roof stops being a
   coplanar paper strip. [U: needs the survey in 1.5; if facade rows are 0 for most
   buildings, fall back to today's `hmax`.]
5. **Sprites follow the ground.** `s3d_height_at` gains `s3d_floor_at(x,z)` so a billboard
   at an object's foot point stands at `L` of that cell (and at ramp height). Without
   this, plane-2 floors lift the world but not the actors.
### 1.3 Data format for authored hints (lives under `game/ct/`)

Authoring cost must be small: the auto solver covers most cells; hints only correct.

**Engine ABI (game-agnostic).** Add to `ct_scene` (ABI 9 -> 10, `runtime/module.h`):

```c
#define CT_HEIGHT_UNIT 4                 /* px per height step */
enum { CT_ROLE_AUTO = 0, CT_ROLE_FLOOR, CT_ROLE_WALL, CT_ROLE_ROOF,
       CT_ROLE_RAMP, CT_ROLE_THIN, CT_ROLE_VOID };
typedef struct {
    int8_t  floor;   /* ground level under the cell, steps; ramps: level at the low edge */
    uint8_t top;     /* top of the block standing on it, steps above floor; 0: not given */
    uint8_t role;    /* CT_ROLE_* (AUTO: infer) */
    uint8_t slope;   /* bits 0-1: direction it rises (N E S W); bits 2-6: rise in steps */
} ct_scene_height;
...  const ct_scene_height *height;   /* map_w x map_h, stride CT_SCENE_MAX_CELLS; NULL: none */
```

Fields with `AUTO`/0 mean "use the solver"; so a hint file may be sparse and a module
with no hints leaves `height == NULL` (backward compatible, ABI check refuses a
mismatched module as today). 4 bytes per cell, max 64x64 = 16 KB, no dynamic allocation.
`scene3d.c` consumes it in `find_runs`/`mark_runs` (a given `top` or `role` wins over the
solver) and `emit_cell` (floor y0, slope).

**Authored files (ours, GPL; contain no game data, only cell coordinates):**

```
game/ct/hints/profile.toml   # tile profile: role by tile key (shared by all maps)
game/ct/hints/maps/0x005.toml   # per-map overrides (map id = the game's location id)
```

`profile.toml` (the 3dSen idea: a profile keyed by *what the tile is*, authored once,
reused on every map; 3dSen profiles are hand-made per game, PRIOR-ART.md A [V]):
```toml
# key = FNV-1a of the tile's 32 bitplane bytes (palette- and tileset-independent) [U]
[[tile]]  key = "7a3c9e01"  role = "roof"   # a roof-edge tile
[[tile]]  key = "11b20f66"  role = "wall"   top = 3
[[tile]]  key = "c0de5a5a"  role = "thin"   # a fence
```

The module resolves a cell's role by looking up its L1/L2 quarter keys at fill time
(it already has the tilemaps and VRAM-decoded tiles via `sprites.c` helpers [U]).
Pixel-hash keys survive tileset reshuffles between maps [U: needs a check that
identical art really repeats across maps with identical bytes; if not, key by
(tileset id, tile index), which `scene.c` can read].

`maps/<id>.toml` (overrides, rectangles in cells, later entries win):

```toml
default_wall = 3
[[region]] rect = [4, 10, 9, 12]  role = "wall"  top = 2
[[region]] rect = [12, 3, 12, 5]  role = "ramp"  floor = 0  slope = "N:4"
[[region]] rect = [0, 0, 63, 5]   role = "void"          # hide a stray off-screen room
```

**Compiler.** `game/ct/tools/hints_build.py`: validates TOML, expands profile+regions
into a per-map blob `hints.bin` (map id -> run-length coded rows), emitted next to the
module at build time. `game/ct/scene.c` loads it once, and on each map-generation change
fills a static `ct_scene_height[64*64]` (tile lookups, then region overrides). The module is
the only place that sees the map id; the engine sees only heights. Add `hints_build.py`

### 1.4 Authoring workflow (tools)

1. `scene_probe.py` (exists) gains `--hints-png`: the map picture with the solver's
   result overlaid (colour = level, arrows = ramps, outline = role) from a WRAM dump.
   The author sees what the auto pass already got right. [U: to be built]
2. `hints_build.py --lint`: rects in map bounds, no overlapping contradictory roles,
   every `map id` present in the 512-location table, `top` within 1..12.
5. Optional later: a Tiled-style GUI (https://www.mapeditor.org/, GPL-2.0 editor;
   idea only, we would write our own TOML). [U: not evaluated]

### 1.5 The survey that decides how much to author (cheap, do first)
Add `game/ct/tools/height_survey.py` over a WRAM dump per location (the repo already
dumps WRAM with `--wram`): per map output (a) number of plane values, (b) cells with
transition bit, (c) per-column facade/roof row counts, (d) ramp cells whose neighbour
pair is unambiguous. Gate for 1.2 step 4: if the median building has >= 1 facade row
and facade != run length for most runs, the heuristic holds without authoring. [U]

### 1.6 Prior art and licences

| Source | What | Licence | Use |
| --- | --- | --- | --- |
| 3dSen (https://geodstudio.net/) | per-game hand-made profile | closed | idea only |
| Octopath/HD-2D: UE4, Acquire (https://www.unrealengine.com/en-US/spotlights/octopath-traveler-s-hd-2d-art-style-and-story-make-for-a-jrpg-dream-come-true) | scenes are hand-modelled 3D (modular pieces; several stair variants because identical stairs read as copy-paste, https://www.apartment507.com/blogs/japan-gaming/untold-stories-of-octopath-traveler-development-surpassing-the-deified-pixel-art) | n/a | idea only; confirms modular heights are authored, not derived. No technical talk found [U] |
| GitExl/CTViewer (https://github.com/GitExl/CTViewer) | map viewer with Z-plane, tile movement, collision, sprite-priority overlays | MIT | code and ideas usable with notice; README does not say how bits are decoded, so read its source before trusting any bit meaning [U] |
| grabbiel/chrono (https://github.com/grabbiel/chrono) | `ct_maps.py` decodes AttrA/AttrB tile properties (levels, blocking, priority) | Apache-2.0 | usable with notice; PR page text only, bit layout not read [U] |
BelthasarProject (https://github.com/TemplarServant/BelthasarProject, MIT) is a peer
native port with no 3D or height work in its README. I found no permissively licensed tool that lifts a 2D tilemap to extruded 3D with
authored heights (searched Tiled/Unity/Godot/GitHub terms; Tilemap-Creator-3D by fkate
is a Unity mesh-tile editor, licence not checked [U]). The design above is original.

## 2. P0 correctness items

Shared primitive: **`s3d_match(sc, prov, bgrx, layers)`** in `frontend/hd2d/hd2d.c`, a
sampler that answers "does the console frame show the scene's map here?". It generalises
`s3d_box_mask` (which today only runs when `ui_z >= 0`, compares colours, and runs
only for FIELD) and is the basis of 2.1-2.3.

```
struct s3d_match { int seen, hit; uint8_t row_bad[29]; uint8_t tile_bad[29][33]; };
```

For each 8x8 screen tile whose pixels are won by BG1 or BG2 (`layers[k] & 7` in 0..1),
sample 4 pixels (corners of a 4x4 grid inside the tile). With `sc`, the expected tile is
`layer.tiles[(ty)*STRIDE + tx]` at map pixel `(camera + screen)`; the observed tile is the
provenance entry (`sched_prov()[k].chr/.pal/.flags` = same VRAM address, palette and
flip as the expected word) [V: fields exist]. A sample is a *hit* when chr address,
palette and flips all equal; otherwise (provenance off) fall back to the existing colour
comparison against decoded VRAM/CGRAM, as `s3d_box_mask` does [V]. The scene's expected
layer of the winner must be derived (BG1 vs BG2 by `layers` low bits, matching
`ct_scene_layer` order 0/1, SCENE.md layers front to back [V]). Cost: about 250 tiles x 4
samples, an integer compare each, per frame: below the existing `analyze()` pass's work.
`sched_set_prov(1)` costs PPU CPU time [V comment, sched.h:114]: enable it only while
the preset wants 3D, and keep the colour fallback for headless/off.

### 2.1 Reliable UI and menu detection while in 3D

Problem [V]: UI is found by the BG3 top-priority class (`ui_z`, `analyze()`), by the
text-band box mask (`s3d_box_mask`: runs >= 16 tiles within 10 rows of UI text pixels),
and `scene_amt` eases effects off only when `ui_frac > 0.15` (BG3 pixels). Menus
drawn on BG1/BG2 or as sprites, name entry (reported blurred, VISUAL-DESIGN P0 #2),
and cursors are missed.

Algorithm (changes in `hd2d_render`, `analyze`, `s3d_box_mask`):

1. Run `s3d_match()` every frame the module reports FIELD/WORLD (also when 2D is
   shown, so it can gate entry into 3D, 2.2).
2. `mismatch = 1 - hit/seen` (with `seen >= 24`; fewer = "no map view": menu/black).
3. **Classify the frame**, in this order, with a 6-frame hysteresis (a counter that must
   win 3 frames to flip):
   - `mismatch > 0.55` spread over >= 8 tile rows -> **screen is not the map**
     (menu, name entry, wrong map): 2D path, `scene_amt = 0` (no DOF, bloom,
     vignette; reuse the existing `scene_amt` ramp but driven by mismatch, not by
     `ui_frac`: `scene_amt = clamp((0.55-mismatch)/0.25, 0, 1)`).
   - `0.03 < mismatch <= 0.55`, mismatching tiles forming a band (>= 8 contiguous marked
     tiles on a row, rect-merged) -> **window**: mark the rect(s) as UI; lay the
     console's pixels flat, with the console's translucency (pass the rect list as the
     existing `t_box` mask; today mask is an 8x8-tile bitmap, which already supports
     this; just widen its producer).
   - else map view, no UI beyond BG3.
4. **Sprites that are UI**: OAM entries not owned by any `ct_scene.object[].piece[]`
   [V: pieces are given] and drawn while a window rect exists are cursors, name
   tags and icons: draw them flat (into the UI layer) instead of "stand at their own
   bottom edge" (HD2D.md). Without a window rect, unowned entries stay effects.
5. **Module hint (cheap and precise, optional):** `ct_scene.ui_rects[8]` (x,y,w,h in
   screen px, set by the module from its own window state). If non-empty it replaces step
   3's window detection; the match stays the authority for "screen is not the map".
   Engine stays generic: rectangles only.

Test: replay frames 3700 (narrator on black; existing `test_hidden_map_3d.py`), a text
box frame (`test_3d_box.py` exists [V]), plus a new menu/name-entry replay: assert
the 3D coverage (mean abs diff of the 3D picture against the 2D picture, outside the box
rect) is 0 for menu frames and that the box rect equals the console's pixels.

### 2.2 Scene-gate self-check

Problem [V]: `want3` is true when the module says FIELD/WORLD, mode != 7, and BG1/BG2
enabled on some screen (`hd2d_render`). A map "not on screen" but still resident
produced the "grid of garbage tiles" class of bug (`test_hidden_map_3d.py` documents one).

Algorithm (in `hd2d_render`, replaces the `want3` tail):

```
match = s3d_match(sc, ...);                   /* 2.1 */
frame_ok  = match.seen >= 24 && 1.0f - (float)match.hit/match.seen <= 0.55f;
if (frame_ok)  { ok_run++; bad_run = 0; } else { bad_run++; ok_run = 0; }
if (sc->map_generation != g.s3.gate_gen) {   /* new map: allow 2 settle frames */
    g.s3.gate_gen = sc->map_generation; g.s3.settle = 2; }
gate = g.s3.gate;                              /* sticky */
if (g.s3.settle) { g.s3.settle--; }
else if (bad_run >= 3) gate = 0;               /* leave 3D fast: wrong pixels are worse */
else if (ok_run  >= 4) gate = 1;               /* re-enter slowly */
want3 = ... && gate;
```

Enter slowly, leave fast: the crossfade (S3D_FADE_STEP, 8 frames) already hides
transitions. During a screen shake, a mosaic effect, or a fade, tiles still match
(mosaic does change observed pixels, not the provenance chr [U]; with the colour
fallback mosaic/fade would mismatch; use the provenance path or exempt brightness < 4,
as `s3d_box_mask` does [V]). Camera scroll is exact because the scene's camera is the
console's (`camera_x/y`, SCENE.md A0.5 [V]); scroll-wrap garbage would show as mismatch.

Test: new env `CT_HD2D_GATE_TEST=offset:40` perturbing `sc->camera_x` in the renderer
only (debug), expect `gate` false within 4 frames and an unchanged 2D picture; and the
existing frames-3700/4400 pair. Log "hd2d: gate off (hit/seen)" for CI grep.

### 2.3 Small-map framing

Problem [V]: `s3d_render` centres a map smaller than the view on a dark ground; the
bounds come from `s3d_map_bounds` (non-empty cells; `find_runs`). Multi-room grids and
stray tiles make the bounds larger than the room, and the console's void is filled with
whatever the scene has there.

Algorithm (all in `s3d_render`, `find_runs`):

1. **Room flood.** On the map's cells (`empty[y][x]` from `find_runs`), flood from the
   player's cell `(object[player].x/16, y/16)` over non-empty cells, 8-connected, allowing
   gaps of 1 cell (doors, tile seams). `room[y][x]`. 64x64 = 4096 cells: per frame is
   fine, but only the player cell changes, so memoise on (generation, cell). Fall back to
   all non-empty cells if the flood is < 4 cells. Bounds = bounding box of `room`.
2. **Visibility.** Clip geometry to the room's rect + 1 cell margin: a scissor in map
   space (`uRoom` uniform, discard in `scene3d.vert`/`frag` outside) rather than a rebuild,
   so it is free per frame. Cells of other rooms stop appearing next to the room.
3. **Zoom to fit.** With bounds size `Wm x Hm` px (+ 2 x `margin`=24), `zx = 256/(Wm+48)`;
   `zz = span1/(Hm+48)` where `span1 = far_z - near_z` computed at `dist(zoom=1)`
   (the code computes both of those already, linear in `dist`). `zfit = min(zx, zz)`.
   Use `zoom_eff = clamp(zfit, g.s3.zoom, 1.75)` only when `zfit > g.s3.zoom`, i.e. the
   room is smaller than the view; quantise to {0.75, 1, 1.25, 1.5, 1.75} so the
   billboard snap rule (VISUAL-DESIGN rule 3 / P1 #6) keeps integer scale; ease between
   steps over 20 frames; reset at once when `map_generation` changes.
4. **Void colour.** Clear the framebuffer to the console's backdrop colour: CGRAM
   index 0 x brightness (that is what the console shows outside every layer, standard
   SNES behaviour [V: hardware; confirm `ct_scene.cgram[0]` is captured]) instead of
   dark grey, and let the existing distance haze fade the floor edge into it. Keep the
   vignette. No code path is needed in the module.
Test: Crono's house replay frames (small room): the picture's void fraction (pixels of
CGRAM[0]) falls and the map fills >= 70% of the picture width; a big map (bell plaza)
is unchanged byte for byte (`zfit <= zoom`).

### 2.4 Blended overlays and translucent sprites

Facts [V]: BG3 (fog, light shafts, colour-math overlays in map views) and colour math are
not drawn in 3D (HD2D.md "Where these are wrong"); translucent sprites are opaque. The
layer byte already carries a `math` bit per pixel. The behind buffer stores colours
*without* colour math.

Stage A (cheap, ship first): **2D fallback while math is visible.**
In `analyze()` count pixels with the math bit set whose layer is BG1/BG2/OBJ (not UI).
`math_frac`; if `math_frac > 0.02` for 3 frames, `want3 = 0` (hysteresis 8 frames to
re-enter). Fades to black use brightness, not math [V: `sc->brightness`], so they are
unaffected; add/subtract fades to a colour are math, and also correct to show in 2D.
Cost: darkness/fog scenes lose 3D; correctness over flair (VISUAL-DESIGN P0 #5).

Stage B (the real fix): **transfer the console's colour-math delta.**
1. PPU adds an optional third buffer, `pre-math colour` (the winner's colour before the
   math step, after brightness is not applied; same style as `PpuSetBehindBuffer`);
   presented as `sched_premath()`. Hash-neutral when off, tested like the behind
   buffer (`test_snes_bus`) [U: not read, design only].
2. In the 3D pic pass, for each pixel with the math bit:
   `delta = console_final - console_premath` (signed, per channel), then
   `out = clamp(shaded3D + delta, 0, 1)`. Exact for add and subtract; for half modes it is
   exact when `shaded3D == premath` and the sunlit floor keeps the original colours
   (HD2D.md step 3 [V]), so it is a close approximation. It also covers fixed-colour
   fades, flashes and darkness.
3. Alignment: the delta is in console screen space while geometry is shifted by the
   tilt. Fine for large soft overlays (fog, darkness, glow); for hard-edged shapes
   (a sprite shadow, a translucent text window) mis-registration shows, so UI rects
   and OBJ pixels are excluded: OBJ with math gets a billboard alpha of 0.5 (half) or the
   additive blend (add) drawn in the sprite pass; the console's `OBJ` math bit tells which
   [U: per-sprite math needs the palette 4-7 rule from the PPU, `ppu.c` not read].
Test: Stage A: a replay frame with a known colour-math overlay (find one with the
`F2` class debug view or `--debug-view 1`) shows identical 2D pixels; a frame with no
math keeps 3D. Stage B: compare `hd2d3d` against `hd2d` mean luma within 5% on overlay
frames, and a flat-floor synthetic scene where `shaded3D == premath` gives output
== console final within 1/255.

## 3. Ranked build list (max 10)
Order is risk first (P0 correctness), then the geometry that removes the paper look.

1. **Scene gate + `s3d_match`** (2.1/2.2). Files: `frontend/hd2d/hd2d.c`
   (new `s3d_match`, `hd2d_render` want3 tail, state in `g.s3`),
   `runtime/sched.h` consumer only. Test: extend `game/ct/test/test_hidden_map_3d.py`
   with a camera-offset fault injection (`CT_HD2D_GATE_TEST`), assert 2D within 4 frames
   and a "gate off" log line; frames 3700/4400 still pass.
2. **UI/menu classification** (2.1): mismatch-driven `scene_amt`, window rects by band,
   unowned-OAM-as-UI. Files: `hd2d.c` (`analyze`, `s3d_box_mask`, `s3d_render` UI
   uniform), `shaders/scene3d_ui.frag` or the existing UI pass [path not read, U].
   Test: new replay at a menu/name-entry frame; assert zero DOF difference outside a
   box and flat pixels equal the console's inside; keep `test_3d_box.py`.
3. **Colour-math fallback (Stage A)** (2.4). Files: `hd2d.c` (`analyze` math count,
   `hd2d_render`). Test: replay frame with overlay -> 2D identity vs `hd2d` preset;
   no-math frame stays 3D.
4. **Small-map framing: room flood, clip, zoom-to-fit, backdrop colour** (2.3). Files:
   `scene3d.c` (`find_runs`, bounds API `s3d_room_bounds`), `hd2d.c` (`s3d_render` camera
   block), `shaders/scene3d.vert` (`uRoom` clip). Test: house replay frame: map covers
   >= 70% of width; bell plaza byte-identical.
5. **Level solver** (1.2 steps 1-3, 5): plane -> floor level, ramps from connectivity,
   retaining walls from level difference, `s3d_floor_at` for sprites. Files:
   `scene3d.c` (`find_runs`, `mark_runs`, `floor_place`, `emit_cell`, `emit_sides`),
   `scene3d.h`. No ABI change. Test: unit test in `test/` (C, synthetic `ct_scene` with a
   plane-2 balcony and a 3-cell stair): assert vertex heights; golden shot of a real
   multi-plane map.
6. **Height survey tool + `--hints-png` overlay** (1.4, 1.5). Files:
   `game/ct/tools/height_survey.py`, `game/ct/tools/scene_probe.py`. Test: runs on the two
   replay dumps; plane histogram matches the A0.3 notes (leader on plane 1). Decides
   items 7-9, so run it before them.
7. **Facade/roof split + eave** (1.2 step 4). Files: `scene3d.c mark_runs`, `emit_cell`.
   Test: `scene_decode`-style check on the survey's expected facade row counts;
   golden shot of a house.
8. **`ct_scene_height` ABI + `hints_build.py` + `game/ct/scene.c` loader** (1.3).
   Files: `runtime/module.h` (ABI 11), `game/ct/scene.c`, `game/ct/hints/*`,
   `game/ct/tools/hints_build.py`, `frontend/hd2d/scene3d.c`, `tools/check_agnostic.py`
   run. Test: `hints_build.py --lint` in ctest; unit test that a one-cell `top` override
   changes exactly that cell's vertices; ABI mismatch is refused.
9. **Tile profile** (pixel-hash role keys, `profile.toml`) + first 20 hand-authored maps
   (most visited, from replay coverage). Files: `game/ct/hints/profile.toml`, `scene.c`,
   `hints_build.py --report`. Test: coverage report in CI shows no regressions; shot diff
   on the top maps.
10. **Colour-math delta transfer (Stage B)** (2.4). Files: `third_party/snes/ppu.c`
    (premath buffer, hash-neutral), `runtime/sched.c/.h`, `hd2d.c`, `scene3d_pic.frag`.
    Test: `test_snes_bus` equality with the buffer on/off; synthetic flat scene output ==
    console final within 1/255.

## 4. Open questions
- Plane values 0-3 and transition bit meaning on our own dumps (probe 1.1/1.5).
- Whether stair classes encode direction (the connectivity rule avoids needing it).
- Whether tile pixel hashes repeat across maps (profile keys, 1.3).
- Mosaic/fade behaviour of provenance fields; per-OBJ math exposure in `ppu.c`.
- No primary technical source for Octopath's world building found.
