<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Engine and game module

The C this project compiles is translated from the ROM, so any program that contains it
also contains code derived from the game. To keep that code out of anything distributed,
the build is split in two:

| Part | What it is | Can be distributed? |
| --- | --- | --- |
| **Engine:** `ct_sdl_engine`, `ct_boot_engine` | runtime, scheduler, interpreter, frontend, HD-2D renderer, the translator scripts | yes (GPL-3.0-or-later, plus the MIT and ISC parts) |
| **Game module:** `ct_game.so` | the generated code, built on the user's machine from their own ROM | **no:** it never leaves that machine |

The engine reads its function tables through `ct_tables_cur` (`runtime/func_table.h`).
A module replaces them when it loads (`runtime/module.h`). Without a module the engine
runs every function in the interpreter. That's slower, but it measured about 290 fps
headless on a Ryzen Z1 Extreme, so it still works as a fallback.

## Building a module

With CMake, as a developer:

```
CT_ROM=rom/your_dump.sfc cmake -S . -B build && cmake --build build -j
build/ct_sdl_engine --module build/ct_game.so --hd2d
```

Without CMake, which is what an installed engine will run on first launch:

```
python3 tools/build_module.py --game game/ct --rom rom/your_dump.sfc \
    --out ~/.local/share/ct-recomp --native
```

The script does five things:
1. checks the ROM's hash
2. decodes the overlays
3. translates the game to C
4. compiles the C in parallel and links `ct_game.so`
5. deletes the generated C

It took about 40 s on a Ryzen Z1 Extreme. `--native` tunes the code for the local CPU.

## Code in RAM (overlays)

A game that decompresses code into RAM is handled in four steps. `game/<id>/overlays.toml`
lists each blob (source pointer, size, destination, FNV-1a hash and the entry points with
their M/X) and never holds bytes; `tools/find_overlays.py` writes it from profiles of
the replays. The build decompresses each blob from the user's ROM (`overlay_images.py`),
checks the hash, and the translator decodes the entries from that image. Each compiled
function carries the hash of its byte extent. At run time (`runtime/overlay.c`) a call to
an entry runs the native code only while the RAM bytes still hash the same; otherwise the
interpreter runs it. Code that patches itself is therefore always correct, and the
interpreter is the defined exception for it. A stub the game builds in RAM
byte by byte is an `[[extern]]` hook that reads the bytes at run time (`Wram_MvnStub` in
the CT module).

Entries that join a save/restore sequence in the middle, or calls through a table of
unknown length, can come back with an M/X the decoder cannot know. The translator then
decodes the continuation for each of the four M/X pairs that decode and the generated code
picks one from the live flags (`Function.plp_interp` after a `PLP`, `Function.dyn_alts`
after a `JSR (abs,X)`). For a call the extra pairs are only kept when they end the way the
assumed pair does, because callers know one set of exits. Pairs that do not decode, or end
differently, stay with the interpreter's dispatch.

## Game patches

A game's generated code can't be edited by hand, so a game changes its behaviour through
patches. In `funcs.toml`, a `[[func]]` with `patch = "NAME"` makes the translator call
`int ct_patch_NAME(CPU *)` on entry to that function. The patch lives in the game's own
C (`game/<id>/*.c`, compiled into the module with the generated code). If it returns 0,
the translated body runs as usual. If it returns nonzero, it has done the work and the
return (RTS/RTL) itself. The interpreter never calls patches, so a patch only runs where
the function runs natively. Co-op is built this way (`game/ct/coop.c`).

The patches read state the engine gives them, through `sched.h`:

- how many players there are (`sched_players`);
- who has each party slot (`sched_slot_player`);
- each port's last auto-read (`sched_joypad_read`);
- whether a second view is allowed (`sched_views`), and the second view itself
  (`sched_view2`), which the frontend draws with `sched_render_view2`.

With one player, every co-op patch returns 0 without touching memory. Frame hashes then
match a build without the patches: the gato_win replay's 13,950 per-frame hash lines are
identical. Module ABI 5 added the patches and that API.

The player's preferences come through `sched.h` too (`sched_pref`): the always-run patches
(`game/ct/always_run.c`, on the eight field direction routines) read
`SCHED_PREF_ALWAYS_RUN`, and return 0 when it is off. Module ABI 6 added that API: a
module with those patches needs it from the engine, so an older module is rebuilt.

Simulated rumble (`runtime/rumble.h`) is the other way round: patches tell the engine
something. They call `rumble_event(low, high, ms)` or `rumble_event_for(player, ...)`,
which only queue an event (nothing in the machine changes), and the module may export a
frame hook, `ct_module_info.rumble`, called at each frame edge while rumble is on, that
raises events of its own and says whether the game is in a menu or other quiet screen, so
the engine's generic detectors (screen shake, flashes, low booms in the sound) keep still
there. `game/ct/rumble.c` patches the battle's damage routine, two event commands and the
time gate (docs/SCENE.md, "Rumble"); each returns 0. Module ABI 8 added that API and
export. The gato_win replay's frame hashes are the same with rumble on and off.

Module ABI 9 added two more optional exports, for the frontend's add-ons, in
`ct_module_info` after the rumble hook: `music` (`ct_music_hooks`: the engine passes every CPU
write to an APU port to it, and it gives song, fade and stop events in the game's song
ids at each frame edge, plus the APU RAM byte naming the sound-effect voices; audio
packs, [ASSET-PACKS.md](ASSET-PACKS.md#music-audio-packs)) and `text` (`ct_text`: the
characters in each line of the game's text boxes, and the VRAM characters of the lines,
which the engine finds on the screen; HD text, [USER-GUIDE.md](USER-GUIDE.md#hd-text)).
The text comes from a patch on the game's glyph routine (`game/ct/text.c`) that only
reads memory, so hashes match a build without it. Both are only read while the player
uses them. Save states from ABI 7 and 8 builds still load (`SAVESTATE_ABI_MIN`): the
exports change nothing a state holds.

Module ABI 10 adds `CT_SCENE_UI`, a scene kind with no map payload for full-screen
menus, name entry, shops and save/load screens. It lets renderers keep those frames
plain and sharp without also disabling the 2D effects used by battles and titles.
The Chrono Trigger module identifies it by the menu NMI vector (`$C2840E`). This is a
semantic scene-interface change only; save states from ABI 7 onward still load.

## Guarantees

- `module_parity` (ctest, needs the ROM): the engine plus the module matches the static
  build frame for frame. It compares CPU, memory, video and audio hashes over 3,000
  frames of the Leene Square replay. It has also been verified over all 9,200 frames,
  for both the CMake-built and the script-built `-march=native` module.
- The module exports `ct_module_get`, and optionally `ct_module_pose` (a field object's
  whole frame for asset packs, runtime/module.h; found by name, so an engine and a module
  without it still fit, and the ABI is unchanged). The loader checks the module ABI and the
  struct layouts before installing the tables.
- `ct_boot_engine` contains no generated functions (checked with
  `nm build/ct_boot_engine | grep ' f_'`).

## First run

`ct_sdl_engine` started without `--module` sets itself up (`frontend/firstrun.c`):

1. **The ROM:** `--rom FILE`, then `$CT_ROM`, then the last ROM that passed, else
   it waits for a file dropped on the window. Each candidate is checked against
   the size and `rom_sha1` in `game/<id>/game.toml`. A wrong file is named on
   the screen, with a hint if it has a copier header.
2. **The module:** `ct_game.so` in the per-game preferences folder
   (`SDL_GetPrefPath("ct-recomp", <id>)`, so `~/.local/share/ct-recomp/<id>/` on
   Linux). `ct_game.stamp` beside it records the ROM's SHA-1 and the module ABI it
   was built for. If the module is missing, or the stamp doesn't match this ROM
   and this engine, the engine runs `tools/build_module.py ... --native` as a
   child process and shows its step lines on a progress screen. A module that
   matches but won't load is rebuilt once.
3. **Play:** the module is loaded and the game starts. If the build fails, the
   screen shows the last error lines and any key or button starts the game in
   the interpreter instead.

`--interp-only` skips the module (only step 1 runs). Ctrl+Q or closing the window
quits at any point, and also stops a running build. `--build-only` runs steps 1 and 2
without a window, loads the module once to check it, and exits: 0 when it is ready, 1
otherwise (no usable ROM, the build failed, or the module won't load), instead of
waiting for a dropped file or offering the interpreter. The launcher's
`ct-recomp --build-module ROM` is `--rom ROM --build-only`.

The engine contains no game names: the game id comes from the build (the game
directory's name, compiled in as the default) or `--game ID`. The game's files and
the tools come from `--data-dir DIR`. Its default is the source tree; packagers
set it with `-DCT_ENGINE_DATA_DIR=...` to where they install `game/`, `tools/`,
`recomp/` and `runtime/` (the build tool compiles the module against
`runtime/`'s headers). Python 3.11+ and a C compiler must be installed, and
`$CT_PYTHON` overrides `python3`.

To see the screens headless: `--shot status:PATH` writes the setup screen each
time it changes, and a `%d` in PATH numbers the shots.

## Checking a package

```
python3 tools/check_dist.py PACKAGE_DIR_OR_FILE... --rom rom/your_dump.sfc
```

It exits 1 if anything in the package was built from the ROM, and also looks
inside `.zip`, `.tar(.gz/.bz2/.xz)` and `.AppImage` files (the last needs
`unsquashfs`). It looks for:

- a module's or a generated source's file name (`ct_game*.so`, `bank*.c`,
  `ct_funcs.c`, `ct_overlays.c`)
- an ELF file that exports `ct_module_get`, which catches a renamed, stripped
  module
- generated function names (`f_XXXXXX_mNxN`)
- any of about 4,000 sampled 64-byte windows of the ROM's non-padding data,
  checked with one hash-set lookup per anchor-byte offset. A whole ROM is always
  found. In a test with random pieces hidden in other data, it found about 99% of
  the 4 KiB pieces and all of the 16 KiB ones. `--samples N` raises coverage for
  shorter pieces.

Without `--rom`, as in CI (`.github/workflows/port.yml`), the name, ELF-export and
generated-name checks still run and the ROM-window check is skipped with a notice.

The ctest `check_dist` runs it on the engine programs, which must pass, and on
`ct_game.so`, `ct_boot` and `ct_sdl`, and on ROM pieces hidden in nested archives,
which must all fail. It takes about 3 s.

## Packages

The Flatpak and the AppImage hold the engine only: `-DCT_ENGINE_ONLY=ON` builds it with
no game directory and no `$CT_ROM`. They also bundle what `build_module.py` needs, a
Python and zig as the C compiler. See [PACKAGING.md](PACKAGING.md).

## Not done yet

- A stripped module renamed to something else is caught only by its
  `ct_module_get` export. Generated code that is linked statically into some
  other program, stripped and renamed, would be found only through the ROM
  windows, and compiled code doesn't contain those.
- The module stamp doesn't cover the game's data files (`funcs.toml` and the
  others). An engine update that changes them without bumping the module ABI
  keeps the old module until you delete it.
- Android: build an arm64 module on the PC and copy it over. This needs the `ucontext`
  compatibility layer from the krtp0028 fork.
