<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Cutscene adaptation

This is the first renderer-facing cutscene stage for the native PS5 HD-2D
line. It preserves authored timing and composition; it does not replace event
scripts or invent camera movement.

## Implemented foundation

`ct_scene.flags` (module ABI 11) carries two generic facts:

- `CT_SCENE_SCRIPTED`: the game controls the shot. The renderer follows
  `camera_x/camera_y` but ignores live camera offsets (currently gyro) until control
  returns to the player.
- `CT_SCENE_EXACT_2D`: the scene uses presentation not represented by
  `ct_scene`. The console composite is used for the whole frame.

Chrono Trigger marks field event control from WRAM `$0138`. On the world map,
the normal party foot is at screen `(128,112)`; a different party/camera
relation or `$0138` marks an authored pan. This classification does **not**
force ordinary field dialogue or event scenes to 2D: they may stay HD-2D when
the existing console-comparison gate verifies them.

The separate cutscene engine is recognized through its `$C30548`, `$C30529`
and `$C308C9` NMI vectors. It stays exact 2D because its handlers can mix Mode
7, Mode 1, HDMA matrices and overlays inside one sequence. This covers the
bike race and Epoch cockpit without flipping between 2D and 3D mid-shot.

Scene/map cuts restart the existing eight-frame 2D-to-3D crossfade. Script
boundaries, map-generation changes and camera jumps over 32 pixels restart a
12-frame smooth aperture ease; normal camera pans keep their focus and follow
the console scroll exactly. A mismatch, UI or exact-2D scene still falls back
to the console frame immediately.

## Deliberate limits

- There is no HD Mode 7 reconstruction yet. Its per-scanline matrix and HDMA
  state need a dedicated scene description and ROM/hardware comparison.
- Event scripts, dialogue timing, fades and MSU-1 playback are unchanged.
- The flags describe presentation context; `CT_SCENE_SCRIPTED` is never a
  blanket request to disable HD-2D.
- Hardware acceptance remains pending until a native PS5 title can be rebuilt
  with the pinned native-app kit and tested on the console.

## ROM-free regression coverage

- All three special-cutscene NMI vectors select scripted, exact 2D output.
- Field event control sets and clears the scripted flag.
- World-map free movement, independent flyover pans, event control and wrap
  are distinguished with synthetic WRAM/OAM.
- Scripted shots suppress live camera offsets; normal shots retain them.
- Focus starts sharp, reaches the preset in 12 frames, keeps smooth pans,
  resets on a camera cut and clears on a 2D fallback.

Hardware acceptance should cover the title/opening, Crono's bedroom, dialogue
opening, room changes, world flyover, battle return, bike race, Epoch cockpit
and a long MSU-1 session.
