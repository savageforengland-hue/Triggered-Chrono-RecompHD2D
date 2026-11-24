#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 jerbanpls
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a package for content derived from the ROM: nothing built from
the user's ROM may be distributed (docs/MODULES.md).

usage: check_dist.py PATH... [--rom ROM] [--samples N] [--quiet]

PATH is a directory (scanned recursively, symlinks not followed) or a
file. Archives are looked inside, nested ones too: .zip, .tar (also
.tar.gz/.bz2/.xz), and .AppImage (its squashfs, with unsquashfs if it is
installed; without it the AppImage's raw bytes are scanned and a warning
printed).

A file fails the check if
  - its name is a game module's (ct_game*.so) or a generated source's
    (bank*.c, ct_funcs.c, ct_overlays.c);
  - it is part of an asset pack (docs/ASSET-PACKS.md): a pack.toml
    manifest, a picture named by a content key (tiles/, sprites/ or
    objects/ followed by 16 hex digits and .png), or an audio pack's
    sound (an .ogg, .oga, .opus, .flac, .wav or .mp3 file in a music/ or
    packs/ folder, or one whose bytes are Ogg or FLAC there under any
    name). Packs dumped from the game hold its art, and audio packs most
    often recordings of its music; they are for personal use and never
    ship;
  - it is an ELF file that exports ct_module_get: a game module, even
    renamed and stripped (runtime/module.h);
  - it contains a generated function's name (f_XXXXXX_mNxN: the symbols
    and strings the translator emits);
  - it contains a 64-byte window of the ROM. The ROM is sampled: about
    N windows (default 4096), spread over its 64 KiB banks, skipping
    padding and other low-variety data (fewer than 16 distinct bytes).
    Only windows starting with one of a few anchor byte values are
    sampled, so a file is checked at just the offsets holding an anchor
    byte, each with one set lookup. A copy of the ROM is always found;
    of random pieces of non-padding ROM data hidden in other bytes, the
    default found about 99% of 4 KiB pieces, all 16 KiB ones, and 70% of
    1 KiB ones (more --samples find more of the short ones).

Without --rom (CI, which never has one) the name, ELF-export and
generated-name checks still run, and the ROM-window check is skipped
with a notice.

Exit status: 0 clean, 1 ROM-derived content found, 2 usage error.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

WINDOW = 64
BANK = 0x10000
MIN_DISTINCT = 16
GENERATED_NAMES = ('ct_game*.so', 'ct_game*.so.*', 'bank*.c', 'ct_funcs.c', 'ct_overlays.c')
SYMBOL_RE = re.compile(rb'f_[0-9A-F]{6}_m[01]x[01]')
MODULE_ENTRY = b'ct_module_get'   # runtime/module.h
PACK_MANIFEST = 'pack.toml'       # asset packs (docs/ASSET-PACKS.md)
PACK_PICTURE_RE = re.compile(r'(^|[/!\\])((tiles|sprites|objects)[/\\][0-9a-f]{16}'
                             r'|poses[/\\][0-9a-f]{8}|sheet_[a-z0-9_]+_\d+x)\.png$', re.I)
# Audio packs' sound ([[music]]): in a music/ or packs/ folder (any depth below).
PACK_AUDIO_DIR_RE = re.compile(r'(^|[/!\\])(music|packs)[/\\]', re.I)
PACK_AUDIO_EXT_RE = re.compile(r'\.(ogg|oga|opus|flac|wav|mp3|pcm)$', re.I)
PACK_AUDIO_MAGIC = (b'OggS', b'fLaC', b'MSU1')


# Byte tables that are part of no game's data but turn up in many programs
# (compilers, codecs, libc): a ROM window inside one of them proves nothing,
# so none is ever sampled. 256-byte tables, each also doubled so a window
# that wraps the end is covered.
def _generic_tables() -> list[bytes]:
    bitrev = bytes(int(f'{i:08b}'[::-1], 2) for i in range(256))
    ident = bytes(range(256))
    return [t + t for t in (bitrev, ident, ident[::-1])]


GENERIC_TABLES = _generic_tables()


def is_generic(window: bytes) -> bool:
    return any(window in t for t in GENERIC_TABLES)


class RomSamples:
    """A hash set of sampled ROM windows and the anchor bytes they start with."""

    def __init__(self, rom: bytes, target: int):
        counts = [0] * 256
        for b in rom:
            counts[b] += 1
        # Anchors: mid-frequency byte values, never the usual fill bytes, so
        # they are spread through the data rather than clumped in one table.
        order = sorted((v for v in range(256) if v not in (0x00, 0xFF) and counts[v]),
                       key=lambda v: counts[v])
        mid = len(order) // 2
        self.anchors = bytes(sorted(order[mid - 2:mid + 2]))
        per_bank = max(1, target // max(1, len(rom) // BANK))
        self.windows: set[bytes] = set()
        anchor_re = re.compile(b'[' + b''.join(re.escape(bytes([a])) for a in self.anchors) + b']')
        for base in range(0, len(rom), BANK):
            bank = rom[base:base + BANK + WINDOW - 1]
            cands = [w for w in (bank[m.start():m.start() + WINDOW]
                                 for m in anchor_re.finditer(bank))
                     if len(w) == WINDOW and len(set(w)) >= MIN_DISTINCT
                     and not is_generic(w)]
            n = min(per_bank, len(cands))   # evenly spaced through the bank
            self.windows.update(cands[k * len(cands) // n] for k in range(n))
        self.anchor_re = anchor_re

    def find(self, data: bytes) -> int:
        """Offset of the first sampled ROM window in data, or -1."""
        windows = self.windows
        for m in self.anchor_re.finditer(data):
            i = m.start()
            if data[i:i + WINDOW] in windows:
                return i
        return -1


def elf_defines(data: bytes, symbol: bytes) -> bool:
    """Whether data is a 64-bit ELF whose dynamic symbol table defines
    symbol: a game module, even stripped and renamed, exports its entry."""
    if data[:4] != b'\x7fELF' or data[4] != 2 or len(data) < 64:
        return False
    order = 'little' if data[5] == 1 else 'big'
    def u(off: int, n: int) -> int:
        return int.from_bytes(data[off:off + n], order)
    shoff, shentsize, shnum = u(0x28, 8), u(0x3A, 2), u(0x3C, 2)
    if not shoff or shentsize < 64 or shoff + shnum * shentsize > len(data):
        return False
    sections = [shoff + k * shentsize for k in range(shnum)]
    for sh in sections:
        if u(sh + 4, 4) != 11:   # SHT_DYNSYM
            continue
        off, size, link, entsize = u(sh + 0x18, 8), u(sh + 0x20, 8), u(sh + 0x28, 4), u(sh + 0x38, 8)
        if link >= shnum or entsize < 24:
            continue
        stroff = u(sections[link] + 0x18, 8)
        for s in range(off, min(off + size, len(data)), entsize):
            name, shndx = u(s, 4), u(s + 6, 2)
            if shndx and data[stroff + name:stroff + name + len(symbol) + 1] == symbol + b'\0':
                return True
    return False


class Checker:
    def __init__(self, samples: RomSamples | None, quiet: bool, allow: dict[str, str] | None = None):
        self.samples = samples
        self.allow = allow or {}
        self.quiet = quiet
        self.files = 0
        self.findings: list[str] = []
        self.warnings: list[str] = []

    def fail(self, where: str, why: str) -> None:
        self.findings.append(f'{where}: {why}')
        print(f'check_dist: FAIL {where}: {why}')

    def warn(self, where: str, why: str) -> None:
        self.warnings.append(f'{where}: {why}')
        print(f'check_dist: warning: {where}: {why}', file=sys.stderr)

    # ---- one file's bytes ----
    def check_bytes(self, where: str, name: str, data: bytes) -> None:
        self.files += 1
        before = len(self.findings)
        base = os.path.basename(name)
        for pat in GENERATED_NAMES:
            if fnmatch.fnmatchcase(base, pat):
                self.fail(where, f'named like ROM-derived output ({pat})')
                break
        if base.lower() == PACK_MANIFEST:
            self.fail(where, 'an asset pack manifest (packs are personal-use only, never shipped)')
        elif PACK_PICTURE_RE.search(where):
            self.fail(where, 'a picture from an asset pack or a sprite sheet export '
                             '(named by a content or pose key)')
        elif PACK_AUDIO_DIR_RE.search(where) and (PACK_AUDIO_EXT_RE.search(base) or
                                                  data[:4] in PACK_AUDIO_MAGIC):
            self.fail(where, 'sound from an audio pack (packs are personal-use only, never '
                             'shipped)')
        if elf_defines(data, MODULE_ENTRY):
            self.fail(where, f'a game module (exports {MODULE_ENTRY.decode()})')
        m = SYMBOL_RE.search(data)
        if m:
            self.fail(where, f'generated function name {m.group().decode()} at offset {m.start()}')
        allowed = self.allow.get(hashlib.sha256(data).hexdigest()) if self.allow else None
        off = self.samples.find(data) if self.samples and not allowed else -1
        if allowed:
            print(f'check_dist: notice: {where}: ROM-window check skipped (--allow: {allowed})')
        if off >= 0:
            self.fail(where, f'a 64-byte window of the ROM at offset {off}')
        if not self.quiet and len(self.findings) == before:
            print(f'check_dist: ok {where}')
        self.look_inside(where, name, data)

    # ---- archives ----
    def look_inside(self, where: str, name: str, data: bytes) -> None:
        low = name.lower()
        if data[:4] == b'PK\x03\x04' or low.endswith('.zip'):
            try:
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    for info in z.infolist():
                        if not info.is_dir():
                            self.check_bytes(f'{where}!{info.filename}', info.filename,
                                             z.read(info))
            except zipfile.BadZipFile:
                if low.endswith('.zip'):
                    self.warn(where, 'not a readable zip')
            return
        if re.search(r'\.(tar|tgz|tbz2?|txz|tar\.(gz|bz2|xz))$', low) or data[257:262] == b'ustar':
            try:
                with tarfile.open(fileobj=io.BytesIO(data)) as t:
                    for m in t:
                        if m.isfile():
                            f = t.extractfile(m)
                            if f:
                                self.check_bytes(f'{where}!{m.name}', m.name, f.read())
            except tarfile.TarError:
                self.warn(where, 'not a readable tar')
            return
        if data[:4] == b'\x7fELF' and (low.endswith('.appimage') or b'AI\x02' == data[8:11]):
            self.appimage(where, data)

    def appimage(self, where: str, data: bytes) -> None:
        # The squashfs follows the ELF runtime: at the end of its section
        # header table.
        if data[4] != 2:   # ELFCLASS64 only (every current AppImage runtime)
            self.warn(where, 'AppImage: 32-bit runtime, contents not checked')
            return
        little = data[5] == 1
        def u(off: int, n: int) -> int:
            return int.from_bytes(data[off:off + n], 'little' if little else 'big')
        offset = u(0x28, 8) + u(0x3A, 2) * u(0x3C, 2)
        if data[offset:offset + 4] != b'hsqs':
            found = data.find(b'hsqs', 0, 1 << 22)
            if found < 0:
                self.warn(where, 'AppImage: no squashfs found, contents not checked')
                return
            offset = found
        tool = shutil.which('unsquashfs')
        if not tool:
            self.warn(where, 'AppImage: unsquashfs not installed, only the raw bytes were checked')
            return
        with tempfile.TemporaryDirectory(prefix='check_dist.') as tmp:
            img = os.path.join(tmp, 'image.sqfs')
            with open(img, 'wb') as f:
                f.write(data[offset:])
            out = os.path.join(tmp, 'root')
            r = subprocess.run([tool, '-no-xattrs', '-d', out, img], capture_output=True,
                               text=True)
            if r.returncode != 0:
                self.warn(where, f'AppImage: unsquashfs failed: {r.stderr.strip()[:200]}')
                return
            self.walk(out, f'{where}!')

    # ---- directories ----
    def walk(self, root: str, prefix: str = '') -> None:
        for dirpath, dirs, files in os.walk(root):
            dirs.sort()
            for fn in sorted(files):
                path = os.path.join(dirpath, fn)
                if os.path.islink(path) or not os.path.isfile(path):
                    continue
                with open(path, 'rb') as f:
                    data = f.read()
                self.check_bytes(prefix + os.path.relpath(path, root), fn, data)

    def check_path(self, path: str) -> None:
        if os.path.isdir(path):
            self.walk(path, path.rstrip('/') + '/')
        else:
            with open(path, 'rb') as f:
                data = f.read()
            self.check_bytes(path, os.path.basename(path), data)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='+', metavar='PATH')
    ap.add_argument('--rom', help='the ROM, for the ROM-window check (skipped without it)')
    ap.add_argument('--samples', type=int, default=4096)
    ap.add_argument('--quiet', action='store_true', help='print only failures and the summary')
    ap.add_argument('--allow', action='append', default=[], metavar='FILE',
                    help='a list of "SHA256 reason" lines: files with exactly these hashes (pinned '
                         'third-party binaries) skip only the ROM-window check, since generic '
                         'tables in them can match ROM bytes by chance')
    a = ap.parse_args(argv[1:])
    samples = None
    if a.rom is None:
        print('check_dist: notice: no --rom given: the ROM-window check is skipped; '
              'the name, ELF-export and generated-name checks still run')
    else:
        try:
            with open(a.rom, 'rb') as f:
                rom = f.read()
        except OSError as e:
            print(f'check_dist: cannot read the ROM: {e}', file=sys.stderr)
            return 2
        samples = RomSamples(rom, a.samples)
        print(f'check_dist: {len(samples.windows)} ROM windows sampled '
              f'(anchors {samples.anchors.hex(" ")})')
    allow: dict[str, str] = {}
    for path in a.allow:
        try:
            with open(path, encoding='utf-8') as f:
                for line in f:
                    line = line.split('#', 1)[0].strip()
                    if line:
                        h, _, why = line.partition(' ')
                        if len(h) != 64:
                            print(f'check_dist: {path}: bad line: {line}', file=sys.stderr)
                            return 2
                        allow[h.lower()] = why.strip() or 'allowed'
        except OSError as e:
            print(f'check_dist: cannot read --allow {path}: {e}', file=sys.stderr)
            return 2
    c = Checker(samples, a.quiet, allow)
    for p in a.paths:
        if not os.path.exists(p):
            print(f'check_dist: no such file or directory: {p}', file=sys.stderr)
            return 2
        c.check_path(p)
    print(f'check_dist: {c.files} files checked, {len(c.findings)} problems, '
          f'{len(c.warnings)} warnings')
    return 1 if c.findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
