#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 jerbanpls
# SPDX-License-Identifier: GPL-3.0-or-later
"""The scene the game module describes (game/ct/scene.c, runtime/module.h)
must match an independent decoding of the same WRAM and OAM
(tools/scene_probe.py --text, from docs/SCENE.md): map, camera, every
layer's tilemap, every cell's class and flags, and the objects with the OAM
entries that draw them. Checked in Leene Square (frame 8700, static build)
and the bell plaza (9200, the engine with the module loaded), that a
battle (gato_win, 10800) is not a field scene, and the world map
(world_map_save, 7600, the module: worldmap_probe.py --text, the same
records at the world map's sizes plus the cloud overlay).

usage: test_scene.py STATIC_BOOT ENGINE_BOOT MODULE REPLAY_DIR PROBE
"""
import os
import subprocess
import sys
import tempfile


def capture(boot: list[str], script: str, frames: int, d: str, tag: str) -> tuple[str, str]:
    s, w, v = (os.path.join(d, f'{tag}.{x}') for x in ('scene', 'wram', 'vram'))
    r = subprocess.run(boot + ['--frames', str(frames), '--script', script, '--scene', s,
                               '--wram', w, '--vram', v], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
        raise SystemExit(f'failed: {" ".join(boot)} at {frames}')
    return s, w, v


def check(probe: str, scene: str, wram: str, vram: str, want_mode: int, tag: str) -> int:
    expect = scene + '.expected'
    subprocess.run([sys.executable, probe, wram, '--vram', vram, '--text', expect], check=True)
    with open(scene) as f:
        got = f.read().splitlines()
    with open(expect) as f:
        want = f.read().splitlines()
    if got and got[0] != f'mode {want_mode}':
        print(f'test_scene: {tag}: {got[0]}, expected mode {want_mode}')
        return 1
    if len(got) > 1:   # the module's generation counter: any, but counted from 1
        head = got[1].split()
        if head[-2] != 'gen' or int(head[-1]) < 1:
            print(f'test_scene: {tag}: bad map line {got[1]}')
            return 1
        got[1] = ' '.join(head[:-1] + ['?'])
    for n, (a, b) in enumerate(zip(got, want), 1):
        if a != b:
            print(f'test_scene: {tag}: line {n} differs\n  module: {a[:160]}\n  probe:  {b[:160]}')
            return 1
    if len(got) != len(want):
        print(f'test_scene: {tag}: {len(got)} lines, probe {len(want)}')
        return 1
    objs = [l for l in got if l.startswith('object')]
    drawn = [l for l in objs if len(l.split()) > 6]
    if want_mode == 1 and (len(drawn) < 4 or 'player -1' in got):
        print(f'test_scene: {tag}: {len(drawn)} objects with pieces, {got[3]}')
        return 1
    if want_mode == 2 and (not drawn or not drawn[0].startswith('object 0 ')):
        print(f'test_scene: {tag}: the party has no pieces')
        return 1
    print(f'test_scene: {tag}: {len(got)} lines match ({len(objs)} objects, {len(drawn)} drawn)')
    return 0


def check_mode(scene: str, want_mode: int, tag: str) -> int:
    """A view with no map payload: only its generic scene kind is valid."""
    with open(scene) as f:
        head = f.readline().strip()
    if head != f'mode {want_mode}':
        print(f'test_scene: {tag}: {head}, expected mode {want_mode}')
        return 1
    print(f'test_scene: {tag}: {head}')
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2
    static_boot, engine_boot, module, replay, probe = argv[1:]
    leene, gato = os.path.join(replay, 'leene_square.txt'), os.path.join(replay, 'gato_win.txt')
    world = os.path.join(replay, 'world_map_save.txt')
    wprobe = os.path.join(os.path.dirname(probe), 'worldmap_probe.py')
    # Into the working directory (the build tree): ROM-derived files stay there.
    with tempfile.TemporaryDirectory(dir=os.getcwd()) as d:
        name_scene, _, _ = capture([static_boot], leene, 1800, d, 'name_entry')
        bad = check_mode(name_scene, 3, 'name entry 1800')
        bad |= check(probe, *capture([static_boot], leene, 8700, d, 'leene'), 1, 'leene 8700')
        bad |= check(probe, *capture([engine_boot, '--module', module], leene, 9200, d, 'plaza'),
                     1, 'plaza 9200 (module)')
        bad |= check(probe, *capture([static_boot], gato, 10800, d, 'battle'), 0, 'battle 10800')
        bad |= check(wprobe, *capture([engine_boot, '--module', module], world, 7600, d, 'world'),
                     2, 'world 7600 (module)')
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv))
