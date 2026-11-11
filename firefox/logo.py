#!/usr/bin/env python3
#
# DOS-XC /firefox/logo.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import os
import sys

from PIL import Image

ART = [
    "...........K..K.",
    "..........KLKKLK",
    ".......KKKKFFFFK",
    "....KKLLFFFWKFFK",
    "...KNKKLFFFFFFFK",
    "..KNNGGKKKFFFFK.",
    ".KNGGGGNNNKFFFK.",
    ".KNGGGNNNNNKFFK.",
    "KNNNGGNNNGNKFFK.",
    "KNNNNNNNGGNKFFK.",
    "KFKNNNNNNNKFFFK.",
    "KLFKNNNNNKFFFK..",
    ".KLFKKKKKFFFFK..",
    ".KLLFFFFFFFFK...",
    "..KKLLLFFFKK....",
    "....KKKKKK......",
]

COLOURS = {
    ".": (0, 0, 0, 0),
    "K": (8, 20, 64, 255),       # outline
    "N": (24, 44, 120, 255),     # globe, deep blue
    "G": (52, 98, 196, 255),     # globe, land
    "F": (40, 140, 255, 255),    # fox
    "L": (150, 214, 255, 255),   # fox, light fur
    "W": (255, 255, 255, 255),   # eye
}

def drawing():
    image = Image.new("RGBA", (16, 16))
    for y, row in enumerate(ART):
        assert len(row) == 16, (y, row)
        for x, c in enumerate(row):
            image.putpixel((x, y), COLOURS[c])
    return image

def enlarged(art, size, height=None):
    height = height or size
    scale = max(1, min(size, height) // 16)
    big = art.resize((16 * scale, 16 * scale), Image.NEAREST)
    canvas = Image.new("RGBA", (size, height))
    canvas.paste(big, ((size - big.width) // 2, (height - big.height) // 2))
    return canvas

def wordmark(path, fill):
    with open(path, "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 372 99">\n'
                f'  <text x="0" y="78" font-family="sans-serif" font-weight="bold" font-size="84" fill="{fill}">'
                "DOSfox</text>\n</svg>\n")

def main():
    out = sys.argv[1]
    content = os.path.join(out, "content")
    os.makedirs(content, exist_ok=True)
    art = drawing()
    for size in (16, 22, 24, 32, 48, 64, 128, 256):
        enlarged(art, size).save(os.path.join(out, f"default{size}.png"))
    for name, size in (("about-logo", 192), ("about-logo@2x", 384),
                       ("about-logo-private", 192), ("about-logo-private@2x", 384)):
        enlarged(art, size).save(os.path.join(content, name + ".png"))
    enlarged(art, 300, 236).save(os.path.join(content, "about.png"))
    rects = []
    for y, row in enumerate(ART):
        for x, c in enumerate(row):
            if c != ".":
                r, g, b, _ = COLOURS[c]
                rects.append(f'<rect x="{x}" y="{y}" width="1.02" height="1.02" fill="#{r:02x}{g:02x}{b:02x}"/>')
    with open(os.path.join(content, "about-logo.svg"), "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" shape-rendering="crispEdges">\n')
        f.write("\n".join(rects))
        f.write("\n</svg>\n")
    wordmark(os.path.join(content, "about-wordmark.svg"), "context-fill")
    wordmark(os.path.join(content, "firefox-wordmark.svg"), "context-fill light-dark(#20123a, #fbfbfe)")

if __name__ == "__main__":
    main()
