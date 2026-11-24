<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# HD-2D renderer

An optional way of presenting the game. The picture is drawn as a lit diorama, with
depth of field, bloom, soft shadows under characters and a filmic color grade, in the
style of modern "HD-2D" releases.

**It draws nothing of its own.** Every pixel comes from the running game's own frame,
which is generated from your ROM. The renderer only changes how that frame is lit,
blurred and graded, so the project ships no game art.

## Using it

```
CT_ROM=rom/your_dump.sfc cmake -S . -B build && cmake --build build -j
build/ct_sdl --hd2d                 # window
build/ct_sdl --hd2d --fullscreen    # or press F11; automatic under gamescope
```

| Key | Action |
| --- | --- |
| F1 | Cycle the preset: `off`, `subtle`, `hd2d` (default), `deck`, `hd2d3d` (the 3D diorama, below), `deck3d` (the 3D diorama for the Steam Deck) |
| F2 | Cycle the debug view: layer classes, depth, blur amount, split compare (original pixels on the left), background plane, off |
| F3 | Cycle the aspect: 4:3 (default), 8:7 pixel aspect, stretch |
| F11 | Toggle fullscreen |
| F5 | Reload the asset packs ([ASSET-PACKS.md](ASSET-PACKS.md)) |
| R3 / L3 (controller) | Same as F1 / F3, for handhelds without function keys |

Each change shows a short on-screen message. Preset and aspect are saved to
`~/.local/share/ct-recomp/hd2d/settings.ini` (SDL's per-user preferences folder), and
command-line options override them.

| Option | Effect |
| --- | --- |
| `--preset off\|subtle\|hd2d\|deck\|hd2d3d\|deck3d` | Choose the starting preset |
| `--render-scale 50-100\|auto` | The 3D diorama's render scale, in percent ([below](#render-scale-sharpening-and-ambient-occlusion)) |
| `--sharpen 0-1\|auto` | Its sharpening |
| `--ao 0-1\|auto` | Its ambient occlusion strength |
| `--aspect 4:3\|pixel\|stretch` | Choose the picture shape |
| `--debug-view N` | Start in debug view N (0 off, 1 classes, 2 depth, 3 blur, 4 split compare, 5 background plane) |
| `--log-gpu` | Print the renderer's frame time every 60 frames (stalls the GPU to measure) |
| `--shot F:PATH` | At frame F, render offscreen and save a PNG (repeatable) |
| `--shot-size WxH` | Size of those screenshots (default 1920x1080) |

To see where the time goes, set `CT_HD2D_PROFILE=FILE`. Every frame then appends a line
to FILE with the GPU time of each pass, from timestamp queries read back a few frames
later so they don't stall anything, plus the CPU time of the scene analysis and the
uploads, and with `--log-gpu` the frame's total. It needs timer queries: OpenGL 3.3, or
OpenGL ES with `EXT_disjoint_timer_query`.

To render headlessly, for example on CI or over SSH, set `SDL_VIDEODRIVER=offscreen`.
That gives a GPU context through EGL with no window (add `CT_HD2D_GLES=1` for an OpenGL
ES 3.0 context only, to try the ES path on a desktop driver; `CT_HD2D_GLERR=1` reports
GL errors):

```
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy build/ct_sdl --hd2d --fast \
    --frames 9200 --script game/ct/test/replay/leene_square.txt --shot 9200:leene.png
```

## How it works

1. **Layer G-buffer.** The PPU (`third_party/snes/ppu.c`) writes a second buffer next to
   the color frame. It holds one byte per pixel, saying which layer won that pixel and at
   what priority: `z << 4 | math << 3 | layer`. The scheduler presents it with the frame
   (`sched_layers()`), along with a snapshot of PPU state (`sched_ppu_state_get()`). This
   doesn't change normal output: frame and state hashes match the unmodified PPU on all
   9,200 frames of the Leene Square replay.

   **Behind buffer.** Optionally (`PpuSetBehindBuffer`, `sched_set_behind()`), the PPU
   also records, per pixel, the *runner-up*: the best layer that lost to the winner
   there, or the backdrop if none did. It stores that layer's color (B G R x, brightness
   applied, no color math) and its layer byte (`z << 4 | layer`). The scheduler presents
   them with the frame (`sched_behind()`, `sched_behind_layers()`), and returns NULL for
   frames drawn without them. The per-pixel renderer finds the runner-up by continuing
   down the mode's priority list past the winner. The whole-line renderer keeps a
   second z buffer: a beaten winner moves into it, and a loser that beats it replaces
   it. Its draw functions come in two compiled versions, so with the buffer off nothing
   extra runs. `test_snes_bus` checks that both renderers give the same runner-ups. The
   frontend turns the buffer on only for the presets that use it (subtle and HD-2D).
   Hashes stay identical with it on or off.
2. **Classification** (`shaders/light.frag`). The layer byte sorts each pixel into one of
   six classes:
   - *backdrop*
   - *far planes*: low BG3/BG4
   - *ground*: low-priority BG1/BG2
   - *foreground*: high-priority BG1/BG2, such as arches, roofs and canopies
   - *actors*: sprites
   - *UI*: BG3 at the top priority level in mode 1, such as text boxes, menus and the
     battle HUD

   UI pixels skip every effect and stay sharp.
3. **Point lights** (`shaders/emit.frag`), at 64×56:
   - Pixels that are bright, saturated, and clearly brighter than their surroundings
     (flames, lamps, windows, glowing magic) become light sources.
   - The light map is blurred twice and lights the surfaces around each source.
   - Its strength follows the scene's darkness: full in interiors and at night, a
     quarter in daylight.
   - UI is never a light source.
4. **Lighting at console resolution.** Pixel-scale lighting suits pixel art, so this pass
   runs at 256×224:
   - Relief normals from brightness give Lambert lighting from one warm key light.
   - Actor silhouettes cast soft shadows down and to the right.
   - Ground darkens where it meets foreground and actors (contact occlusion).
   - Actor edges facing the light get a rim light.
   - Distance haze is applied to the far planes.
   The pass also writes a *background plane*. It is the same picture, except that under
   foreground pixels it holds the behind buffer's pixel, lit the same way (shadows,
   point lights, haze; no relief). Where only the backdrop is behind a foreground pixel,
   nothing was drawn there, so the foreground stands in for it.
5. **Sharp-bilinear upscale** to the output size. This pass splits the picture into the
   background plane and a foreground plane (foreground color and coverage). With the
   **diorama tilt** (`tilt`: off in Subtle, 0.022 in HD-2D), the foreground is drawn
   about 2% larger around the focus point. Roofs, arches and canopies then shift a few
   pixels away from the focus, about 3 source pixels at the picture's edges, as tall
   things near the camera do in a tilted view. The pixels they move off show the
   background plane. The enlargement maps the picture into itself, so the edges never
   need filling. The foreground never covers UI or a pixel that was in front of it,
   such as a high-priority sprite. Where nothing is behind it, it stays where it was.
   The upscale is not a pass of its own. Storing two picture-size half-float planes and
   reading them back cost more than working them out again from the small console-size
   targets, so `shaders/upscale.frag` is compiled into the passes that read the picture
   (blur amount, bloom source, final). They evaluate it where they used to sample the
   stored planes.
6. **Tilt-shift depth of field** at half resolution, done separately for the two planes
   and then composited (foreground over background):
   - The background plane holds the hidden pixels under the foreground, so blurring it
     never smears an in-focus arch or roof into the blurred ground around it.
   - The foreground plane is blurred as color plus coverage, so a blurred roof's edge
     fades out over the sharp ground behind it instead of ending in a hard cut.
   - A sharp band follows the actors' vertical center, and blur grows above and below it.
   - Planes away from the focal depth also blur.
   - Each sample is weighted by its own blur amount, so in-focus pixels don't bleed halos.
   - Actors stay mostly sharp wherever they stand.
   - Small blurs take fewer samples (about 1.75 texels apart, up to 15).
   - The effect scales with how much of the screen is ground or foreground, so title
     cards and black-backed screens keep it off.
7. **Bloom**, from two Gaussian levels at 1/4 and 1/8 resolution. One pass makes both the
   bloom source and the plain copy that the side fill starts from.
8. **Grade:** exposure and a soft highlight shoulder (identity below 0.7, so blacks stay black), saturation, split toning (cool shadows, warm
   highlights), a vignette and light grain.
9. **Side fill:** outside the 4:3 picture, a blurred, mirrored and dimmed reflection of
   its edges. Skipped when the picture covers the whole output.

Menus that cover more than 15–30% of the screen with UI ease the scene effects off.

## The 3D diorama (`hd2d3d`)

The `hd2d3d` preset rebuilds map views as real geometry, in the manner of the HD-2D
remakes: floors, walls that rise, roofs on top of them, characters standing up as
billboards, a perspective camera tilted back from straight down, a key light with a
shadow map, and the depth of field driven by real depth. The world map becomes a
diorama landscape ("The world map" below). Everything else (battles, menus, mode 7,
title and transition screens) is the `hd2d` picture, pixel for pixel, with a short
crossfade (8 frames) between the two.

**It still draws nothing of its own.** The geometry comes from the game's map as the
running game holds it in WRAM, and every surface is textured with the console's own
tile pixels, decoded from VRAM and CGRAM as they are drawn.

### Where the scene comes from

The renderer is game-agnostic: it never reads game memory. The game module describes
what is on screen through an optional export, `ct_module_info.scene`
(`runtime/module.h`, `ct_scene`): the map's size, its tile layers as BG tilemap words
(tile, palette, priority, flips) with their VRAM character base, a class per cell
(floor, solid, wall-ish, water, stairs, unknown) with flags (overhead: drawn over
sprites; event), the objects with their foot point and the OAM entries that draw them,
the player's object, the camera in map pixels, a map generation counter and a mode
(map view or other). The engine adds the console's state (`ct_scene_capture`: VRAM,
CGRAM, OAM, the OBJ size and base, brightness). A module with no scene export, or a
mode other than `CT_SCENE_FIELD` or `CT_SCENE_WORLD`, gets the 2D picture.

For this game, `game/ct/scene.c` fills it from the addresses in [SCENE.md](SCENE.md). A
frame is a map view while the field loop's NMI handler is installed (`$0500`), and the
world map while the overworld's is (`game/ct/world_scene.c`); battles, menus and the
title install their own. The generation counter changes when
the map grids, property bytes or metatile tables hash differently, which also catches
tiles changed in place. The `scene_decode` test checks the description against an
independent decoding of the same WRAM and OAM (`scene_probe.py --text`).

### Heights

The game keeps no heights, so `frontend/hd2d/scene3d.c` infers them, per map column,
from the cells (all of them generic: a cell's class and its tiles' priority bits):

1. A cell is *raised* if it is blocked (solid or wall-ish) or has overhead tiles, and has
   pixels at all (the empty void around a map is nothing).
2. Each run of raised cells in a column, seen from the south, stands up from its south
   edge as a wall facing the camera: up to `hmax` cells (3, 48 px) high, fewer for a
   shorter run. The rest of the run lies flat on top of the wall, moved south by the
   wall's height. A tile's screen row is then `z - height`, where the original's
   three-quarter view drew it, so the art keeps its layout.
3. A run whose blocked cells go deeper than its wall is a box: side walls close it where
   the neighbouring columns differ, textured with the wall's own tiles. Shorter runs
   (posts, trunks, canopies over a trunk, arches) stay cut-outs, and see-through where
   the tiles are.
4. Overhead tiles over walkable cells belong to the blocked run north of them (a
   canopy over its trunk, a roof's eaves); in such a cell only the overhead tiles rise,
   the ground under them stays a floor. A long run of overhead tiles with nothing
   blocked in it (railings, edges drawn over sprites) stays on the floor.
5. Floor a wall leaves uncovered behind it is filled with the ground of the cell north
   of its run.
6. Sprites are billboards: each visible OAM entry, standing at its object's foot point
   on the floor, made taller by `cos(tilt) / sin(tilt)` (at most 1.4) so it keeps its
   drawn proportions under the tilt. An object standing in a raised cell whose priority
   puts it over that cell's tiles in the original (priority 3, or tiles that aren't
   overhead) stands in front of the wall, or on top of it, at the same screen row.
   Entries no object owns (effects, markers) stand at their own bottom edge.

Where these are wrong:

- Height is a guess. Walls are at most `hmax` cells; a tall building shows its upper
  facade lying on the roof. Ground-coloured blocked cells (invisible walls at the edge of
  a path) rise as low walls of ground.
- The walk planes (bridges, balconies: the cells' `plane`) and stairs are flat. The
  game's two walk levels aren't separated, so an upper floor is at floor level behind
  the wall that should hold it up.
- Shaped cells (wall-ish: corners, diagonals) count as blocked, so diagonal walls are
  stepped.
- BG3 (in map views mostly fog, light shafts and other color-math overlays) and color
  math in general aren't drawn in 3D; if any visible pixel uses SNES color math, that
  frame stays on the 2D path so fog, darkness masks and translucent sprites keep their
  console blend. Otherwise, the UI from BG3's top level is laid flat over the picture
  as in the 2D path. Translucent or tinted sprites are opaque in frames without math.
- What the console draws into its BG1 tilemap only (a text box's window) isn't in the
  scene, which is the map as the game keeps it. Each frame, per 8x8 tile of BG1 on the
  screen, the console's BG1 pixels are compared with the scene's tile decoded from VRAM
  and CGRAM (`s3d_box_mask` in `hd2d.c`); a run of at least 16 differing tiles across a
  row is a box, and the console's pixels there are laid flat with the UI. Scattered
  differences (tiles the game changed, color math) are left alone, and so is a frame
  where most tiles differ.
- Sprites are flat cut-outs; one that the original split between priorities (half
  behind a roof edge) is drawn whole at its foot point.
- Clearly horizontal poses (at least one 8 px tile wider than tall) lie on the map
  plane at their supporting surface instead of standing up as a billboard. This lets
  sleeping or prone characters rest on beds and other broad raised geometry.
- The camera sees further and wider at the top of the picture than the console's, so
  it stops sooner at the map's edges, keeping the floor at the picture's edges on the
  map; it then doesn't follow the player all the way to an edge. A map smaller than the
  view is centered, on a dark ground.

The camera can be changed with `CT_HD2D_3D`, a comma-separated list:
`tilt=40` (degrees from straight down), `fov=30` (vertical field of view), `zoom=0.75`
(1: the console's 256 px across at the focus), `hmax=3` (wall cap in cells),
`stretch=1.2` (billboard height scale; default from the tilt) and `aoradius=12` (the
ambient occlusion's reach, in map pixels), and for the world map `wtilt=40`,
`wzoom=0.75` and `marks=1` (entrance marks).

### Drawing

1. The map's quads (one per 8x8 tile of BG2 and BG1: floor ones first) are rebuilt when
   the generation changes, or when the set of empty tiles in VRAM does (a map whose
   tiles were still loading). The sprites' quads are rebuilt every frame. VRAM (as a
   16-bit texture) and CGRAM are uploaded every frame, so animated tiles and palettes
   show as the console changes them.
2. A 1024×1024 shadow map from the key light, low from the south-west, of the raised
   geometry and the sprites (cut-outs tested per pixel).
3. The surfaces into a target with a depth buffer, at the render scale (the picture's
   size by default): each pixel decodes its tile's bitplanes and palette, is lit
   relative to the sunlit floor (which keeps the original colors), shadowed with 4 PCF
   taps, lit by the 2D path's point-light map at the screen position the console drew
   it, hazed with distance and scaled by the console's brightness. Alpha holds the depth
   for the depth of field (0.5 at the focus); the material target holds the quad's
   normal for the ambient occlusion.
4. Ambient occlusion from that depth buffer, at half the render size, blurred
   (below).
5. Below a render scale of 100%: FSR 1's EASU up to the picture's size, which also
   applies the occlusion.
6. The UI from the console frame on top, flat and sharp, at the picture's size.
7. The 2D path's depth of field, bloom, grade and side fill, compiled again with
   `scene3d_pic.frag` in place of the upscale: one plane, real depth. The tilt-shift band
   is turned down to a quarter, as depth does most of the blur. At the native render
   scale this is where the occlusion is applied, and the final pass sharpens (RCAS after
   EASU, or CAS).

A GPU or driver that cannot build the 3D programs keeps `hd2d3d` and `deck3d` on the 2D
path (the log says so).

Two hook points are left for asset packs: the cell hook (`s3d_set_cell_hook` in
`scene3d.h`), where a pack can replace a cell's generated geometry with its own, and the
tile lookup (`tileTexel()` in `shaders/scene3d.frag`), the one function every 3D
surface samples its texture through, keyed by the tile (VRAM address, which gives the
tile id, and palette), where a pack can return its own texture. Asset packs use both,
and two more: glTF models drawn as a mesh of their own with `scene3d.frag`'s `MODEL`
variant (`hd2d_pack_models`), and a sprite hook (`s3d_set_sprite_hook`) that draws a
character as one billboard of a whole pose picture (`S3D_PACKED`).

### Render scale, sharpening and ambient occlusion

Four settings, in the menu's **Look** tab and in `settings.ini` (the command-line options
override them for one run):

| Setting | Values | Default | Effect |
| --- | --- | --- | --- |
| `render_scale` | `auto`, `50`, `60`, `67`, `75`, `85`, `100` | `auto`: 100 in `hd2d3d`, 67 in `deck3d` | The 3D scene is drawn at this percentage of the picture's size, then upscaled with AMD FSR 1 (EASU) and sharpened with its RCAS. The UI, the 2D path and everything outside map views stay at full size. |
| `sharpen` | `auto`, `0`, `0.25`, `0.5`, `0.75`, `1` | `auto`: off at 100%, 0.5 below | At 100%, AMD CAS at that sharpness (0 is off). Below 100%, RCAS's strength (`FsrRcasCon`'s `exp2(-stops)`: 1 is the most, 0.5 one stop less). |
| `ao` | `auto`, `0`, `0.25`, `0.5`, `0.75`, `1` | `auto`: 0.75 | Ambient occlusion strength. 0 turns it off and skips its passes. |
| `ao_size` | `auto`, `half`, `quarter` | `auto`: half in `hd2d3d`, quarter in `deck3d` | The occlusion's resolution, of the render size each way. |

**Where the upscale goes.** EASU runs right after the surfaces and the occlusion, before
the UI, the depth of field, the bloom and the grade, which then read the upscaled
picture as they read the native one:

- *Quality.* The in-focus picture is the part a viewer looks at, and the part that needs
  EASU's edges. The depth of field and the bloom are blurs: computed after the upscale,
  they look the same at every render scale. The UI (text boxes, menus) is drawn over
  the upscaled picture at full size, so it is never scaled. The grain and the vignette
  are added at the output size. RCAS sharpens in the final pass, where the picture is
  read: only its in-focus part, since the depth of field mixes in its own, unsharpened
  blur where things are out of focus, and the bloom source reads the picture unsharpened
  (sharpened bokeh and bloom only add ringing and noise). This follows AMD's guidance:
  scale after the main render and before the noise-adding effects and the UI.
- *Cost.* After the upscale the post passes cost what they cost at 100%, about 1.35 ms
  at 1080p on the Ally. Upscaling after them would save part of that, but EASU would then spend its
  work on the blurred picture, the grain and the vignette would have to be added before
  it (and be scaled and sharpened), and the UI would need its own pass. The surfaces
  pass is the one that scales with the render size (about 0.9 ms at 1080p, about 0.45 ms
  at 67%), and the occlusion with it.

To keep the extra pass cheap, EASU writes one 32-bit target (`RGB10_A2`: the color
through FSR's reversible tonemap, so highlights over 1 survive, and the UI's mask in
alpha); the passes after it take the depth and the material from the render-size
targets, which hold them at that size anyway. At 1080p that brought EASU from 0.53 ms
(half-float color plus material) to 0.28 ms.

**FSR 1 and CAS** come from AMD's FidelityFX repositories (FSR v1.0.2, CAS v1.0.2, MIT;
`third_party/fidelityfx/`, with the commits and hashes in its README). Their GLSL path
needs GLSL 4.00 / ES 3.10 (`ffx_a.h` uses `bitfieldExtract`; EASU's reference callbacks
use `textureGather`), which OpenGL 3.3 core and OpenGL ES 3.0 don't have, so the renderer
compiles `ffx_fsr1_cas.glsl`, a port of the parts it uses to GLSL 3.30 / ES 3.00 at
32-bit float: the headers' math unchanged, the taps fetched by the caller with
`texelFetch`. It is MIT, like its source.

**Ambient occlusion** (`shaders/aoprep.frag`, `ssao.frag`, `aoblur.frag`) is our own, a
normal-oriented hemisphere: for each surface pixel, 12 samples (8 at quarter size) in
the hemisphere around its normal, in view space, within 12 map pixels (`aoradius`),
tested against the 3D depth buffer. A sample that something is in front of counts as
occluded, faded out when that something is much nearer than the radius, so a wall in
front of a far floor casts no halo on it. The normals aren't reconstructed from depth:
every quad has one of six, which the surfaces pass writes to its material target. A
first pass packs depth, normal and a sprite flag into one small texture, so each sample
is one cached fetch; a 4×4 pattern rotates the samples per pixel, and a 5-tap bilateral
blur each way, weighted ½ 1 1 1 ½, averages the pattern out exactly while keeping to
its side of depth edges. A joint bilateral upsample (against the picture's depth)
applies it, in EASU or, at 100%, where the picture is read. It darkens only the lit
surfaces: sprites and billboards neither receive it nor occlude (the shadow map gives
them their own shadows, and their flat cards would darken the floor behind them), and
the UI and the void around the map get none. The result is the soft contact shadow of
the HD-2D remakes where walls meet floors, along fences and stalls, in corners and
under eaves.

Why not AMD's CACAO (FidelityFX, MIT too): it is a set of compute shaders, and OpenGL
3.3 and ES 3.0 have no compute. A port to fragment passes would keep little of it. GTAO
gives physically better results on curved surfaces, but needs a horizon search along
several directions per pixel; this scene is axis-aligned boxes with known normals, where
a hemisphere with a range check and the true normals does as well, for less.

### The Steam Deck variant (`deck3d`)

`deck3d` is the `deck` look (the shorter depth of field, one bloom level, black bars at
16:10) with the 3D diorama at a render scale of 67% and the occlusion at a quarter of the
render size. The Steam Deck still starts in `deck`; choose `deck3d` with R3 or in the
menu. Its cost is in [Performance](#performance): on the Ally, a little under what
`hd2d3d` cost before these options.

### The world map

The overworld comes as `CT_SCENE_WORLD` (`runtime/module.h`): a map of 8 px cells (192 x
128 here, one tile each) that wraps at both edges, its two BG layers as tilemap words, a
walk class per cell (floor, water, blocked; the cover flag; exits as event cells), the
camera, the party and the other sprites as objects, and a third layer, the clouds, as an
*overlay* with its own scroll and blend. `frontend/hd2d/scene3d_world.c` makes a landscape
of it, and the same passes as the field draw it.

**Relief.** The walk class says floor, water or blocked, and "blocked" lumps mountains,
forest, the shore band and buildings together (some games also mark mountain tops as
water: unwalkable). So each cell's kind comes from its pixels, BG1 over BG2 as the
console composites them, unless the module named it (`CT_CELL_MOUNTAIN`, `_FOREST`,
`_SHORE`, `_FLAT`):

| Kind | Rule | Height |
| --- | --- | --- |
| water | a water cell whose pixels are mostly blue (blue above red, at least green); or any cell almost all blue | -5 px, flat |
| forest | blocked, mean color green-dominant | a canopy: 4 px at its edge, 8 inside, ±2 by the cell's brightness (tree tops light, gaps dark) |
| shore | blocked, within 2 cells of water, not a dark red-brown | -1.5 px |
| mountain | blocked, mean color red-dominant (browns, rock), in a patch at least 5 cells across | a dome over the footprint, up to 48 px six cells in, ±5 by the cell's brightness against the mountain's mean (lit ridges up) |
| flat | blocked, anything else (buildings, walls, bridges, thin brown patches) | 0 |
| floor | walkable | 0 |

A tile corner takes the mean of its four cells, or the water level if any of them is
water, so the water stays flat and the land slopes into it over one cell: beaches and
cliffs come out of that. Normals come from the corner heights (central differences), go
to the shader as a small texture, and light the slopes; the shadow map is of everything
above 0.75 px (canopies, mountains, sprites).

Where the guess is wrong, the module corrects it by metatile: `relief[]` in
`game/ct/world_scene.c` maps (world map id, layer, metatile index) to a kind, like a
3dSen profile; `worldmap_probe.py` keeps a copy for the `scene_decode` test. It holds
Guardia Castle on `$1F0` (grey-brown stone that read as mountain, green-lit walls as
forest), which now lies flat.

**Mesh.** One quad per tile and layer, on the four corner heights, so a tile drapes over
the slope instead of stretching; BG1's quad uses the same corners and wins the depth test
by being drawn second (`GL_LEQUAL` on the world map: a lift would let slivers of BG2
show on the slopes). Water quads get a shimmer in the shader: a slow swell of ±3.5% and
glints that twinkle on 3x3-pixel cells, both on the console's pixel grid; no texture of
our own. The mesh is one copy of the map, built when the map changes (27,533 quads for
1000 A.D., about 10 ms on the Ally); a wrapping map is drawn up to nine times, offset by
the map's size, for the copies the view's ground footprint reaches.

**Clouds.** The overlay is drawn as a plane 110 px above the ground, fixed in the world
and moved by its scroll, blended at the console's half-add (`overlay_blend / 16`, times
0.8). It thins to a quarter over the player (where the line of sight crosses it). It also
shades the ground: each surface pixel looks the cloud layer up where the sun's ray to it
crosses the plane (four taps, for a soft edge), so the shadows drift with the clouds.

**Sprites.** Billboards that face the camera, standing on the ground at the object's foot
point (the height under it), and snapped to the target's pixel grid: the foot lands on a
whole pixel, and one sprite pixel is a whole number of target pixels, the ground's own
scale at that depth rounded (docs/PRIOR-ART.md, A2: no mixed pixel densities). The
pieces keep their console layout around the foot. OAM entries no object owns (the era
caption) lie flat over the picture where the console drew them, and skip the effects as
UI does.

**Camera.** The field's tilt and zoom by default; `worldmap_tilt` and `worldmap_zoom` (the
Look tab, settings.ini; `CT_HD2D_3D=wtilt=..,wzoom=..`) give the world map its own, e.g.
a higher one (tilt 25, zoom 60%). It looks at the party, a little north of its feet, and
never stops at an edge. BG3 isn't UI on the world map (it is the clouds), so the UI layer
is off there.

**Entrance marks**, off by default (`CT_HD2D_3D=marks=1`): a small pulsing diamond in
the middle of each entrance (a group of touching exit cells), drawn by the shader alone.

**Render scale, FSR and occlusion** apply as in the field: the surfaces are drawn at the
render scale, EASU brings them up to the picture's size, and the ambient occlusion runs
on their depth. The ground gives ssao.frag the nearest of its five normals (up unless a
slope is steeper than about 45°), which darkens valleys between peaks and the feet of
mountains and forests; water gets none (it would darken under every shore), and the
cloud plane leaves the occlusion's input as the ground under it wrote it. The sprites are
snapped to the render target's pixels, so below 100% FSR's upscale spreads one sprite
pixel over a non-integer number of output pixels, as it does for the ground.

**The pause menu and transitions** change the NMI vector, so the scene is `OTHER` and the
picture crossfades to the 2D path, as in the field.

Where this is wrong:

- The kinds are a guess from mean colors. Brown roofs and walls in a patch five cells
  wide become small hills; green-lit stone becomes forest; a sandy cliff away from water
  becomes a mountain. Each needs a `relief[]` entry. Only `$1F0` (1000 A.D.) was tuned;
  the other eras' maps share the engine but not necessarily the tiles.
- The heights don't come from the art. A mountain is a dome over its footprint, not
  the peak where the picture draws it, and the picture is painted for a three-quarter
  view, so its own shading doubles the 3D light a little.
- Towns, castles and bridges lie flat: nothing stands up as a wall. Cover cells (here the
  bottom edge of towns, where the walker draws half hidden) are plain floor.
- Sprites are flat cut-outs, and a billboard near the top of the picture leans with the
  view. The ferry and other play-area sprites are grouped by touching pieces.
- Text boxes on the world map, if any, are not shown in 3D (the UI layer is off).
- Packs replace textures through `tileTexel()`, the clouds included; the cell hook
  (`s3d_set_cell_hook`) and the packs' models work on the world map too, the pose
  billboards don't (the world map's sprites have no poses).
- The cloud plane is one quad over the view's footprint; at very low tilts its far edge
  can show.

## Performance

Measured on a ROG Ally (Ryzen Z1 Extreme, Radeon 780M, Mesa radeonsi) at 1706×1120
(`--scale 5`, about 1080p's pixel count), over the Leene Square replay at normal speed,
with `--log-gpu`:

| | Median | 95th percentile |
| --- | --- | --- |
| Before the upscale was folded into the passes that read it | 2.6–4.4 ms | 4.1–5.3 ms |
| Now | 1.4–2.3 ms | 1.9–3.2 ms |

These times include the uploads and the CPU-side scene analysis (about 0.03 ms). Each range
covers 3 to 6 interleaved runs, made while other work loaded the machine (load average
3–18). The variation between runs is larger than a few tenths of a millisecond: the
handheld's clocks follow the load, and the 95th percentile follows the GPU clock. The
quietest runs gave 2.6 / 4.1–4.5 ms before and 1.4 / 1.9–2.6 ms now.

On the GPU, from `CT_HD2D_PROFILE`, the frame went from about 2.2 ms to 0.9–1.4 ms. Most of
the gain is that the two picture-size half-float planes (about 27 MB written and read back
per frame) are gone. The depth-of-field blur is now the largest pass (0.3–0.5 ms), then the
final pass (0.25–0.3 ms). Desktop AMD and NVIDIA GPUs have a large margin.

The 3D diorama, from `CT_HD2D_PROFILE` on the same Ally at `--scale 5` (a 1493×1120
picture), over the Leene Square and bell plaza frames of the replay (8300–9200, unpaced,
while other work loaded the machine): a median of 2.4 ms of GPU time per frame, against
1.2 ms for `hd2d` in the same run. The surfaces pass is the largest (about 0.96 ms: every
pixel decodes its tile from VRAM and takes four shadow taps), then the final pass
(0.39 ms), the depth of field (0.35 ms) and its blur amount (0.28 ms); the shadow map
costs 0.05 ms. The CPU side (scene capture, sprite quads, uploads including VRAM) is about
0.18 ms; a map's mesh rebuild, on a map change, a few milliseconds once. Both presets
showed 95th percentiles of 15–19 ms in that run: the GPU's clock drops while other work
loads the machine, so those are the machine's, not the passes'.

The 3D diorama's render scale and ambient occlusion, from `CT_HD2D_PROFILE` on the same
Ally: replay frames 8400–9200 (Leene Square to the bell plaza), started from a save
state at frame 8400 so each run is short; the median GPU time per frame of each run,
then the median of 3 runs per row, the runs interleaved (their spread was within 2%).
"Before" is `hd2d3d` built from the commit before these options; `hd2d` and `deck` (2D)
are for comparison. Picture sizes: 1067×800 in a 1280×800 window (the Steam Deck's
screen), 1440×1080 at 1080p, 1493×1120 as above.

| GPU ms per frame | 1280×800 | 1920×1080 | 1706×1120 |
| --- | --- | --- | --- |
| `hd2d3d` before | 1.43 | 2.35 | 2.46 |
| `hd2d3d` 100%, no occlusion (`--ao 0`) | 1.44 | 2.37 | 2.45 |
| `hd2d3d` 100%, CAS (`--ao 0 --sharpen 0.5`) | 1.45 | 2.39 | 2.47 |
| `hd2d3d` 100% with occlusion (the default) | 1.67 | 2.67 | 2.77 |
| `hd2d3d` 67% + FSR, no occlusion | 1.26 | 2.09 | 2.23 |
| `hd2d3d` 67% + FSR with occlusion | 1.40 | 2.32 | 2.48 |
| `deck3d` (67% + FSR, occlusion at a quarter) | 1.32 | 2.24 | 2.34 |
| `deck` (2D) | 0.61 | 1.07 | 1.07 |
| `hd2d` (2D) | 0.67 | 1.11 | 1.16 |

At 1080p, 67% with the occlusion breaks down as: surfaces 0.50 ms (0.92 at 100%),
occlusion 0.15 (input 0.07, occlusion 0.04, blur 0.03; 0.30 at 100%, where it is at
half the picture's size), EASU with the occlusion applied 0.34 (0.28 without), final pass
with RCAS 0.42 (0.48 at 100%: it reads the 32-bit upscaled picture, not the half-float
one). `deck3d`'s quarter-size occlusion costs 0.09 ms there. CAS adds about 0.01–0.02
ms to the final pass. So 67% with FSR and the occlusion costs what `hd2d3d` cost before
(−3% to +1%), and `deck3d` is 5–8% under it. With this branch's first versions (EASU
writing half-float color plus the material, the occlusion sampling the render-size
depth and material directly) the same configuration was about 0.5 ms over.

The Steam Deck (RDNA2, 8 compute units at up to 1.6 GHz, against the 780M's 12 at up
to 2.7 GHz, with similar memory bandwidth) wasn't measured; from those ratios `deck3d`
at 800p should take roughly 2–3 ms there, against about 1 ms for `deck`. That fits in a
60 Hz frame, so `deck3d` is offered, but the Deck still starts in `deck`, the lighter
choice for its battery.

The world map in `hd2d3d`, same Ally at `--scale 5` (a 1493×1120 picture), over frames
7400-8700 of a walk across the 1000 A.D. map (unpaced, load average 7-8), median GPU time
per frame:

| | GPU | surfaces | occlusion (input, AO, blur) | EASU | final |
| --- | --- | --- | --- | --- | --- |
| 100%, occlusion (the default) | 2.89 ms | 1.06 | 0.29 | - | 0.51 |
| 67% + FSR, occlusion | 2.51 ms | 0.59 | 0.14 | 0.35 | 0.45 |

The rest: shadow map 0.03 ms, depth of field 0.35, its blur amount 0.29-0.31, bloom
0.24-0.30. Before the render scale and occlusion existed, the world map took 2.4 ms. The world mesh is built when
the map changes: about 10 ms, some fifteen times while a map loads (its tables arrive
over several frames, behind the fade-in), then not again. The CPU side per frame
(sprites, the overlay's tilemap, uploads) is about 0.14 ms.

The behind buffer costs CPU time in the PPU. Headless (`ct_boot`, 9,200 frames of the
Leene Square replay), the game runs at about 388 frames per second without it and
about 314 with it (`--behind`). That is about 0.6 ms more per frame, so it is only on
while a preset uses it.

## Asset packs

With `--pack`, replacement art is composited at the output resolution in the upscale
(`shaders/upscale.frag`), before the lighting, depth of field and grade see the picture.
It is driven by a third G-buffer, the PPU's provenance buffer. In the 3D looks, packs
also add glTF models to the map and draw characters with whole-pose pictures. Details,
the format and the cost are in [ASSET-PACKS.md](ASSET-PACKS.md).

## HD text

With a font set (`hd_font=`, `--hd-font`; [USER-GUIDE.md](USER-GUIDE.md#hd-text)), the
final pass (`shaders/final.frag`, also the 3D looks') draws one more picture over
everything but the on-screen message: the text boxes' text, rasterized on the CPU with
stb_truetype at the screen's scale (`frontend/hdtext.c`, `hd2d_set_text_overlay`),
uploaded only when it changes, premultiplied and linearly filtered. The console's glyph
pixels are taken out of the source picture before the upscale, so the lighting and the
grade see an empty box. The lines are found on the screen from the provenance G-buffer,
as asset packs' tiles are. With no font, the pass is as before (one branch not taken).
In the 3D diorama the dialogue box's window is part of BG1, which the 3D scene replaces;
the renderer lays the box's tiles flat over the scene (below), and the text over them.

## Limits and next steps

- The behind buffer holds one hidden layer, not all of them. A pixel with two layers
  stacked under the foreground shows only the upper one when revealed. Sprites hide
  other sprites at the same pixel before the PPU sees them, so a sprite is never the
  runner-up of another sprite.
- The tilt moves whole classes. An object drawn partly with high-priority tiles and
  partly with low-priority ones (such as a tent whose top is in front of the actors and
  whose bottom is not) splits at that line by the tilt's few pixels. Pixels changed by
  color math (translucent clouds, for example) carry the blended color with them.
- Only the per-pixel PPU path runs in the game. The whole-line renderer's behind
  tracking is covered by the unit test only. Its 4x4 mode 7 path doesn't write the
  buffer.
- Classification uses PPU state only, with no per-game tables, so it stays game-agnostic
  (`tools/check_agnostic.py`). Some scenes will misclassify. Mode 7 scenes get
  tilt-shift only.
- The renderer works under OpenGL ES 3.0 for Android, but that hasn't been tested on a
  device.
