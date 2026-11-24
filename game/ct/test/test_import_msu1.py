# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic MSU-1 imports; no copyrighted recordings or ROM required."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'import_msu1', Path(__file__).resolve().parents[1] / 'tools/import_msu1.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ImportTests(unittest.TestCase):
    def pcm(self, root, name, loop=0):
        (root / name).write_bytes(b'MSU1' + struct.pack('<I', loop) + b'\x00' * 16)

    def test_profile_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for track in [1, 15, 24, 84, 100]:
                self.pcm(root, f'chrono-{track}.pcm')
            self.assertEqual(module.import_pack(root), (3, [84, 100]))
            text = (root / 'pack.toml').read_text()
            self.assertIn('song = 15\nfile = "chrono-15.pcm"\nloop = false', text)
            self.assertIn('song = 24\nfile = "chrono-24.pcm"\nloop = true', text)
            with self.assertRaises(FileExistsError):
                module.import_pack(root)
            self.assertEqual((root / 'pack.toml').read_text(), text)

    def test_bad_loop_is_atomic(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.pcm(root, 'chrono-1.pcm')
            self.pcm(root, 'chrono-2.pcm', loop=4)
            with self.assertRaises(ValueError):
                module.import_pack(root)
            self.assertFalse((root / 'pack.toml').exists())

    def test_ambiguous_and_duplicate_tracks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.pcm(root, 'a-1.pcm')
            self.pcm(root, 'b-1.pcm')
            with self.assertRaises(ValueError):
                module.import_pack(root)
            self.assertEqual(module.import_pack(root, 'b'), (1, []))
            self.pcm(root, 'a-01.pcm')
            with self.assertRaises(ValueError):
                module.import_pack(root, 'a')

    def test_truncated_and_wrong_magic(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'broken.pcm'
            for data in [b'', b'RIFF' + b'\0' * 12, b'MSU1' + b'\0' * 9]:
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    module.inspect_pcm(path)


if __name__ == '__main__':
    unittest.main()
