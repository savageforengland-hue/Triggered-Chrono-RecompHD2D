<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Visual QA of the 3D diorama in the field (`hd2d3d`, `deck3d`)

A pass over the start of the game after the report "had visual bugs" (ROG Ally,
radeonsi, window 1038x556, quit at frame 4690), and then "bright boxes in a grid-like
pattern when first hearing the narrator speak to Crono".

**How:** `leene_square.txt` from the start to frame 9200, `ct_sdl_engine --module`,
`SDL_VIDEODRIVER=offscreen`, private XDG dirs, `--shot F:PATH --shot-size WxH`, in
`hd2d3d` at 1038x556 and 1280x800 and in `deck3d` at 1920x1080, each frame against the
same frame in `off`. Frames every 100-300 over 2000-7320, densely over 3400-5000 (the
narrator, Crono's room, Mom's dialogue), and 8700 and 9200 (Leene Square, the bell).
The screenshots are ROM-derived: they stay in the build dir (`build/qa/`), never in
git.

## Bugs

| # | Frames | Look, size | What | Cause | Status |
| --- | --- | --- | --- | --- | --- |
| Q1 | 3560-4250 | `hd2d3d`, `deck3d`, every size | **The user's "bright boxes in a grid".** The narrator ("Crono...") speaks on a black screen; the 3D picture shows a lit floor of repeated yellow and grey tiles under the text box. | The module reports a field map view (map $10F, 16x16 cells) while the console has BG1 and BG2 off on both screens (TM = $10, TS = $00): the 3D path meshed a map the console doesn't show. | Fixed by `qa2d` in `hd2d.c` (`want3`: no 3D while BG1 and BG2 are off on both screens); this branch checked the same cause, with a fix of its own that was dropped so the two don't conflict. |
| Q2 | 4290-5460, 5930-6990, every text box in a map view | `hd2d3d`, `deck3d`, every size | **The text box window is missing**: the text floats over the room without its box (the 2D looks show the box). Hard to read over bright floors. | The window is drawn on BG1, in the console's tilemap only. The scene's BG1 is the map as the game keeps it in WRAM, so the 3D scene never had the box, and the UI pass laid only BG3's top level (the text) flat. | **Fixed.** `s3d_box_mask` (`hd2d.c`) compares, per 8x8 BG1 tile on the screen, the console's BG1 pixels with the scene's tile decoded from VRAM and CGRAM; runs of at least 16 differing tiles in a row, within 10 tile rows of UI text, are a box, and `ui3d.frag` lays the console's pixels there flat with the UI. Regression: `sdl_3d_box` (frame 4300: the box's middle in `hd2d3d` within 12 levels of `off`, grey-blue). |
| Q3 | 4290-5500 | `hd2d3d`, `deck3d` | Crono asleep in bed stands upright over the bed, and a dark grey rectangle replaces the blanket. | The bed's cells are blocked, so they rise as a box; the sleeping pose is a billboard standing at the object's foot point on top of it. The grey rectangle is probably the blanket's tiles (or a part of the pose) at the wall's face, seen from the tilt. Not investigated further. | Open |
| Q4 | 4000-7000 (the house) | `hd2d3d`, `deck3d` | In the small rooms of the house (one 64x64 map holding both floors), the room sits in the upper part of the picture with a big dark void below it, and pieces of the other floor show at the picture's left edge and bottom. | The camera stopped at the bounds of every cell with pixels in the whole map, including rooms that the console never showed together. `scene3d.c` now floods the walkable component(s) visible to the console camera, retains two cells of nearby walls and roof geometry, builds and bounds only that active room, and requests a rebuild when the console enters another room. Small rooms also receive a conservative zoom-to-fit, capped at 1.25x. | **Fixed in code.** `test_scene3d` covers two disconnected rooms, the empty transition between them, the rebuild at the second room, active-room bounds and zoom. ROM-derived screenshots still need regenerating on a ROM-enabled machine. |
| Q5 | 2000-3500 (the opening flyover) | `hd2d3d`, `deck3d` | The world map in the opening shows a different place than the console (more ocean, south of the fair). | The world camera centers on the player object; in the attract flyover the console's camera pans on its own, away from it. | Open |
| Q6 | about 6100, 6500 | `hd2d3d`, `deck3d` | The text box's opening animation (a thin grey bar before the text) isn't drawn in 3D. | Q2's box needs UI text near it, so the void below a map isn't taken for a box; the bar has no text yet. A few frames. | Open (minor) |

Looked at and fine: Leene Square (8700) and the bell plaza (9200) at all three sizes;
the 1038x556 window (a 4:3 picture, 741 px wide, with the side fill, as in the 2D
looks); the fade-in at 4200-4300 (the 3D picture is scaled by the console's
brightness, as the 2D is); `deck3d` at 67% render scale (no FSR or AO artefacts seen
at these frames). Not covered: a run of consecutive frames for flicker, door
transitions frame by frame.

## Screenshots (in the build dir, not committed)

- Q1 before: `build/qa/a_1038/03850.png` (`hd2d3d`), reference `build/qa/a_off/03850.png`.
- Q2 before: `build/qa/b_1038/04300.png`; after: `build/qa/f_1038/04300.png`,
  `build/qa/f_1038/05950.png`, `build/qa/g_1280/04300.png`,
  `build/qa/g_1920/04300.png` (`deck3d`); reference `build/qa/f_off/04300.png`.
- Q3, Q4 before: `build/qa/f_1038/04690.png`; reference `build/qa/f_off/04690.png`.
- Q5: `build/qa/a_1038/03000.png`; reference `build/qa/a_off/03000.png`.
