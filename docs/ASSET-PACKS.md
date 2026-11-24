<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Asset packs

An asset pack replaces the game's art, one 8x8 background tile or one sprite at a time,
with pictures of any resolution. The renderer draws them at the screen's resolution,
not the console's 256×224. A pack can also replace the game's songs with recordings of
your own ([Music: audio packs](#music-audio-packs)).
not the console's 256×224. A pack can also replace a character's whole animation frame
(a **pose**) with one picture, and put **3D models** (glTF) into the 3D view.

The project ships no packs, no art and no music. You make a pack on your own computer,
from your own ROM:

1. **Dump** the tiles and sprites the game shows while you play.
2. **Upscale** them into placeholders that are sharper than the originals.
3. **Edit** any of the PNGs by hand, or paint new ones.
4. **Reload** with F5 and see the change at once.

## Legal notes: read these first

- **A pack made by dumping or upscaling is the game's art.** The dumped pictures are the
  game's own pixels. An upscaled or edited copy of them is still derived from them. Such
  a pack is **for your personal use only**. Never share it, upload it or distribute it,
  in whole or in part, including single pictures. Every `pack.toml` the dump writes says
  so at the top.
- **Art you make yourself is yours.** A picture you painted from scratch belongs to you,
  and you can share it under any terms you choose. That includes a pack whose pictures
  are all your own work, keyed to the game's tiles. A picture traced over or edited from
  a dumped one is still derived from the game's art.
- **An audio pack made from the game's music is for your personal use only.** A
  recording of the soundtrack, a rip, a remaster or an arrangement of it, or a
  recording of the game playing, is the game's music, or derived from it. Never share,
  upload or distribute such a pack, or any file in it. Music you composed and
  recorded yourself is yours to share under the terms you choose; keyed to the game's
  song ids, it is still only your music.
- **The project's packages never contain a pack.** `tools/check_dist.py` fails any
  package that holds a `pack.toml`, a picture named by a content key under
  `tiles/`, `sprites/` or `objects/`, or a sound file (`.ogg`, `.oga`, `.opus`,
  `.flac`, `.wav`, `.mp3`, or Ogg or FLAC data under any name) in a `music/` or
  `packs/` folder ([PACKAGING.md](PACKAGING.md)). Packs live only in your data folder.
- **Sprite sheets are the game's art too.** The sheets `--pack-export-sheets` writes
  hold the game's own frames. A sheet you paint over, or feed to an AI image-to-image
  tool, is still derived from them, and so is every pose picture cut from it. Keep
  them to yourself, like a dumped pack.
- **Models you build yourself are yours,** as long as nothing in them (textures,
  shapes traced from screenshots) comes from the game. A texture cut from a dumped tile
  makes the model derived from the game's art.
- **The project's packages never contain a pack.** `tools/check_dist.py` fails any
  package that holds a `pack.toml`, or a picture named by a content key under
  `tiles/`, `sprites/` or `objects/`, a pose picture under `poses/`, or an exported
  sheet (`sheet_NAME_4x.png`)
  ([PACKAGING.md](PACKAGING.md)). Packs and sheets live only in your data folder.

## Where packs live

Packs are kept in the game's folder in your user data directory:

```
~/.local/share/ct-recomp/ct/packs/NAME/        ($XDG_DATA_HOME/ct-recomp/ct/packs/NAME/)
~/.var/app/io.github.dittobyte.CtRecomp/data/ct-recomp/ct/packs/NAME/   (Flatpak)
```

A `NAME` that contains a `/` is taken as a directory path instead, anywhere. Exported
sprite sheets go next to the packs, in `ct/sheets/NAME/`.

## The workflow

```
# 1. Dump: play (or replay a script); every new tile and sprite is written once.
ct_sdl --pack-dump mydump

# 2. Upscale 4x into a new pack (the built-in pixel-art scaler).
python3 tools/pack_upscale.py mydump --scale 4 --out my4x

#    Or with an upscaler you installed, for example an AI one, run once per picture:
python3 tools/pack_upscale.py mydump --scale 4 --out my4x-ai \
    --external 'realesrgan-ncnn-vulkan -i {in} -o {out} -s {scale}'

# 3. Play with it. Packs stack: the first --pack wins where two have the same key.
ct_sdl --pack my4x
ct_sdl --pack myedits --pack my4x

# 4. Edit a PNG in my4x/sprites/ (or copy it into myedits/), save, press F5.
```

The engine-only build (`ct_sdl_engine`, the AppImage and Flatpak) takes the same options.

### Dumping

`--pack-dump NAME` turns on the PPU's provenance G-buffer (below). At the end of each
frame, every tile and sprite drawn that the pack doesn't have yet is written:

- `tiles/KEY.png`: an 8×8 background tile in its palette.
- `sprites/KEY.png`: one OAM entry's whole sprite (8×8 to 64×64) in its palette.
- `objects/KEY.png`: sprites drawn together, as one picture (see below).

Color 0 is transparent. Pictures are in the orientation they are stored in VRAM. A tile
the game shows flipped is the same tile, and the renderer flips its replacement the
same way.

Dumping again into the same `NAME` adds only what is new, so you can dump in several
sessions. The Leene Square replay (9,200 frames, the start of the game up to the fair)
gives about 2,900 tiles, 1,600 sprites and 2,600 objects, 42 MB. The dump writes
uncompressed PNGs; the upscaler's output is compressed.

**Objects.** The SNES draws a character from several OAM pieces. To help you find a
character's pieces, each frame's sprites whose boxes touch or overlap are grouped. A
combination not seen before gets a picture of the assembled object and an `[[object]]`
entry listing its pieces (their keys, offsets and flips). The objects are a finding aid:
replacing art still happens per piece. A character standing next to another forms one
group, and each animation frame forms a new one, so there are many.

### Upscaling

`tools/pack_upscale.py PACK --scale N (--out NEW | --in-place)`, with N from 2 to 6.

The built-in scaler is edge-directed pixel-art scaling in the manner of xBRZ, written
for this project from the published idea rather than from any implementation. For every
2×2 block of source pixels it compares color differences along the two diagonals over
the surrounding 4×4 pixels. Where one diagonal is clearly the more continuous, the
corner between the four pixels is cut along it, with a 45° line, or a shallow or steep
one where the neighbors continue the edge. Coverage is supersampled, so edges come out
smooth while flat areas and the palette stay as they are. Alpha is treated as a channel
like the others: outlines stay crisp, and holes stay holes. It runs on the dumped tiles,
never on the whole frame, so tile seams don't move. The Leene Square dump (7,085
pictures) takes about a minute on 4 cores (`--jobs`).

`--external CMD` runs your own program once per picture. `{in}`, `{out}` and `{scale}`
in CMD are replaced with the input PNG, the PNG to write and the factor. CMD is split
like a shell command line but not run through a shell. If the program drops the
transparency (many AI upscalers write RGB), the alpha channel is taken from the
built-in scaler's result.

The upscaled pack's `pack.toml` is the original's with `scale` set and `upscaled_from`
added. Entry sizes (`w`, `h`) stay the console sizes, since that is what a picture is
mapped onto, whatever its resolution. Upscaling a pack whose scale is not 1 needs
`--force`.

These upscales are **placeholders**: a guess at detail the original never had, to be
replaced by real art piece by piece. An MMPX scaler (MIT, designed for sprites) would
be a good second built-in option. It isn't implemented yet; `--external` can run one
now.

### Editing

Open any PNG in your editor and paint. A replacement can have any size, but it must keep
the piece's aspect ratio, because it is stretched over the original piece. Save it as
8-bit RGBA, RGB, gray or palette PNG (16-bit and interlaced PNGs are not read).
Transparent pixels show what is behind the piece. Press **F5** in the game to reload
every pack.

To keep your own work apart from the generated placeholders, put it in a pack of its
own and list it first: `--pack myart --pack my4x`. Copy the entry from the placeholder
pack's `pack.toml` into yours (the `[[tile]]` block with its `key`, `file`, `w` and `h`),
and put your picture at `file`.

## Poses: a whole frame as one picture

The SNES draws a character from several OAM pieces, and a `[[tile]]` replaces one piece.
A **pose** is the character's whole animation frame, as the game assembles it: one
picture for all its pieces, so you can paint (or upscale) a character without seams
between pieces, and at any size.

- **Keys come from the game module.** The module reads each field object's current
  frame from the ROM and the object table ([SCENE.md](SCENE.md), A1) and gives it a stable key
  (`ct_scene_pose`, runtime/module.h). In this game it is
  `1 << 24 | kind << 16 | id << 8 | frame`, written as 8 hex digits (`01000003`: kind 0,
  a party member; id 0; frame 3). The same pose has the same key on every map and in any
  object slot.
- **The picture** is any whole multiple of the pose's size (`w` x `h` console pixels,
  the bounding box of its OAM pieces), with alpha. Another shape is stretched to fit,
  with a warning.
- **Mirrors:** the game stores a left-facing frame as a frame of its own (it flips the
  8x8 tiles inside it, not the whole frame). `flip = "h"` draws a picture mirrored, so
  one picture can serve both facings: give the second pose's entry the first one's file.
- **Fallback:** a pose wins over the `[[tile]]` replacements of its pieces; where there
  is no pose, the pieces' own replacements are used; where neither, the original.
- **2D:** the picture covers the bounding box of the object's OAM entries in the frame
  drawn. Where it has pixels beyond the original's (a bigger hat, a smiley over the whole
  box) they go over what is behind the character, by the PPU's priority (z level) and,
  among sprites, OAM order: a roof or a sprite in front still hides it. Where the box
  isn't the pose's size (the frame changed, a piece is hidden), the pose isn't used for
  that frame.
- **3D:** the character is one billboard of the picture instead of its 8x8 pieces, lit
  and casting a shadow like them.

Poses come from field objects. The world map's sprites, battles and menus keep their
per-piece replacements.

## Sprite sheets for artists

`--pack-export-sheets NAME` writes every pose seen while you play onto sheets, one per
character (sprite record), into `ct/sheets/NAME/` in your data folder:

| File | What it holds |
| --- | --- |
| `sheet_pc00_1x.png`, `sheet_pc00_4x.png` | The character's poses, one per cell, 8 cells a row, all lined up on the foot point, at 1x and at `--pack-export-scale N` (default 4) |
| `sheet_pc00.json` | The index: per pose its key, rectangle (1x pixels), size, foot point and, if it is another pose mirrored, `mirror_of` |
| `poses/KEY.png`, `poses.txt` | Every pose as the game drew it, and the list; a later export into the same NAME starts from them |
| `maps/map_005.txt` | Each map seen: per cell its class and tile words (for `[[model]] tiles`) |
| `README.txt` | What these are, and the personal-use reminder |

On a sheet, magenta lines run through the gutters between cells; cyan ticks in the gutter
below and left of each cell mark the column and row of the foot point. Paint only inside
the cells: the importer cuts each pose's rectangle and ignores everything else.

The round trip:

```
# 1. Export: play (or replay a script) through the scenes you want.
ct_sdl --pack-export-sheets crono --pack-export-scale 4

# 2. Edit ~/.local/share/ct-recomp/ct/sheets/crono/sheet_pc00_4x.png in Aseprite, Krita,
#    or an AI img2img tool (keep the size and the transparency), save it as a copy.

# 3. Import: each edited cell becomes poses/KEY.png and a [[pose]] entry in the pack.
python3 tools/pack_import_sheet.py ~/.local/share/ct-recomp/ct/sheets/crono/sheet_pc00_4x_edit.png \
    --index ~/.local/share/ct-recomp/ct/sheets/crono/sheet_pc00.json \
    --pack mychars --changed-from ~/.local/share/ct-recomp/ct/sheets/crono/sheet_pc00_4x.png

# 4. Play with it (F5 reloads after another import).
ct_sdl --pack mychars --pack my4x
```

The sheet's scale is worked out from its width (any whole multiple of the 1x size).
Cells left fully transparent are skipped, unless the pose is the mirror of one you drew:
then that picture is used with `flip = "h"`. `--changed-from` skips cells that are as
exported, so only your edits become replacements. Importing again replaces the entries
of the same keys.

## Models

A `[[model]]` puts a glTF 2.0 model (`.gltf` with its buffers, or `.glb`) into the 3D
view (`hd2d3d`, `deck3d`), on the field and on the world map:

- **Where:** by map id and a rectangle of cells (`cells = [x0, y0, x1, y1]`, or
  `cell = [x, y]`), or on every cell whose tiles are given ones (`tiles = [layer, TL, TR,
  BL, BR]`, the tile words of a 16 px field cell's four 8x8 tiles in that layer; on the
  world map a cell is one tile: `[layer, word]`), with `size = [w, h]` cells for the
  footprint. `map` may then be left out (any map).
- **Units and axes:** map pixels, y up, x east, z south (toward the camera): glTF's own
  axes, with +z the model's front. A field cell is 16 px, a world map cell 8 px. The
  model's origin goes to the middle of the footprint, on the ground (the world map's
  ground height there).
- **Placing:** `scale` (map px per model unit, one number or `[x, y, z]`; default 1),
  `rotate = [x, y, z]` in degrees (about x, then y, then z), `offset = [x, y, z]` in map
  px after that.
- **Looks:** the base color texture (PNG, nearest neighbour; `filter = "linear"` for
  smooth, mipmapped), the material's base color factor, vertex colors (`COLOR_0`), and
  `color = [r, g, b, a]` over all of it. Alpha modes MASK and BLEND are drawn as cut-outs
  (alpha >= 0.5). No lighting model of the file's own: the model is lit by the scene's
  key light, with its shadow map (it casts and takes shadows) and the ambient occlusion,
  as the map's own walls are.
- **`hide_cells = true`** removes the generated geometry of the footprint's cells
  (floor, walls, roofs and their side walls), so the model replaces it instead of
  standing in it.

A model is read when a map first shows it, kept until F5 (which reloads it), and baked
into the map's geometry once per map. Every node's transform is applied; animation,
skins and morph targets are ignored.

**From Blender:** model in map pixels (or in cells and set `scale = 16`), Y up in the
glTF exporter (its default, `+Y Up`), the origin at the footprint's middle on the floor,
and export glTF Binary (`.glb`) or glTF Separate. Blender's -Y (front view) is glTF's
+z, the side the camera sees. **Sprytile** and **Crocotile 3D** build models from tile
sheets: export glTF/GLB (Crocotile can export glTF) with the tile size as the unit, then
`scale` = the tile size in map pixels over the model's tile size (16 px tiles, 1 unit
per tile: `scale = 16`). Keep the texture as a PNG, and keep its filtering nearest for
pixel art.

To find the cells: run with `CT_PACK_WHERE=1` set, which prints the map id and the
player's cell once a second, or look in an export's `maps/map_ID.txt`.

## Using packs

| Option or key | What it does |
| --- | --- |
| `--pack NAME` | Load a pack. Repeat it to stack packs, highest priority first. `--pack none` loads none. |
| `--pack-dump NAME` | Dump new tiles and sprites into pack NAME while playing |
| `--pack-export-sheets NAME` | Write every pose seen to sprite sheets in `sheets/NAME` (above) |
| `--pack-export-scale N` | The sheets' second scale, 1 to 16 (default 4) |
| F5 | Reload the packs (after editing their PNGs or models) |
| `CT_PACK_WHERE=1` | (environment) Print the map and the player's cell once a second |

The packs given with `--pack` are remembered in `settings.ini` (`packs=a,b`) and loaded
on the next start without the option. `--pack none` clears the list.

Packs are drawn by the GL renderer. With the HD-2D looks, the replacement goes through
the lighting, depth of field and grading like the original pixels. With the plain look
(no `--hd2d`), a loaded pack switches the window to the GL renderer with the `off`
preset. That shows the original look, at full resolution, with the replacements. Your
saved look is kept. `--no-hd2d` with packs loads none.

## How it works

1. **Provenance G-buffer** (`PpuSetProvBuffer`, `sched_set_prov`, `sched_prov`). The PPU
   records, per output pixel, where the main screen's winning pixel came from: the 8×8
   character's VRAM address and bit depth, the palette and the CGRAM index drawn, the
   texel within the character, and for a sprite the OAM entry, its name, size and the
   texel within the whole sprite (`PpuProv` in `third_party/snes/ppu.h`). With it,
   the scheduler keeps the VRAM, CGRAM and OAM as they were when the frame was copied
   (`sched_prov_mem`). It is off unless a pack is loaded or being dumped. Sprite
   evaluation is compiled in two versions, so the path with it off is unchanged. With
   it on, frame and state hashes are still identical, over all 9,200 frames of the Leene
   Square replay.
2. **Keys** (`packs_key`): a 64-bit FNV-1a hash.
   - A **BG tile key** hashes the character's decoded 8×8 color indices and its
     palette's colors (color 0 left out: it is transparent), plus the bit depth.
   - A **sprite piece key** hashes the same for one OAM entry's whole sprite, its
     characters gathered with the 16×16 name-page wrap, and its 16-color palette.
   - The same tile in two palettes (day and night, or a fade the game does by changing
     the palette) gets two keys, as in Mesen's HD packs. Fades done with the brightness
     register don't change any key.
   - **Pose keys** name a character's whole animation frame, instead of the OAM pieces
     it is drawn with. They come from the game module, which decodes the game's own
     sprite frame format ([Poses](#poses-a-whole-frame-as-one-picture)). `[[object]]`
     groups stay a finding aid.
3. **Per frame**, `packs_map_frame` turns the provenance into a map: per console pixel,
   the atlas entry that replaces it, the texel it showed and the palette color it was
   drawn with. Keys are cached per frame by the provenance's raw identity (address,
   palette, size), so each distinct tile is hashed once a frame. Poses go first
   (`packs_set_poses`, from `frontend/pack_scene.c`): the objects of the scene the
   module described at the previous frame edge (the OAM drawn now is the one uploaded
   after it), each placed on its entries' bounding box in the frame's OAM; a pixel of
   one of those entries maps to the pose picture's texel at that screen position, and so
   does a pixel behind the object where the picture has cover (flag 64 in the map: that
   pixel, not the behind G-buffer, shows under the picture's transparent texels).
4. **The atlas**: every replacement PNG of the loaded packs, shelf-packed into pages of
   an array texture (256 to 4,096 texels square, as needed), mipmapped.
5. **Compositing** (`shaders/upscale.frag`, compiled into the passes that read the
   picture). For each output pixel, the replacement is sampled at the pixel's own
   position within the console texel it covers, flipped as the console flipped the
   original. Where a replacement texel is larger than an output pixel, it is drawn
   sharp, with only its edges smoothed (as sharp-bilinear does for console pixels).
   What the console and the lighting did to the original's palette color carries over
   to the replacement: a ratio (brightness, lighting) or, where color math changed the
   pixel, an offset. Fades, tints and the HD-2D lighting therefore apply to
   replacements too. Transparent texels show the behind G-buffer (the layer under the
   piece), which is turned on whenever a pack is loaded. Pixels with no replacement
   are drawn as before.

With no pack loaded none of this runs. HD-2D screenshots (presets off, hd2d and hd2d3d,
four frames of the Leene Square replay at 1920×1080) are byte-identical to the build
without packs.

### Cost

Measured on a ROG Ally (Radeon 780M, Mesa radeonsi) at 1706×1120 over 6,000 frames of
the Leene Square replay, with the 4× pack (4,474 pictures in one 4,096² page, 64 MB):

| | GPU per frame (median), no pack | with the pack |
| --- | --- | --- |
| Preset `off` | 0.24 ms | 0.36 ms |
| Preset `hd2d` | 1.00 ms | 1.28 ms |

On the CPU, building the map takes about 0.13 ms per frame, and the provenance
G-buffer about 0.4 ms in the PPU (`ct_boot --prov`: 39 s instead of 35 s for 9,200
frames). The atlas is held in RAM and on the GPU (64 MB for the 4× Leene pack).

Models and poses, on the same Ally in `hd2d3d` at `--scale 5` (1280×1120), the last 240
frames before Leene Square's frame 8700 (unpaced, other builds running, so totals are
noisy; `CT_HD2D_PROFILE`), median per pass:

| | surfaces | shadow map | whole frame |
| --- | --- | --- | --- |
| No pack | 1.034 ms | 0.051 ms | 2.77 ms |
| Three test models (two houses, a cube: 40 triangles, 2 textures) | 1.034 ms | 0.050 ms | 2.92 ms |
| The same and the leader's poses (51 pictures at 4×) | 1.034 ms | 0.051 ms | 2.73 ms |

The differences are within the noise: a model costs its triangles and one draw call per material part, twice (the shadow map
and the surfaces); baking a map's models takes a fraction of a millisecond, once per map.
On the CPU, poses add the scene capture and one pose lookup per object each frame: the
2D map took 0.10-0.12 ms per frame with them, against 0.02-0.03 ms with only models
loaded (the 0.13 ms above was with the full 4× pack).

## The format

A pack is a directory holding `pack.toml`, PNGs and glTF files. `pack.toml` is a small
TOML subset: `[pack]`, then `[[tile]]`, `[[object]]`, `[[pose]]` and `[[model]]` entries
of `key = value` lines.
Values are strings, numbers or arrays of numbers. It is data only: nothing in a pack
can run code.

```toml
[pack]
version = 1          # this format
name = "my4x"
scale = 4            # informational: the factor its pictures were made at
upscaled_from = "mydump"

[[tile]]
key = "67570e230f375981"      # packs_key, 16 hex digits
kind = "sprite"               # "bg" or "sprite" (informational)
file = "sprites/67570e230f375981.png"
w = 8                         # the original piece's size in console pixels
h = 8
bpp = 4                       # tags, informational:
layer = "OBJ"                 #   BG1-BG4 or OBJ
palette = 0                   #   sprite palette 0-7
oam = 30                      #   the OAM entry it was first seen in
first_frame = 801             #   the frame it was first seen in

[[object]]                    # a finding aid: sprites drawn together
key = "27cd0626975fbc46"
file = "objects/27cd0626975fbc46.png"
w = 48
h = 96
first_frame = 801
pieces = [
  { key = "a8d096398c37d0df", x = 40, y = 56, hflip = 0, vflip = 0, oam = 0 },
]

[[pose]]                      # a whole animation frame as one picture
key = "01000003"              # the module's pose key, 8 hex digits
file = "poses/01000003.png"   # any whole multiple of w x h, with alpha
w = 16                        # the pose's size in console pixels
h = 32
flip = "h"                    # optional: draw the picture mirrored ("v", "hv" too)

[[model]]                     # a glTF model in the 3D view
map = 0x005                   # map id
cells = [27, 33, 29, 35]      # the footprint: x0, y0, x1, y1 (or cell = [x, y])
file = "models/house.glb"     # glTF 2.0, .gltf or .glb
scale = 1.0                   # map px per model unit, or [x, y, z]
rotate = [0, 90, 0]           # degrees about x, then y, then z
offset = [0, 0, 0]            # map px: x east, y up, z south
filter = "nearest"            # or "linear"
color = [1, 1, 1, 1]          # times the material's color
hide_cells = true             # remove the footprint's generated geometry

[[model]]                     # the same model on every cell showing these tiles
tiles = [0, 0x0123, 0x0124, 0x0133, 0x0134]   # layer, then TL TR BL BR tile words
size = [2, 2]                 # the footprint, in cells, from the matching cell
file = "models/well.glb"
```

Only `key`, `file`, `w` and `h` are needed for a `[[tile]]` or a `[[pose]]`. A
replacement picture can be any size up to 2,048 pixels on a side. A `[[model]]` needs
`file` and a place (`map` with `cells`/`cell`, or `tiles`). Its file is checked with
cgltf (`third_party/cgltf`) when the pack loads, and read into triangles when a map first
shows it.

### Compared with Mesen's HD packs and RT64's texture packs

The format follows the ideas of Mesen's `hires.txt` (GPL-3.0), the closest prior art for
tile-level replacement ([PRIOR-ART.md](PRIOR-ART.md)). There is no mainline SNES
dialect of `hires.txt`, so `pack.toml` is the definition here.

- **The same:** a pack is a folder of PNGs and a manifest. A tile is keyed by its pixels
  and its palette, so one tile in two palettes is two entries. There is a scale. A dump
  mode records what your own ROM displays, like Mesen's HD Pack Builder.
- **Different:**
  - Keys are hashes of the decoded tile and its palette's 15-bit colors, not the tile's
    raw bytes and palette bytes. They are the same for 2bpp, 4bpp and 8bpp tiles
    wherever they sit in VRAM.
  - SNES sprites are whole OAM entries (up to 64×64), not 8×8 tiles, plus the
    `[[object]]` grouping. NES packs don't need that.
  - Pictures can be any size per entry, not one scale per pack.
  - There are **no conditions yet**. Mesen can make a replacement depend on, for
    example, a memory value or a neighboring tile. The format has room for that: a
    `when` table in an entry is a planned addition and is ignored today.
  - No background images, sound or palette overrides.
- **From RT64** (MIT): a database keyed by content hash, PNG for authors, and later an
  optional packed container (pre-compressed textures in one file) for loading speed.
  For now a pack is only PNG plus the manifest, loaded in full at start and on F5.

## Music: audio packs

Standard MSU-1 `.pcm` recordings are supported directly, including header loop
points. See [MSU1.md](MSU1.md) for the existing-pack importer and compatibility limits.

A pack's `pack.toml` can map the game's songs to sound files of your own. While a
mapped song plays, the console's music is muted and your file plays instead, looped
and faded as the game asks. Sound effects stay the console's.

**Legal:** read [the notes above](#legal-notes-read-these-first). A pack holding the
game's music, in any form, is for your personal use only.

### Making one

1. **Find the song ids.** Play with `--music-log` and the engine prints every song the
   game asks for, its fades and its stops, and whether a pack replaces it:

   ```
   ct_sdl --music-log
   music: frame 1999: play song 0x11 (not in a pack)
   music: frame 3546: volume to 255/255 over 231 frames
   music: frame 3551: play song 0x12 (not in a pack)
   music: frame 3997: volume to 0/255 over 66 frames
   ```

   `ct_boot --music-log` prints the same headless (with `--script` and a replay).
2. **Put the files in a pack.** Ogg Vorbis (`.ogg`), FLAC (`.flac`), WAV (`.wav`), or MSU-1 PCM (`.pcm`),
   any supported sample rate, mono or stereo (MSU-1 PCM is fixed at 44.1 kHz stereo):

   ```
   ~/.local/share/ct-recomp/ct/packs/mymusic/
       pack.toml
       music/battle.ogg
   ```
3. **Map them in `pack.toml`:**

   ```toml
   [pack]
   version = 1
   name = "mymusic"

   [[music]]
   song = 0x45                 # the game's song id, as --music-log prints it
   file = "music/battle.ogg"   # relative to the pack
   loop = true                 # at the end, go on from loop_start (the default)
   loop_start = 441000         # in the file's samples (frames), default 0
   loop_end = 5292000          # default: the file's end
   volume = 0.8                # 0-4, default 1
   fade = "game"               # follow the game's fades (default); "none": ignore them
   ```

   Only `song` and `file` are needed. A song that should play once (a fanfare) has
   `loop = false`.
4. **Play with it:** `ct_sdl --pack mymusic`. Music and art packs stack like art packs
   (the first pack that maps a song wins), and are remembered in `settings.ini` the
   same way. F5 and the menu's **Reload packs** read the music again too. Audio packs
   work in every look, including the original picture with `--no-hd2d` (only the art
   needs the GL renderer).

The engine-only build (`ct_sdl_engine`, the AppImage, the Flatpak) takes the same
options; the music is followed by the game module (it needs no ROM data in the engine).

### How it works

1. **The game's sound commands** (`game/ct/audio.c`). The game asks its sound program
   for everything through a block of four bytes at WRAM `$7E1E00` (a command and three
   arguments) and a call to `$C70004` (`Audio_ProcessCommand`, `$C70140`), which sends
   the block through the APU ports, `$2143` down to `$2140`, the command last
   (`$C7061C`; a song's through `$C70A39`). The engine passes every CPU write to an APU
   port to the game module (`snes_apu_port_hook`, `ct_music_hooks` in
   `runtime/module.h`); a write to port 0 while the four ports equal `$1E00-$1E03` is a
   command. The ones that matter here:

   | Command | Arguments | What it does |
   | --- | --- | --- |
   | `$10`, `$11`, `$14`, `$15` | song id | Load the song and play it. The CPU then uploads the song's data to the SPC700 (up to about 50 frames) and clears `$1E00` when done; that is when the song starts, and when the replacement does. Song 0 is silence. |
   | `$81` | time, volume | The music's volume to `volume` (0-255) over `time` steps of about 2.06 frames (32 steps were measured at 66 frames); 0: at once |
   | `$F0` | | Stop the music |
   | `$18`-`$2F` | effect id, pan | Sound effects (not followed) |

   The same song asked for again (a new room with the same music) plays on without a
   restart, as the console's does. A command sent again because the SPC700 was slow to
   answer is one event.
2. **Music and sound effects apart.** The sound program plays effects on voices it
   takes from the music for the time: `$19` effects on voices 4-5, `$18` on voices
   6-7. APU RAM `$86` holds those voices as a bit mask while they play. While a
   replacement plays, the engine leaves every other voice out of what it hears
   (`sched_set_voice_mute`: `dsp_host_mute` and `dsp_host_keep_addr` in the vendored
   DSP): the music is silent and the effects stay, sample by sample. The console itself
   runs as before; only the engine's copy of its output changes.
3. **Mixing** (`frontend/packs/music.c`). The file is streamed (stb_vorbis, dr_flac,
   dr_wav), resampled to the output rate with the engine's band-limited resampler, and
   added to the game's sound after the game's own resampler, before the volume. Starts,
   stops and switches ramp over 20 ms; fades move linearly in time, as the game's do.

With no `[[music]]` entry loaded, none of this runs: the engine doesn't watch the ports
and the sound is byte for byte what it was (the `music_pack` test compares the sound
up to the replaced song; `module_parity` and the lockstep tests cover the rest).

### Limits

- **No echo while a song is replaced.** The console's echo (reverb) is fed by music and
  effects together, so while the music is muted the echo return is left out as well:
  sound effects play dry.
- **The first frame:** the music is muted from the frame edge after the game starts a
  song, so up to one frame (16 ms) of the console's song can be heard at a switch.
- **Save states:** after a state is loaded the replacement stops and the console's music
  plays until the game starts its next song (a state doesn't say which song was
  playing).
- **Fades** follow the game's commands with the measured step time; a fade the sound
  program makes on its own (inside a song's own data) isn't seen.
- Only the music the CPU asks for: songs the game plays some other way, if any, keep the
  console's music.
- The watcher is the game module's: `ct_sdl_engine --interp-only` (no module) plays no
  replacements. It needs no game patch, so the interpreter follows the music where the
  game is linked in (the static `ct_sdl --interp-only`).

## In the 3D diorama (scene3d)

The 2D compositing covers every preset that draws the console's picture. The 3D
diorama (`hd2d3d`, `frontend/hd2d/scene3d.*`) draws tiles itself, as textured quads:

- **BG tiles are replaced there too.** Every 3D surface samples through
  `tileTexel(uint tile, vec2 uv)` in `shaders/scene3d.frag`, keyed by the packed tile
  value (VRAM address, palette, depth). Once a frame, for each distinct tile the map's
  mesh shows, the renderer asks the frontend (`hd2d_pack_tiles`) for its atlas entry.
  The frontend answers with `packs_key_tile` (the same key `packs_key` gives a pixel of
  the tile, from the scene's VRAM and CGRAM) and `packs_lookup_tile`. The answers go
  into a lookup texture, by VRAM address and palette, and `tileTexel` samples the
  atlas where there is an entry (with the quad's own derivatives, so distant floors
  are mipmapped) and the console's tile where there isn't.
- **Sprites are replaced in 3D as whole poses only.** A character with a `[[pose]]` is
  one billboard of the picture (`S3D_PACKED`: the tile value's low 18 bits are then an
  atlas entry, sampled by `tileTexel` over the whole quad, in the shadow pass too),
  placed by `s3d_set_sprite_hook`. Per-piece `[[tile]]` replacements of sprites still
  aren't: the 3D path splits sprites into 8×8 pieces.
- **The C API for other renderers:** `packs_key` / `packs_key_tile` and
  `packs_lookup_tile(key)` give a replacement's atlas rectangle (`packs_rect`: page, x,
  y, w, h, and the original's size); `packs_atlas` and `packs_entries` give the atlas
  and the entry table, which `hd2d_pack_atlas` uploads.
- **`[[model]]` entries** are drawn as a mesh of their own (`hd2d_pack_models`): the
  frontend (`frontend/pack_scene.c`) matches the entries to the scene's map, bakes the
  meshes (`packs_model_mesh`) into world space once per map, and hides the cells with
  `hide_cells` through `s3d_set_cell_hook` (which both the field's and the world map's
  builders ask). The renderer draws them with `scene3d.frag` compiled with `MODEL`: the
  same lighting, haze, point lights and outputs as the map's surfaces, with the vertex
  normal (and the nearest of the fixed ones for the occlusion), one draw call per
  material part, in the shadow map's pass too, at each copy of a wrapping world map.

## Limits

- Mode 7 isn't replaced: it has no tiles in the usual sense.
- Mid-frame palette or VRAM changes: keys use the VRAM and CGRAM at the end of the frame.
  A tile drawn with a palette that HDMA changes partway down the screen gets the key of
  the palette at the end.
- A `[[tile]]` replacement covers exactly the original piece's pixels. It can't draw
  outside them (a sprite can't get bigger than its original), and its transparent parts
  show the one layer under it (the behind G-buffer), not every layer. A pose covers its
  whole box (below).
- Each animation frame and each pair of characters standing together is a new
  `[[object]]`, so dumps have many of them.
- The whole-line PPU renderer (unused in the game) doesn't record provenance.
- The atlas holds every replacement at once. A full game's dump, upscaled 4×, could go
  past the 4,096² page and use several pages (each 64 MB). A packed, streamed container
  is the fix, later.
- The 3D preset replaces BG tiles and whole poses, not single sprite pieces.
- A pose replaces the original only within the bounding box of the object's OAM
  pieces: it can't be drawn bigger than the frame's box. Its size limit is 256 x 256
  console pixels. Poses come from field objects only (the module's decoder): not the
  world map's sprites, battles or menus. Frames of 12 pieces, which the game builds over
  several frames, have no pose.
- Models are drawn in the 3D view only, not in the 2D looks. They have no animation,
  skinning or morph targets, take PNG textures only (no JPEG, no KTX2), and have no
  transparency other than cut-outs. They are placed again whenever the map changes
  (also in place, as when a door opens): one placed by `tiles` follows the tiles, one
  placed by `cells` stays.
