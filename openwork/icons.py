#
# DOS-XC /openwork/icons.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import sys
from PIL import Image, ImageDraw, ImageFont

OVERSAMPLE = 8
INK = (68, 68, 68, 255)
GRAY = (130, 130, 130, 255)
LIGHT = (200, 200, 200, 255)
WHITE = (255, 255, 255, 255)
BLUE = (43, 87, 154, 255)
SKY = (66, 139, 202, 255)
RED = (209, 52, 56, 255)
GREEN = (33, 115, 70, 255)
GOLD = (232, 184, 74, 255)
ORANGE = (210, 71, 38, 255)

APPS = {
    "PAPERS": ((43, 87, 154, 255), "P"),
    "NUMBERS": ((33, 115, 70, 255), "N"),
    "SHOW": ((196, 43, 28, 255), "S"),
    "MAIL": ((240, 180, 0, 255), "M"),
}


class Pen:
    """Draws on a 16-unit grid, oversampled for antialiasing."""

    def __init__(self, size):
        self.size = size
        self.k = size * OVERSAMPLE / 16
        self.im = Image.new("RGBA", (size * OVERSAMPLE, size * OVERSAMPLE), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    def p(self, points):
        return [(x * self.k, y * self.k) for x, y in points]

    def rect(self, x0, y0, x1, y1, fill=None, outline=None, width=1):
        self.d.rectangle(self.p([(x0, y0), (x1, y1)]), fill=fill, outline=outline, width=int(width * self.k) if outline else 0)

    def poly(self, points, fill=None, outline=None, width=1):
        self.d.polygon(self.p(points), fill=fill)
        if outline:
            self.line(points + [points[0]], outline, width)

    def line(self, points, color, width=1):
        self.d.line(self.p(points), fill=color, width=max(1, int(width * self.k)), joint="curve")

    def oval(self, x0, y0, x1, y1, fill=None, outline=None, width=1):
        self.d.ellipse(self.p([(x0, y0), (x1, y1)]), fill=fill, outline=outline, width=int(width * self.k) if outline else 0)

    def arc(self, x0, y0, x1, y1, start, end, color, width=1):
        self.d.arc(self.p([(x0, y0), (x1, y1)]), start, end, fill=color, width=max(1, int(width * self.k)))

    def text(self, x, y, s, color, size, bold=False, italic=False):
        font = ImageFont.load_default(size=size * self.k)
        layer = Image.new("RGBA", self.im.size, (0, 0, 0, 0))
        ImageDraw.Draw(layer).text((x * self.k, y * self.k), s, font=font, fill=color, anchor="mm",
                                   stroke_width=int(self.k * 0.7) if bold else 0, stroke_fill=color)
        if italic:
            w, h = layer.size
            layer = layer.transform(layer.size, Image.AFFINE, (1, 0.25, -h * 0.125, 0, 1, 0), Image.BICUBIC)
        self.im.alpha_composite(layer)

    def done(self):
        return self.im.resize((self.size, self.size), Image.LANCZOS)


def page(p, x0=3, y0=1, x1=13, y1=15, fold=3, fill=WHITE):
    p.poly([(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)], fill=fill, outline=GRAY, width=0.8)
    p.poly([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=LIGHT, outline=GRAY, width=0.6)


def lines(p, x0, x1, ys, color=GRAY, width=0.9):
    for y in ys:
        p.line([(x0, y), (x1, y)], color, width)


def envelope(p, x0=1.5, y0=4, x1=14.5, y1=13, fill=WHITE, color=GRAY):
    p.rect(x0, y0, x1, y1, fill=fill, outline=color, width=0.9)
    p.line([(x0, y0), ((x0 + x1) / 2, (y0 + y1) / 2 + 0.5), (x1, y0)], color, 0.9)


def arrow_left(p, x0, y, x1, color, head=3, width=1.6):
    p.line([(x1, y), (x0 + 1, y)], color, width)
    p.poly([(x0, y), (x0 + head, y - head), (x0 + head, y + head)], fill=color)


def arrow_right(p, x0, y, x1, color, head=3, width=1.6):
    p.line([(x0, y), (x1 - 1, y)], color, width)
    p.poly([(x1, y), (x1 - head, y - head), (x1 - head, y + head)], fill=color)


def folder(p, fill=GOLD, x0=1, y0=3, x1=15, y1=13):
    p.poly([(x0, y0), (x0 + 5, y0), (x0 + 6.5, y0 + 1.5), (x1, y0 + 1.5), (x1, y1), (x0, y1)], fill=fill)
    p.rect(x0, y0 + 3, x1, y1, fill=tuple(min(255, c + 25) for c in fill[:3]) + (255,))


def i_paste(p):
    p.rect(2, 3, 12, 15, fill=(196, 150, 90, 255), outline=(140, 100, 50, 255), width=0.8)
    p.rect(5, 1.5, 9, 4.5, fill=LIGHT, outline=GRAY, width=0.7)
    page(p, 7, 6, 15, 15.5, 2.5)
    lines(p, 8.5, 13, (9.5, 11.5, 13.5), BLUE, 0.8)


def i_cut(p):
    p.oval(1.5, 10, 6.5, 15, outline=BLUE, width=1.3)
    p.oval(9.5, 10, 14.5, 15, outline=BLUE, width=1.3)
    p.line([(5.5, 10.5), (11, 1)], INK, 1.3)
    p.line([(10.5, 10.5), (5, 1)], INK, 1.3)


def i_copy(p):
    page(p, 1.5, 1, 10, 11.5, 2.5)
    page(p, 6, 4.5, 14.5, 15, 2.5)
    lines(p, 7.5, 12.5, (8.5, 10.5, 12.5), BLUE, 0.8)


def i_select_all(p):
    p.rect(2, 2, 14, 14, outline=GRAY, width=0.9)
    lines(p, 4, 12, (5, 8, 11), BLUE, 1.4)


def i_find(p):
    p.oval(1.5, 1.5, 10.5, 10.5, fill=(220, 235, 250, 255), outline=INK, width=1.3)
    p.line([(9, 9), (14.5, 14.5)], INK, 2.2)


def i_bold(p):
    p.text(8, 8.4, "B", INK, 13, bold=True)


def i_italic(p):
    p.text(8, 8.4, "I", INK, 13, italic=True)


def i_underline(p):
    p.text(8, 7, "U", INK, 11)
    p.line([(3.5, 14.3), (12.5, 14.3)], INK, 1.1)


def i_bullets(p):
    for y in (3.5, 8, 12.5):
        p.oval(1.5, y - 1.2, 3.9, y + 1.2, fill=BLUE)
        p.line([(6, y), (14.5, y)], INK, 1.1)


def i_align(p, kind):
    for i, y in enumerate((2.5, 5.5, 8.5, 11.5, 14)):
        w = 12 if i % 2 == 0 else 8
        x0 = 2 if kind == 0 else (16 - w) / 2 if kind == 1 else 14 - w
        p.line([(x0, y), (x0 + w, y)], INK, 1.1)


def i_clear_format(p):
    p.text(6.5, 7.5, "A", INK, 12)
    p.poly([(8, 13), (12, 9), (15, 12), (11, 16)], fill=(236, 130, 160, 255))
    p.poly([(8, 13), (10, 11), (13, 14), (11, 16)], fill=WHITE, outline=GRAY, width=0.5)


def i_date(p):
    p.rect(1.5, 2.5, 14.5, 14.5, fill=WHITE, outline=GRAY, width=0.9)
    p.rect(1.5, 2.5, 14.5, 5.5, fill=BLUE)
    for x in (4, 7, 10, 13):
        for y in (8, 11):
            p.rect(x - 1, y - 1, x + 0.6, y + 0.6, fill=GRAY)


def i_word_count(p):
    p.text(8, 4.5, "ABC", INK, 6.2)
    p.text(8, 11.5, "123", BLUE, 6.2)


def i_autosum(p):
    p.line([(13, 3.5), (13, 2), (3, 2), (8.5, 8), (3, 14), (13, 14), (13, 12.5)], INK, 1.5)


def i_function(p):
    p.text(7.8, 8, "fx", INK, 11, italic=True)


def i_clear(p):
    p.poly([(1.5, 10), (8, 3.5), (14.5, 10), (10, 14.5), (6, 14.5)], fill=(236, 130, 160, 255))
    p.poly([(1.5, 10), (5, 6.5), (11.5, 13), (10, 14.5), (6, 14.5)], fill=WHITE, outline=GRAY, width=0.6)
    p.line([(6, 14.8), (15, 14.8)], GRAY, 0.8)


def slide(p, x0, y0, x1, y1, fill=WHITE):
    p.rect(x0, y0, x1, y1, fill=fill, outline=GRAY, width=0.9)


def i_new_slide(p):
    slide(p, 1, 3, 15, 12.5)
    p.rect(3, 5, 13, 6.6, fill=ORANGE)
    lines(p, 3.5, 11, (8.5, 10.5), GRAY, 0.8)


def i_layout(p):
    slide(p, 1, 2.5, 15, 13.5)
    p.rect(2.8, 4.3, 13.2, 6.3, fill=LIGHT)
    p.rect(2.8, 7.6, 7.6, 11.8, fill=LIGHT)
    p.rect(8.4, 7.6, 13.2, 11.8, fill=LIGHT)


def i_delete(p):
    p.line([(3, 3), (13, 13)], RED, 2.4)
    p.line([(13, 3), (3, 13)], RED, 2.4)


def i_duplicate(p):
    slide(p, 1, 1.5, 11, 8.5)
    slide(p, 5, 7.5, 15, 14.5)
    p.rect(6.8, 9.2, 13.2, 10.6, fill=ORANGE)


def i_play(p):
    p.poly([(4, 2), (14, 8), (4, 14)], fill=GREEN)


def i_play_current(p):
    slide(p, 1, 2, 12, 11)
    p.poly([(8, 7), (15, 11), (8, 15)], fill=GREEN)


def i_new_mail(p):
    envelope(p, 1, 5, 13, 14)
    p.poly([(12.5, 0.5), (13.4, 2.6), (15.6, 2.8), (13.9, 4.2), (14.4, 6.4), (12.5, 5.2), (10.6, 6.4), (11.1, 4.2), (9.4, 2.8), (11.6, 2.6)], fill=GOLD)


def i_reply(p):
    p.line([(4, 7), (10, 7)], BLUE, 1.6)
    p.arc(6, 7, 14, 14.5, 270, 90, BLUE, 1.6)
    p.poly([(1, 7), (5.5, 2.8), (5.5, 11.2)], fill=BLUE)


def i_reply_all(p):
    i_reply(p)
    p.poly([(0, 7), (3, 4), (3, 10)], fill=SKY)
    p.line([(0.5, 7), (1, 7)], WHITE, 0.6)


def i_forward(p):
    p.line([(6, 7), (12, 7)], BLUE, 1.6)
    p.arc(2, 7, 10, 14.5, 90, 270, BLUE, 1.6)
    p.poly([(15, 7), (10.5, 2.8), (10.5, 11.2)], fill=BLUE)


def i_send_receive(p):
    envelope(p, 1, 5, 11, 12.5)
    p.arc(6, 1, 15, 10, 300, 200, GREEN, 1.4)
    p.poly([(5.5, 4.8), (8.2, 6.8), (8.6, 3.2)], fill=GREEN)


def i_read(p):
    p.poly([(1.5, 7), (8, 2.5), (14.5, 7), (14.5, 14), (1.5, 14)], fill=WHITE, outline=GRAY, width=0.9)
    p.line([(1.5, 7), (8, 11), (14.5, 7)], GRAY, 0.9)


def i_unread(p):
    envelope(p, 1.5, 3.5, 14.5, 12.5, fill=(220, 235, 250, 255), color=BLUE)


def i_send(p):
    p.poly([(1, 7.5), (15, 1.5), (11, 14.5), (7.5, 10), (6.5, 14), (5.5, 9.5)], fill=(220, 235, 250, 255), outline=BLUE, width=0.9)
    p.line([(5.5, 9.5), (15, 1.5)], BLUE, 0.8)


def i_attach(p):
    p.arc(4, 1, 12, 9, 180, 360, INK, 1.2)
    p.line([(4, 5), (4, 11.5)], INK, 1.2)
    p.line([(12, 5), (12, 12)], INK, 1.2)
    p.arc(6.5, 9, 12, 15, 0, 180, INK, 1.2)
    p.line([(6.5, 12), (6.5, 5.5)], INK, 1.2)
    p.arc(4, 9, 9, 14, 0, 180, INK, 1.2)


def i_account(p):
    p.oval(5, 1.5, 11, 7.5, fill=GRAY)
    p.d.pieslice(p.p([(2, 8.5), (14, 20)]), 180, 360, fill=GRAY)


def i_settings(p):
    import math
    for i in range(8):
        a = i * math.pi / 4
        x, y = 8 + 6 * math.cos(a), 8 + 6 * math.sin(a)
        p.line([(8, 8), (x, y)], INK, 2.6)
    p.oval(3, 3, 13, 13, fill=INK)
    p.oval(5.8, 5.8, 10.2, 10.2, fill=WHITE)


def i_inbox(p):
    p.poly([(1, 9), (3.5, 3), (12.5, 3), (15, 9), (15, 14), (1, 14)], fill=WHITE, outline=INK, width=0.9)
    p.poly([(1, 9), (5, 9), (6, 11), (10, 11), (11, 9), (15, 9), (15, 14), (1, 14)], fill=(220, 235, 250, 255), outline=INK, width=0.9)


def i_sent(p):
    p.poly([(1, 8), (15, 2), (9, 15)], fill=WHITE, outline=INK, width=0.9)
    p.line([(1, 8), (8, 9), (15, 2)], INK, 0.8)
    p.line([(8, 9), (9, 15)], INK, 0.8)


def i_trash(p):
    p.rect(3.5, 4.5, 12.5, 15, fill=WHITE, outline=INK, width=0.9)
    p.line([(2, 3.8), (14, 3.8)], INK, 1.2)
    p.rect(6, 1.5, 10, 3.8, outline=INK, width=0.8)
    for x in (6, 8, 10):
        p.line([(x, 6.5), (x, 13)], GRAY, 0.8)


def i_folder(p):
    folder(p)


def i_drafts(p):
    page(p, 2, 1, 12, 15)
    lines(p, 3.5, 9, (5, 7, 9), GRAY, 0.8)
    p.poly([(9, 13.5), (14, 6), (15.5, 7), (10.5, 14.5), (8.6, 15.2)], fill=GOLD, outline=INK, width=0.6)


def i_new_doc(p):
    page(p)


def i_open(p):
    p.poly([(1, 3), (6, 3), (7.5, 4.5), (13, 4.5), (13, 13), (1, 13)], fill=GOLD)
    p.poly([(3, 7), (15.5, 7), (13, 13), (1, 13)], fill=(250, 214, 120, 255))


def i_save(p):
    p.poly([(1.5, 1.5), (12.5, 1.5), (14.5, 3.5), (14.5, 14.5), (1.5, 14.5)], fill=(90, 120, 190, 255))
    p.rect(4, 1.5, 11.5, 6, fill=WHITE)
    p.rect(9, 2.3, 10.5, 5.2, fill=(90, 120, 190, 255))
    p.rect(3.5, 9, 12.5, 14.5, fill=WHITE)


def i_save_as(p):
    i_save(p)
    p.poly([(9, 15.5), (14, 9), (15.8, 10.4), (10.8, 16)], fill=GOLD, outline=INK, width=0.5)


def i_info(p):
    p.oval(1, 1, 15, 15, outline=INK, width=1.1)
    p.oval(7, 3.5, 9, 5.5, fill=INK)
    p.rect(7.1, 7, 8.9, 12.5, fill=INK)


def i_close(p):
    p.line([(3.5, 3.5), (12.5, 12.5)], INK, 1.4)
    p.line([(12.5, 3.5), (3.5, 12.5)], INK, 1.4)


def i_back(p):
    p.oval(0.8, 0.8, 15.2, 15.2, outline=WHITE, width=1.2)
    arrow_left(p, 4, 8, 12, WHITE, 3, 1.4)


def i_help(p):
    p.text(8, 8, "?", INK, 13)


def i_computer(p):
    p.rect(1, 2, 15, 11, fill=SKY, outline=INK, width=0.9)
    p.rect(6, 11, 10, 13.5, fill=GRAY)
    p.rect(4, 13.5, 12, 14.8, fill=INK)


def i_mark(p):
    p.line([(2, 8.5), (6, 12.5), (14, 3.5)], GREEN, 2)


def i_cancel(p):
    p.line([(4, 4), (12, 12)], RED, 1.6)
    p.line([(12, 4), (4, 12)], RED, 1.6)


def i_section(p):
    slide(p, 1, 2.5, 15, 13.5)
    p.rect(2.8, 6, 13.2, 8.4, fill=ORANGE)
    p.line([(3, 10.5), (10, 10.5)], GRAY, 0.8)


def i_replace(p):
    p.text(5, 4.5, "ab", INK, 7)
    p.text(10.5, 12, "ac", BLUE, 7)
    p.arc(1.5, 6, 8, 14, 90, 180, GRAY, 0.9)
    p.poly([(4.8, 14), (3.2, 12.4), (3.2, 15.6)], fill=GRAY)


def i_symbol(p):
    p.text(8, 7.6, "\u03a9", INK, 13)


RIBBON = [
    i_paste, i_cut, i_copy, i_select_all, i_find, i_bold, i_italic, i_underline, i_bullets,
    lambda p: i_align(p, 0), lambda p: i_align(p, 1), lambda p: i_align(p, 2),
    i_clear_format, i_date, i_word_count, i_autosum, i_function, i_clear,
    i_new_slide, i_layout, i_delete, i_duplicate, i_play, i_play_current,
    i_new_mail, i_reply, i_reply_all, i_forward, i_send_receive, i_read, i_unread, i_send, i_attach,
    i_account, i_settings, i_inbox, i_sent, i_trash, i_folder, i_drafts,
    i_new_doc, i_open, i_save, i_save_as, i_info, i_close, i_back, i_help, i_computer, i_mark, i_cancel, i_section,
    i_replace, i_symbol,
]


def strip(size):
    out = Image.new("RGBA", (size * len(RIBBON), size), (0, 0, 0, 0))
    for i, draw in enumerate(RIBBON):
        p = Pen(size)
        draw(p)
        out.alpha_composite(p.done(), (i * size, 0))
    return out


def app_icon(size, accent, letter, mail):
    p = Pen(size)
    if mail:
        envelope(p, 5, 3.5, 15.5, 12.5, fill=WHITE, color=accent)
    else:
        p.rect(5, 1, 15, 15, fill=WHITE, outline=accent, width=0.8)
        lines(p, 10.5, 13.5, (4.5, 7, 9.5, 12), accent, 1)
    p.rect(0.5, 3, 10.5, 13, fill=accent)
    dark = (40, 30, 0, 255) if sum(accent[:3]) > 400 else WHITE
    p.text(5.5, 8, letter, dark, 8.5, bold=True)
    return p.done()


def main(folder):
    strip(16).save(folder + "/RIBBON16.PNG")
    strip(32).save(folder + "/RIBBON32.PNG")
    for name, (accent, letter) in APPS.items():
        app_icon(16, accent, letter, name == "MAIL").save(folder + "/" + name + ".PNG")
        app_icon(32, accent, letter, name == "MAIL").save(folder + "/" + name + "32.PNG")


main(sys.argv[1])
