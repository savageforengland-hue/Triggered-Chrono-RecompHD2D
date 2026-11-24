<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Visual design target: the HD-2D and 3D looks

What "right" looks like for the `hd2d` / `deck` presets (the flat looks) and
`hd2d3d` / `deck3d` (the diorama), the checks a QA pass applies to a screenshot, and the
improvements worth doing next. The reference is the modern HD-2D remakes (Dragon Quest
III HD-2D, Octopath Traveler): a miniature stage, lit warm, pixel art kept crisp, the
edges of the stage falling out of focus. How the renderer works is in
[HD2D.md](HD2D.md); this file says what it must achieve.

Constraints that shape every choice:

- **No art of our own.** Every pixel comes from the user's ROM at runtime. Effects are
  lighting, blur, geometry and grade over that. Personal asset packs may add art; the
  default look must not depend on them.
- **Game-agnostic engine.** Nothing in `runtime/`, `frontend/`, `tools/`, `test/`,
  `recomp/` may name or special-case a game. Per-game knowledge lives in the game module
  and reaches the renderer through `ct_scene`.
- **Handheld first.** The Ally at 1080p and the Deck at 800p, GL 3.3 and GLES 3.0. A look
  that needs more than about 3 ms of GPU time at 1080p on the Ally is not a default.

## The three rules

When rules below conflict, these win, in this order.

1. **Never worse than the plain picture.** If the renderer cannot show something
   correctly (a scene it cannot rebuild, a layer it cannot place, a UI it cannot
   classify), it shows the console's picture for that thing. A wrong 3D frame is a bug;
   a 2D frame is a fallback. Information the console showed (text, cursors, HUD, fades,
   flashes, weather) is never lost.
2. **UI is sacred.** Text boxes, menus, HUD, cursors, name entry, the on-screen message
   and HD text are never blurred, lit, shadowed, hazed, bloomed into, graded, tilted or
   sharpened. They are the console's pixels at an even scale.
3. **One pixel density.** Everything in the scene sits on one pixel grid at a given
   depth. A sprite pixel is the same size as the floor pixel under its feet. No smooth
   filtering inside the art; blur comes only from the depth of field.

## Targets per element

Numbers are for a 1080-line picture; scale lengths by `picture height / 1080`.

### Camera (3D)

| | Field | World map |
| --- | --- | --- |
| Tilt (degrees from straight down) | 40 (allowed 35-45) | 40 default; 25-35 reads better as a map |
| Vertical FOV | 30 (allowed 25-35) | same |
| Zoom | 0.75; see "integer zoom" below | 0.6-0.75 |

- The art is painted for a three-quarter view with floors and walls both at 1:1. Below
  35 the walls flatten into the floor and the diorama reads as a tilted photo; above 45
  floors squash and the upper screen loses resolution. 40 is the balance.
- FOV stays narrow. HD-2D is a long-lens miniature: at 30 the floor's scale already
  varies by about +22% (bottom row) to -22% (top row) around the focus row. A wider FOV
  makes billboards at the picture's edges lean and breaks rule 3.
- The camera looks at the player, never shows the outside of the map when the map is
  larger than the view, and moves in whole console pixels (no sub-pixel shimmer of
  the floor texture while scrolling).
- **A map smaller than the view is framed, not floated.** The room fills at least half
  the picture's height (zoom in up to 1.25 if needed) and sits centered. Parts of the
  map that the console never shows together with this room (other rooms stored on the
  same map) must not be visible at the picture's edges.
- **The void** around a map is near-black and neutral (the current `0.07, 0.075, 0.09`),
  with the vignette over it. It never takes the backdrop color or a tint from the
  grade's split tone strong enough to read as a color (no dark red or brown field).
- Gyro and user offsets are clamped so the three points above still hold.

### Depth of field and tilt-shift

- **Flat looks:** a sharp band of 36% of the picture's height centered on the actors;
  full blur reached 34% further out. Maximum blur radius 6 px (3.5 px on `deck`). The
  player and anything at the player's row is pixel-sharp.
- **3D:** blur comes from real depth. Sharp from the focus distance -10% to +15% of the
  view distance; the tilt-shift band adds at most a quarter of the flat look's.
  Foreground (nearer than focus) blurs at most half as much as the background, so a
  wall at the bottom edge doesn't become a smear over the player.
- Actors are never blurred by more than 30% of the local amount. NPCs the player can
  talk to must be readable anywhere on screen.
- DOF is off (not eased; off) on any frame that is mostly UI, a title card, a
  black-backed screen, a menu, or a mode 7 scene's HUD. **A full-screen menu is all UI
  whatever layers it is drawn on.**
- No halos: an in-focus edge next to a blurred region shows no glow of its own color
  in the blur, and no blurred color bleeds onto it.

### Bloom

- Sources are lights only: flames, lamps, windows, magic, sun on water. Threshold 0.7,
  strength 0.45 (0.4 on `deck`). White UI, white text, white clothing and bright
  ground (sand, snow, paper) must not glow: bloom from a pixel needs brightness *and*
  either saturation or contrast against its surroundings.
- Bloom never raises a mid-grey region's brightness by more than 4%. A daylight scene
  with bloom on and off differs visibly only around lights and speculars.

### Grade

- Exposure 1.06, saturation 1.12, contrast 1.04, split tone 0.35 (cool shadows, warm
  highlights), vignette 0.35, grain 0.018. These stay: the SNES palette is already
  saturated, and the remakes' look is warmth and light, not more saturation.
- Blacks stay black: a console pixel of (0,0,0) is at most 2/255 on output, outside
  grain. Fades to black end in black, fades to white in white.
- Hue is preserved: no element shifts hue by more than about 8 degrees from the console
  picture in daylight. Skin tones and the hero's palette are the test.
- Grain is invisible on a still screenshot at arm's length; it only breaks banding.

### Light (3D)

- One key light, warm, low from the south-west, constant per scene. The sunlit floor
  keeps the console's colors exactly (before grade); everything else is darker, never
  brighter, except by point lights.
- South walls about 8-15% darker than the sunlit floor, side walls 20-30%. Never more:
  the art carries its own shading.
- **Interiors and night** (dark scenes, as the 2D path's scene darkness measures): the
  key light goes down to a third and the point lights go up to full; the picture gets
  its light from lamps and windows. A dark interior must not look like a daylight
  scene with a vignette.
- Point lights: warm, radius 24-40 console px, add at most +60% to a surface's own
  color (they brighten the art; they don't paint it orange).
- Sprites take 40% of the scene light at most (their own shading is the art), plus
  shadows. A sprite in a wall's shadow is visibly, not heavily, darker (15-25%).

### Shadows

- Every actor on the ground has a shadow touching its feet. Shadow map shadows follow
  the silhouette, fall up-right of the feet (light from south-west), are soft over 1-2
  console px, and darken the floor by 30-40%, with a cool tint.
- No shadow acne, no peter-panning gap over 1 console px at the feet, no shadow
  swimming while the camera scrolls (shadow map texels snapped to the world).
- Flat looks: the soft down-right shadow, offset at most 3 console px.
- Flying things and effects (OAM entries no object owns) cast none.

### Ambient occlusion (3D)

- Strength 0.75, reach 12 map px. Reads as a soft contact shadow where walls meet
  floors, in corners, under eaves; maximum darkening 35%.
- None on sprites, UI, water, the void. No dark outline around sprites, no banding,
  no 4x4 pattern visible, no halo on a far floor behind a near wall.

### Pixel density and sprites

- Art is point-sampled. A console pixel is a square block of output pixels with hard
  edges (sharp-bilinear: at most 1 output px of blend at a block's edge).
- **Integer zoom at the focus row (target).** Zoom is nudged to the nearest value
  where one console pixel at the focus row is a whole number of output pixels (at
  1440x1080 and zoom 0.75 it is 4.22: choose 4, zoom 0.711). The player's sprite then
  has even pixels.
- **Billboards are snapped** in the field as on the world map: the foot lands on a
  whole target pixel and one sprite pixel is a whole number of target pixels, the
  floor's scale at the feet, rounded. A sprite's pixels are all the same size.
- Sprites stand at their foot point, upright (facing the camera, not leaning with the
  view), stretched only by the tilt compensation (`cos/sin`, capped 1.4). A sprite's
  proportions on screen are within 5% of the console's.
- A multi-piece sprite never splits: no seam, gap or overlap between OAM pieces.
- Translucent and color-math sprites (ghosts, shadows drawn as sprites, spell effects)
  are translucent, not opaque blocks.
- Sprites sort against walls as the console's priorities say: nobody is cut in half
  by a wall he stands in front of, nobody shows through a roof he is under.

### Text boxes and UI

- **A text box is the console's box:** the window, its border and its text, together,
  at the console's position, flat on the screen, on every frame the console shows it,
  open/close animation included. Text without its box, a box without its text, or an
  empty-looking box for more than the console's own delay are all bugs.
- UI scale: console pixels as even blocks; nearest or sharp-bilinear. In the 3D looks
  the UI is drawn at picture size after the upscale, never through FSR, CAS or RCAS.
- Text contrast: text against what is behind it has a luminance ratio of at least
  4.5:1 everywhere. If the scene shows through a translucent window, the scene behind
  it is the *graded scene dimmed as the console's color math dims it*, never brighter.
- HD text (when a font is set) replaces glyph pixels only: same lines, same box, same
  colors, baseline within 1 console px of the console's, no leftover console glyph
  pixels and no HD glyph outside the box. With no font set the picture is unchanged.
- Cursors, selection hands, damage numbers, name tags and the HUD are UI wherever they
  come from (BG3 or sprites): rule 2 applies. In a battle the HUD and the damage
  numbers are sharp even when the field behind is blurred.
- The on-screen message is on top of everything and fades in under 2 s.

### Water

- World map: flat, 5 px under the land; a slow swell (±3.5%) and sparse glints on the
  console pixel grid. Glints are small (3x3 console px), rare (under 1% of water
  pixels lit at once) and never flicker faster than 4 Hz. No AO under shores.
- Field water cells (target): the same swell and glints, plus a darkening toward the
  far edge; no geometry. The game's own water animation must stay visible; ours is
  added on top and is weaker than it.
- Water never receives sprite billboards' shadow-map shadows at full strength
  (half), and never shows z-fighting with the shore.

### Sky, haze and distance

- Haze starts beyond the focus distance and reaches at most 25% at the far edge of
  the view by day, cool and light (the horizon's color), 10% warm-dark in interiors.
  It never tints the player's row.
- Far planes (low BG3/BG4, parallax backgrounds) are behind the diorama, hazed and
  blurred most; in 3D they are the backdrop behind the map, not dropped.
- Overlays the console blends over the scene (fog, light shafts, clouds, rain, snow,
  darkness masks, spotlights) are part of the picture. In 3D they are drawn over the
  scene with the console's blend, or the scene falls back to 2D. Silently losing them
  breaks rule 1 (a dark cave drawn fully lit is a bug, not a limit).
- World map clouds: a layer above the ground with soft shadows that drift with it;
  thinner over the party. No hard rectangular edge of the cloud plane, ever.

### Transitions

- Fades: the 3D picture follows the console's brightness every frame; a fade to
  black reaches black on the same frame as the console.
- Entering a verified 3D map eases in over 8 frames. A missing, inconclusive or
  mismatched scene switches straight to 2D; the renderer never fades out a stale 3D
  frame. No transition may show the wrong map, a grid of garbage tiles, a half-loaded
  tilemap or a frame of void.
- **The scene gate.** A 3D frame is drawn only when the scene is consistent with what
  the console is showing: the map's tiles are loaded, the camera is inside the map, at
  least one layer the console has enabled is in the scene, and the screen is not
  blanked. Otherwise: the 2D path. When in doubt, 2D.
- Screen effects the console does with windows, mosaic or color math (iris, flashes,
  mosaic dissolves, wipes) appear in 3D, or the scene falls back to 2D for their
  duration.
- No frame anywhere shows uninitialised memory, a stale target from another preset,
  or a different picture size than the frame before (no one-frame jumps on preset or
  aspect change).

### Frame, aspect, side fill

- The picture is 4:3 (or 8:7, or stretched) at any window size; UI text is legible
  down to 640x480 and at odd sizes (1038x556, 1280x800, 1920x1200).
- Side fill is dim (under 45% of the picture), blurred beyond recognition, shows no
  seam at the picture's edge and no UI, text or 3D void in it. In menus and on
  black-backed screens it is black.

## Acceptance checks

Each is something to verify on a screenshot (`--shot`), most against the same frame
with `--preset off`. "Console px" is one source pixel; B is its size in output pixels.
A branch touching the renderer should pass all P0 checks in every preset it can
affect, at 1920x1080 and at one odd size (1038x556).

### P0: correctness (a failure is a bug)

| # | Check |
| --- | --- |
| U1 | UI pixels identical to the plain picture: for every pixel the layer classifier (or the module) marks UI, output color equals `--preset off` at the same frame, same size (max channel difference 1/255, grain excluded). Applies to text boxes, menus, HUD, cursors. |
| U2 | No frame with a blank text box, and no text without its box: whenever `--preset off` shows a box with glyphs, the same glyph pixels are present inside a box in every preset (console glyphs or HD text). |
| U3 | Full-screen menus (pause menu, shop, name entry, save/load, config) are pixel-equal to `--preset off` over the whole picture apart from grade-free scaling: no blur, bloom or vignette gradient across them. Debug view 3 (blur amount) is black on them. |
| U4 | HD text: every line's baseline within 1 console px of the console glyph row; no console glyph pixel left under it; nothing drawn outside the box; with no font, output is bit-identical to a build without HD text. |
| S1 | No garbage: no 3D frame whose floor is a repeating grid of one or two tiles, uninitialised tiles, or a map other than the one `--preset off` shows. Check the 10 frames either side of every map change, battle start/end and menu open/close. |
| S2 | Flat-camera parity: with `CT_HD2D_3D=tilt=0,zoom=1`, floor and sprite pixels of the 3D picture are within 1 console px of their place in the `--preset off` picture (lighting aside). This proves the scene is the console's. |
| S3 | At the default camera, a sprite's foot pixel is within 1 console px of where the floor under it projects, every sprite pixel is the same size (B rounded, ±0 between pixels of one sprite), and sprite height/width on screen is within 5% of 1.0 in console proportions. |
| S4 | Nothing lost: every sprite visible in `--preset off` is visible in 3D (count OAM-covered regions), unless the console hides it behind a higher-priority tile. Every enabled blended overlay (fog, darkness, weather) is visible or the frame is 2D. |
| S5 | No sprite piece seams: a multi-piece sprite has no 1-px gap, overlap or offset between pieces at any size. |
| S6 | No void in play: when the map is larger than the view, no void pixel is visible. When smaller, the map covers at least 50% of the picture's height, is centered within 5%, and nothing but void surrounds it. |
| S7 | No z-fighting or cracks: no single-pixel lines of void or of a lower layer between adjacent tiles, at 100% and 67% render scale, at 1920x1080 and 1038x556. |
| T1 | Fades: mean luminance of the picture divided by the console frame's mean luminance stays within ±10% of its mid-scene value throughout a fade; the last frame of a fade-out is black (max 2/255). |
| T2 | Crossfades: across a 3D/2D switch, each frame is a blend of two frames that each pass S1; no single frame differs from both its neighbours by more than they differ from each other (no flash). |
| F1 | Picture rectangle: aspect within 0.5% of the chosen one at every window size; nothing drawn outside it but side fill; side fill contains no UI. |
| F2 | Blacks: a console-black pixel is at most 2/255 on output (4/255 with grain) in every preset. |
| P1 | Presets are independent: a screenshot in preset A is identical whether or not presets B, C were used earlier in the run (no stale targets, uniforms or eased state). |
| P2 | GL and GLES give the same picture (`CT_HD2D_GLES=1`): max difference 2/255 away from edges; `CT_HD2D_GLERR=1` reports nothing. |

### P1: the look (a failure is a defect against the target)

| # | Check |
| --- | --- |
| L1 | The player is sharp: the blur amount (debug view 3) is 0 over the player's sprite and the floor row at its feet. |
| L2 | The top and bottom 10% of a map view are visibly out of focus in `hd2d`; in 3D the far edge is, the near edge at most half as much. |
| L3 | Actors anywhere: blur amount over any sprite is at most 0.3. |
| L4 | No DOF halos: across an in-focus/blurred edge, no overshoot of either side's color more than 2 px into the other. |
| L5 | Bloom only from lights: with bloom on vs off, a daylight exterior's mean luminance changes by under 4%; no white text, UI or clothing has a glow. |
| L6 | The sunlit floor at the focus keeps its color: hue within 8 degrees and luminance within +10/-5% of `--preset off` (grade included). |
| L7 | Wall shading: lit south walls 8-15% darker than adjacent sunlit floor of the same tile color; no wall darker than 35%. |
| L8 | Every grounded actor has a shadow that touches its feet (gap at most 1 console px), 30-40% dark, soft edge 1-2 console px. |
| L9 | AO: visible at wall/floor joins, max 35% dark, no outline around sprites, no dither pattern at 100% zoom of the screenshot. |
| L10 | Pixel edges are hard: across any art edge at the focus, the transition is at most 1 output px wide (100% scale) or 2 (67% with FSR); no ringing brighter than either side by more than 6%. |
| L11 | Dark scenes are dark: an interior or night scene's mean luminance is within ±15% of `--preset off`'s; lamps and windows are its brightest regions. |
| L12 | Haze: at most 25% toward the fog color at the far edge, 0 at the player's row. |
| L13 | Vignette: corners 25-35% darker than center on a flat grey scene, no visible ring. |
| L14 | Water glints: under 1% of water pixels at once; none on land; aligned to console px. |
| L15 | Cloud layer: no straight edge of the cloud plane visible; cloud shadows move with the clouds. |
| L16 | Scrolling: stepping the camera one console px at a time, the floor texture moves by exactly B output px each step (no shimmer, no swim of shadows or AO). |

### Performance checks

- `hd2d` under 1.5 ms, `hd2d3d` under 3.0 ms, `deck3d` under 2.5 ms median GPU per frame
  at 1080p on the Ally (`CT_HD2D_PROFILE`); no fix may add more than 0.1 ms without
  saying so. Map changes: no frame over 16 ms from mesh rebuilds after the fade-in.

## Improvements, in priority order

Toward the DQ3 HD-2D look, within the constraints. P0 items are correctness the look
depends on; nothing below them is worth doing while they fail.

**P0: the look must not break the game**

1. **Text boxes in 3D keep their box.** The dialogue window is on a layer the 3D scene
   replaces, so text floats bare over the scene (HD2D.md says so). Fix generically: the
   module names the window's region (a `ct_scene` UI rectangle list, or a "this BG
   region is UI" flag), and ui3d draws those console pixels flat, with the console's
   translucency. Same for any UI on BG1/BG2 or sprites (cursors, name tags).
2. **Full-screen menus are UI** whatever layer they are on. A screen where the module
   says "menu" (or where no map view or battle is running and the frame is mostly
   opaque BG with glyph-like tiles) gets no DOF, bloom or vignette. Name entry is
   blurred today.
3. **The scene gate** (see Transitions): no 3D frame from a map that isn't on screen.
   Implemented as a 6x6 sample of visible BG1/BG2 pixels checked against the module's
   tilemap, VRAM and CGRAM at the console camera coordinates. Fewer than six comparable
   samples or more than 25% color mismatches sends the frame directly to 2D.
4. **Small maps are framed.** Zoom to fit, hide map regions outside the room the
   camera is in (the module can give a visible rectangle; the engine can also flood
   from the player's cell across non-void cells), neutral void.
5. **Blended overlays and translucent sprites** drawn in 3D with the console's blend,
   or 2D fallback while one is on. Darkness and fog are gameplay information.

**P1: the diorama**

6. **Snap field billboards and choose an integer zoom** (rule 3). Implemented: the
   field camera snaps its focus-row pixel density to an integer at the current render
   width, and upright billboards use integer target-pixel scales with snapped corners.
7. **Light by time and place.** Derive the key light's strength and color from the
   scene (the 2D path's darkness measure; the palette's mean hue): strong warm sun
   outdoors, weak cool fill indoors with lamps and windows doing the work, as the
   remakes do. Place point lights in the world (unproject the emit map onto the floor
   and walls) so they light walls beside them and cast sprite shadows away from them.
8. **Walls that read as walls.** Per-map height hints from the game module (the
   3dSen-profile idea in PRIOR-ART.md): wall height per region, the two walk planes
   separated, stairs sloped. Removes facades lying on roofs, the biggest geometry
   tell. Engine stays generic: the hint is a height per cell.
9. **Sprite grounding.** A small soft blob under each actor in addition to the shadow
   map (the remakes do both), a rim light on the edge facing the key, and a fake
   normal bulge (PRIOR-ART A2) so characters aren't paper.
10. **Far planes as a backdrop** behind the map with haze and the most blur, instead
    of void: parallax backgrounds (sky, distant mountains) are what makes HD-2D
    exteriors read as places.

**P2: polish**

11. **Field water:** swell and glints on water cells, a fake reflection (the scene
    mirrored, blurred, 15%) on still water.
12. **DOF quality:** near-field capped at half, a disc-shaped kernel at radii over
    4 px so lights become soft discs (the remakes' bokeh), still weighted to avoid
    halos.
13. **Atmosphere:** very light dust motes or light shafts through windows in
    interiors, generated procedurally on the console pixel grid, off by default on
    `deck`; height fog in exteriors' far third.
14. **Bloom with a warm tint and a light anamorphic stretch** around point lights
    only.
15. **Packs:** per-tile height, normal and emissive maps (data only) so a personal
    pack can supply what inference cannot; documented in ASSET-PACKS.md when built.

What not to do: more saturation, chromatic aberration, film-heavy grain, CRT effects
inside these presets, smooth-filtered (xBR-like) sprites in the default look, a wider
FOV, or camera motion the game didn't ask for (sway, shake of our own).
