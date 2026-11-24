<!--
SPDX-FileCopyrightText: 2026 ct-recomp contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# PlayStation 5 (homebrew)

The HD-2D/MSU-1 integration and its validation status are tracked in
[PS5-INTEGRATION.md](PS5-INTEGRATION.md). The previously published CT5 1.0
executable is unchanged; rebuild before testing these source changes.

The port runs natively on a jailbroken PS5 as an ELF payload. It is built with the
open-source [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), not Sony's SDK.
There's no PKG and no Linux layer: the payload talks to the console's own video, audio
and pad libraries through the [ps5-payload-dev SDL2 fork](https://github.com/ps5-payload-dev/SDL).

> **Status: experimental.** It cross-compiles and links against the console's system
> libraries, but nobody has run it on hardware yet. Please report what happens. The log is
> described below.

## What works and what doesn't

| | |
| --- | --- |
| Original renderer (the SNES picture, scaled) | yes: SDL's software renderer to the TV, fullscreen |
| Audio, DualSense, saves, save states, in-game menu | yes, through SDL (DualSense uses its PS5 mapping) |
| Game code | compiled in (`ct_sdl.elf`), or a module (`ct_sdl_engine.elf` + `ct_game.so`), or the interpreter |
| HD-2D renderer, asset packs' art, HD text | **no** by default: these need OpenGL 3.3, and the PS5 has no GL driver. SDL loads `libOSMesa.so.8` if one is present, but that's software GL and too slow for HD-2D's passes. If the GL context fails, the port falls back to the original renderer. |
| First-run setup (finding the ROM, building the module) | no Python or compiler on the console: build on your PC (see below) |

## You need

- A PS5 whose firmware has a public kernel exploit that starts an ELF loader. For
  example, 13.20 is covered by Relapse (7.00 to 13.60). Don't update the console.
- A Linux PC, or WSL, with `clang` 15 or later, `lld`, CMake 3.30 or later, Python 3.11+
  and `git`.
- Your own headerless Chrono Trigger (USA) 1.0 dump.

## Build

```sh
# 1. The SDK (v0.45 was used here).
wget https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
sudo unzip ps5-payload-sdk.zip -d /opt
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

# 2. SDL2 (PS5 fork) and libsamplerate into the SDK. Do this once.
packaging/ps5/build-deps.sh

# 3a. Recommended: the game compiled in, from your ROM.
CT_ROM=rom/your_dump.sfc packaging/ps5/build.sh
#   -> build-ps5/homebrew/ct-recomp/   (for your console only)

# 3b. Or the engine only (no game code, so it can be shared):
packaging/ps5/build.sh
#   -> build-ps5/homebrew/ct-recomp/
#   Its game module, built from your ROM with the PS5 compiler:
python3 tools/build_module.py --game game/ct --rom rom/your_dump.sfc \
    --out build-ps5 --cc "$PS5_PAYLOAD_SDK/bin/prospero-clang"
#   -> build-ps5/ct_game.so; run build.sh again to put it in the folder
```

`ct_sdl.elf` is the safest choice, because it's one self-contained payload.
`ct_sdl_engine.elf` loads `ct_game.so` with `dlopen`. The SDK supports that, but loading
a module that binds back to the program's own symbols hasn't been tried on a console yet.
Without the module, the engine runs the whole game in the interpreter. That's slower,
but the PS5's Zen 2 CPU should keep up.

As with every build of this port, a file built with `CT_ROM` set holds code made from your
ROM. Keep it on your own console and don't share it (docs/MODULES.md).

## GPU build: HD-2D, the 3D diorama, up to 4K (experimental)

`packaging/ps5/title/gl/build-title-gl.sh` builds title **PPSA28361, "CT5 HD-2D"**, a
separate app that draws on the PS5's GPU through
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), an OpenGL 4.6 driver built on
Mesa/Gallium, and its SDL2 bridge. The HD-2D renderer is on by default, and
Display > Resolution picks 1080p, 1440p or 4K (the default) for the next start.

Building it takes, in order (the script's header has the details):

1. ps5-opengl: `make source-fetch` and `make sdk-gl46`, then
   `PS5_DYNAMIC_SCANOUT=1 PS5_SCANOUT_HEIGHT=2160 PS5_SCANOUT_FPS=120
   toolchain/install-ps5-opengl-gl46.sh build/sdk/ps5-opengl-gl46` for runtime display modes.
   Its host compiler test needs clang-18's UBSan runtime; without it, run the install and
   link-surface steps by hand.
2. `packaging/ps5/title/gl/sdl-bridge.patch` applied to that checkout, then
   `integration/SDL2/build.py native`. The patch adds runtime display modes and the PS5
   audio driver to the bridge's SDL.
3. The script, with `GL_PREFIX`, `SDL_PREFIX`, `GL_SRC` and `KIT` (the boilerplate at
   4f531c4b) set.

## Mods (asset packs) on the PS5

The GPU build (CT5 HD-2D) loads [asset packs](ASSET-PACKS.md) from
`/data/ct-recomp/packs/NAME/`, with one folder per pack, copied over FTP or from a USB drive.
Fonts for HD text go in `/data/ct-recomp/fonts/`. The menu's **Packs** page lists every pack
in that folder: turn each one on, choose its order, reload them, or switch on **Dump new
art**. Dumping writes the tiles and sprites you see into `packs/dump/`, so you can copy
them to a PC, edit them and copy them back. The original renderer (PPSA28360) can't draw
packs. It still plays their music.

## Install as a title (eboot.bin, recommended)

`packaging/ps5/title/build-title.sh` builds a real PS5 app folder, not a payload:

```
PPSA28360/
  eboot.bin            the program (a fake-signed SELF)
  sce_sys/param.json   title ID PPSA28360, "CT5"
  sce_sys/icon0.png
  sce_module/libc.prx  the runtime shim the title loads
```

It uses [ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate)
at a pinned commit and applies `packaging/ps5/title/boilerplate.patch` to it. The patch adds
the SDK library path at link time and fixes the RELRO file offset for programs where
`.data.rel.ro` comes before `.got`. The engine is compiled against the boilerplate's
pinned SDK and PacBrew's SDL2. It is linked in as one object, with missing game hooks
resolved to NULL. Set `CT_ROM` to compile the game in.

On the console you need etaHEN or kstuff (fake-signed SELFs), plus ShadowMountPlus:

1. Copy the `PPSA28360` folder to `/data/homebrew/` over FTP.
2. Put your ROM in it as `rom.sfc`. The title sees its folder as `/app0`.
3. Run the exploit, then kstuff/etaHEN, then ShadowMountPlus. **CT5** appears on the
   home screen. Start it like any game.

Before `main`, the title checks every system function it imports. Any that the console
didn't bind are listed in a notification and in `/data/ct-recomp/imports.log`; calling one
of those crashes with a page fault. A title process binds fewer libc functions than a
payload does: `getcwd` was one, so `getcwd`, the environment, directory streams and a few
others are implemented in `packaging/ps5/title/title.c` on top of libkernel.

The title writes its log to the first writable folder in its list (see the next section),
normally `/data/ct-recomp/ct-recomp.log`, because `/app0` is read-only.

## Install as a payload folder (websrv launcher)

`packaging/ps5/build.sh` makes a homebrew app folder, `build-ps5/homebrew/ct-recomp/`:

```
ct-recomp/
  eboot.elf          the payload (ct_sdl.elf, or ct_sdl_engine.elf + ct_game.so)
  homebrew.js        the launcher tile: lists the ROMs here and starts the one you pick
  sce_sys/icon0.png  the tile's icon
```

1. Copy the whole `ct-recomp` folder to `/data/homebrew/` over FTP (e.g. ftpsrv, port 2121),
   or to a USB drive's `homebrew/` folder (exFAT or FAT32).
2. Put your ROM in that folder as `rom.sfc`.
3. Run the exploit for your firmware. Then start the websrv payload and open the Homebrew
   Launcher (or `http://PS5_IP:8080`). **CT Recomp** appears as a tile: pick it, then pick
   the ROM.

You can also send `eboot.elf` to the ELF loader (port 9021) yourself:

```sh
$PS5_PAYLOAD_SDK/bin/prospero-deploy -h PS5_IP -p 9021 build-ps5/homebrew/ct-recomp/eboot.elf
```

Sent that way, it has no ROM argument. It looks for `rom.sfc` (or any single `*.sfc` /
`*.smc`), and `ct_game.so` for the engine build, in these folders, in this order: its own
folder, `/app0` (a title's folder), `/mnt/usb0/ct-recomp/`, `/mnt/usb1/ct-recomp/`, `/data/ct-recomp/`. If there's no
ROM, a notification tells you where to put it.

The controls are the same as with a controller on the desktop (docs/USER-GUIDE.md). To quit,
hold **Create + Options** for 2 seconds, then press **Cross**. Settings and saves go in
`/user/home/<user id>/ct-recomp/`, through SDL's PS5 preferences path.

## When something goes wrong

Everything the program prints goes to `ct-recomp.log` in the first of those folders that
exists, which is normally the app folder. It's line-buffered, so it's complete up to a crash.
Fetch it over FTP or from the USB drive. When you report a problem, include it with your
firmware version and the loader you used.

## How the port is put together

- `CMakeLists.txt`: the SDK's toolchain file sets `PS5`. That turns on `CT_PS5`: the
  `-DCT_PS5` define, the project's own context switch (`runtime/compat`) in place of the
  console libc's `getcontext`, an empty target in place of `-lm`, `.elf` program names, and
  `ct_sdl_engine` built without the first-run flow.
- `frontend/ps5.c` / `ps5.h`: the log file, the splash screen, notifications, and finding
  the ROM and module in the folders above. Elsewhere this file compiles to nothing.
- `frontend/main.c` (`#ifdef CT_PS5`): always fullscreen, the ROM and module from those
  folders, and the original renderer when no GL context can be made.
- `frontend/main.c` also takes a bare ROM path as its argument on the PS5, which is what the
  launcher passes.
- `packaging/ps5/`: the dependency and build scripts, and the launcher tile
  (`homebrew/homebrew.js`).
