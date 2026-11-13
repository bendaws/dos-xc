#!/usr/bin/env python3
#
# DOS-XC /tools/dsp.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import lzma
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

KEYS = ('Name', 'Version', 'Author', 'Description', 'Arch', 'Path', 'StartIn', 'Required', 'Env', 'Driver',
        'Provides', 'Requires', 'Firewall')
DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1

def metadata_text(info):
    lines = []
    for k in KEYS:
        if k == 'Env':
            lines += [f'Env={value}\r\n' for value in info.get('Env', '').split('\n') if value]
        elif k in info and (k not in ('StartIn', 'Required') or info[k]):
            lines.append(f'{k}={info[k]}\r\n')
    return ''.join(lines)

def parse_metadata(text):
    info = {}
    for line in text.splitlines():
        if '=' in line and not line.startswith((';', '#')):
            key, value = line.split('=', 1)
            for k in KEYS:
                if k.lower() == key.strip().lower():
                    if k == 'Env' and info.get('Env'):
                        info['Env'] += '\n' + value.strip()
                    else:
                        info[k] = value.strip()
    return info

def xz_compress(raw, x86):
    """One .xz stream (CRC-32 checks, LZMA2 preset 6e, the x86 filter for programs), as os/pkg/unxz.c reads."""
    tool = shutil.which('xz')
    if tool:
        command = [tool, '-c', '-T0', '--format=xz', '--check=crc32'] + (['--x86'] if x86 else []) + ['--lzma2=preset=6e']
        return subprocess.run(command, input=raw, stdout=subprocess.PIPE, check=True).stdout
    filters = ([{'id': lzma.FILTER_X86}] if x86 else []) + [{'id': lzma.FILTER_LZMA2, 'preset': 6 | lzma.PRESET_EXTREME}]
    return lzma.compress(raw, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, filters=filters)

def pack(folder, out, info):
    for required in ('Name', 'Version', 'Arch'):
        if not info.get(required):
            raise SystemExit(f'error: {required}= is required')
    info.setdefault('Author', '')
    info.setdefault('Description', '')
    info.setdefault('Path', '')
    meta = metadata_text(info).encode('utf-8')
    data = bytearray(b'DSPK' + struct.pack('<HHI', 2, 0, len(meta)) + meta)
    files = sorted(p for p in Path(folder).rglob('*') if p.is_file() and not p.name.startswith('.'))
    files = [p for p in files if p.relative_to(folder).as_posix().upper() != 'PACKAGE.INF']
    data += struct.pack('<I', len(files))
    stream = bytearray()
    for path in files:
        raw = path.read_bytes()
        encoded = path.relative_to(folder).as_posix().encode('utf-8')
        data += struct.pack('<HII', len(encoded), len(raw), zlib.crc32(raw) & 0xffffffff) + encoded
        stream += raw
    data += xz_compress(bytes(stream), str(info.get('Arch')) != '16')
    Path(out).write_bytes(bytes(data))
    return info

def read(path):
    blob = Path(path).read_bytes()
    if blob[:4] != b'DSPK':
        raise SystemExit(f'error: {path} is not a DOS-XC package')
    version, _, length = struct.unpack_from('<HHI', blob, 4)
    info = parse_metadata(blob[12:12 + length].decode('utf-8'))
    if version == 2:
        at = 12 + length
        count, = struct.unpack_from('<I', blob, at)
        at += 4
        listed = []
        for _ in range(count):
            nlen, size, crc = struct.unpack_from('<HII', blob, at)
            listed.append((blob[at + 10:at + 10 + nlen].decode('utf-8'), size, crc))
            at += 10 + nlen
        stream = lzma.decompress(blob[at:], format=lzma.FORMAT_XZ)
        entries, offset = [], 0
        for name, size, crc in listed:
            raw = stream[offset:offset + size]
            offset += size
            if len(raw) != size or zlib.crc32(raw) & 0xffffffff != crc:
                raise SystemExit(f'error: {path}: {name} is damaged')
            entries.append((name, raw))
        return info, entries
    end = blob.rfind(b'PK\x05\x06')
    count, size, start = struct.unpack_from('<HII', blob, end + 10)
    entries, at = [], start
    for _ in range(count):
        (_, _, _, _, method, _, _, crc, csize, usize, nlen, elen, clen, _, _, _, offset) = \
            struct.unpack_from('<IHHHHHHIIIHHHHHII', blob, at)
        name = blob[at + 46:at + 46 + nlen].decode('utf-8')
        at += 46 + nlen + elen + clen
        lnlen, lelen = struct.unpack_from('<HH', blob, offset + 26)
        body = blob[offset + 30 + lnlen + lelen:offset + 30 + lnlen + lelen + csize]
        raw = zlib.decompress(body, -15) if method == 8 else body
        if zlib.crc32(raw) & 0xffffffff != crc or len(raw) != usize:
            raise SystemExit(f'error: {path}: {name} is damaged')
        entries.append((name, raw))
    return info, entries

def install(path, root):
    info, entries = read(path)
    target = Path(root) / 'Programs' / info['Name']
    for name, raw in entries:
        if name.endswith('/'):
            (target / name).mkdir(parents=True, exist_ok=True)
            continue
        out = target / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(raw)
    target.mkdir(parents=True, exist_ok=True)
    (target / 'PACKAGE.INF').write_bytes(metadata_text(info).encode('utf-8'))
    drivers = [name for name in info.get('Driver', '').split(';') if name]
    if drivers:
        folder = Path(root) / 'SYSTEM' / 'Drivers'
        folder.mkdir(parents=True, exist_ok=True)
        for name in drivers:
            (target / name).replace(folder / name)
    return info

def main():
    if len(sys.argv) < 3:
        raise SystemExit('Build and inspect DOS-XC packages (.dsp) on the build machine.\n\n  tools/dsp.py pack FOLDER OUT.dsp Name=... Version=... Author=... \\\n               Description=... Arch=16|32|64 [Path=...] [StartIn=program]\n               [Env=NAME=value ...] [Driver=A.DRV;B.DRV]\n               [Provides=role;...] [Requires=name[>=version];...]\n  tools/dsp.py list FILE.dsp\n  tools/dsp.py install FILE.dsp ROOT      extract to ROOT/Programs/<Name>\n                                          and write its PACKAGE.INF\n\nThe format (see docs/PACKAGES.md): "DSPK", u16 version 2, u16 flags,\nu32 metadata length, the metadata ("Key=value" lines), the file list, then\none .xz stream of the files\' data.  Version 1 (a ZIP archive) is still read.\n')
    command = sys.argv[1]
    if command == 'pack':
        info = {}
        inf = Path(sys.argv[2]) / 'PACKAGE.INF'
        if inf.exists():
            info.update(parse_metadata(inf.read_text()))
        for item in sys.argv[4:]:
            key, _, value = item.partition('=')
            key = next((k for k in KEYS if k.lower() == key.lower()), key)
            info[key] = info[key] + '\n' + value if key == 'Env' and info.get(key) else value
        pack(sys.argv[2], sys.argv[3], info)
        print(f'Packed {sys.argv[3]} ({Path(sys.argv[3]).stat().st_size // 1024} KiB)')
    elif command == 'list':
        info, entries = read(sys.argv[2])
        for k in KEYS:
            if info.get(k):
                print(f'{k}: {info[k]}')
        for name, raw in entries:
            print(f'{len(raw):10}  {name}')
    elif command == 'install':
        info = install(sys.argv[2], sys.argv[3])
        print(f'Installed {info["Name"]} {info["Version"]} into {sys.argv[3]}/Programs/{info["Name"]}')
    else:
        raise SystemExit('Build and inspect DOS-XC packages (.dsp) on the build machine.\n\n  tools/dsp.py pack FOLDER OUT.dsp Name=... Version=... Author=... \\\n               Description=... Arch=16|32|64 [Path=...] [StartIn=program]\n               [Env=NAME=value ...] [Driver=A.DRV;B.DRV]\n               [Provides=role;...] [Requires=name[>=version];...]\n  tools/dsp.py list FILE.dsp\n  tools/dsp.py install FILE.dsp ROOT      extract to ROOT/Programs/<Name>\n                                          and write its PACKAGE.INF\n\nThe format (see docs/PACKAGES.md): "DSPK", u16 version 2, u16 flags,\nu32 metadata length, the metadata ("Key=value" lines), the file list, then\none .xz stream of the files\' data.  Version 1 (a ZIP archive) is still read.\n')

if __name__ == '__main__':
    main()
