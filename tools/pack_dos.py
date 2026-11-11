#!/usr/bin/env python3
#
# DOS-XC /tools/pack_dos.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import os
from pathlib import Path
import sys
import struct
import zipfile

build = Path(sys.argv[1])
archive = os.environ.get('DOS_PROGRAMS_ZIP')
files = []
if archive:
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if item.is_dir():
                continue
            name = item.filename.replace('\\', '/').rsplit('/', 1)[-1].upper()
            parts = name.split('.')
            if (len(parts) > 2 or not 1 <= len(parts[0]) <= 8 or
                    (len(parts) == 2 and not 1 <= len(parts[1]) <= 3) or
                    any(c not in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.' for c in name)):
                raise SystemExit(f'Unsupported DOS filename: {name!r}')
            if item.file_size > 2 * 1024 * 1024:
                raise SystemExit(f'DOS file too large: {name}')
            files.append((name, source.read(item)))
    for name, executable in files:
        if name.endswith('.EXE'):
            if len(executable)<32 or executable[:2]!=b'MZ' or len(executable)>0x7f000:
                raise SystemExit(f'{name}: unsupported DOS MZ executable')
            header=struct.unpack_from('<14H',executable)
            if not 2<=header[4] or header[4]*16>len(executable) or header[12]+header[3]*4>header[4]*16:
                raise SystemExit(f'{name}: Invalid MZ header or relocation table')
        if name.endswith('.COM') and len(executable)>0xff00:
            raise SystemExit(f'{name}: COM executable is too large')
    if len(dict(files)) != len(files) or len(files) > 64:
        raise SystemExit('Duplicate filenames or too many DOS files')
block = 97
with (build / 'dos-files.inc').open('w') as manifest, (build / 'dos-data.bin').open('wb') as payload:
    manifest.write(f'DOS_FILE_COUNT equ {len(files)}\nfiles:\n')
    for name, data in files:
        manifest.write(f'    db "{name}", 0\n    times 13-(($-files) % 32) db 0\n')
        manifest.write(f'    db 0\n    dd {block}, {len(data)}\n    dw 0, 0\n    times 6 db 0\n')
        padded = data + bytes(-len(data) % 2048)
        payload.write(padded)
        block += len(padded) // 2048

    manifest.write(f"    times (64-{len(files)})*32 db 0\n")
