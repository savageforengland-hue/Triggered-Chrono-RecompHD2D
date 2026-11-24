#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a pack.toml for a user's existing MSU-1 PCM directory.

No ROM patching, copying or audio conversion. The Chrono Trigger profile uses
the original song IDs and one-shot policy documented by the MSU music driver:
https://github.com/qwertymodo/ChronoTrigger-MSU1/blob/master/chrono_msu1_music.asm
Patch-only attract/video tracks are deliberately excluded from that profile.
"""
import argparse
import json
from pathlib import Path
import re
import struct


CT_ONESHOTS = {15, 28, 36, 37, 43, 46, 48, 61, 63, 72}


def inspect_pcm(path):
    with path.open('rb') as stream:
        header = stream.read(8)
        stream.seek(0, 2)
        size = stream.tell()
    if len(header) != 8 or header[:4] != b'MSU1':
        raise ValueError(f'{path.name}: missing MSU1 header')
    if size < 12 or (size - 8) % 4:
        raise ValueError(f'{path.name}: empty or incomplete stereo frames')
    loop = struct.unpack('<I', header[4:])[0]
    if loop >= (size - 8) // 4:
        raise ValueError(f'{path.name}: loop point outside audio')


def import_pack(directory, prefix=None, profile='chrono-trigger'):
    directory = Path(directory).resolve(strict=True)
    tracks = {}
    for path in sorted(directory.iterdir()):
        match = re.fullmatch(r'(.+)-(\d+)\.pcm', path.name, re.IGNORECASE)
        if not match or (prefix is not None and match[1] != prefix):
            continue
        if not path.resolve().is_relative_to(directory):
            raise ValueError(f'{path.name}: symlink leaves the pack folder')
        if any(c in path.name for c in '"\\:') or any(ord(c) < 32 for c in path.name):
            raise ValueError(f'{path.name!r}: unsupported manifest filename')
        name, track = match[1], int(match[2])
        if track in tracks.setdefault(name, {}):
            raise ValueError(f'{name}: duplicate track {track}')
        tracks[name][track] = path
    if len(tracks) != 1:
        raise ValueError('select one PCM filename prefix with --prefix; '
                         f'found {len(tracks)} matching prefixes')
    name, files = next(iter(tracks.items()))
    lines = ['[pack]', 'version = 1', f'name = {json.dumps(name, ensure_ascii=False)}', '']
    count, skipped = 0, []
    for track, path in sorted(files.items()):
        if not 1 <= track <= (83 if profile == 'chrono-trigger' else 255):
            skipped.append(track)
            continue
        inspect_pcm(path)
        loop = profile != 'chrono-trigger' or track not in CT_ONESHOTS
        lines.extend(['[[music]]', f'song = {track}',
                      f'file = {json.dumps(path.name, ensure_ascii=False)}',
                      f'loop = {str(loop).lower()}', ''])
        count += 1
    if not count:
        raise ValueError('no supported tracks found')
    # Exclusive creation: never replace a user's manually edited manifest.
    with (directory / 'pack.toml').open('x', encoding='utf-8') as out:
        out.write('\n'.join(lines))
    return count, skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--prefix', help='e.g. chrono_msu1 for chrono_msu1-1.pcm')
    parser.add_argument('--profile', choices=['chrono-trigger', 'identity'],
                        default='chrono-trigger',
                        help='identity maps tracks 1-255 directly, all looping')
    args = parser.parse_args()
    try:
        count, skipped = import_pack(args.directory, args.prefix, args.profile)
    except (OSError, ValueError) as error:
        parser.exit(1, f'MSU-1 import: {error}\n')
    print(f'Created {args.directory / "pack.toml"}: {count} tracks')
    if skipped:
        print('Skipped tracks outside this profile:', ', '.join(map(str, skipped)))
    print('Enable this folder through the existing asset-pack settings.')


if __name__ == '__main__':
    main()
