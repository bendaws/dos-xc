#
# DOS-XC /dosoffice/icons.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import sys
from PIL import Image, ImageDraw


def page(accent):
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(2, 0), (10, 0), (14, 4), (14, 15), (2, 15)], fill=(255, 255, 255, 255), outline=(64, 64, 64, 255))
    d.polygon([(10, 0), (10, 4), (14, 4)], fill=(200, 200, 200, 255), outline=(64, 64, 64, 255))
    d.rectangle([0, 6, 8, 13], fill=accent, outline=tuple(max(0, c - 70) for c in accent[:3]) + (255,))
    return im, d


def writer(path):
    im, d = page((40, 90, 190, 255))
    for y in (2, 4):
        d.line([(4, y), (9, y)], fill=(120, 120, 120, 255))
    for y in (8, 10, 12):
        d.line([(10, y), (12, y)], fill=(120, 120, 120, 255))
    d.line([(2, 8), (3, 11), (4, 8), (5, 11), (6, 8)], fill=(255, 255, 255, 255))
    im.save(path)


def sheets(path):
    im, d = page((30, 140, 60, 255))
    for x in (9, 12):
        d.line([(x, 6), (x, 14)], fill=(150, 150, 150, 255))
    for y in (8, 11):
        d.line([(9, y), (13, y)], fill=(150, 150, 150, 255))
    d.line([(2, 8), (6, 12)], fill=(255, 255, 255, 255))
    d.line([(6, 8), (2, 12)], fill=(255, 255, 255, 255))
    im.save(path)


writer(sys.argv[1] + "/WRITER.PNG")
sheets(sys.argv[1] + "/SHEETS.PNG")
