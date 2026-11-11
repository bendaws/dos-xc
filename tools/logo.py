#!/usr/bin/env python3
#
# DOS-XC /tools/logo.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import sys
from pathlib import Path
from PIL import Image

WIDTH = int(sys.argv[1]) if len(sys.argv) > 1 else 26
VGA = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170), (170, 85, 0),
       (170, 170, 170), (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255), (255, 85, 85),
       (255, 85, 255), (255, 255, 85), (255, 255, 255)]
ANSI = [0, 4, 2, 6, 1, 5, 3, 7]          # VGA color -> ANSI color number
BACKGROUND = None                         # transparent / white pixels: keep the screen color

source = Image.open(Path(__file__).resolve().parents[1] / '.github/dosxc.png').convert('RGBA')
box = source.getbbox()
source = source.crop(box)
height = round(source.height * WIDTH / source.width / 2) * 2
image = source.resize((WIDTH, height), Image.LANCZOS)

def color(pixel):
    r, g, b, a = pixel
    if a < 128 or (r > 230 and g > 230 and b > 230):
        return BACKGROUND
    return min(range(16), key=lambda i: sum((x - y) ** 2 for x, y in zip(VGA[i], (r, g, b))))

def sgr(fg, bg):
    codes = ['0']
    if fg is not None:
        codes.append(str((90 if fg >= 8 else 30) + ANSI[fg & 7]))
    if bg is not None:
        codes.append(str(40 + ANSI[bg & 7]))
    return '\\033[' + ';'.join(codes) + 'm'

lines = []
for y in range(0, height, 2):
    out, state = '', None
    for x in range(WIDTH):
        top, bottom = color(image.getpixel((x, y))), color(image.getpixel((x, y + 1)))
        if top is None and bottom is None:
            cell, want = ' ', (None, None)
        elif top is None:                  # lower half only: 0xDC in the bottom color
            cell, want = '\\xdc', (bottom, None)
        elif bottom is None:
            cell, want = '\\xdf', (top, None)
        elif top == bottom:
            cell, want = '\\xdb', (top, None)
        elif bottom < 8:
            cell, want = '\\xdf', (top, bottom)
        elif top < 8:
            cell, want = '\\xdc', (bottom, top)
        else:                              # two bright colors: the background cannot be bright
            cell, want = '\\xdf', (top, bottom & 7)
        if want != state:
            out += sgr(*want)
            state = want
        out += cell
    lines.append(out.rstrip() + '\\033[0m')
while lines and lines[-1] == '\\033[0m':
    lines.pop()
print('/*\n * DOS-XC /os/shell/logo.h\n * Copyright (c) 2026 Ben Daws - see LICENSE.txt\n */\n')
print(f'#define LOGO_WIDTH {WIDTH}')
print(f'#define LOGO_LINES {len(lines)}')
print('static const char *const logo[LOGO_LINES] = {')
for line in lines:
    print(f'    "{line}",')
print('};')
