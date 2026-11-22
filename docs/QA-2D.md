<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# QA: the flat looks and the screen overlays

This pass checks what HD-2D draws around the picture and over it: the `off`, `subtle`, `hd2d` and
`deck` looks, the HD text, the menu and the messages, the picture's shape and the window. The
3D field look (scene3d) and the world map and battles had their own passes. It was run on
2026-11-14 on a ROG Ally (radeonsi, GL 4.6), headless (SDL's offscreen driver), over the Leene
Square replay (`game/ct/test/replay/leene_square.txt`).

The shots are in the build folder, not the repo (they show the game's pictures). Their paths
are relative to `build-qa/qa/`. They were made by a script, `run.sh`, that runs
`ct_sdl_engine --fast --mute` with its own settings folder and no controllers.

## Bugs

| # | Frames | Look | Size | Shots | What | Cause | Status |
|---|---|---|---|---|---|---|---|
| 1 | 3560-4250 (the narrator: "Crono... Good morning, Crono!") | `hd2d3d`, `deck3d` | any (reported at 1038x556) | before: `sheet_h3d_narr.png`, `h3d_1038/f03700.png`; after: `sheet_h3d_narr_fix.png` | A bright grid of tiles in 3D fills the screen. The narrator's box is missing. The console shows black with the box. **This is the user's report.** | The house's map is already loaded, so the module reports a field scene. But BG1 and BG2 are off on both screens (TM $10, TS $00), and the 3D path meshed the loaded map anyway. | **Fixed** (`hd2d_render`: no 3D while BG1 and BG2 are off on both screens). Regression: `sdl_hidden_map_3d` |
| 2 | any | `subtle`, `hd2d`, `off` (side fill on) | tall windows (800x1280) | before: `tall/f04400.png`; after: `tall_fix/f08850.png`, `tall_fix_s.png` | The bars above and below the 4:3 picture are the picture's edge row, smeared down the whole bar at full strength. A bright top edge (the fair) gives bright vertical streaks. | `final.frag`'s side fill mirrored and faded only in x. Rows above and below were clamped to the edge, with no fade. | **Fixed**: it mirrors and fades in y as well. Regression: `sdl_side_fill_tall` (the top rows measured 53.4 against 53.4 by the picture before; now much darker) |
| 3 | 1650-1950, 4650-4700 (name entry) | `subtle`, `hd2d`, `deck` | any | `cmp_1800.png` (top: hd2d, bottom: off), `dbg_1800.png` (debug views 1 and 3) | The letter grid under the hint box is depth-of-field blurred until the letters can't be read. UI must stay sharp. | `analyze()` counts UI only as BG3 with priority in mode 1. The name entry draws its boxes on BG1/BG2 at z >= 4, so they count as ground (the green in debug view 1). `field_amt` goes to 1, and the tilt-shift blurs the lower half. | **Fixed in code:** ABI 10 adds `CT_SCENE_UI`; the game's `$C2840E` menu NMI selects it and `hd2d_render` disables all effects. `scene_decode` now checks name entry frame 1800. Visual shots should be regenerated with the ROM. |
| 4 | 1800 (the hint "Enter a name, then push the Start Button.") | `hd2d` (not `off`) | any | `cmp_1800.png` | Small console-glyph marks show under the HD text (under "name", "then", "push", "Button"). | The diorama tilt enlarges the foreground (the hint box) and reveals the behind G-buffer. That buffer holds the letter row K-T hidden under the box (visible in `dbg_1800.png`, the blur view). HD text takes the console's glyphs out of the front picture only. | **Fixed with 3:** UI frames have no tilt or behind-buffer reveal. Visual shots should be regenerated with the ROM. |
| 5 | 4300-4800 (Mom's dialogue) | `hd2d3d` | 1038x556 | `sheet_h3d_narr_fix.png` (last rows) | The text shows but the box behind it is missing. The 2D looks show the box. | The 3D side (the UI layer over the 3D render). | **Open, for qa3d** |
| 6 | all | all | the user's 1038x556 | (log only) | The picture is soft on the user's desktop, and the window size is odd. `settings.ini` has `window=1920x1029`, but the drawable was 1038x556. | 1920/1038 = 1029/556 = 1.85, so the desktop is scaled to 185%. SDL2 (the X11 driver under XWayland) is given logical pixels, and the compositor scales the 1038x556 picture up. 1920x1029 was a maximized window saved at 100%. Restore itself works: `window=1038x556` comes back exactly (`menu_gl`, `menu_plain`). The saved `aspect=2` (stretch) also widens the 2D views to 1.87:1, which is that setting working as designed. | **Not a bug in the restore. Open as a platform issue.** `SDL_VIDEODRIVER=wayland`, or KDE's "Legacy applications: apply scaling themselves", gives full-resolution pixels. Whether the app should prefer Wayland is a separate decision. |

## Checked and found right

- `off` and `hd2d` at 1038x556, every 150 frames from 150 to 9200 (`sheet_off_1038.png`,
  `sheet_hd2d_1038.png`): no misplaced or stretched picture, no seams. The side fill at the
  sides is soft and dim, and fades are the same as in `off`, apart from bugs 3 and 4.
- `hd2d` at 2560x1080 (`wide_s.png`): the side fill is soft and dimmed, with no seams.
- `subtle` and `deck` at 1280x800, every 150 frames (`sheet_subtle_1280.png`,
  `sheet_deck_1280.png`): the deck look's black bars at 16:10, as designed. Bug 3 shows in both.
- The pixel aspect (8:7) at 1024x768 with HD text, every 50 frames from 3000 to 5000 and frames
  4400-4407 one after another (`sheet_pix_1024.png`): no flicker. The HD text sits in its
  boxes, and the fades match.
- The settings menu over the picture at 1038x556, restored from `settings.ini`, through both
  the GL and SDL_Renderer paths (`menu_both.png`): laid out, readable, not clipped.

## Not covered in this pass

Integer scale and CRT (the original picture), asset packs in 2D, the `--pad-test` checklist,
and 1920x1080. These were not shot, for lack of time.
