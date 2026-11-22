<!-- SPDX-FileCopyrightText: 2026 jerbanpls
     SPDX-License-Identifier: GPL-3.0-or-later -->
# QA: the world map, battles and mode 7 (hd2d3d / deck3d)

Visual QA of the world map diorama, battles and mode 7 scenes, and the transitions
between them. Screenshots are ROM-derived and live in the QA build dir
(`build-qa/shots/`, not committed). All shots at 1038x556 (the Ally window) unless
noted; `off` is the reference at the same frame.

## What was covered

| Route | Frames | Looks | Result |
| --- | --- | --- | --- |
| `world_map_save.txt` | 7300-8700 every 100, 7704-7800 every 4 | hd2d3d, deck3d, hd2d, off | looked at, see W1-W5 |
| mode 7 race (`--poke-wram 59:12061^2`) and Epoch flight (`^4`) | 200-300 | hd2d3d, off | see W8 |
| `gato_win.txt`, `gato_battle.txt` | 4000-14000 | hd2d3d, off | **did not reach a battle** (W6) |
| other eras (`--poke-wram 7253:100^N`) | 7500 | hd2d3d, off | **poke missed, all showed 1000 A.D.** (W6) |
| 1280x800, 1920x1080, 5-frame flicker runs, FSR 67% | - | - | not done (time) |

## Bug list

| # | Frame / route | Look, size | Shot (build-qa/shots/) | Description | Likely cause | Status |
| --- | --- | --- | --- | --- | --- | --- |
| W1 | 1000 A.D. map, 7500-7700, `world_map_save` | hd2d3d 1038x556 | `w_3d_1038/f7500.png`, `f7700.png` | None found: terrain, water, clouds, castle, party and caption look right; the mountains and forest read well, the ferry stands on the water | - | OK |
| W2 | Pause-menu entry, 7704-7748 | hd2d3d | `tr_3d/`, `tr_sheet.png` | The fade-out to black (7720-7744) and the menu fade-in match the console's own (same black frames in `off`, `tr_off/`). The black frame is the game's, not a renderer fault. After it the 3D look settles to the final brightness over ~40 frames (mean 0.3081 vs the console's flat 0.3179), a slow crossfade, not a glitch | `S3D_FADE_STEP` easing | not a bug; noted |
| W3 | Pause menu / save menu, 7750-8100 | hd2d3d 1038x556 | `w_3d_1038/f7800.png`, `f7900.png`, `rep/f7800.png` | The menu is drawn by the 2D path *with the tilt-shift depth of field*: the panels' top and bottom edges and the top-left stats box are blurred and the text there fades, as if out of focus. `off` (`w_off_1038/f7800.png`) is sharp | 2D path's DoF (`coc.frag`/`dofblur`) is applied to full-screen menu pictures; the focus band is meant for a map | **Fixed in code:** the module reports `CT_SCENE_UI` for `$C2840E` and the renderer uses a plain, effect-free frame. Visual shots should be regenerated with the ROM. |
| W4 | World map, 7400/7600 | hd2d (2D) | `w_hd2d_1038/f7400.png`, `h_crop.png` | Thin green outlines on the Truce/Porre town roofs and walls, and a green line along the cover edge of the buildings (coordinator's finding, confirmed) | top-down world-map buildings were tilted as a foreground plane, revealing strips of the green behind buffer | **Fixed in code:** 2D `CT_SCENE_WORLD` frames disable tilt and foreground/depth DoF. Visual shots should be regenerated with the ROM. |
| W5 | Brightness / forced blank on the world map, fade-in 7300 | hd2d3d | `w_3d_1038/f7300.png` | The map follows the console's fade (dim map at 7300, black 7720-7744, bright again): `uDepth.z` carries `sc->brightness`. TM layer enables on the world 3D path not exercised | - | brightness OK; TM left to qa3d |
| W6 | Replay routes in `ct_sdl_engine` | all | `scan8.png`, `b_scan.png`, `b_scan2.png`, `b_rt.png`, `probe.png` | In `ct_sdl_engine` the routes do not run on the frame numbers of the docs (`ct_boot` timeline): `world_map_save` is already on the 1000 A.D. map from frame 2500, and `gato_win`/`gato_battle` show the world map from 4000 and Crono's house with the "That brat, Lucca" text from 8500 to 14000, never a battle. Two runs of the same route also differ (md5 of the frame 7800 shot) by a few frames | unknown: the SDL frontend's frame timing against the script (QOL, resume, pacing) differs from `ct_boot`; the mode 7 poke at frame 59 does work, which fits a divergence that starts later | open: battle fallback, battle crossfade and the other eras were **not** visually QA'd. Use `ct_boot --save-state` / `savestate_*` tests or find the SDL divergence first |
| W7 | Other eras (`$1F1`-`$1F7`) | hd2d3d | `era1.png`-`era7.png` (all 1000 A.D.) | Not reached (W6). The data in docs/SCENE.md W13 predicts three problems: `$1F2` has no blue ("sea" is a dark void tile: should not shimmer), `$1F5` is a flat sky with a full-map overlay (nothing to lift), `$1F7` is a 37x35-metatile map in a blank 96x64 grid (the 3D plain would be the size of the grid) | height heuristic keyed on blue/green/red means | open; recommended: fall back to 2D for `$1F5` and `$1F7` and tune `$1F2`'s sea to a flat dark plain once seen |
| W8 | Mode 7 race (frames 200, 260) and Epoch cockpit (200, 300), `--poke-wram 59:12061^2` / `^4` | hd2d3d 1038x556 vs off | `m7sheet.png` (`m7r_3d/`, `m7e_3d/`, `m7e_off/`) | Both scenes draw (2D path), nothing missing. The race: dark road with the horizon blurred by the depth of field and the sky glowing (bloom), acceptable. The Epoch cockpit: the sky is washed out, nearly white at frame 200 and the cloud/fire colors at 300 are paler and haloed against `off` (peach and yellow bands): the bloom and grade over-expose a bright mode 7 picture | 2D path bloom/grade tuned for the dark maps; mode 7's wide bright sky exceeds the threshold | **Fixed in code:** mode 7 follows the correctness fallback and bypasses DoF, bloom and grade. Visual shots should be regenerated with the ROM. |

## Research options added

`ct_sdl_engine` had no way to poke WRAM like `ct_boot`. Two options were added
(frontend/main.c, `port:`), for QA runs only:

- `--poke-wram F:ADDR^MASK` as `ct_boot`'s: XOR a byte at frame F's edge.
- `--set-wram F1-F2:ADDR=V`: hold a byte at every edge from F1 to F2.

`--poke-wram 59:12061^2` reaches the mode 7 race in `ct_sdl_engine`; the era poke
(`7253:100^N`) needs the SDL timeline of W6 first.

## Tests run

`ctest -R "scene3d|scene_decode|savestate_world_map|mode7_race|battle_no_flicker|lockstep_world_map_save"`
(all pass), `tools/check_agnostic.py` (0 hits), `reuse lint` (compliant).
