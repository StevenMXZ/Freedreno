import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('packager', Path(__file__).with_name('package-freedreno.py'))
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def elf(path, dependency):
    data = bytearray(512)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HH', data, 16, 3, 183)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 2)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 4, 0, 0, 0, 512, 512, 8)
    struct.pack_into('<IIQQQQQQ', data, 120, 2, 4, 200, 200, 200, 48, 48, 8)
    struct.pack_into('<QQQQQQ', data, 200, 1, 0, 5, 300, 0, 0)
    name = dependency.encode() + b'\0'
    data[300:300+len(name)] = name
    path.write_bytes(data)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        elf(self.root / 'libGL.so.1', 'libgallium-test.so')
        elf(self.root / 'libgallium-test.so', 'libc.so')
        self.output = self.root / 'driver.zip'

    def test_only_runtime_pair_and_metadata_are_published(self):
        (self.root / 'debug-symbols.txt').write_text('not shipped')
        packager.package(self.root, self.output, '42')
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {'meta.json', 'libGL.so.1', 'libgallium-test.so'})
            metadata = json.loads(archive.read('meta.json'))
            self.assertEqual(metadata['type'], 'freedreno')
            self.assertEqual(metadata['packageVersion'], '42')
            self.assertNotIn('sourceCommit', metadata)
            self.assertNotIn('architecture', metadata)

    def test_revision_changes_only_display_metadata(self):
        for mesa_version in ('26.3.0', '27.0.0'):
            with self.subTest(mesa_version=mesa_version):
                name = f'libgallium-{mesa_version}-devel.so'
                elf(self.root / 'libGL.so.1', name)
                elf(self.root / name, 'libc.so')
                packager.package(self.root, self.output, '42.1', revision=2)
                with zipfile.ZipFile(self.output) as archive:
                    meta = json.loads(archive.read('meta.json'))
                    self.assertEqual(meta['driverVersion'], f'{mesa_version}-R2')
                    self.assertEqual(meta['packageVersion'], '42.1')
                    self.assertEqual(set(archive.namelist()), {'meta.json', 'libGL.so.1', name})
                    self.assertEqual(archive.read(name), (self.root / name).read_bytes())

    def test_invalid_revision_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'positive integer'):
            packager.package(self.root, self.output, '42', revision=0)
        self.assertFalse(self.output.exists())

    def test_missing_paired_gallium_fails_before_creating_zip(self):
        (self.root / 'libgallium-test.so').unlink()
        with self.assertRaises(FileNotFoundError):
            packager.package(self.root, self.output, '42')
        self.assertFalse(self.output.exists())

    def test_wrong_architecture_is_rejected(self):
        path = self.root / 'libgallium-test.so'
        data = bytearray(path.read_bytes())
        struct.pack_into('<H', data, 18, 62)
        path.write_bytes(data)
        with self.assertRaises(ValueError):
            packager.package(self.root, self.output, '42')
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
