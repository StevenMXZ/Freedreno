#!/usr/bin/env python3
"""Create the three-file Freedreno OpenGL package accepted by Ludashi Components."""
import argparse
import json
from pathlib import Path
import struct
import zipfile


def needed(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', data, 16) != (3, 183):
        raise ValueError(f'{path.name}: expected ARM64 ELF shared library')
    ph = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size < 56 or ph + size * count > len(data):
        raise ValueError('Invalid ELF program headers')
    headers = [struct.unpack_from('<IIQQQQQQ', data, ph + i * size) for i in range(count)]
    dynamic = next((h for h in headers if h[0] == 2), None)
    if not dynamic or dynamic[2] + dynamic[5] > len(data):
        raise ValueError('Invalid ELF dynamic section')
    tags = []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5] - 15, 16):
        tag, value = struct.unpack_from('<QQ', data, offset)
        if tag == 0:
            break
        tags.append((tag, value))
    strings = next(v for k, v in tags if k == 5)
    segment = next(h for h in headers if h[0] == 1 and h[3] <= strings < h[3] + h[5])
    start = segment[2] + strings - segment[3]
    result = []
    for tag, value in tags:
        if tag == 1:
            pos = start + value
            if pos >= len(data):
                raise ValueError('Invalid ELF string offset')
            end = data.index(b'\0', pos, min(pos + 512, len(data)))
            result.append(data[pos:end].decode())
    return result


def package(libdir, output, version):
    gl = libdir / 'libGL.so.1'
    if not gl.is_file():
        candidates = sorted(p for p in libdir.glob('libGL.so*') if p.is_file())
        if not candidates:
            raise ValueError('libGL missing')
        gl = candidates[0]
    gallium_names = [name for name in needed(gl) if name.startswith('libgallium') and name.endswith('.so')]
    if len(gallium_names) != 1 or Path(gallium_names[0]).name != gallium_names[0]:
        raise ValueError('libGL must depend on exactly one paired libgallium')
    gallium = libdir / gallium_names[0]
    needed(gallium)
    driver_version = gallium.name.removeprefix('libgallium-').removesuffix('.so')
    metadata = dict(schemaVersion=1, type='freedreno', name='Freedreno DRI3',
                    author='StevenMXZ', packageVersion=version, vendor='Mesa',
                    driverVersion=driver_version, libraryName='libGL.so.1')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr('meta.json', json.dumps(metadata, indent=2) + '\n')
        archive.write(gl, 'libGL.so.1')
        archive.write(gallium, gallium.name)
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('libdir', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.libdir, args.output, args.version), indent=2))
