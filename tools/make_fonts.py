#!/usr/bin/env python3
#
# DOS-XC /tools/make_fonts.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import struct
import sys
from pathlib import Path

HEADER = struct.Struct('<4sBBHBBBBBHB32s32s')

def load(path):
    data = Path(path).read_bytes()
    magic, width, height, mask_size, variable, spacing, baseline, mean, size, weight, slope, name, family = \
        HEADER.unpack_from(data)
    if magic != b'!Fnt':
        raise SystemExit(f'{path}: not a bitmap font')
    mask = data[HEADER.size:HEADER.size + mask_size]
    glyphs = sum(bin(b).count('1') for b in mask) * 256
    start = HEADER.size + mask_size
    per_glyph = (len(data) - start - glyphs) // glyphs
    if per_glyph % height or width > 16:
        raise SystemExit(f'{path}: unexpected glyph size {per_glyph}')
    if not mask[0] & 1:
        raise SystemExit(f'{path}: no glyphs for code points 0-255')
    per_row = per_glyph // height
    rows = [[int.from_bytes(data[start + c * per_glyph + y * per_row:start + c * per_glyph + (y + 1) * per_row], 'little') & 0xffff
             for y in range(height)] for c in range(256)]        # a few source rows carry stray bits past the glyph
    widths = list(data[start + glyphs * per_glyph:start + glyphs * per_glyph + 256])
    if not variable:
        widths = [width] * 256
    return {'height': height, 'baseline': baseline, 'spacing': spacing, 'rows': rows, 'widths': widths,
            'name': name.split(b'\0')[0].decode()}

def main():
    out = [
        '/*',
        ' * DOS-XC /desktop/fonts.h',
        ' * Copyright (c) 2026 Ben Daws - see LICENSE.txt',
        ' */',
        '',
        '/* Glyphs from the SerenityOS bitmap fonts, used under the BSD 2-Clause',
        '   license: see desktop/LICENSE.SerenityOS. */',
        '',
        '#pragma once',
        '',
    ]
    for item in sys.argv[2:]:
        name, _, path = item.partition('=')
        font = load(path)
        out.append(f'static const unsigned char {name}_widths[256] = {{')
        for i in range(0, 256, 32):
            out.append('    ' + ','.join(str(w) for w in font['widths'][i:i + 32]) + ',')
        out.append('};')
        out.append(f'static const unsigned short {name}_rows[{256 * font["height"]}] = {{')
        for glyph in font['rows']:
            out.append('    ' + ','.join(hex(r) for r in glyph) + ',')
        out.append('};')
        out.append(f'#define {name.upper()}_HEIGHT {font["height"]}')
        out.append(f'#define {name.upper()}_BASELINE {font["baseline"]}')
        out.append(f'#define {name.upper()}_SPACING {font["spacing"]}')
        out.append('')
    Path(sys.argv[1]).write_text('\n'.join(out))

if __name__ == '__main__':
    main()
