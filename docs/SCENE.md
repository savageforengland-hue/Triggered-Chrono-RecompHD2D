<!--
SPDX-FileCopyrightText: 2026 jerbanpls
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Scene description: what the game keeps, and what it would take to use it

Research spike for two planned features:

- **(A) a 3D field renderer**: rebuild the current map as geometry at runtime (floors,
  walls that rise, roofs, a perspective camera, sprites as billboards);
- **(B) co-op**: player 2 picks commands for party members 2 and 3 in battle, then
  steers a follower in the field, then, if it can be done, a **split screen** with a
  second view that follows player 2's character.

Both need the game module to hand the engine a per-frame *scene description*. This
document records where the game keeps that data, how each address was checked, a
proposed generic `ct_scene` struct, and whether each feature is feasible.

Nothing here changes game logic. Tools added: `game/ct/tools/scene_probe.py` (prints
the scene from a WRAM dump; draws the whole map with `--png`, lists OAM by object
with `--oam`) and `game/ct/tools/ldis.py` (a linear listing of ROM code with
funcs.toml names). Both read the user's own ROM or dumps. Their output is
ROM-derived: keep it in the build dir and never commit it.

## Summary of verdicts

| Question | Verdict |
| --- | --- |
| Whole map in WRAM (all layers, every tile)? | **Yes.** Up to 64x64 metatiles of 16 px per layer, in bank $7E. |
| Per-tile collision and properties in WRAM? | **Yes.** Three property bytes per tile, plus exit and zone grids. |
| BG tile graphics fully in VRAM for the map? | **Yes.** Loaded at map load; animated tiles are changed in place. |
| Object state map-wide? | **Mostly.** Positions, scripts and movement run for all 64 slots. Animation frames only advance for objects near the camera. |
| OAM | **Culled** to the camera window (+4 tiles); one VRAM sprite slot per active object. Only the current frame is uploaded. |
| Any object's sprite frame without OAM? | **Yes** (A1): from ROM and WRAM, for 4- and 8-piece frames (`game/ct/sprites.c`), checked pixel for pixel against the PPU. |
| (A) 3D rebuild of the field | **Yes.** All inputs are resident. A height map has to be *inferred*, because the game has none (see A0.9). |
| 3D world map (1000 A.D., `$1F0`)? | **Yes.** Mode 1, no HDMA, whole 1536x1024 px map and 8 px terrain classes in WRAM (W1-W3, W9). Mountain vs forest needs the picture. All eight overworlds load and are mode 1 (W13); the Epoch is not found (W12). Mode 7 is a separate cutscene engine, two of its ten scenes (a bike race, the Epoch cockpit) use it (W11). |
| (B1) battle co-op | **Yes.** One hook on the battle's pad word, keyed by the active-menu byte. |
| (B2) P2 steers a follower | **Partial.** Hook the follower routine. Proper wall collision needs the leader's collision code, which is tied to the camera. |
| (B3) true split screen | **Partial (feasible with module work).** Background: yes, from WRAM/VRAM alone. Sprites: no from VRAM. They need a module-side sprite renderer, or a hook that widens the activation window. Map exits, triggers and battles stay tied to the leader. |

## Conventions and how things were checked

- WRAM addresses are given as bank $7E/$7F offsets (`$7E1800` = WRAM offset `0x1800`;
  `$7F2000` = offset `0x12000` in a `--wram` dump).
- **The field engine runs with D = $0100** (checked: `--log-entry C0092B` prints
  `DP 0100`). Direct-page operands in field code such as `$97`, `$99` and `$FE` are therefore
  `$0197`, `$0199` and `$01FE`. Camera code runs with D = $1D00 (`$0A` is `$1D0A`).
  Battle code runs with D = $0000 and DB = $7E.
- Object arrays are indexed by **2 x slot**. There are 64 slots: each array is $80 bytes.
  `$7F2000` holds the number of objects in the location's script.
- Replays: `leene_square.txt` (frame 9200: bell plaza, location $1B7), the same replay
  to frame 8700 (Leene Square, location $005), and `gato_win.txt` (battle, frames
  10650-11000). For a follower, the run that stops at frame 8700 adds
  `--poke-wram 8000:2981^82`, which puts PC 2 in party slot 2 while the game is on the world map, before the fair loads
  (`$7E2980-$7E2982` = party PC ids, `$80` = empty). Lucca then loads as follower 1 and follows Crono.
- Per-frame series: `--snap-at C0A810@8600 DIR` (the field object update) writes one
  WRAM snapshot per frame, 256 in all.

Typical commands, from a build dir:

```
CT_ROM=... ./ct_boot --frames 9200 --script ../game/ct/test/replay/leene_square.txt \
    --wram spike/l.wram --vram spike/l.vram
python3 ../game/ct/tools/scene_probe.py spike/l.wram --vram spike/l.vram --png spike/map.png --oam
```

## A0. Field map data

### A0.1 Map load and map-change detection

- **Location id: `$7E0100`** (u16, 9 bits used). Field_CalcLocationRecordOffset
  ($C0092B) reads it as `$00/$01` with D=$0100 and multiplies by 14 into the location
  records at `$F60000`. With `--watch 100:2`, leene_square shows $1B1 (title demo), $1F0
  (world map), $10F, $002, $001 (Crono's house), $005 (Leene Square), $1B7 (bell plaza)
  and $006 (Gato's area). Each change is written at $C00CB6 or $C21138/$C224AC (world
  map exits).
- **Load sequence:** Field_LoadLocationResources ($C000F4) loads music, tileset gfx
  ($C00960), the L3 tileset, BG palettes, then Map_LoadAndApplyConfig ($C0A33B: map
  header and grids), then the tile assembly ($C009DD, $C00A14). `--at C0092B` fires once per
  map load. **Map-change hook:** the return of Field_LoadLocationResources. Bump a
  "map generation" counter there, so the renderer rebuilds its geometry once. Polling
  `$0100` is not enough, because a map can be reloaded under the same id. Event
  commands that copy tiles (EventCmd_CopyTiles $C03D97, Field_CopyMapRectLayers
  $C0AF4E, opened treasure Field_ReapplyOpenedTileGroups) also change the map in place.
  The renderer must either re-hash the map grids per frame (cheap: 3 x 16 KB) or hook those routines too.

### A0.2 Tilemaps: the whole map is in WRAM

Map_LoadAndApplyConfig decompresses the map to `$7EB500`. The header is
`$7EB500-$7EB505` (C0A385-C0A40F):

- `$B500` bits 0-1 / 2-3: L1 width / height.
- `$B500` bits 4-5 / 6-7: L2 width / height.
- `$B501` bits 0-1 / 2-3: L3 width / height.
- `$B501` bit 7: L3 present (`$0BCA`); bits 6, 5 and 4 go to `$0BC9` and `$1D36`.
- `$B504` and `$B505`: more flags (`$0BD7`, `$0BDF`).

Each 2-bit size field n gives a width of 16 x (n+1) and a height of 16 x (n+1) as well (the
code computes `((b & $0C) + 4) * 4`). The sizes are stored in 16 px metatiles:

| Layer | Width | Height |
| --- | --- | --- |
| L1 | `$0BCB` | `$0BCD` |
| L2 | `$0BCF` | `$0BD1` |
| L3 | `$0BD3` | `$0BD5` (`$0BCA` bit 0 = L3 on) |

Verified sizes: the bell plaza is 48x48 on all three layers, and Leene Square is 48x64.

Map_BuildTilePropGrid ($C0A521) then unpacks the data into **fixed 64 x 64 grids, with
rows of 256 bytes**:

| Address | Content |
| --- | --- |
| `$7E3000 + y*256 + x` | L1 metatile index, low 8 bits |
| `$7E3040 + y*256 + x` | L2 metatile index, low 8 bits |
| `$7E3080 + y*256 + x` | L3 metatile index |
| `$7E30C0 + y*256 + x` | property byte 0: bit 0 = L1 index bit 8, bit 1 = L2 index bit 8, bits 2-7 = solidity class (A0.3) |
| `$7E7000 + y*256 + x` | property byte A |
| `$7E7040 + y*256 + x` | property byte B |
| `$7E7080 + y*256 + x` | exit grid: index into the location's exits, `$80` = none (Map_BuildExitGrid $C0A66B) |
| `$7E70C0 + y*256 + x` | camera-zone / treasure grid (Camera_CheckZoneTable $C09AA1; `$80` = none) |

The wrap masks are `$1D1E`/`$1D1F` (`$3F`, 64 tiles).

**Metatiles** (Field_DrawL1TileRowStrip $C07612): L1 and L2 share one table of
**512 x 8 bytes at `$7EB000`**. Each entry holds four SNES BG tilemap words: TL, TR, BL, BR. Each word carries
the tile number, palette, priority, H flip and V flip. L3 has its own table at `$7EC000`
(Field_DrawL3TileRowStrip $C07D66).

**VRAM holds only a window.** BG1/BG2/BG3 tilemaps are 64x32 at VRAM words
$6000/$6800/$7000 (`$2107-$2109` = $61/$69/$71). Row and column strips are streamed in as
the camera moves: Field_Draw*Strip into a buffer, then Vblank_Upload*Col*
($C086AB...). So the renderer must build geometry from the **WRAM grids**, not from
VRAM tilemaps.

*Verified:* from one WRAM+VRAM dump, `scene_probe.py --png` draws all of BG2+BG1 for
the map. That is 768x768 px for the bell plaza (frame 9200, where the camera shows
256x224) and 768x1024 px for Leene Square (frame 8700). The result is a correct picture of the
whole map, including the parts far off-screen.

### A0.3 Per-tile properties

The meanings below come from the code that reads each bit. **Confirmed** means a run agrees.
Everything else is a reading of the code that still needs a test.

- **Solidity class** = `$7E30C0 >> 2`. This is the value Camera_UpdateScroll ($C09AD3) dispatches on.
  - 0 = walkable.
  - 1 = solid. *Confirmed:* across 256 frames of walking, the tile under the leader was never class 1.
    The class-1 cells trace the walls, trees and tents in an ASCII dump of
    Leene Square.
  - 2-$15 = shaped, partial solidity: corner and diagonal shapes looked up in a table.
  - $16 and up = special handlers ($17, $18, $19 and others). The leader stood on classes $18, $19 and $1D
    while climbing the central stairs, so these are probably stairs and ramps.
- **Byte B (`$7E7040`)**:
  - bits 0-1: z-plane. They go to `$6A`, then `$0155`, the leader's current plane. It was 1 in every
    Leene Square tile under the leader. Community docs describe two walk planes plus transition tiles:
    "plane 1 / plane 2 / transitional", where the plane you are not on is solid.
  - bit 2: plane-transition flag (`$6C`).
  - bit 6: sprite priority for the object's **bottom** half on this tile.
  - bit 7: blocks NPCs (C0AEE1: velocity zeroed).
  - bit 3: unknown.
- **Byte A (`$7E7000`)**:
  - bits 0-1: auto-move direction; bits 2-3: auto-move speed ($08/$10/$20). Used for conveyors, wind
    and slopes (C09D6E).
  - bit 4: touch trigger. When the leader is on such a tile, the code sets `$0117` bit 4 and stores the tile in `$015B` (C09D43).
  - bit 6: sprite priority for the object's **top** half.
- **Overhead**: the metatile words' BG priority bit. In Leene Square, 494 of 3072 L1 cells
  have at least one priority-1 quarter. These draw over sprites, the way roofs and treetops do.
  The per-object sprite priority (`$0C00`/`$0C01` = $20 or $30, top/bottom half) is
  taken from the tile under the object (C0ADA8/C04B9F), so a sprite goes behind
  such tiles.

### A0.4 Tile graphics and palettes

Field_LoadTilesetGfx ($C00960) loads 6 chunks of $1000 bytes per map into VRAM words
$2000-$4FFF (Field_LoadTilesetChunkTo3000, from `$F6:1C00 + 8*tileset`). The BG1/2 character base is
`$210B` = $11 (word $1000). BG3 has its own tileset (`$210C` = $05). Mode 1, with BG3
priority (`$2105` = $09). The main screen is BG1+BG2+OBJ, and BG3 goes to the subscreen for color math.
So **every BG tile the map uses is in VRAM** while the map is loaded.
Vblank_UpdateAnimTileWords ($C0ED15) and the palette animators change tiles and
colors in place. Reading VRAM and CGRAM each frame picks those changes up.

### A0.5 Camera

- **Camera position in map pixels** = (`$1D0A`*8 + (`$1D87` & 7), `$1D0E`*8 + (`$1D89` & 7)).
  `$1D0A`/`$1D0E` are the camera's left and top edges in 8 px units. `$1D0C`/`$1D10` are the right and bottom
  edges. `$1D87`/`$1D89` are the BG1 HOFS/VOFS values written to the PPU, and
  `$1D8B..` hold BG2 and BG3. The scroll registers carry a ring offset for the VRAM window, so they are
  not map coordinates. *Verified* over 256 frames: Obj_ComputeScreenPos ($C0AB45)
  computes screen = obj_px - camera exactly this way. Example: camera (272,489), Crono at
  (408,624.9), screen (136,135).
- The camera follows the leader (`$0197`): Camera_CommitFrameDeltas ($C09175) and
  Camera_UpdateScroll ($C09AD3). Map bounds are set in Camera_InitMapBoundsAndMasks ($C07399) and
  clamped by Camera_ClampScrollX/Y ($C07506/$C07553). Camera zones are in the `$70C0` grid.

### A0.6 The object table (64 slots, index = 2*slot)

| Address | Meaning (verified where marked) |
| --- | --- |
| `$1100` | type: 0/1/2 = PC in party slot 1/2/3, i.e. leader/follower1/follower2 (EventCmd_LoadPcIfInParty $C0421E, which writes $80 if the PC is not in the party); 3 = PC loaded by the script as an actor (EventCmd_LoadPc $C04476); 4 = NPC; 7 = empty or placeholder; bit 7 = removed. **v** |
| `$1101` | PC id or NPC sprite id. **v** |
| `$1800` / `$1880` | X / Y in **1/16 px** (u16). The high byte is the 16 px tile, i.e. the foot point. **v** |
| `$0A00` / `$0A80` | screen X/Y (9 bit), active objects only. **v** |
| `$1900` / `$1980` | per-frame velocity dx/dy (signed, 1/16 px). |
| `$1600` | facing: 0 up, 1 down, 2 left, 3 right (EventCmd_ObjectFace* $C0556C... store it at C0554D). **v** (the leader walking up: 0) |
| `$1000` | flags (bit 7 kept, bit 0 set when a PC loads) |
| `$0F00` | bit 7 = active, i.e. near the camera. **v** |
| `$0F01` | current animation frame ($FF = never shown). **v** |
| `$1200` / `$1280` | 24-bit source of the sprite's tile graphics. PCs: ROM, e.g. $D272C0. NPCs: decompressed into `$7F8000-$7FFFFF` at map load. **v** |
| `$1500` / `$1480` | animation table pointer (bank $E4) / size. **v** |
| `$1680`/`$1681`, `$1601`, `$1780` | animation index, timer and mode. |
| `$0C00` / `$0C01` | OAM priority for the top and bottom halves ($20/$30). **v** |
| `$0D00` | OAM tile number of the object's VRAM sprite slot. **v** (OAM pieces match it) |
| `$1700` | VRAM offset of that slot. |
| `$1180` | script pointer (into `$7F2000+`); `$1080` = script priority queue link. |
| `$1A81`, `$1B01`, `$1C80` | drawn flag, collision flags, etc. |

**Party slots:**

- `$0197` = leader slot*2. `$0199` = follower 1, `$019B` = follower 2 ($80 = none). These
  are set by EventCmd_LoadPcIfInParty ($C0421E: `STX $97` / `STX $99`). **v** (leader 1, follower 1 = 3
  after the poke).
- Party PC ids are at `$7E2980-$7E2982`.

**Simulated map-wide?** Script slices (Field_ReadHVLatch $C05A46 to
EventScript_RunObjectScriptSlice $C05A93) and movement (Field_UpdateDrawObjects
$C0A810 to Obj_ApplyMoveVelocity) run for every slot, whatever the camera. **v**: in 256
frames at Leene Square, NPC 20 moved on 249 frames without ever being active, and NPC 8 moved on
234 frames while off-screen. Animation does *not* advance off-screen.
Obj_AnimTickAndQueue and Obj_WriteOamEntry are called only after Obj_UpdateVisibility
($C0A88D) succeeds. So an off-screen object keeps its last `$0F01`, or $FF if it was never shown.
**Activation** happens when the object comes within the camera window +4 tiles
horizontally, +2/+5 vertically. It assigns a sprite palette and a VRAM tile slot
(Obj_AssignSpritePalette $C07170, Obj_AssignSpriteTileSlot $C06F9A). PCs have fixed
slots. NPCs share the rest: slot owners are in `$0B88-$0B97`, about 16 slots.

### A0.7 OAM

- Shadow OAM is **`$7E0700-$7E091F`**. Vblank_UploadOam ($C0ECA3) sends it by DMA to OAM.
  **v**: identical to OAM in the `--vram` dump at frame 9200.
- It is **culled and y-sorted**. Only active objects write entries: Obj_WriteOamEntry ($C0A98A) and
  Obj_YSortListInsert ($C0A9CD), which use the screen-row buckets at `$0E00/$0E80`.
- A normal field sprite is four 16x16 pieces from its slot, tiles t, t+2, t+4, t+6, with
  t = `$0D00` & ~7. `scene_probe.py --oam` groups OAM entries by object this way without
  ambiguity (*v*, both dumps).
- Sprite graphics: each frame, the *current* frame's tiles are copied from `$7F`
  staging buffers (`$7F3800`... ) into the object's VRAM slot by DMA. The queue is at `$0940-$09AF`
  (Vblank_UploadSpriteTileQueue $C0B1B2). VRAM therefore holds one frame per active object
  and nothing for the rest.

### A0.8 Followers (field)

The leader's script runs EventCmd_ActAsPartyMember ($C03002) each frame and dispatches on `$1100`:

- 0: Obj_RecordMovement ($C09E29). Each step is appended to a 128-entry ring in `$7F0C00`
  (dx), `$7F0C80` (dy), `$7F0D00/$7F0D80` (pad intent), `$7F0E00/$7F0E80` (sprite priority) and
  `$7F0F00` (move flags). The head index is `$01AB`.
- 1: **Field_MoveFollower1 ($C0A26B)** replays the ring at index `$01AC`, keeping 16 entries
  behind, or 24 when running.
- 2: **Field_MoveFollower2 ($C0A2CE)** replays it at index `$01AD`.

*Verified:* at frame 8700, after the poke, the head is 88 and follower 1 is at 65. Follower 1
(obj 3) is about 1 tile behind the leader, and its priority entries match.
Followers do no collision of their own. They retrace the leader's path.

### A0.9 What A (3D rebuild) can use

Floors come from the walkable cells: solidity class 0, plus the shaped classes. Walls come from
class-1 cells next to walkable cells. A wall's *height* is not stored anywhere. Options:

- a fixed height per wall run;
- rows of class-1 cells above a walkable row, counted until a priority/overhead tile or the
  next walkable row;
- per-map overrides (data the user's build can generate, not shipped).

Roofs and overhangs come from BG priority-1 quarters of L1/L2. Bridges and balconies come from z-plane
2 cells. Stairs come from classes $18/$19/$1D (to confirm). Billboards come from objects
(position, facing, frame), with their pixels from the OAM pieces of active objects (A0.7).
The camera is A0.5. Everything is in WRAM/VRAM/CGRAM at the end of the frame, so
the module can fill the scene with no game hooks except the map-change counter.

## A1. Field object sprites: drawing any object's frame from ROM and WRAM

The game builds an object's frame only while the object is active, that is, near the camera (A0.6).
VRAM holds only those frames (A0.7). `game/ct/sprites.c` does the same assembly from the source data,
for **any** object, whether or not it is in OAM. `ct_sprite_frame(obj, rgba, max, &info)` draws the
current frame as RGBA. It also returns the frame's size, the foot point inside the image, its pieces,
its palette and a stable key. It only reads.
`game/ct/test/test_sprites.c` (ctests `sprites_leene` and `sprites_follower`) checks it
against what the console drew (A1.7). The module hands it to the engine as poses
(`ct_game_pose` in `game/ct/scene.c`, the optional `ct_module_pose` export in
`runtime/module.h`): asset packs replace a character's whole frame with one picture
by that key, and the sprite sheet export draws every pose seen
([ASSET-PACKS.md](ASSET-PACKS.md), "Poses").

### A1.1 The sprite record (bank $E4)

Each loaded object comes from a sprite record. The load routines read the record and fill the
object's arrays:

| Loaded by | Record | Object type `$1100` |
| --- | --- | --- |
| EventCmd_LoadPc ($C04476) and EventCmd_LoadPcIfInParty ($C0421E), through Obj_LoadPcSpriteRecord ($C0448D) | `$E4F000 + 5*id` (id = PC 0-6) | 0-3 |
| EventCmd_LoadNpc ($C04590) | `$E4F023 + 5*id`, i.e. record `id + 7` | 4 |
| EventCmd_LoadEnemy ($C046DF) | `$E4F600 + 10*id` | 5 |

The five bytes of a PC or NPC record are below. Enemy records use the same first five bytes.

| Byte | Meaning | Goes to |
| --- | --- | --- |
| 0 | graphics index g: a 24-bit pointer at `$E42000 + 3g` | `$1200/$1280` (A1.4) |
| 1 | assembly index a: a 24-bit pointer at `$E42300 + 3a` | `$1300/$1380`, the frame assembly table (A1.3) |
| 2 | palette index p | `$1400` = `$18 * p`; colors 1-12 are at `$E40000 + $18p` (A1.5) |
| 3 | animation index n: 16-bit pointers at `$E42600 + 2n` (frames) and `$E42800 + 2n` (durations) | `$1500` (frames) and `$1580` (durations). `$1480` = (next pointer - this one) / 4, the bytes per facing |
| 4 | frame format, bits 0-1 | `$1201` |

The four table pointers are the field's direct-page words `$01AF`, `$01B2`, `$01B5` and `$01B8`.
*Verified* in a WRAM dump at frame 8700: they hold `$E42000`, `$E42300`, `$E42600` and `$E42800`.
Among the first records, format 0 is by far the most common. Records 15, 94, 95, 99, 102, 163 and 176 use
format 1, and a few others use format 2.

### A1.2 The animation table: from animation state to frame number

Sub_CB3A ($C0CB3A) is called from the frame builders and selects the frame:

- frame = `$E4:[$1500 + facing * $1480 + 4 * anim + step]`, where facing is `$1600` (0 up, 1 down,
  2 left, 3 right), anim is `$1680` and step is `$1681`.
- So the table is four facing blocks of `$1480` bytes. Animation `anim` starts at `4 * anim` in its
  block. **An animation can be longer than 4 steps.** The walk cycle reaches step 5.
- A `$FF` byte ends the animation. The game goes back to step 0 and uses the byte at step 0. It sets
  `$1681` = `$FF`, which the next tick increments to 0.
- `$1780` is the animation mode. Mode 3 shows the fixed frame `$1301`. Mode 2 plays animation `$1781`,
  keeping a loop count in `$7F0B01`. On `$FF`, mode 2 keeps the frame it has.
- Obj_AnimTickAndQueue ($C0C98A) counts the timer `$1601` down. When the timer expires, the routine
  advances the step, reloads the timer from `$E4:[$1580 + 4 * anim + step]` (0 there means "back to
  step 0") and queues the object for a rebuild.
- **The game rebuilds a frame only on a tick.** So a change of facing or animation shows at the next
  tick. The test sees this once in 7 checkpoints.
- Only active objects tick (A0.6), so an off-screen object keeps its step. Its frame still follows any
  change of facing or animation that its script makes.

`ct_sprite_current_frame` returns `$0F01`, the frame last built, for an active object. For any other
object it returns the frame that this selection gives.

### A1.3 Frame assembly: pieces, offsets, flips and priority

Obj_BuildSpriteFrameStep ($C0CAD9) dispatches on the format `$1201 & 3`: format 0 to $C0CBDC, format 1
to $C0CEF5 and format 2 to $C0D4F7. Field_ProcessAnimQueue ($C0CA76) runs the build queue and stops when the raster
passes line `$6B`. The frame record is at `$1300:$1380 + frame * size`:

| Format | Pieces (16x16) | 8x8 tile words | Offset pairs | Record size |
| --- | --- | --- | --- | --- |
| 0 | 4 | 16 | 4 | `$28` (40) |
| 1 | 8 | 32 | 8 | `$50` (80) |
| 2 | 12 | 48 | 12 | `$78` (120). Built over several frames (`$1B00` steps). **Not decoded.** |

- **Tile words.** A frame of n pieces has 4n words. Each word holds a source tile number in bits 0-10,
  counted in 32-byte tiles from the graphics source, and an **H flip in bit 14**. Sub_E534 ($C0E534) copies a
  flipped tile through the bit-reverse table at `$C0FD00`, and Sub_E687 ($C0E687) copies the others as
  they are. No other bits are used, and there is no V flip.
- **Tile order.** The words are written in order to a WRAM staging buffer. The buffer is DMAed into the
  object's VRAM slot as two rows of 2n tiles. VRAM rows are 16 tiles wide, so the second row starts at tile t+16.
  So piece k is words 2k and 2k+1 (its top half) and words 2n+2k and 2n+2k+1 (its bottom half).
  Its OAM tile is `$0D00 + 2k`, a 16x16 OBJ.
- **Offsets.** After the tile words come n pairs (dx, dy) of signed bytes. $C0CCC5 copies them into an
  OAM template at `$7F4800 + $1700`, 8 bytes per piece: x as a sign-extended word, then y, tile and
  attribute. The OAM entry ends up at screen position (`$0A00` + dx, `$0A80` + dy). `$0A00`/`$0A80` is
  the foot point on the screen: the map position (`$1800 >> 4`, `$1880 >> 4`) minus the camera (A0.5).
- **Attribute.** The attribute is `$0F81` (palette slot << 1) | `$0D01` (tile bit 8) | priority.
  - A dy byte below `$E8` takes `$0C00`, the top half's priority.
  - A dy byte of `$E8`-`$FF` (-24 to -1, near the feet) takes `$0C01`, the bottom half's.
  - `$0C00`/`$0C01` come from the tiles under the object (A0.3).
  - No OAM flip bits are set.
- **Overlap.** Entry k comes before entry k+1 in OAM, so piece 0 draws in front.

### A1.4 Where the tile graphics come from

`$1200/$1280` holds the 24-bit source, the pointer at `$E42000 + 3g`:

- **PCs** (types 0-3): uncompressed 4bpp tiles in ROM banks `$D2-$D5`. Crono's are at `$D272C0`.
  Sub_E687 has one copy loop each for banks `$D2`, `$D3`, `$D4` and `$D5`.
- **NPCs and enemies**: the pointer points to compressed data.
  - EventCmd_LoadNpc runs Gfx_DecompressVector ($C30002) into `$7F:[$71]`. This is a heap in
    `$7F8000-$7FFFFF` that grows as the map loads objects.
  - Field_LookupOrRegisterId0920 ($C05C90) lets objects with the same graphics index share one copy.
  - Then `$1200` = `$7F` and `$1280` = the heap address.
  - The data stays there while the map is loaded, so an off-screen NPC's tiles are always in WRAM.
- **Per-frame DMA: yes.** On each rebuild, the CPU copies the frame's tiles through `$2180` into the
  staging buffer `$7F3800 + $200*i`. That address goes in `$0D80`. Sub_E952 ($C0E952) and Sub_E97A
  ($C0E97A) pick i, and `$0BC0` records each buffer's owner. The copy is then queued at `$0940-$09AF`,
  and Vblank_UploadSpriteTileQueue ($C0B1B2) sends it to the object's VRAM slot. Each active object
  has one VRAM slot (`$0D00`, `$1700`), which holds only its current frame.

### A1.5 Palettes

- Obj_AssignSpritePalette ($C07170) runs when an object activates.
  - It picks one of the 8 OBJ palettes and stores slot << 1 in `$0F81`. Objects whose `$1400` match
    share one, tracked in `$0B00/$0B80`.
  - It copies **12 colors (24 bytes) from `$E4:[$1400]`** with MVN into colors 1-12 of that palette in
    the CGRAM shadow at `$7E2000`. OBJ palette s is at `$7E2100 + 32s`.
  - It writes the same colors to the same place in a second copy, at `$7E2200`.
- *Verified* at frame 8700:
  - every OBJ palette in CGRAM equals the shadow at `$7E2100`;
  - Crono's colors 1-12 (`$1400` = 0) equal `$E40000-$E40017`;
  - the colors of NPCs 12 (`$0C78`) and 16 (`$00D8`) equal their ROM entries.
- Colors 13-15 are not loaded from the record.
  - NPC palettes have `$7C1F` there, an unused magenta. The party's palette has other colors there.
  - No pixel of any frame checked uses them. The test counts such pixels, and there were none.
  - `sprites.c` takes them from the CGRAM shadow while the object holds a palette slot. Otherwise it
    leaves them at 0.
- Color 0 is transparent.

### A1.6 The key for texture packs

`info.key = kind << 16 | id << 8 | frame`. kind is 0 for a PC, 1 for an NPC and 2 for an enemy. id is
`$1101`. The key is the sprite record plus the frame number in its assembly table. So one pose has
one key on every map and in every slot.

- A pack replaces a pose as one image of `info.w x info.h`, anchored at the foot point
  (`info.origin_x/y`).
- The palette source is given separately (`info.palette_src`), for packs that want recolors to get
  their own key.
- Examples from Leene Square: `000018` is Crono's frame `$18`, `000204` is Lucca following him, and
  `014F01` is NPC `$4F`'s frame 1.

### A1.7 Verification (test_sprites)

The test plays `leene_square.txt`. At each checkpoint frame's edge it decodes every object. It then
checks each object that the scene (`ct_scene`, game/ct/scene.c) lists with OAM entries:

1. **Pieces.** The decoded image is placed at the object's screen foot point (`$0A00/$0A80`). Each
   on-screen pixel must have the same color index as the object's own OAM entries. Those entries are
   drawn from VRAM with the console's OBJ size, name base and flips. Pixels covered by another sprite or
   by a BG count too.
2. **Colors.** Each color index used must have the same color as the entry's palette in CGRAM.
3. **Screen.** The test takes the PPU's output for the *next* frame. Where the layer buffer shows OBJ and
   the front OAM entry belongs to this object, the pixel must have the decoded color with brightness
   applied. The next frame is the right one because OAM and tiles at frame F's edge are uploaded in F's
   VBlank and drawn in F+1. Pixels changed by color math are skipped, and there were none.
4. **Frames.** From 300 frames before the first checkpoint onward, the test watches every frame.
   Whenever the game builds a new frame for an active object, the frame that the animation state selects
   (A1.2) must be the one built. At the checkpoints themselves, a difference is only reported, since it
   is a change that waits for the next tick.
5. **Off screen.** Each object off the screen must decode. When it later appears on screen with the
   same key, its image must be identical.

Results. Both ctests pass, and every count below is all equal:

| Run | Checkpoints | On screen | Piece pixels | Screen pixels | Builds checked |
| --- | --- | --- | --- | --- | --- |
| `sprites_leene`: walking in Leene Square, then the bell plaza | 8500 8600 8650 8700 8805 9100 9200 | 4-11 per checkpoint | 15216 | 14685 | 398 |
| `sprites_follower` (`--poke 8000:2981^82`: Lucca follows Crono) | 8600 8650 8700 | 10-11, Lucca among them | 7667 | 7132 | 261 |

**Off-screen decoding.** Six NPCs were decoded off screen *before the game had ever built a frame for
them* (`$0F01` = `$FF`): 14, 21 and 23 in Leene Square, and 16, 17 and 31 in the bell plaza. Each later
appeared on screen with the same key and an identical image. With `--png DIR`, the test writes every
decoded frame, on screen or off, as a PNG. These images are ROM-derived, so keep them in the build tree.

### A1.8 Limits

- Format 2 (12 pieces, built over several frames) is not decoded, and `ct_sprite_frame` returns -1 for
  it. No format 2 object was on screen in the runs checked.
- Format 1 (8 pieces, rows of 16 tiles) is read the way the code builds it: $C0CEF5 uses `$50`-byte
  records and `$200`-byte rows. No format 1 object appears in the replays, so it has not been checked
  against the PPU.
- Colors 13-15 of an off-screen object are unknown and come back as 0. No frame checked uses them.
- An off-screen object's frame is the one its frozen animation state selects. On activation the game
  shows that same frame at its first tick, though by then it may have advanced the step once. A second
  view that wants the object animated must advance the step itself, from the duration table (A1.2).
- The pieces carry the top/bottom priority choice (`info.piece[k].top`). Drawing behind BG priority
  tiles is left to the renderer.
- Palette changes made by events, and sprites drawn by other systems (battle, the world map), are not
  covered.

### A1.9 Proposed engine-facing interface (not in module.h; no CT_MODULE_ABI change yet)

The coop and scene3d branches will wire this up. It is keyed on the `ct_scene` object list, so the
engine never sees game slots:

```c
/* One object's current animation frame, as the module draws it from
   game data (not from OAM/VRAM): for objects off the camera, a second
   view, and whole-pose texture replacement. */
typedef struct {
    uint16_t w, h;            /* image size, px */
    int16_t  origin_x, origin_y;   /* the object's foot point inside it */
    uint64_t key;             /* stable per pose (module-defined; this game:
                                 kind << 16 | id << 8 | frame) */
    uint32_t palette_key;     /* separate, for recolors (0: none) */
    uint8_t  from_live;       /* 1: the frame the game shows now; 0: derived */
    uint8_t  n_bands;         /* priority bands, top to bottom */
    struct { int16_t y0, y1; uint8_t priority; } band[4];   /* OBJ priority 0-3 per row range */
} ct_sprite_image;

/* Draw object `index` of the last ct_scene as RGBA8 into rgba (max
   pixels); 0, or -1 if the module can't draw it (then fall back to OAM). */
typedef int (*ct_module_sprite_fn)(int index, uint8_t *rgba, int max, ct_sprite_image *out);
```

- At the next ABI bump this goes in `ct_module_info` after `scene`, as `sprite_size` and `sprite`.
  Until then, the static programs can call `ct_sprite_frame` directly.
- The module maps a scene index to a game slot. scene.c lists the non-empty slots in order, the same
  walk that test_sprites does.
- The bands carry this game's split priority: `$0C00` for pieces with dy < -24 and `$0C01` for the
  rest. With them, a renderer can draw the upper body over a priority-2 tile and the feet under it.

## B0. Split-screen gate

**Can the engine draw a second view of the same map with a different camera, from
WRAM/VRAM alone?**

| Part | From resident data? | Notes |
| --- | --- | --- |
| BG1/BG2/BG3 | **Yes** | WRAM grids + metatile tables + VRAM tiles + CGRAM (A0.2, A0.4). The engine composes its own tilemap. The VRAM window is never used. |
| Animated tiles and palettes | Yes | They are shared, and changed in VRAM/CGRAM. |
| Object positions | **Yes** | Map-wide (A0.6). |
| Sprite pixels for objects near camera 2 | **No** from VRAM; **yes** from ROM/WRAM through `game/ct/sprites.c` (A1) | Not in VRAM unless the object is also near camera 1. Graphics exist in ROM/`$7F8000+` (`$1200/$1280`), but the frame layout comes from the bank-$E4 animation tables and the per-frame assembly code (Obj_BuildSpriteFrameStep $C0CAD9). |
| Their animation frame | **No** | Frozen while inactive. |
| Their palette | **No** | Only active objects get a sprite palette slot in CGRAM. |
| HDMA, window and color-math effects, L3 fog | Approximate | They are screen-relative and tied to camera 1. |

**What breaks, and the minimum hooks:**

1. **Sprites in view 2.** There are two ways:
   - **(a) Widen activation.** Hook Obj_UpdateVisibility and Obj_RefreshVisibilityWindow so the
     window is the union of both cameras' windows. Cheap, and frames, palettes and VRAM slots then exist. Limits:
     about 16 VRAM sprite slots, 8 OBJ palettes, 128 OAM entries and the vblank DMA budget
     (the recomp can raise the last). It also changes game state: activation runs activate
     scripts, so a run with it no longer matches the original. That is acceptable in co-op mode only.
   - **(b) A module-side sprite rasterizer.** Recommended for the long term. Read `$1200/$1280`
     (gfx), the bank-$E4 tables (frame to pieces), the palette source and the frame number. Advance the
     animation in the module for objects in view 2 only. Game state is unaffected.
2. **Camera 2.** The engine or module computes it, following P2's object, clamped like
   Camera_ClampScrollX/Y. It needs no game hook.
3. **Leader-bound logic.** Exits (`$7080` grid), touch triggers (byte A bit 4), talking
   (Player_FindFacingObject $C01CFC), the camera, and event and battle starts all use `$0197`
   only. P2 cannot take an exit or start an event. A **tether** rule is needed:
   when the leader changes map, enters an event (player control off) or starts a battle, snap P2 back
   to the follower ring (zero `$01AC`, as Field restore does) and merge the views.
4. **Map changes** reload everything for the leader's map only. Both views must be on the same
   location. There is a map-generation hook (A0.1).
5. **Scripted follower pathing.** In cutscenes the event script moves PCs explicitly
   (EventCmd_MoveToCoords, FollowPc and so on). P2 input must be ignored while the event system
   owns the object.

**Verdict: PARTIAL.** The background, map and positions are fully resident, so a second
view's world is a pure renderer job. Sprites need hook 1(a), or the rasterizer in 1(b). P2 cannot
trigger map logic, so the views must be tethered on exits, events and battles.
Minimum for a playable prototype: the `ct_scene` export, the map-generation counter, hook
1(a), and the tether in 3.

## B1 prep

(Built since: `game/ct/coop.c` has B1, B2 and the B3 prototype, described in
[USER-GUIDE.md](USER-GUIDE.md#two-players-co-op) and [MODULES.md](MODULES.md#game-patches).
The tether lets go of follower 1 when the game places it (a map change), snaps it back
into line after a battle on the same map or at an event, and uses `$0138` as the event
flag. Sprites for the second view use hook 1(a).)

### Battle: whose menu, and where the pad is read

- **Active command menu: `$7E95D5`.** It holds the party slot (0-2) of the PC whose command menu is
  open, or $FF if none. *Verified* (gato_win, `--watch 95D5:2`): it goes $FF to $00 when Crono's
  gauge fills (frame 10753, $C1058A) and back to $FF after the command (frame 10821,
  $C11732). Related: `$A6D9-$A6DB` is the list of ready PCs, `$A6DD` the cursor in that
  list (BattleMenu_CycleActivePcPrev/Next $C111E1/$C11218, driven by left/right), and
  `$95D6-$95D8` the ready queue (BattleMenu_DequeueReadyBattler $C11B67).
- **Pad path (all WRAM copies; the game reads `$4218` once per frame):**
  1. Pad_ReadAndDecode ($C284EC) reads `$4218/$4219` (pad 1) into `$00F2/$00F3` (previous value in `$00FE`).
     Menu_VblankInputService also copies pad 2 (`$421A`) to `$041F`. The game never uses it.
  2. Pad_ApplyButtonRemap ($C28545) applies the player's button config (`$2993-$299A`) and
     writes **`$00F8/$00F9`** (held, remapped). Field movement reads `$00F9`
     (Player_ApplyPadMoveIntent $C088EE).
  3. In battle, BattleSys_UpdatePadRepeat ($CD0340) turns `$F8` into new presses with
     auto-repeat (`$CC97-$CC9B`) and ORs them into **`$00EE/$00EF`**.
     BattleMenu_ProcessInput ($C11153) and the list and target handlers read those bits: `$EE`
     $80 = A, $40 = X; `$EF` $08/$04/$02/$01 = up/down/left/right. Battle_ZeroResultEE
     ($C1179C) clears them. *Verified:* `$EE` becomes $80 at $CD0389 on the A presses in gato_win
     (frames 10780, 10820, ...).
- **B1 hook:** at the entry of BattleSys_UpdatePadRepeat ($CD0340), when `$95D5` is 1 or 2, give
  it P2's pad, remapped the same way, instead of `$F8`. Also mask P2's left/right so P2 cannot
  cycle to slot 0 (or let them cycle only among slots 1 and 2), and give P1 nothing while P2's menu is open. The engine already
  keeps four ports (`sched_set_joypad(port, ...)`). No battle-logic change is needed.

### Field: the follower routine to hook

- **Field_MoveFollower1 ($C0A26B)** (follower 2: $C0A2CE). It is entered from
  EventCmd_ActAsPartyMember ($C03037/$C03027) with `$016D` = the object. A P2 hook replaces
  the ring replay: it sets `$1900/$1980` from P2's d-pad and sets facing and animation the way
  Player_SelectMoveAnim ($C09E84) does for the leader.
- **Collision gap:** the leader's solidity test is inside Camera_UpdateScroll ($C09AD3)
  and the camera stepping (Camera_MoveDispatch $C08A6D), which also scroll the camera.
  Generic object movement (Obj_ApplyMoveVelocity $C0AA07, collision branch C0AD90) only
  honours byte B bit 7 (NPC-blocking). A P2 follower therefore needs either a module-side
  solidity check (read `$30C0` class and the z-plane, A0.3) or a refactored copy of the
  leader's test. On returning control, reset the ring indices so follower replay resumes
  cleanly (Obj_RecordMovement's reset path at C09DC3 zeroes `$01AB-$01AD`).

## Text boxes and sound commands (HD text, audio packs)

Read with `ct_boot --log-entry` on `leene_square.txt` (the first dialogue, frames
4699-4800) and `gato_win.txt` (songs and fades through frame 14,000), `--prov` dumps of
frame 4800, and `ldis.py`. **Verified**: a run agrees. Used by `game/ct/text.c` and
`game/ct/audio.c`.

### T1. The text engine (verified)

- `Text_EngineTick` (`$C25823`, through `$C20009`) runs with D = `$0200` and dispatches on
  the text state `$0215` (`Text_DispatchState`, `$C2584A`) and the mode `$0230`
  (`$C25839`). `Text_DecodeLoop` (`$C258B2`) reads the string at the long pointer
  `$0231`: a byte `$A0-$FF` is a glyph, `$21-$9F` a dictionary substring (pointer table
  at `$DEFA00`, played through `Text_PlaySubstring`, `$C25BF5`, which comes back into
  the loop), below `$21` a control code (table at `$C25903`: names, numbers, line
  breaks `$05`-`$08`, page breaks `$09`-`$0C`, waits).
- Every glyph goes through `Text_EmitGlyph` (`$C25DC4`): code in `$0235`, the pen's x in
  `$0234` (px from the line's left), the canvas pointer in `$0210-$0212`, the format in
  `$0214` (low nibble 0: 2 bits per pixel, `Text_DrawGlyph2bpp`; else 4,
  `Text_DrawGlyph4bpp`), the line's glyph count in `$0217`. The advance is the byte at
  `$C260E6 + code - $A0`. Glyphs are 12x12: the left 8 columns at `$FF2060 + 24 * code`
  and the right 4 at `$FF3860 + 24 * (code / 2)` (high nibble for even codes), 2 planes
  row by row.
- The encoding: `$A0-$B9` A-Z, `$BA-$D3` a-z, `$D4-$DD` 0-9, then
  `! ? / “ ” : & ( ) ’ . , = - + % ♪ (space) ♡ … ∞ #` from `$DE` to `$F3`.

### T2. The dialogue box (verified)

- `Dialog_RenderRequest` (`$C020F2`, D = `$0100`) points the engine at the canvas
  `$7EF000` (2 bits per pixel). The canvas is one line: 32 x 2 tiles. The column table
  `$C25FE6` puts tile column c at byte 16c (c < 16) or 512 + 16(c - 16), the lower tile
  row 256 bytes after the upper; glyph rows start 4 px down. `Dialog_InitWindowArea`
  (`$C020B2`) clears it for each line.
- The line is `$0131` (0-3; `Dialog_HandleRenderResult`, `$C02126`, steps it on a line
  break and goes back to 0 on a new page). Line L is uploaded to BG3's characters
  `$100 + 64L` (VRAM `$5800 + $200 L` in the field) and `Dialog_BuildTextAreaTilemap`
  (`$C02B78`) lays the tiles out in that order. The glyphs are BG3 palette 2's color 3,
  the shadow color 1; the window's textured fill behind them is BG1.
- Frame 4800, the box at the bottom: lines at y = 137, 153, 169, x = 0. At 6000 the box is
  at the top. The provenance G-buffer finds them wherever they are.

### T3. Sound commands (verified)

- The game writes a command and three arguments to `$7E1E00-$1E03` and calls `$C70004`
  (`Audio_ProcessCommand`, `$C70140`, D = `$1E00`), which sends them through `$2143`,
  `$2142`, `$2141`, `$2140` (`$C7061C`; for a song `$C70A39`, after
  `Audio_NormalizeSongCmd`, `$C70734`, and the song's sample upload, `$C70655`) and clears
  `$1E00` when it is done (`$C70194`). Commands: `$10 $11 $14 $15` play song `$1E01`
  (`$10`-`$17` through the table at `$C70AD9`, `$12 $13 $16 $17` do nothing; song 0 is
  silence); `$18`-`$2F` sound effects; `$81` volume (`$1E01` steps of about 2.06 frames,
  `$1E02` the target 0-255); `$F0` stop. A load takes the CPU 5-60 frames (song `$30`,
  the title: 36; `$45`, the Gato fight: 61); the song starts when it ends.
- Songs seen on these replays: `$30` (title), `$11` (opening), `$12`, `$0F`, `$1E`,
  `$06`, `$42`, `$45` (the fight). The same song asked for again (`$06` at 8078, 8981,
  10002, a map change) plays on.
- Effects take voices from the music: `$18` effects voices 6-7, `$19` voices 4-5; APU RAM
  `$86` is their bit mask while they play (`$C0`, `$40`, `$30`, `$20` seen), and music
  key-ons on those voices wait.

## World map

The world map is a different engine from the field. It is the bank-$C2 "menu shell"
(funcs.toml names it `Overworld_*` and `Menu_*`), not the field loop. Everything below was
read on `world_map_save.txt` (frames 7252-7720, location `$1F0`, "1000 A.D."), with the
main repo's `ct_boot`. Method as above: WRAM, VRAM and PPU-write dumps (`--wram`,
`--vram`, `--log-ppu`), `--watch`, `--snap-at`, and `ldis.py` for the code. **Verified**
means a run or a dump agrees. **Code** means read from the disassembly only. The probe is
`game/ct/tools/worldmap_probe.py` (end of this section). World-map code runs with D = $0000.

### W1. Video mode: mode 1, no HDMA, no mode 7 (on `$1F0`)

- `$2105` = `$09`: **mode 1 with BG3 priority**. *Verified* (`--log-ppu 2105:2133@7599`,
  one frame of every PPU write): `$211B-$2120` only ever get the identity values the NMI
  shadow holds. No mode 7 sampling, no window, no mosaic.
- **No HDMA.** The NMI handler clears `$420C` (C20181) whenever it updates the screen,
  and the logged frame has no PPU write outside the NMI's own timing (lines 228-230).
  The world map has nothing per scanline.
- Layers: `$212C` = `$17` (BG1, BG2, BG3, OBJ on the main screen); `$212D` = `$13`
  (BG1, BG2, OBJ on the subscreen); `$2130` = `$02` (color math against the subscreen);
  `$2131` = `$44` (half-add, **BG3 only**). So BG3 is a translucent **cloud layer** blended
  with what is under it. BG1 draws over BG2.
- The NMI at `$C2015E` copies a DP shadow to the PPU every frame, so read the shadow:
  `$1E` BGMODE, `$20/$22/$24` BG1/2/3 HOFS, `$26/$28/$2A` BG1/2/3 VOFS, `$2C-$37` the
  mode 7 matrix and center (identity, unused), `$38-$49` window, layer and color-math
  registers. Set once at entry (frame 7252) and left alone, *verified* with
  `--log-ppu 210B:210C@1`: `$2107-$2109` = `$61`, `$69`, `$79` (BG1 map at VRAM word
  `$6000`, BG2 `$6800`, BG3 `$7800`, each 64x32); **`$210B` = `$22`: BG1 and BG2 tiles
  (4 bpp) at word `$2000`**; `$210C` = `$77`: BG3 tiles at `$7000`; `$2101` (OBSEL) =
  `$60`: OBJ tiles at words 0 and `$1000`, small objects 16x16, large 32x32.

**Other world maps, and mode 7.** The overworld ids are `$1F0-$1F7`
(Overworld_SetMapDescPtr C2274D reads `$C6FD00 + 2*(id-$1F0)`). The eight descriptors
all decompress to the same sizes (tile defs 4096, map 12288, props 512, regions 3072), so
one engine and one layout apply; only `$1F0` was run. A **separate mode 7 engine** exists:
`Mode7Engine_Entry` ($C30000) is entered by EventCmd_Mode7Scene ($C03FAF) with a scene id
below `$90`. It decompresses a 34,175-byte overlay from `$C31313` into `$7E3000`
(ct_decompress.py), installs its own NMI (`$C30548`, `$C30529`) and dispatches on
`id & $1F` through a 16-word table at `$7E3043` (11 distinct handlers). The overlay writes
`$211B-$2120` at 30+ sites (e.g. `$7E3096`, `$7E565A`, `$7E5C59`, `$7E73D5`, `$7E9E31`),
writes `$420C` at seven (`$7E879E`, `$7E87DA`, `$7E8A18`, `$7E8B0C`, `$7E8BC0`, `$7E8C25`,
`$7E8CDD`) and sets HDMA channel registers at 13, so it does use per-scanline matrices.
**No replay reaches one of these scenes** except the title (frames 65-1516: the pendulum
and logo, no mode 7). A one-byte poke does reach the others: see W11 for the scene list,
which two scenes use mode 7 (a bike race, the Epoch cockpit), and their per-line tables. docs/PRIOR-ART.md's
"the world map is partly mode 7" does not hold for any of the eight overworlds (W13: all are mode 1).

### W2. Where the map and its graphics live

All eight maps share one layout in bank $7E, loaded by Overworld_LoadAllMapGfx (C22CC1)
from the descriptor at `$ED` (`$C6:$FD10 + $17*n`; `$00ED` held `$FD10` for `$1F0`). Each
piece is a Gfx_DecompressVector call (`$C30002`) with the destination in `$0303-$0305`:

| What | Where | Size | Source (descriptor byte, table) |
| --- | --- | --- | --- |
| BG1 metatile definitions | `$7E3000` | 256 x 8 bytes | `+$10`, `$C6FF00` (one 4 KB blob, both halves) |
| BG2 metatile definitions | `$7E3800` | 256 x 8 bytes | (second half of that blob) |
| **BG1 map**, sparse (0 = nothing) | `$7E4000` | 96 x 64 bytes | `+$11`, `$C6FF20` (one 12 KB blob, both layers) |
| **BG2 map**, the ground | `$7E5800` | 96 x 64 bytes | (second half) |
| Terrain properties | `$7E7000` | 512 bytes | `+$12`, `$C6FF80` |
| Region nibbles | `$7E7200` | 64 rows x `$30` bytes | `+$13`, `$C6FFA0` |
| Exit records | `$7E7E00` | 7 bytes each, count `$7E1B38` | `+$15`, `$C6FFC0` (staged at `$7F0400`) |
| Event triggers | `$7E8300` | 3 bytes each, count `$7E1B39` | same blob |
| BG tile graphics | VRAM word `$2000` | 4 x `$1000` bytes, staged at `$7F9000` | `+$0C..+$0F`, `$C6FDF0` |

- **96 x 64 metatiles of 16 px = 1536 x 1024 px**, the whole map in WRAM, and
  **toroidal**: Overworld_WrapStepTarget (C23A8F) wraps x at `$0600` and y at `$0400`, and
  the camera wraps at 192 x 128 tiles of 8 px (Overworld_InitCameraScroll C22720). A map
  byte is a metatile index.
- A metatile definition is four SNES tilemap words in the order **TL, TR, BL, BR**
  (tile 0-9, palette 10-12, priority 13, flips 14-15), as in A0.2.
- **How the layer assignment was verified.** On the dump at frame 7600, for every non-empty
  word of the BG1 tilemap in VRAM (`$6000`, 64x32) and of BG2 (`$6800`), expand the WRAM map
  with each candidate map base (`$4000`/`$5800`), definition base (`$3000`/`$3800`) and
  quadrant order (all 24), and count the words found at the matching 8 px position
  (columns mod 64, rows mod 32). BG1 = `$4000` + `$3000` + TL,TR,BL,BR: **307 of 307**.
  BG2 = `$5800` + `$3800`: **1152 of 1152**. Every other choice matched about half or less.
- **VRAM holds every tile the map uses**, so `worldmap_probe.py --png` redraws the whole
  1536x1024 world from WRAM + VRAM alone (checked by eye against `--shot` screens). The BG
  tilemaps in VRAM are only the camera window, streamed as the party walks
  (Overworld_ScrollLayer*, Overworld_Build*Strip*, C20568-C20ADC), as in the field.
- BG1 holds the trees, mountains and buildings over the ground: 5,280 of its 6,144 cells
  are empty on `$1F0`. Water/tile animation and palette cycling were **not examined**.
- Region nibbles: `$7200 + y*$30 + x/2`, high nibble for even x (Overworld_GetRegionId
  C262ED; `$00` = x metatile, `$01` = y metatile). Mostly 0 on `$1F0`, with ids 1-2 in
  places. What they drive (music? the place name?) is **not verified**.

### W3. Terrain properties

`$7E7000` has two bytes per **ground** metatile index (the `$5800` map byte). The byte is
`idx*2 + (tile_y & 1)` and the nibble is the high one for even `tile_x`, the low one for
odd, with x, y in 8 px tiles (a 192 x 128 grid): Overworld_FetchTileProps C239D5 with
HwMath_MulBy60 C262AE. The properties come from BG2 only. A nibble:

| Value | Meaning | Verified |
| --- | --- | --- |
| 0 | walkable | yes |
| bit 0 (1) | blocked: mountains, dense forest, the shore band | yes (runs below) |
| bit 1 (2) | water: blocked on foot (C239C3 tests bits 0-1); every ocean tile is 2, **and so are the upper parts of mountains** | code + the data |
| bit 2 (4) | cover: walkable, the party sprite draws partly hidden (C239AE) | yes: stepped through |

*Verified* by laying the classes over the `--png` picture (the 3D world map work): the
peaks of every mountain on `$1F0` are 2, not 1, so "2" means "not on foot" (sea or high
ground) and only the picture tells water from rock. The cover cells (4) are the bottom
rows of towns and the castle's gate, where the walker passes behind the town's art, not
woods. A block of metatiles south of the middle (about metatiles 39-57 x 50-63) is drawn
as open sea but has land classes (0 and 1); the party can't reach it.

**Collision rule.** When the party steps to tile (x,y), the game fetches the nibbles of
(x-1,y-1), (x,y-1), (x-1,y), (x,y) into `node+$22..$25` and refuses the step if either of
the two **upper** ones (row y-1) has bit 0 or 1 (C239C3: `AND #$0303`). Four runs on `$1F0`
from tile (55,35), one held direction each (`--script`, then `--wram`): *right* stopped at
tile 56 (the target's probes were `[0,1]`); *up* stopped at row 34 (probes `[1,1]`);
*left* stopped at tile 46 (probes `[1,0]`); *down* went on to row 36 (probes `[0,0]`).
The class-4 tiles at (55,34) were walked through. The rule predicts all four stops.
`worldmap_probe.py --terrain` draws the classes: sea blue, land green, mountain, forest
and cliff band red, small rectangles at towns and castles yellow.

Bit 0 does not tell a mountain from a forest from a cliff. The renderer needs the ground
metatile index (which picture it is) for that.

### W4. Camera and scroll

- Party position in world px: `$7E0283` (x), `$7E0285` (y), u16. Tile position (8 px):
  `$0102/$0103` (Overworld_StorePartyTilePos C23ACB). The party **moves 1 px per frame in
  8 px steps** (Overworld_SetStepTarget C23A56 targets ±8 px).
- **Camera top-left, in world px, = (px_x - 128, px_y - 112)**, mod (1536, 1024): the party
  is always at screen (128,112). *Verified* over 100 frames of walking (`--snap-at
  C2015E@7330`): BG1 HOFS 312, 315, 318, 320 for px 440, 443, 446, 448; VOFS 168, 171, 174,
  176 for py 280, 283, 286, 288. `$E3/$E7` hold that / 8 (u16). The BG1 shadow `$20`/`$26`
  is the exact camera until the wrap (the PPU keeps 10 bits), so derive it from `$0283`,
  `$0285`.
- BG2 scrolls with BG1. **BG3 (the clouds) scrolls alone** (`$24`, `$2A`; the dump had
  HOFS `$FFB4`, VOFS `$26`).

### W5. Party marker, other sprites, vehicles

- **Sprites are plain OAM.** The NMI (C20192) DMAs `$7E0700` (`$220` bytes) to OAM each
  frame; the shell fills it from its sprite-node list (Menu_BuildSpriteOam C20D08, shared
  with the menus). So a renderer takes them as in the field scene: OAM entries, VRAM tiles
  (OBJ at words 0 and `$1000`), CGRAM `$80+`. All pieces are 16x16 (OBSEL `$60`).
  **Verified** on the dump: the party is OAM entry 7, tile `$10E`, palette 4, priority 2,
  at screen (120,95), i.e. world position - camera - **(8,17)** (440,280 - 312,168 =
  128,112). A 4-piece ferry (entries 3-6, palette 2, at screen 168..184 x 148..164) is an
  ordinary sprite object, not part of the map. The "1000 A.D." caption is 3 entries at
  y = 192, priority 3.
- Sprite graphics: Overworld_LoadPartySpriteGfx (C22B61) decompresses a sheet to `$7F9000`,
  copies a 1 KB page per party member (ids from `$7E2980-$7E2982`, as in the field) to
  `$7FB000/$B400/$B800` and a palette row each to `$7FBC00/BC40/BC80`; a DMA puts them in
  VRAM word `$1000+`. Code, consistent with the dump (tile `$10E` is in the second table).
- The leader's node is at **`$7E0D70`** (pointer in `$4E`): `+$14` x px, `+$18` y px,
  `+$1C/$20` velocity, `+$22..$25` the four terrain nibbles, `+$26`, `+$28` animation and
  facing, `+$2A/$2C` the step target. *Verified:* `+$14/+$18` equal `$0283/$0285` through
  a walk (443, 446, 448). `$28` read 1 walking down and 3 walking right; the order
  0 up, 1 down, 2 left, 3 right is likely (C239AA) but 0 and 2 were not seen.
- **Followers** (code only: every replay has a one-PC party): each step the leader pushes
  its position in a 16-entry ring, x at `$7E1B5B`, y at `$7E1B7B`, head byte offset
  `$7E1B5A` (Overworld_PushPosHistory C23934, init C2391C). Magus (PC id 6) has his own
  animation offset (+12, C23974).
- **Vehicles (Epoch): not found** (W12 has what was checked since). The 1000 A.D. dump has no vehicle but the ferry, and no
  vehicle state byte was identified. World event flags are cached from `$7F01F0` (16
  bytes) to `$7E1BA7` (Overworld_CacheEventFlags C226D6; `$7F0000` goes to `$1BA6`), so
  the Epoch is probably a flag there plus its own sprite, or a mode 7 scene (W1). Reaching
  it needs a save with the Epoch. Treat it as unknown.

### W6. Entrances (exits) and event triggers

- **Exit records**, `$7E7E00 + 7*i`, `i < $7E1B38` (80 slots on `$1F0`, 77 active):
  `+0` bit 7 = active, bits 0-6 = x; `+1` bits 0-5 = y (both in 16 px **metatiles**);
  `+2` a parameter (to `$1B58`, Overworld_FetchExitParam C26263); `+3/+4` the destination
  location id (word & `$1FF`), `+4 >> 1` its facing (`$0104`); `+5/+6` the destination
  field tile x, y (`$0102/$0103`) (C224A5-C224C4, the world-to-field `$0100` write).
  *Verified:* the record at (27,17) says location `$001`, Crono's house, which is where
  the replay came out of (`--watch 100:2`: `$001 -> $1F0` at frame 7252, and the party
  stands at world px 440,280 = metatile 27,17). Neighbouring cells repeat a destination
  (a town covers several metatiles). Others on `$1F0`: (26,14) -> `$005`, (26,16) -> `$00F`.
- **Detection:** the party's pixel position / 16 is compared with every active record
  (Overworld_FindTriggersAtPos C232CD, called at C23299; the match index goes to `$7E1B32`,
  `$FFFF` = none), then Overworld_DispatchTriggers (C23349) acts. An entrance is the
  **metatile under the party**: no facing, no button, no terrain class. Two more tables use
  the same scheme: event triggers at `$7E8300` (3-byte records, count `$1B39`; the active
  bit is cleared when one fires, so each fires once) and a third at `$7E8180` (count
  `$1B3A`). Those two are read from code only.

### W7. What identifies the world map

- **NMI vector `$7E0500`** (a JML to the loop's NMI): `$C2015E` while the world map runs.
  *Verified* with `--watch 501:3` over 7,800 frames of `world_map_save`: `$C0EA63`
  (field), `$C30548`/`$C308C9` (the opening's engine, frames 65-1516), `$C2840E` (the name
  entry, and the pause menu), `$C2015E` for frames 1946-3543 and 7252-7720, `$C2840E`
  again from 7720 when the menu opens. So the vector alone separates the world map from its
  pause menu. Menu_InstallInterruptVectors (C20110) installs it.
- Also read `$7E0100` in `$1F0-$1F7` for the map id. A map-change counter: hash
  `$7E3000-$7E6FFF` (16 KB), as the field one hashes its grids. Nothing writes those while
  walking; a reload under the same id re-runs Overworld_LoadAllMapGfx (C22CC1).

### W8. Proposed `ct_scene` extension for the world map

**Since implemented** (ABI 7), in this form: `mode` `CT_SCENE_WORLD`; 8 px cells (one tile
each) at row stride `CT_SCENE_WORLD_STRIDE` (256) for the cells and the tilemaps;
`CT_CELL_COVER`; exits as `CT_CELL_EVENT` cells (no separate list: the renderer needs only
where they are); `wrap`, and BG3 as an *overlay* (`layer[2]`, `overlay_w/_h` in tiles,
`overlay_x/_y` its scroll, `overlay_blend` in sixteenths) at the end of `ct_scene`; and
four blocked kinds a module can name (`CT_CELL_MOUNTAIN`, `_FOREST`, `_SHORE`, `_FLAT`).
`game/ct/world_scene.c` fills it; terrain 2 maps to water and 1 to solid, and the
renderer looks at the pixels for the rest (W3: 2 is also mountain tops), with a
per-metatile `relief[]` correction table. The party is object 0 (its pieces: the
entries at its sprite's place, W5); the other play-area entries are grouped into
objects by touching pieces; the era caption (y = 192) belongs to none, and the renderer
lays it flat. `worldmap_probe.py --text` decodes the same records independently for the
`scene_decode` test. The renderer is HD2D.md, "The world map". The original proposal:

The current struct (`runtime/module.h`, ABI 6) already has most of it: layers as expanded
8x8 tilemaps (`tiles`, `chr_base`, `bpp`), `cells` with a class, objects as OAM entry
lists, the camera, `map_id`/`map_generation`. What is missing is a mode, bigger and finer
maps, a wrap flag, the exits and the cloud layer. Proposal (a `CT_MODULE_ABI` bump):

```c
enum { CT_SCENE_OTHER = 0, CT_SCENE_FIELD = 1, CT_SCENE_WORLD = 2 };   /* ct_scene.mode */
/* The map side limit becomes 192 x 128 for CT_SCENE_WORLD (8 px cells), 64 for the field. */
enum { CT_CELL_COVER = 1 << 2 };   /* ct_scene_cell.flags: walkable, the walker draws partly hidden */

typedef struct {
    uint16_t x, y;                 /* in 16 px metatiles, as the game keeps them */
    uint32_t dest_map;             /* the game's id for where it leads */
} ct_scene_exit;

typedef struct {                   /* valid when mode == CT_SCENE_WORLD */
    uint8_t  wrap;                 /* the map wraps at both edges */
    uint8_t  n_exits;
    int32_t  party_x, party_y;     /* world px of the party's foot point (the camera centre) */
    int32_t  cloud_x, cloud_y;     /* layer[2] scrolls on its own (px) */
    uint8_t  cloud_blend;          /* sixteenths of the layer under it added: 8 here */
    ct_scene_exit exit[128];
} ct_scene_world;
```

How this game fills it: `mode` from the NMI vector (W7); `map_id` = `$0100`; `map_w`,
`map_h`, `cell_px` = 192, 128, 8 and `cells` from W3 (floor for 0, solid for bit 0, water
for bit 1, `CT_CELL_COVER` for bit 2, `raw` = the nibble); `layer[0]` = BG1 and `layer[1]`
= BG2, tilemap words expanded from W2 over the full 192 x 128 tiles (BG1 empty cells 0),
`chr_base` `$2000`, `bpp` 4; `layer[2]` = the BG3 clouds (VRAM `$7800`, `chr_base` `$7000`,
2 bpp) scrolled by `cloud_*` from `$24/$2A`; `camera` and `party` from W4; objects from OAM
(W5: foot point = OAM screen position + (8,17) + camera); `exit` from W6;
`cloud_blend` from `$2131`. Pixels still travel in VRAM and CGRAM. Full-screen menus use
`CT_SCENE_UI` (NMI `$C2840E`) so renderers keep them plain and sharp; `CT_SCENE_OTHER`
keeps the ordinary 2D-scene fallback for battles, titles and every other screen.

### W9. Feasibility: a 3D world map

**Done** (HD2D.md, "The world map"): a heightfield from the cells' pixels, mountains as
domes, forests as canopies, flat water with a shader shimmer, the clouds as a plane that
also shades the ground, wrapping by drawing the neighbouring copies, billboards on the
pixel grid. The notes below are the spike's.

**Verdict: feasible, and easier than the field.** It is plain mode 1 with no HDMA; the
whole map is resident, flat, in two tile layers; the camera is the party's position;
sprites are ordinary OAM; collision is an explicit grid; entrances are an explicit list.
What the renderer does with it:

- A ground mesh from BG2 and a decoration pass from BG1. The classes give the skeleton of
  a **heightfield**: water at sea level, walkable (0, 4) flat, class 1 raised. But class 1
  mixes mountains, dense forest and the cliff band, so the height should come from the
  picture as well: a per-metatile-index table built at load from the tile pixels
  (no shipped data), or the BG1 cell on top (mountain and tree art sit on BG1). A first
  version can use sea 0, land 1 level, class-1 cells raised a fixed step and smoothed, with
  BG1's non-empty cells stood up as billboards (trees, towns, mountains).
- **Wrapping:** 1536 x 1024 px, so render the neighbouring tiles of the camera or sample
  modulo.
- Sprites as billboards from OAM (the party, the ferry); the caption stays 2D.
- The BG3 clouds as a scrolling translucent plane above the ground (half-add).
- An orbit camera is easy: the party's position is exact. The 8 px steps and the 1 px/frame
  speed belong to the game state, not the view.

Risks: classes do not separate mountains from forests (above); vehicles and flight are
unknown and flight may be the mode 7 engine (W1); the 2D picture's per-tile priority (the
"cover" look, trees over the sprite) has no direct mesh equivalent; water and palette
animation are palette cycling only (W14); the other seven maps load and differ in what
class 2 means (W13).

### W10. For an HD mode 7 renderer (bsnes-hd style)

- The **1000 A.D. overworld never uses mode 7**, so no HD mode 7 path is needed for it:
  draw it from W2-W4 in 3D or as the plain mode 1 picture.
- Per-scanline matrix: none on `$1F0` (no HDMA, W1). The mode 7 engine overlay (W1) holds
  the matrix stores and HDMA tables; W11 reaches its scenes, logs the per-line matrix and
  finds the tables (`$7EF100` A,B and `$7EF488` C,D).
- Sprites over mode 7: the engine's own NMI is a different handler (`$C30548`); whether it
  also DMAs `$0700` to OAM was not checked.

### Probe

```
./ct_boot --frames 7600 --script ../game/ct/test/replay/world_map_save.txt \
    --wram spike/w.wram --vram spike/w.vram
python3 ../game/ct/tools/worldmap_probe.py spike/w.wram --vram spike/w.vram \
    --png spike/world.png --terrain spike/terrain.png --regions spike/regions.png
```

It prints whether the dump is the world map (vector and id), the party, the camera, the BG
scroll, the active exits and the OAM entries as screen and world positions. The PNGs are
ROM-derived: build dir only.

### W11. The mode 7 engine: which scenes, how to reach one, its per-line tables

Everything here was run with `ct_boot` from `build-mod` (static, no `--module`) on
`world_map_save.txt`'s first frames. **Verified** = a run or a dump agrees; **code** = read
from the disassembly (`ldis.py`, with the new `--overlay FILE@ADDR` to list WRAM overlay code
from a `ct_decompress.py` output). The probes are `scene_reach.sh`, `m7_matrix.py`,
`m7_tables.py` and `m7_scan.py` (end of this section).

**It is the game's cutscene engine, and only some of its scenes are mode 7.** The overlay
at `$7E3000` (W1) holds ten scene handlers: the title, the credits and ending cutscenes,
and two real mode 7 scenes. Free flight on the world map is not among them.

*How it is entered.* The event command table `$C05D6E` (256 words, `jumptable` in
funcs.toml) has `EventCmd_Mode7Scene` ($C03FAF) at index **`$FF`** (`$C05D6E + 2*$FF =
$C05F6C`, verified against the table bytes). Its argument byte (`$7F2001,X`, stored to
`$DF`) picks the case: `< $80` = this engine; `$80` Warp_TransitionStub, `$81`
Warp_GateEntryStub, `$82` Warp_TransitionShortStub, `$83-$8F` Sub_FDFFE5 (all four are bank
$FD stubs, code); `$90-$9F` = a 16-word table at `$C0400E` of screen-state commands (they set
`$39` and wait on `$0350`; **not** this engine; code). So "mode 7 scene ids" are `$00-$7F`
and the engine uses only `id & $1F`. Mode7Engine_Entry ($C30000) gets the id in A, stores it
at **`$0384`**, installs the NMI `$C30548`, decompresses the overlay (W1) and jumps to
`$7E3000`, which clears `$0380-$0396`, sets DP to `$4300` for the DMA setup (so the HDMA
channel stores are DP-relative `STA $70` etc.; a scan for `STA $43xx` finds only channel 7),
runs the handler `JSR ($3043,X)` with X = `(id & $1F) * 2`, and at the end jumps to
`$C00000` ReentryVectors. The engine's NMI (`$C308C9`) writes `$420C` from the shadow
**`$038C`**, INIDISP from `$038D`, and calls the overlay's frame routine `$7E39BE`;
`$0392` nonzero means the scene is over. The table at `$7E3043` has 16 words, so ids 16-31
index into the code after it and are invalid (they end at once: ids 16-31 logged 13,891-13,892 PPU writes
each in the sweep).

| id (`$0384 & $0F`) | Handler | What it shows (seen on screen) | Mode 7? |
| --- | --- | --- | --- |
| 0, 9 | `$7E3B48` | **a bike race on a road**, with `1000 REST` and a rank panel (the 2300 A.D. highway race, by the look) | **yes**: mode 1 sky/city above line 57, mode 7 road below, rotating matrix. Variant: `$44` is incremented when `id & $0F` is not 0 |
| 1 | `$7E6B04` | the end credits cutscene: staff text on black, then Zeal's floating continent over a sea of clouds, the Blackbird flying past, a scene on a rampart, the throne room | no mode 7 in the 600 frames logged (modes 0, 1); screenshots to frame 1,500 |
| 2 | `$7E5978` | **the title screen**: the pendulum, `CHRONO TRIGGER` logo, copyright | no (mode 1) |
| 3 | `$7E671F` | black for 800 frames, then the same cloud/continent fly-over as in 1 | no (modes 0-2) |
| 4 | `$7E53BB` | stars, a fire-ball (Lavos) shrinking toward a planet | no (mode 1; HDMA frames: none) |
| 5 | `$7E7092` | all the NPC sprites scattered over a black screen (the ending's character parade, backgrounds missing) | no |
| 6 | `$7E78DD` | **the Epoch cockpit** looking out over clouds, then a flight into a tunnel of fire with a white flash | **yes**: mode 7 sky above line 168, mode 1 cockpit panel below |
| 7, 8 | `$7E6248` | the planet turning (8: grey, with the words "BUT... THE FUTURE REFUSED TO CHANGE.") | no |
| 10, 11, 14, 15 | `$7E8538` | ends at once | - |
| 12 | `$7E8E95` | credits text rolling over stars | no |
| 13 | `$7E8EA8` | `CHRONO TRIGGER  The End` | no |

Flags in the id byte (code: the overlay tests them with `BIT`): `$80` = enable IRQ/NMI path at
entry (C30038), `$20` skips the song start in the id-0/9 handler (`$7E3B51`), `$40` is read by
scenes 0/9, 6 and elsewhere (`$7E3C54`, `$7E4F82`, `$7E677D`); what they select was not
traced. The sweep saw that ids 32+n (bit 5) draw the same as n, and 64+n / 96+n differ
(the race showed 37 frames of HDMA, 142-178, in place of 84 before the interpreter stopped).

*How to reach a scene headlessly (verified).* No replay reaches one apart from the title,
but the boot's own `FF 02` can be edited: the event script of the first frames is in WRAM
(`$7F2000`: the byte count, then commands from `$7F2001`), `EventCmd_Mode7Scene` runs at
**frame 60** and the argument byte is `$7F2061` (`--wram` at frame 60 shows `... ea 00 ff 02`;
`--log-entry C03FAF` says A=`$01FE`, Y=`$5F` there, `C30000` is entered at frame 65 with
A=`$0002`). `--poke-wram 59:12061^XX` XORs it before it is read, so XX = `2 ^ id` makes the
same boot start scene `id`. Checked: `^01` enters `C30000` with id 3 (frame 65), and each of
ids 0-127 ran for 600 frames (`scene_reach.sh m7 ID FRAMES OUT`). The location's event script
is compressed (so patching the ROM image would not do; the recompiled code is also not
affected by ROM edits, only data), which is why the one-byte poke is the route. Limits:
scenes 0/9 stop at frame 225 with `overlay: more than 64 nested overlay functions` at
`$7E3749` (the interpreter's overlay depth limit; whether the real race runs past it, or this
entry lacks state the game sets before it, is **open**), scene 1 runs 1,500+ frames, and the
others run to their end.

*Callers in the event scripts: not enumerated.* A location's script is a compressed stream:
location record (14 bytes at `$F60000 + 14*loc`) `+8` is an index into 3-byte pointers at
`$FCF9F0`, NewGameSave_DecompressToWork ($C056D4) decompresses it to `$7F2000`.
`m7_scan.py` decompresses every one (`ct_decompress.py`) and lists `FF xx` with `xx < $80`,
but the scripts have no length table in the repo, so the scan can not tell an operand from a
command: it reports 1,087 hits, most of them operands. Treat its output as a candidate list
(to be confirmed by running the location), not as data. The scenes that are certainly
called are the title (id 2, the boot script) and, by what they show, the ending
sequence (credits, parade, planet, `The End`); the race and the Epoch cockpit are entered
from field locations that no replay visits.

*Per-scanline state (verified).* `m7_matrix.py LOG FRAME` rebuilds, per scanline, BGMODE, the
matrix A B C D (8.8 signed, each a write-twice pair), the centre X0/Y0 and the BG1 scroll from
`--log-ppu 2105:2133@F` (it needs the previous frame in the log too for the carried values).
HDMA writes are logged with the line they happened on: all land at **dot 278-331 of the line
before the one they shape** (the first write of a frame is at line 0, the first visible line is 1;
the logged line L shapes line L+1; checked on mode switches, `2105` at logged line 168 and the
panel starting at line 169, and not against a reference emulator).

- **Scene 6, sky (frames 98-457 of the sweep):** `$038C` = `$3F` (HDMA channels 0-5; set at
  frame 97 at `$7E7A62`), 225 entries each of `$211B` and `$211C` (A, B) and of `$211D`/`$211E`
  (C, D) per frame, none rotated: **A = D = s(line), B = C = 0**. s is 128 (0.5) at line 0,
  rises hyperbolically to 836 at line 108 and is 0 at **line 120 (the horizon)**; below it
  816 at line 132, 370 at line 156 (the floor mirrors the sky). `$2105` = 7 until logged line
  167 and 1 from 168 (the cockpit panel; BG1 scroll `$210D/E` = 256, 87 there, 0, 565-578 above).
  X0 = 128 and Y0 move every frame (436, 351, 112 at frames 298, 449, 559), the camera
  advancing. `$2132` (fixed colour, 67 lines) draws the glow. From **frame 458** (`$038C` =
  `$1F`) the tunnel: A, D up to 508, B = C = 0 as well (radial zoom, no rotation); the
  frame has no `$2105` write (mode 7 stays).
- **Scene 0/9, the race (frames 142-225):** `$038C` = `$3E` (channels 1-5, at frame 139).
  Mode 1 up to line 56 (logged), then mode 7 with `$210D/E` = (1015, 24) written at the
  switch, **A = D = 0 on the straight and B = -C = 24480 / (line - 32)** (the horizon at line
  32, hidden under the mode 1 part): B = 1020 at line 56, 765 at 64, 612 at 72, 382 at 96, 128 at 224
  (fits the integers printed). X0 = 1032..1152 (30 per 15 frames, the road advances), Y0 = 248.
  In a bend (frame 222, the last full frame before the stop) the matrix is rotated:
  (A, B, C, D) = (432, -218, 218, 432) at line 100 and (249, -125, 125, 249) at 150, i.e.
  A = D = k cos t, B = -C = -k sin t with the same 1/(line - 32) k and t about 26.8 degrees
  (tan t = 0.505 at every line); X0, Y0 = (962, 366), BG1 scroll (849, 147).
- **HDMA sources (verified, `m7_tables.py`):** per-line tables in bank `$7E`, 4 bytes a line
  (two words), 225 lines: **`$7EF100`: A, B** and **`$7EF488`: C, D** (mode 3 style: the writes
  per line are `$211B` lo/hi, `$211C` lo/hi and `$211D`/`$211E` likewise). In scene 6 the second table's D word is at `$7EF48A` (it equals A) and C at
  `$7EF488` (zero, as B); in the race B is at `$7EF102` and C at `$7EF488`. The matrix therefore can be read from WRAM at any frame
  without the PPU log. `$211F/$2120` (X0, Y0) and `$210D/E` are written once per frame at
  line 234 (the NMI) and again by HDMA at the mode switches.
- **For an HD mode 7 renderer:** the race and the sky both fit "a plane viewed with a
  per-line scale": scale(line) from the table, a rotation about the centre from the (A, B)
  ratio, X0/Y0 as the centre and BG1 H/V as the scroll; the mode 1 parts (sky, panel) are
  ordinary layers split at the logged line. The mode 7 map and character data in VRAM (interleaved
  low/high bytes) were not read out. Sprites over scenes
  0/9 (the two bikes, `1000 REST`) are in OAM; whether the engine's NMI DMAs OAM from
  `$0700` like the shell was not checked (frame 200 shows them drawn, so something does).

Probes: `scene_reach.sh m7 6 600 spike 299` (scene 6 from frame 299 on), then
`m7_matrix.py spike/scene6.log 300 --every 15 --to 225`, `m7_tables.py spike/scene6.log 300
spike/scene6.wram --first 100`; `m7_scan.py --ids 0,1` for the script scan.

### W12. The Epoch and vehicles on the world map: not found; what is known

No replay or poke reached an Epoch on a world map. What was established:

- **All eight world maps are mode 1.** `$2105` is `$09` on every one of `$1F0-$1F7`
  (the probe's `BGMODE 09` line for each dump, W13), so ordinary flying on a world map would be
  mode 1 with a sprite, not mode 7. The mode 7 engine (W11) has an **Epoch cockpit** scene
  (id 6, seen), but it is a cutscene entered by event command `$FF`; it is not the world map.
- **The world map has no vehicle state byte of its own that was found.** The only vehicle on
  `$1F0` is the ferry: four 16x16 OAM pieces, tiles `$2C, $2E, $4C, $4E`, palette 2 (W5), an
  ordinary sprite node. The OBJ sheet on `$1F0` (VRAM words `$0000-$1FFF`, rendered for this
  spike) holds, besides the party frames: clouds, balloons, a ship in six turning frames
  (the ferry's art), sparkles, and the caption plates (`12000`, `600`, `1000`, `1999`, `2300`,
  `65000000`, `B.C.`, `A.D.`, `?`), but **no Epoch frames**.
- **Flags.** Overworld_CacheEventFlags (C226D6) copies `$7F01F0-$7F01FF` to `$7E1BA7-$1BB6`
  and `$7F0000` (the story counter) to `$1BA6`; Overworld_WritebackEventFlags (C226F0)
  writes them back. Readers in bank $C2 (search of the code, not run): `$1BA7` bit 4,
  `$1BAA` bit 5 and `$1BAF` bit 1 stop the BG3 cloud layer scrolling with the party
  (C21694-C216E4: the scroll `$24`/`$2A` is only advanced when all three are clear);
  `$1BA9` is tested at C295B2/C2E807 and `$1BAD` at C26994 (menu code); `$1BA6` is
  compared with `$36` and `$CC` (menu text, C26985, C27463) and the story counter `$7F0000`
  with `$D4` by Overworld_ApplyStorylineGfx (C22D91: from `$D4` on, maps `$1F0`, `$1F1`
  and `$1F2` get `$800` bytes from the `$9000` buffer put in VRAM word `$0C00`, and `$1F6` the same
  at word `$0000`; code only) and with `$D2`
  at C2311F, in a routine keyed on a destination id at `$029F` with coordinates `$0290/$0292`
  (and a second at `$029E/$029A/$029C`): scripted spots on the map, not a vehicle.
  So the flags that bring the Epoch in are probably in the per-map trigger scripts
  (descriptor byte `+$16`, `$C6FFE0`, decompressed to `$7F0400` by
  Overworld_LoadTriggerScripts C22A16), not in bank-C2 code.
- **`$1F5` is special-cased:** C23BC9 sets the leader node's animation base `+$0F` to `$B0`
  on `$1F5` and `$A0` elsewhere: the party is drawn with another animation set on the sky map
  (an airborne party? the map is the cloud sea, W13). Not run with a flag set.
- **What a test needs:** a save in which the Epoch exists, i.e. a replay past the point where
  the game gives it (not in the repo), or the trigger-script bytes of a map (`$7F0400` on the
  loaded map: untried) to see which flag a script tests. The one-byte `--poke-wram` can set one flag per
  run (`$7F01F0+n` XOR a bit before frame 7252, the map entry's cache at `$C226D6`); 128
  candidate bits were not swept. **Open.**

### W13. Other eras: the eight maps load (verified), with their classes

**How (verified).** The replay writes `$0100 = $1F0` at frame 7252 line 239 and Overworld_SetMapDescPtr
(C2274D) reads it at frame 7254, so `--poke-wram 7253:100^N` (N = 1-7) makes the same run load
`$1F0+N`: the NMI vector stays `$C2015E`, `$0100` reads `$1F0+N`, the exit, region and
trigger tables and the BG tiles are those of the new map, and `worldmap_probe.py --png` redraws it
(`scene_reach.sh world N OUT`). The party is put at the tile the `$1F0` exit gave (55,35 = px 440,280),
which is walkable on `$1F1` and `$1F2`, blocked on `$1F3`, and on water on `$1F4` and `$1F6`: fine for
looking, not for walking. Each loaded in 7,450 frames (about 25 s).

| id | What (verified by the picture, the caption in the shot, or the caption's OAM pieces) | Active exits | Cell nibbles 0 / 1 / 2 / 4 | BG1 cells |
| --- | --- | --- | --- | --- |
| `$1F0` | 1000 A.D.: green islands, hex-pattern sea | 77 of 80 | 3,675 / 2,491 / 18,216 / 194 | 864 |
| `$1F1` | a green archipelago, same hex sea (caption plate is 2 pieces `$1AC,$1AE`; by the look the 600 A.D. map, not confirmed by a caption screenshot) | 67 of 74 | 3,302 / 2,678 / 18,429 / 167 | 756 |
| `$1F2` | a dark brown ruined land, caption `?` (the 2300 A.D. map, by the look) | 20 of 20 | 2,292 / 1,566 / 20,686 / 32 | 574 |
| `$1F3` | desert, jungle and a lava field, deep blue sea; caption plate has 4 pieces (`$18C,$18E,$188`: the long year and `B.C.`): 65,000,000 B.C. by the look | 21 of 25 | 3,457 / 1,917 / 19,160 / 42 | 4,750 |
| `$1F4` | **12000 B.C.** (the caption in the screenshot): snow and ice floes on a dark sea; also a scripted object of 12 OAM pieces (tiles `$40-$64`, palette 1: a dark shape hung on a chain, south of the party in the shot) and a 9-piece trail (tile `$8E`), OAM 3-23 | 7 of 7 | 3,351 / 54 / 21,145 / 18 (+8 of nibble 5) | 624 |
| `$1F5` | a plain sky above a sea of clouds, no caption, no terrain: 98% of the cells are nibble 0 over an empty ground metatile and **BG1 covers the whole map** (one overlay metatile, `#32`, in all 6,144 cells); BG1's scroll is `342` against BG2's `312` in the dump | 14 of 14 | 23,700 / 848 / 0 / 28 | 6,144 |
| `$1F6` | dark sea with ice floes, sparser than `$1F4`, no caption | 7 of 7 | 517 / 30 / 24,015 / 14 | 494 |
| `$1F7` | **1999 A.D.** (the caption in the screenshot): a small map (a domed city, grey roads) in metatiles 14-50 x 2-36 of the 96x64 grid, the rest blank | 0 active of 1 | 22,454 / 525 / 1,584 / 13 | 367 |

All eight have BGMODE `$09`, BG1/BG2 tiles at word `$2000` (the probe's `--chr 2000` works for
all), the same decompressed sizes and the same `$7E3000-$7E8300` layout (W2). `$1F5`'s party
sprite has priority 3 where the others have 2 (OAM entry, W5; consistent with the `$1F5` case in
W12).

**What differs for the height guess** (`worldmap_classes.py`: per class, what the ground tile
looks like, so "2 = sea" can be checked per map). The numbers are the share of the class's
cells whose ground metatile mean colour reads as water/forest/ice/rock:

- **`$1F0`, `$1F1`**: class 2 is 91% water-coloured (`#205` and `#7`, the hex sea); the rest of
  class 2 is mountain tops. Class 1 is forest (`#136, #137, #123, #135, #223`, about 80%) and
  rock (`#66`, with 1/2 mixed: the cliff band, 64 + 64 cells on `$1F0`, 72 + 72 on `$1F1`). On
  `$1F1` `#185` (dark, 176 cells, class 1, an overlay on 33) is deep water near the coast. The
  current `relief[]` (castle metatiles, `$1F0`) does not apply to `$1F1`: its castle and towns
  are other metatiles.
- **`$1F2`**: **there is no blue.** The ground is all rock-coloured (100% of every class);
  the "sea" is a dark ground tile `#7` (mean about 33,37,45, 17,740 cells of class 2) that
  is **void, not water**; class 1 is ruined buildings (`#229` 412 cells, 102 with an overlay;
  `#33`, `#38`) and `#66`-like pairs (`#67, #68`: 1/2 mixed). A water shimmer would be wrong
  here: treat class 2 as a lowered dark plain.
- **`$1F3`**: the sea is class 2 on **tan** ground (`#64`, 13,724 cells, mean 139,115,50) with
  a BG1 overlay on 3,431 of the cells (4,750 overlay cells in all): the blue sea you see is
  drawn by BG1, so the ground tile's pixels lie. The picture of the composite (what the
  renderer already uses) says water; a per-index table keyed on the ground tile would not.
  Class 1 is jungle (`#6, #7, #5, #21, #23`, forest colours, 89%).
- **`$1F4`, `$1F6`**: ice. Class 0 is 96% ice/white ground; class 2 is the dark sea (`#188` on
  `$1F4`, 8,228 cells, `#174` on `$1F6`, 4,016, mean 29,28,37) and 42% of the class on `$1F4`
  reads blue; class 1 is rare (54 and 30 cells) and `$1F4` has 8 cells of nibble **5**
  (blocked + cover): a combination `$1F0` does not have. Ice floes are islands of ground
  tiles in the dark sea.
- **`$1F5`**: flat: no heights; the clouds are the BG1 overlay everywhere (the sea of clouds
  pattern); a cloud layer plus a sky backdrop, nothing to lift. Class 1 (848 cells) is the
  few things standing in it (rocks, trees: `#180, #196, #21, #33, #38`).
- **`$1F7`**: the map is 37 x 35 metatiles (x 14-50, y 2-36); outside it
  nibble 0 over empty ground: a renderer should clamp to the used rectangle or the 3D map
  shows a green plain the size of the grid. The sea tile is `#7` as on `$1F0`.

Colours in each dump are the era's own (the dark `$1F2`, the pale `$1F5`, the blue `$1F3`), so
a shimmer or a height tint from the water colour has to read it from CGRAM per map.

### W14. Animation: what moves on the world map and in the field

**World map: palette cycling, no tile animation (verified).** A 220-frame log of every VRAM
and CGRAM write on `$1F0` (`--log-ppu 2115:2122@7480`, `vram_uploads.py`) has **no VRAM upload
that repeats** (the tile art is static: the whole map redraws from one VRAM dump, W2) and a
full 256-colour CGRAM upload every **10 frames** from the shell. Following the contents
(`vram_uploads.py --cgram-diff`) only these colour indices change, each cycling through 2-4
values: indices **32 and 36-46** (BG palette row 2, colours 4-14: the sea) every 10 frames
on `$1F0`. The same runs on the other maps (the poke of W13):

| map | indices that change | every | distinct values |
| --- | --- | --- | --- |
| `$1F0`, `$1F1` | 32; 36-46 | 10 frames (32 changes in pairs) | 2; 2-4 |
| `$1F2` | 32; 36-46 | 30 frames; 10 frames | 2; 2-4 |
| `$1F3` | 48-63 (5 of them); 64-67 (4) | 8 frames | 4; 3 |
| `$1F4` | 32; 36-46; 64; 75-76 | 10 frames (64: 10, in bursts) | 3; 2-4; 2; 6 |
| `$1F5` | 64; 76-78; 112-116 | 16; 8; 8 frames | 2; 3; 3-4 |
| `$1F6` | 32; 36-46 | 10 frames | 3; 2-4 |
| `$1F7` | 32; 36-46 | 10 frames | 2; 2-4 |

(The routine that does it on the world map was not located; the NMI at `$C2015E` copies a
shadow palette, so it is the shell's. The periods above are what a renderer needs: a water
shimmer is a palette colour change every 8-10 frames, not a tile animation. Which row each
index range is on: 32-47 is BG palette row 2, 48-63 row 3, 64-79 row 4, 112-127 row 7.)

**Field: BG tile animation by a VRAM DMA service, and a palette animator (the first
verified, the second code).**

- *Tile animation.* On Leene Square (`$005`, frames 8500-8700 of `leene_square.txt`) the
  log shows nine 64-word (128 byte = four 4 bpp tiles) uploads, every **4 frames** each
  (three of them on the even-numbered frames, six more in the other phase), to VRAM words
  `$30C0, $3290, $3390` and `$4500, $4540, $4600, $4640, $4680, $46C0`, i.e. inside the BG1/2
  tile area (`$2000-$4FFF`, A0.4), with **3 or 4 distinct contents** each (a 12-16 frame
  cycle). (The uploads at `$0080-$1980`, 128 words every 7-15 frames, are OBJ frames: sprite
  animation, not tiles.) The code is `FdCore_VramDmaServiceStub` ($FDFFFD, jump to
  **`$FDE022`**), which the field NMI calls (`JSL $FDFFFD` at `$C0ED08`, when `$0153` bit 0 is set; when it
  is clear that NMI runs Vblank_UpdateAnimTileWords `$C0ED15` instead): it increments
  `$051D`, and on one parity of that count serves six slots (at `$0500 + $B0, $B5, $BA,
  $BF, $C4, $C9`) and on the other the other six (`$CE ...`). A **slot is five bytes**: a source offset word, the VRAM
  destination word (bit 15 set = slot off) and the source bank; the frame number is bits 9-11
  of the offset, and the next offset and the DMA source come from two tables per slot,
  `$7F0400 + 32*slot + 2*frame` (the step added to the offset) and `$7F0410 + 32*slot +
  2*frame` (the DMA source address), so a slot walks 3-4 frames and wraps. Each service run moves 128 bytes per slot over
  DMA channel 7. The tables and slots are filled per location (the map's animation
  data); **which tiles** a location animates is therefore the slot list (`$0500+$B0..$D8` and
  `$7F0400-$7F04F0`) after the map is loaded: dump WRAM and read it.
- *Palette animation (code).* `AnimTile_FrameTickStub` ($FDFFF7, to `$FDE39C`: the name is a
  misnomer) steps twelve 12-byte slots at `$7E0520-$7E05AF`: byte 0's high nibble is the kind
  (0 off; 1 and 8 take an extended 9-byte record; others are the 6-byte kind), `+3`/`+4` count
  frames against the period in `+5`, kind 1 copies `+2` colours from `$F60000 + [+6]` (frame
  `+3`) into the CGRAM mirror at `$7E2200 + 2*[+1]` (E437); other kinds fade/tint (E4E8,
  E532, E5A4 brightness, E82C tint to white, E57C, E807, E485 scripted frames). The slot
  set comes from PalAnim_LoadMapConfig (`$FDFFF4`, to `$FDE292`) by the location record's
  byte `+3` (`$F60003 + 14*loc`) through the word table at `$FDF9C7` into `$FDFA77`. Not run
  on a changing field: Leene Square's CGRAM is re-uploaded every frame but the colours do
  not change (`--cgram-diff` finds nothing). **Open:** a field map with water shimmer
  (a lake or the sea shore) to read which colours and how fast.

Probes: `vram_uploads.py LOG [--cgram | --cgram-diff]`.

## Proposed `ct_scene` (generic, for runtime/module.h)

**Since implemented,** in a refined form: `ct_scene` in `runtime/module.h`, filled by
`game/ct/scene.c` through the module export `ct_module_info.scene`, and drawn by the 3D
diorama preset ([HD2D.md](HD2D.md), "The 3D diorama"). The differences from the proposal
below: layers come as expanded 8x8 tilemaps (no metatile tables), with their VRAM
character base and depth; cells carry a class (floor, solid, wall-ish, water, stairs,
unknown) and flags; objects list OAM entry numbers, and the engine reads the pieces from
OAM itself; the field / other mode is the field loop's NMI vector at `$0500` (`$C0EA63`;
battles install `$D1E77C`, menus `$C2840E`, the world map `$C2015E`); co-op fields are
left for later. The original proposal follows.

The module fills this struct once per frame at the frame edge. The engine never parses game memory. The
names are game-agnostic on purpose (tools/check_agnostic.py). This is a proposal. It is not added to
module.h yet, because it would change `CT_MODULE_ABI`.

```c
/* One frame's scene, filled by the module (optional export ct_module_scene). */
#define CT_SCENE_MAX_LAYERS  3
#define CT_SCENE_MAX_OBJECTS 64
#define CT_SCENE_MAX_PIECES  8

typedef struct {
    uint16_t w, h;            /* in cells */
    uint8_t  cell_px;         /* 16 */
    uint8_t  present;
    const uint16_t *cells;    /* w*h tile-assembly indices (module-owned, valid this frame) */
} ct_scene_layer;

typedef struct {
    uint16_t entry[4];        /* SNES BG tilemap words TL,TR,BL,BR (tile, pal, prio, flip) */
} ct_scene_metatile;

enum {                        /* ct_scene_cell.flags */
    CT_CELL_SOLID      = 1 << 0,
    CT_CELL_SHAPED     = 1 << 1,   /* partial solidity; shape in .shape */
    CT_CELL_STAIRS     = 1 << 2,
    CT_CELL_TRIGGER    = 1 << 3,
    CT_CELL_EXIT       = 1 << 4,
    CT_CELL_NPC_BLOCK  = 1 << 5,
    CT_CELL_SPR_FRONT_TOP = 1 << 6, /* sprites on this cell draw above priority-2 BG */
    CT_CELL_SPR_FRONT_BOT = 1 << 7,
};
typedef struct {
    uint8_t flags, shape, plane, move;   /* move: auto-move dir|speed */
} ct_scene_cell;

typedef struct {
    int16_t x, y;             /* OAM-style offset from the object's foot point, px */
    uint16_t tile;            /* 9-bit OBJ tile in VRAM, or a module texture id when .from_module */
    uint8_t size, palette, priority, flip;
} ct_scene_piece;

typedef struct {
    int32_t  x16, y16;        /* map position, 1/16 px, foot point */
    uint8_t  kind;            /* 0 none, 1 party, 2 npc, 3 other */
    uint8_t  party_slot;      /* 0 leader, 1, 2; 0xFF */
    uint8_t  facing;          /* 0 up 1 down 2 left 3 right */
    uint8_t  plane;
    uint8_t  on_screen;       /* has pieces in this frame's OAM */
    uint8_t  n_pieces;
    uint16_t frame;           /* animation frame id, 0xFFFF unknown */
    ct_scene_piece piece[CT_SCENE_MAX_PIECES];
} ct_scene_object;

typedef struct {
    uint32_t map_id;          /* location id */
    uint32_t map_generation;  /* bumped on every load or in-place tile change */
    uint8_t  mode;            /* 0 none, 1 field, 2 battle, 3 world map, 4 menu/cutscene */
    uint8_t  player_control;  /* leader takes pad input this frame */
    int32_t  camera_x, camera_y;   /* map px, top-left of the 256x224 view */
    uint16_t bound_w, bound_h;     /* camera limits, px */
    ct_scene_layer layer[CT_SCENE_MAX_LAYERS];
    const ct_scene_metatile *metatiles; uint16_t n_metatiles;  /* shared by layers 0-1 */
    const ct_scene_metatile *metatiles_l3; uint16_t n_metatiles_l3;
    const ct_scene_cell *cells;    /* layer-0 grid, w*h */
    uint16_t bg_chr_base[CT_SCENE_MAX_LAYERS];   /* VRAM word addresses */
    uint8_t  n_objects;
    ct_scene_object object[CT_SCENE_MAX_OBJECTS];
    /* co-op */
    int8_t   battle_menu_slot;     /* party slot whose battle menu is open, -1 none */
} ct_scene;

/* Module export (optional; NULL = no scene). Called at the frame edge. */
typedef int (*ct_module_scene_fn)(ct_scene *out);
/* Optional input routing for co-op: which port drives party slot s this frame. */
typedef int (*ct_module_input_owner_fn)(int party_slot);
```

How this game fills it:

- `map_id` = `$0100`.
- layers from A0.2, metatiles from `$7EB000`/`$7EC000`, cells from `$30C0`/`$7000`/`$7040`/`$7080`.
- camera from A0.5.
- objects from A0.6, with pieces from shadow OAM `$0700` grouped by `$0D00` (A0.7).
- `battle_menu_slot` = `$95D5`.
- `mode` from which main loop is running. That still has to be pinned down (see the open questions).

Pixels never travel through the struct. The engine already has VRAM and CGRAM.

## Rumble

What `game/ct/rumble.c` reads for simulated rumble (runtime/rumble.h; USER-GUIDE.md,
"Rumble"). Found with `ct_boot --watch`, `--log-entry` and `--wav` on the gato_win replay;
`ct_boot --log-rumble` lists what comes of it (the `rumble_replay` test).

- **Battler records: `$7E5E30` + `$80` x slot**, slots 0-2 the party, then the enemies
  (11 in all: Battle_ApplyPendingResults loops to X = `$580`). Current HP `+$00`, max HP
  `+$02`, MP `+$04`, max MP `+$06`, status `+$1A` (bit 7: down). *Verified:* Crono's
  `$5E30` = `$46`/`$46` (70) when the battle starts (frame 10659, $FDAE80); Gato's record
  is the seventh, `$6130` = `$4C` (76). Outside battles the area is reused.
- **Pending results: `$B328` + 4 x slot** ({amount, flags} words; two more lists at `$B354`
  and `$B380`), applied by **Battle_ApplyPendingResults ($C1EC7F)**: each amount is
  subtracted from HP (from MP when the flags have `$4000`), HP is clamped to max HP, and a
  battler at 0 gets status bit 7 (an enemy also $FDABA2). Heals are negative amounts
  (Battle_NegateDamageIfHeal $C1EBF8). *Verified:* every HP change of the fight is a store
  at $C1ECA0: Crono `$46` -> `$3D` (frame 10983, 9 damage), Gato `$4C` -> `$3B` (11115),
  ... Gato `$09` -> `$E7FF` -> 0 (13200, $C1ED0F). The patch takes the HP of all 11 at the
  function's entry; the frame hook compares at the frame edge.
- **Critical hit: `$AE9B` bit 7**, set by the player's hit roll ($C1ED89: random 0-99
  against the attacker's `$5E72`, then the damage times `$1E`), cleared after the hit is
  shown (`$C18D1C`). *Verified:* set at 11544 and 13025; Gato then takes `$22` instead of
  `$11` / `$10` (11729, 13200).
- **The action: `$B18C`**, the tech index (tech data at `$CC1BEB` + 11 x index, read by
  Battle_ExecPlayerTechAction $C1D523, which runs for plain attacks too). A plain attack
  is `$75` + the character (Crono `$75`, Marle `$76`, in coop_battle); Gato's attack sets
  it to 1. The usual tech layout (each character's eight techs from 1, in character
  order; doubles and triples above) is taken from the tech lists, not seen in a replay:
  no replay has a tech. The party is `$2980-$2982` (characters, `$80` none), in battle
  slot order (*verified* in coop_battle: `00 01 80`). `$B1F4`/`$B1F5` change with the
  actor but are not a plain slot number (Crono `00 00`, Marle `80 00`, Gato `00 03`).
- **Screen shake event: command `$F4`, EventCmd_ShakeScreen ($C03D84)**: its operand goes
  to direct page `$BC` (0 also clears `$1D4E`/`$1D4F`). Not in a replay yet.
- **Sound: command `$EA`, EventCmd_PlaySound ($C03C11)**, operand to `$1E01` with command
  `$18` to the sound program. *Verified:* sound `$3F` at frame 3545 of the intro is
  Leene's Bell (the recording: inharmonic partials at 1171, 1373 and 2058 Hz ringing out
  over 2.5 s); the field's other `$18` commands come from elsewhere (menus $C2xxxx,
  battle $C1xxxx).
- **Time gate: Warp_SceneInit ($FDDB97)**, the set-up of the gate's tunnel, reached from
  the three entries the main loop jumps to for `$DF` = `$80`-`$82` (`$C03FE4`). Not in a
  replay yet.
- **Where the game is: the NMI handler at `$0500`** (a JML): `$C0EA63` the field,
  `$D1E77C` a battle (`$D1F4C0` its results screen), `$C2015E` the world map and the
  intro's aerial view, `$C2840E` the menus and name entry, `$C3xxxx` the title. The hook
  lets the detectors (shake, flash) work in the field and in battles only.

## Open questions

- The exact solidity shapes for classes 2-$15, and what the special classes $16+ do
  (stairs, ledges, jump tiles). Next step: a dump in Crono's house (stairs, upper floor), and
  mapping the jump table after C09B86.
- Byte B bit 3, byte A bits 5 and 7, and the zone grid `$70C0` semantics.
- A reliable "player has control" flag (events lock movement). `$0138`
  (checked by Player_ApplyPadMoveIntent) is a candidate. It has not been verified.
- A field / battle / world map / menu mode byte for `ct_scene.mode`.
- The world map ($1F0-$1F7) is a different engine: see "World map". Open there: vehicles
  (the Epoch, W12), what the region nibbles drive, the followers' ring, the callers of the
  mode 7 scenes in the event scripts and why scenes 0/9 stop at frame 225 (W11), the world
  map's palette-cycling routine and a field map with water animation (W14).
- The sprite frame format in bank $E4 and the palette source: answered in A1. Format 2
  (12-piece) frames are still open.
- Whether Marle (PC 1) can be forced into the party by a poke at the fair. Her object there is
  type 3 (EventCmd_LoadPc, a scripted actor), so the follower test used Lucca instead. A replay past the
  pendant scene would give a natural two-PC party for B1 battle tests.

## Sources

- The code and runs above (funcs.toml names come partly from the public-domain
  dscotton/ct_disassembly naming).
- Z-plane behaviour (two walk planes, transitional tiles), as Temporal Flux shows it: Chrono
  Compendium and Data Crystal community pages, e.g.
  [Data Crystal: Temporal Flux event tricks](https://datacrystal.tcrf.net/wiki/Chrono_Trigger_(SNES)/Temporal_Flux_Event_Tricks)
  (`$7E2980` = PC1),
  [Chrono Compendium: Temporal Flux FAQ](https://www.chronocompendium.com/Term/Temporal_Flux_FAQ.html),
  [romhacking.net: CT internals, Z-plane](https://www.romhacking.net/forum/index.php?topic=14449.0).
- Save/party layout: [Data Crystal RAM map](https://datacrystal.tcrf.net/wiki/Chrono_Trigger:RAM_map),
  [TASVideos submission 2240](https://tasvideos.org/2240S) (`$7E2980` party).
