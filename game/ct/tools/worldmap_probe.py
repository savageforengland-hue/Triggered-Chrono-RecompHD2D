#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 jerbanpls
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the world-map "scene" the game holds in WRAM, from ct_boot dumps.

The companion of scene_probe.py for the overworld engine (docs/SCENE.md,
"World map"). Reads a 128 KB WRAM dump (ct_boot --wram) and, with --vram
(ct_boot --vram: VRAM, CGRAM, OAM), prints: whether the dump is the world
map (NMI vector $C2015E and a location in $1F0-$1F7), the party position
and camera, the exit table, and the OAM entries as world positions. With
--png it renders the WHOLE 96x64-metatile map (BG2 ground, then the sparse
BG1 layer) from WRAM tables + VRAM tiles; --terrain writes the per-8px
terrain classes with the exits marked; --regions writes the region nibbles.

The PNGs are made from the running game's data: keep them in the build
dir, never commit them.

With --text it writes the scene as ct_boot --scene does (the module's
world_scene.c, decoded independently), for test_scene.py.

usage: worldmap_probe.py WRAM [--vram FILE] [--png OUT] [--terrain OUT]
                         [--regions OUT] [--chr WORD] [--text OUT]
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_probe as sp  # noqa: E402  (bgr555, tile_px, u16)

# World-map WRAM addresses (docs/SCENE.md, "World map"). Bank $7E, D = $0000.
NMI_WORLD = bytes([0x5C, 0x5E, 0x01, 0xC2])     # JML $C2015E
LOC = 0x0100                    # u16: $1F0-$1F7 are the world maps
TILE_X, TILE_Y = 0x0102, 0x0103   # party position, 8 px tiles
PX_X, PX_Y = 0x0283, 0x0285       # party position, px (u16): 1/8 of it is the tile
CAM_X, CAM_Y = 0xE3, 0xE7         # camera left/top, 8 px tiles (u16)
SHADOW = 0x1E                     # PPU register shadow the NMI copies out
MAP_W, MAP_H = 96, 64             # metatiles of 16 px; 1536 x 1024 px
MAP_FG = 0x4000                   # BG1: sparse layer, 0 = empty
MAP_BG = 0x5800                   # BG2: ground
DEFS_FG = 0x3000                  # 256 x 8 bytes: tilemap words TL, TR, BL, BR
DEFS_BG = 0x3800
PROPS = 0x7000                    # 2 bytes per ground metatile: 4 nibbles
REGION = 0x7200                   # 0x30 bytes per row: 1 nibble per metatile
EXITS = 0x7E00                    # 7-byte records, count at $1B38
N_EXITS = 0x1B38
EVENTS = 0x8300                   # 3-byte records, count at $1B39
N_EVENTS = 0x1B39
OBJ_PX = (16, 16)                 # OBSEL $60: small objects are 16x16
SPR_OFS = (8, 17)                 # screen = world - camera - this, for the party

TERRAIN = {0: 'walkable', 1: 'blocked', 2: 'water', 4: 'cover'}
COLORS = {0: (60, 160, 60), 1: (220, 40, 40), 2: (40, 80, 220), 4: (240, 220, 40)}


def u16(w, a):
    return w[a] | w[a + 1] << 8


def is_world_map(w):
    return bytes(w[0x500:0x504]) == NMI_WORLD and 0x1F0 <= u16(w, LOC) & 0x1FF <= 0x1F7


def terrain(w, tx, ty):
    """The class nibble of the 8 px tile (tx, ty): 0 walkable, bit 0 blocked,
    bit 1 water, bit 2 cover. Taken from the ground layer only."""
    idx = w[MAP_BG + (ty >> 1) * MAP_W + (tx >> 1)]
    b = w[PROPS + idx * 2 + (ty & 1)]
    return b & 15 if tx & 1 else b >> 4


def exits(w):
    out = []
    for i in range(w[N_EXITS]):
        r = w[EXITS + 7 * i:EXITS + 7 * i + 7]
        if r[0] & 0x80:
            out.append({'x': r[0] & 0x7F, 'y': r[1] & 0x3F, 'param': r[2],
                        'dest': (r[3] | r[4] << 8) & 0x1FF, 'facing': (r[4] >> 1) & 3,
                        'dx': r[5], 'dy': r[6]})
    return out


def camera(w):
    """Top-left of the 256x224 view in world px (wraps at 1536 x 1024)."""
    # Usually this equals party - (128,112). During the opening flyover
    # the view moves without the party, so the PPU shadows are authoritative.
    return u16(w, SHADOW + 2) % (MAP_W * 16), u16(w, SHADOW + 8) % (MAP_H * 16)


def png(path, W, H, img):
    raw = b''.join(b'\0' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
    print(f'wrote {path} ({W}x{H})')


def render_map(w, vram, cgram, chr_word, out):
    W, H = MAP_W * 16, MAP_H * 16
    pal = [sp.bgr555(cgram[2 * i] | cgram[2 * i + 1] << 8) for i in range(256)]
    img = bytearray(bytes(pal[0]) * (W * H))
    for mapb, defb in ((MAP_BG, DEFS_BG), (MAP_FG, DEFS_FG)):
        for ty in range(MAP_H):
            for tx in range(MAP_W):
                idx = w[mapb + ty * MAP_W + tx]
                if mapb == MAP_FG and idx == 0:
                    continue
                for q in range(4):
                    e = u16(w, defb + idx * 8 + q * 2)
                    for r in range(8):
                        row = sp.tile_px(vram, chr_word, e & 0x3FF, 7 - r if e & 0x8000 else r)
                        if e & 0x4000:
                            row.reverse()
                        py = ty * 16 + (q >> 1) * 8 + r
                        for x, c in enumerate(row):
                            if c:
                                px = tx * 16 + (q & 1) * 8 + x
                                o = (py * W + px) * 3
                                img[o:o + 3] = bytes(pal[((e >> 10) & 7) * 16 + c])
    png(out, W, H, img)


def render_terrain(w, out, scale=4):
    W, H = MAP_W * 2 * scale, MAP_H * 2 * scale
    img = bytearray(W * H * 3)
    for y in range(MAP_H * 2):
        for x in range(MAP_W * 2):
            c = bytes(COLORS.get(terrain(w, x, y), (255, 0, 255)))
            for dy in range(scale):
                o = ((y * scale + dy) * W + x * scale) * 3
                img[o:o + 3 * scale] = c * scale
    for e in exits(w):                       # exits: white 4x4 px block per metatile
        for dy in range(2 * scale):
            o = ((e['y'] * 2 * scale + dy) * W + e['x'] * 2 * scale) * 3
            img[o:o + 6 * scale] = b'\xff\xff\xff' * 2 * scale
    png(out, W, H, img)


def render_regions(w, out, scale=8):
    W, H = MAP_W * scale, MAP_H * scale
    img = bytearray(W * H * 3)
    for y in range(MAP_H):
        for x in range(MAP_W):
            b = w[REGION + y * 0x30 + (x >> 1)]
            v = b & 15 if x & 1 else b >> 4
            c = bytes(((v * 67) & 255, (v * 131 + 40) & 255, (v * 29 + 90) & 255)) if v else b'\0\0\0'
            for dy in range(scale):
                o = ((y * scale + dy) * W + x * scale) * 3
                img[o:o + 3 * scale] = c * scale
    png(out, W, H, img)


def print_scene(w, oam):
    loc = u16(w, LOC) & 0x1FF
    print(f'NMI vector {bytes(w[0x500:0x504]).hex()}, location ${loc:03X}: '
          f'{"WORLD MAP" if is_world_map(w) else "not the world map"}')
    cx, cy = camera(w)
    print(f'party tile {w[TILE_X]},{w[TILE_Y]}  px {u16(w, PX_X)},{u16(w, PX_Y)}  '
          f'metatile {u16(w, PX_X) >> 4},{u16(w, PX_Y) >> 4}')
    print(f'camera (world px, derived) {cx},{cy}; $E3/$E7 {u16(w, CAM_X)},{u16(w, CAM_Y)} (8 px tiles); '
          f'BG1 scroll {u16(w, SHADOW + 2)},{u16(w, SHADOW + 8)}')
    print(f'BGMODE {w[SHADOW]:02X}  BG1/2/3 HOFS {u16(w, SHADOW + 2)}/{u16(w, SHADOW + 4)}/'
          f'{u16(w, SHADOW + 6)}  VOFS {u16(w, SHADOW + 8)}/{u16(w, SHADOW + 10)}/{u16(w, SHADOW + 12)}')
    tx, ty = u16(w, PX_X) >> 3, u16(w, PX_Y) >> 3
    print(f'terrain under the party: {TERRAIN.get(terrain(w, tx % 192, ty % 128), "?")}')
    ex = exits(w)
    print(f'{len(ex)} active exits of {w[N_EXITS]} ($7E7E00), {w[N_EVENTS]} event triggers ($7E8300)')
    for e in ex[:int(os.environ.get('WM_EXITS', 8))]:
        print(f"  exit at metatile {e['x']},{e['y']} -> location ${e['dest']:03X} "
              f"facing {e['facing']} field tile {e['dx']},{e['dy']} param {e['param']:02X}")
    if oam:
        for i in range(128):
            x, y, t, a = oam[4 * i:4 * i + 4]
            hi = (oam[512 + i // 4] >> (2 * (i % 4))) & 3
            if y >= 0xE0:
                continue
            sx = x - 256 if hi & 1 and x else x | (hi & 1) << 8
            size = 32 if hi & 2 else 16
            print(f'  oam {i:3d} screen {sx:4d},{y:3d} {size}px tile {t | (a & 1) << 8:03X} '
                  f'pal {(a >> 1) & 7} prio {(a >> 4) & 3}  -> world ({(sx + cx) % 1536},{(y + cy) % 1024})')


# The module's corrections to the renderer's pixel guess (world_scene.c
# relief[]): (map id or 0 for all, layer 0 = BG1 / 1 = BG2, metatile index)
# -> ct_scene_cell class. Keep the two in sync (the scene_decode test).
CELL_FLAT = 9
RELIEF = {(0x1F0, 0, i): CELL_FLAT for i in (161, 162, 163, 177, 178, 193, 194, 195, 196)}
RELIEF.update({(0x1F0, 1, i): CELL_FLAT for i in (176, 177, 178, 179, 180, 193, 194, 195, 196,
                                                  209, 210, 211)})
CELL_FLOOR, CELL_SOLID, CELL_WATER = 1, 2, 4
CELL_OVERHEAD, CELL_EVENT, CELL_COVER = 1, 2, 4
CAPTION_Y = 184


def relief(loc, layer, idx):
    return RELIEF.get((loc, layer, idx)) or RELIEF.get((0, layer, idx), 0)


def scene_text(w, vram, oam):
    """The scene as ct_boot --scene writes it for the world map (runtime/
    module.h CT_SCENE_WORLD, game/ct/world_scene.c), decoded here on its
    own from a WRAM dump, VRAM and OAM, for test_scene.py. 'gen' is the
    module's counter: written as '?'."""
    if not is_world_map(w):
        return None
    loc = u16(w, LOC) & 0x1FF
    cx, cy = camera(w)
    px, py = u16(w, PX_X), u16(w, PX_Y)
    vw = lambda a: vram[2 * (a & 0x7FFF)] | vram[2 * (a & 0x7FFF) + 1] << 8
    lines = ['mode 2', f'map 192 128 cell 8 id {loc} gen ?', f'camera {cx} {cy}', 'player 0',
             f'overlay wrap 1 blend 8 size 64 32 scroll {u16(w, SHADOW + 6) & 511} '
             f'{u16(w, SHADOW + 12) & 255}']
    words = {0: [[0] * 192 for _ in range(128)], 1: [[0] * 192 for _ in range(128)]}
    for my in range(MAP_H):
        for mx in range(MAP_W):
            fg, bg = w[MAP_FG + my * MAP_W + mx], w[MAP_BG + my * MAP_W + mx]
            for q in range(4):
                x, y = 2 * mx + (q & 1), 2 * my + (q >> 1)
                words[0][y][x] = u16(w, DEFS_FG + fg * 8 + 2 * q) if fg else 0
                words[1][y][x] = u16(w, DEFS_BG + bg * 8 + 2 * q)
    for layer in range(3):
        lines.append(f"layer {layer} present 1 bpp {4 if layer < 2 else 2} "
                     f"chr {0x2000 if layer < 2 else 0x7000:04X} pal 0")
        if layer < 2:
            rows = words[layer]
        else:   # BG3: 64 x 32, two 32 x 32 screens
            rows = [[vw(0x7800 + (x >> 5) * 1024 + y * 32 + (x & 31)) for x in range(64)]
                    for y in range(32)]
        for y, row in enumerate(rows):
            lines.append(f'tiles {layer} {y} ' + ' '.join('%04X' % e for e in row))
    events = set()
    for e in exits(w):
        if e['x'] < MAP_W and e['y'] < MAP_H:
            events.add((e['x'], e['y']))
    for y in range(128):
        row = []
        for x in range(192):
            mx, my = x >> 1, y >> 1
            n = terrain(w, x, y)
            fg, bg = w[MAP_FG + my * MAP_W + mx], w[MAP_BG + my * MAP_W + mx]
            if n & 2:
                cls = CELL_WATER
            elif n & 1:
                cls = (relief(loc, 0, fg) if fg else 0) or relief(loc, 1, bg) or CELL_SOLID
            else:
                cls = CELL_FLOOR
            flags = (CELL_COVER if n & 4 else 0) | \
                (CELL_OVERHEAD if (words[0][y][x] | words[1][y][x]) & 0x2000 else 0) | \
                (CELL_EVENT if (mx, my) in events else 0)
            row.append(f'{cls}.{flags}.0.{n:02X}')
        lines.append(f'cells {y} ' + ' '.join(row))
    # Objects: the party's pieces at its sprite's place, then groups of
    # touching pieces in the play area.
    ent = []
    for e in range(128):
        x, y, a = oam[4 * e], oam[4 * e + 1], oam[512 + e // 4] >> (2 * (e % 4)) & 1
        x |= a << 8
        if x >= 256:
            x -= 512
        ent.append((x, y, y < 224 and x > -16))
    vis = [v for _, _, v in ent]
    facing = w[0x0D70 + 0x28]
    party = []
    for e in range(128):
        x, y, _ = ent[e]
        if vis[e] and x == 120 and y in (95, 79) and len(party) < 16:
            party.append(e)
            vis[e] = False
    objs = [(px % 1536, py % 1024, facing if facing <= 3 else -1, party)]
    for e in range(128):
        if not vis[e] or ent[e][1] >= CAPTION_Y:
            continue
        group, vis[e] = [e], False
        grew = True
        while grew:
            grew = False
            for k in range(128):
                if not vis[k] or ent[k][1] >= CAPTION_Y or len(group) >= 16:
                    continue
                if any(ent[k][0] <= ent[p][0] + 16 and ent[k][0] + 16 >= ent[p][0] and
                       ent[k][1] <= ent[p][1] + 16 and ent[k][1] + 16 >= ent[p][1] for p in group):
                    group.append(k)
                    vis[k] = False
                    grew = True
        x0 = min(ent[p][0] for p in group)
        x1 = max(ent[p][0] + 16 for p in group)
        y1 = max(ent[p][1] + 16 for p in group)
        objs.append(((cx + (x0 + x1) // 2) % 1536, (cy + y1) % 1024, -1, group))
    for k, (x, y, f, pieces) in enumerate(objs):
        lines.append(' '.join([f'object {k} {x} {y} 0 {f}'] + [str(e) for e in pieces]))
    return lines


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('wram')
    ap.add_argument('--vram')
    ap.add_argument('--png')
    ap.add_argument('--terrain')
    ap.add_argument('--regions')
    ap.add_argument('--chr', default='2000', help='BG1/BG2 character base, VRAM word (hex; $210B = $22)')
    ap.add_argument('--text', help='write the scene as ct_boot --scene does (needs --vram)')
    a = ap.parse_args(argv)
    w = open(a.wram, 'rb').read()
    v = open(a.vram, 'rb').read() if a.vram else None
    if a.text:
        if not v:
            ap.error('--text needs --vram')
        lines = scene_text(w, v[:0x10000], v[0x10200:0x10420])
        with open(a.text, 'w') as f:
            f.write('\n'.join(lines or ['mode 0']) + '\n')
        return 0
    print_scene(w, v[0x10200:0x10420] if v else None)
    if a.terrain:
        render_terrain(w, a.terrain)
    if a.regions:
        render_regions(w, a.regions)
    if a.png:
        if not v:
            ap.error('--png needs --vram')
        render_map(w, v[:0x10000], v[0x10000:0x10200], int(a.chr, 16), a.png)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
