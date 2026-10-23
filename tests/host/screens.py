#!/usr/bin/env python3
"""screens.py - the fake Wimp's screenshots (editor_test with SCREENS=dir)
made into PNGs: tests/host/screens.py dir

editor_test writes, for each screenshot, a display list (name.txt): the
windows open (their boxes, titles and icons), what !ReelEdit drew in each
(OS_Plot rectangles, Wimp_TextOp text, the picture sprite, the graphics
clip), the overlay's picture and an open menu. This draws them as a RISC
OS 5 desktop would, roughly: the Wimp's own parts (title bars, scroll bars,
icon borders, the desktop font) are a likeness, not the real thing; what
!ReelEdit draws is drawn as it was asked for.

Screen: 1920x1080, 2 OS units a pixel. Needs Pillow.

Part of riscos-reeledit. GPL version 2 or later.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SCREEN_W, SCREEN_H = 3840, 2160  # OS units
WIMP = ['#FFFFFF', '#DDDDDD', '#BBBBBB', '#999999', '#777777', '#555555', '#333333', '#000000',
        '#004499', '#EEEE00', '#00CC00', '#DD0000', '#EEEEBB', '#558800', '#FFBB00', '#00BBFF']
DESKTOP = '#7A8CA0'
FONT_FILE = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf'  # (about Homerton's widths)
FONT = ImageFont.truetype(FONT_FILE, 14)
FONT_B = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf', 14)
# (RISC OS's characters &80-&9F that !ReelEdit uses: the arrows)
RO_CHARS = {0x88: '←', 0x89: '→', 0x8A: '↓', 0x8B: '⇧'}


def text_of(raw):
    return ''.join(RO_CHARS.get(b, chr(b)) for b in raw)


def px(x, y):
    """OS units (y up) to pixels (y down)"""
    return x // 2, (SCREEN_H - y) // 2


def colour(bbggrr00):
    v = int(bbggrr00, 16)
    return ((v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255)


class Clip:
    def __init__(self, x0, y0, x1, y1):  # OS units, inclusive
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1

    def inter(self, o):
        return Clip(max(self.x0, o.x0), max(self.y0, o.y0), min(self.x1, o.x1), min(self.y1, o.y1))

    def box(self):  # pixels: left, top, right, bottom (exclusive)
        return (self.x0 // 2, (SCREEN_H - self.y1 - 1) // 2, self.x1 // 2 + 1, (SCREEN_H - self.y0) // 2)

    def empty(self):
        return self.x0 > self.x1 or self.y0 > self.y1


class Screen:
    def __init__(self):
        self.im = Image.new('RGB', (SCREEN_W // 2, SCREEN_H // 2), DESKTOP)
        self.d = ImageDraw.Draw(self.im)
        self.used = None

    def mark(self, x0, y0, x1, y1):
        b = (x0, y0, x1, y1)
        self.used = b if not self.used else (min(self.used[0], x0), min(self.used[1], y0),
                                             max(self.used[2], x1), max(self.used[3], y1))

    def rect(self, x0, y0, x1, y1, fill, clip=None):
        c = Clip(min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))
        if clip:
            c = c.inter(clip)
        if c.empty():
            return
        l, t, r, b = c.box()
        self.d.rectangle((l, t, r - 1, b - 1), fill=fill)

    def frame(self, x0, y0, x1, y1, col, clip=None):
        self.rect(x0, y0, x1, y0 + 1, col, clip)
        self.rect(x0, y1 - 1, x1, y1, col, clip)
        self.rect(x0, y0, x0 + 1, y1, col, clip)
        self.rect(x1 - 1, y0, x1, y1, col, clip)

    def bevel(self, x0, y0, x1, y1, inset=False, clip=None):
        hi, lo = ('#FFFFFF', '#777777') if not inset else ('#777777', '#FFFFFF')
        self.rect(x0, y1 - 3, x1, y1, hi, clip)
        self.rect(x0, y0, x0 + 3, y1, hi, clip)
        self.rect(x0, y0, x1, y0 + 3, lo, clip)
        self.rect(x1 - 3, y0, x1, y1, lo, clip)

    def text(self, s, x, y, fg, clip, right=False, font=FONT, bg=None):
        """baseline at x, y (OS units)"""
        if not s:
            return
        bx, by = px(x, y)
        anchor = 'rs' if right else 'ls'
        l, t, r, b = self.d.textbbox((bx, by), s, font=font, anchor=anchor)
        layer = Image.new('RGBA', (r - l + 2, b - t + 2), (0, 0, 0, 0))
        ImageDraw.Draw(layer).text((bx - l + 1, by - t + 1), s, font=font, fill=fg, anchor=anchor)
        cl, ct, cr, cb = clip.box()
        ox, oy = l - 1, t - 1
        cx0, cy0 = max(cl, ox), max(ct, oy)
        cx1, cy1 = min(cr, ox + layer.width), min(cb, oy + layer.height)
        if cx0 >= cx1 or cy0 >= cy1:
            return
        part = layer.crop((cx0 - ox, cy0 - oy, cx1 - ox, cy1 - oy))
        self.im.paste(part, (cx0, cy0), part)

    def text_width(self, s, font=FONT):
        return int(self.d.textlength(s, font=font)) * 2  # OS units

    def picture(self, img, x, ytop, clip):
        """an image's top left at x, ytop (OS units), 1:1 pixels"""
        bx, by = px(x, ytop)
        cl, ct, cr, cb = clip.box()
        cx0, cy0 = max(cl, bx), max(ct, by)
        cx1, cy1 = min(cr, bx + img.width), min(cb, by + img.height)
        if cx0 < cx1 and cy0 < cy1:
            self.im.paste(img.crop((cx0 - bx, cy0 - by, cx1 - bx, cy1 - by)), (cx0, cy0))


def draw_icon(sc, f, x0, y0, x1, y1, txt, clip):
    fg, bg = WIMP[(f >> 24) & 15], WIMP[(f >> 28) & 15]
    selected, shaded = f & (1 << 21), f & (1 << 22)
    if selected:
        fg, bg = bg, fg
    button = (f >> 12) & 15
    if f & 0x20:
        sc.rect(x0, y0, x1 - 1, y1 - 1, bg, clip)
    if f & 0x4:
        if button == 15:  # writable: a plain border
            sc.frame(x0, y0, x1, y1, '#000000', clip)
        elif button in (3, 9, 5, 6, 7) and f & 0x1:  # a button: raised
            sc.bevel(x0, y0, x1, y1, inset=bool(selected), clip=clip)
        else:
            sc.frame(x0, y0, x1, y1, fg, clip)
    if (f & 0x2) and not (f & 0x1):  # a sprite: the Wimp's pool (a file icon, as a likeness)
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        sc.rect(cx - 34, cy - 44, cx + 34, cy + 44, '#FFFFFF', clip)
        sc.frame(cx - 34, cy - 44, cx + 34, cy + 44, '#000000', clip)
        sc.rect(cx - 22, cy + 20, cx + 22, cy + 24, '#004499', clip)
        sc.text('XML', cx, cy - 20, '#004499', clip, font=FONT_B)
        return
    if f & 0x1 and txt:
        c = Clip(x0, y0, x1 - 1, y1 - 1).inter(clip)
        if shaded:
            fg = '#999999'
        base = (y0 + y1) // 2 - 10 if f & 0x10 else y0 + 12
        w = sc.text_width(txt)
        if f & 0x8:
            x = (x0 + x1 - w) // 2
        elif f & 0x200:
            x = x1 - 12 - w
        else:
            x = x0 + 12
        sc.text(txt, x, base, fg, c)


def draw_window(sc, w, icons, ops):
    i, x0, y0, x1, y1, tbg, wbg, flags, title = w
    clip = Clip(x0, y0, x1 - 1, y1 - 1)
    # the furniture: title bar above, scroll bars at the right and bottom
    top = y1 + 40 if flags & (1 << 26) else y1
    right = x1 + 40 if flags & (1 << 28) else x1
    bottom = y0 - 40 if flags & (1 << 30) else y0
    sc.mark(x0 - 2, bottom - 2, right + 2, top + 2)
    sc.rect(x0 - 2, bottom - 2, right + 1, top + 1, '#000000')
    if flags & (1 << 26):
        sc.rect(x0, y1 + 2, right - 1, top - 1, WIMP[12] if i in (0, 3) else WIMP[tbg & 15])
        tx0, tx1 = x0, right
        if flags & (1 << 24):
            sc.rect(tx0, y1 + 2, tx0 + 39, top - 1, WIMP[1]); sc.bevel(tx0, y1 + 2, tx0 + 40, top)
            sc.frame(tx0 + 8, y1 + 8, tx0 + 26, top - 14, '#000000')
            sc.frame(tx0 + 16, y1 + 16, tx0 + 34, top - 6, '#000000')
            tx0 += 42
        if flags & (1 << 25):
            sc.rect(tx0, y1 + 2, tx0 + 39, top - 1, WIMP[1]); sc.bevel(tx0, y1 + 2, tx0 + 40, top)
            sc.text('✕', tx0 + 9, y1 + 12, '#000000', Clip(tx0, y1, tx0 + 40, top))
            tx0 += 42
        if flags & (1 << 27):
            sc.rect(tx1 - 40, y1 + 2, tx1 - 1, top - 1, WIMP[1]); sc.bevel(tx1 - 40, y1 + 2, tx1, top)
            sc.frame(tx1 - 30, y1 + 10, tx1 - 10, top - 8, '#000000')
            tx1 -= 42
        t = text_of(title)
        sc.text(t, (tx0 + tx1 - sc.text_width(t)) // 2, y1 + 14, '#000000', Clip(tx0, y1, tx1, top))
    if flags & (1 << 28):
        sc.rect(x1 + 2, y0, right - 1, y1, WIMP[3])
        sc.rect(x1 + 6, y1 - 200, right - 5, y1 - 40, WIMP[1])
    if flags & (1 << 30):
        sc.rect(x0, bottom, x1, y0 - 3, WIMP[3])
        sc.rect(x0 + 80, bottom + 6, x0 + 500, y0 - 8, WIMP[1])
    if flags & (1 << 29):
        sc.rect(right - 40 if flags & (1 << 28) else x1 - 40, bottom, right - 1, bottom + 38, WIMP[1])
    # the work area: its colour (255: the program draws all of it), the icons, then the program's drawing
    if wbg != 255:
        sc.rect(x0, y0, x1 - 1, y1 - 1, WIMP[wbg & 15])
    for (ix0, iy0, ix1, iy1, f, txt) in icons:
        draw_icon(sc, f, ix0, iy0, ix1, iy1, text_of(txt), clip)
    gclip = clip
    for op in ops:
        k = op[0]
        if k == 'C':
            gclip = Clip(*op[1:5]).inter(clip)
        elif k == 'R':
            sc.rect(op[1], op[2], op[3], op[4], colour(op[5]), gclip)
        elif k == 'T':
            sc.text(text_of(op[6]), op[1], op[2], colour(op[3]), gclip, right=op[5])
        elif k == 'S':
            x, y, w, h, path = op[1:6]
            raw = open(path, 'rb').read()
            img = Image.frombytes('RGBX', (w, h), raw).convert('RGB')  # (TBGR: R, G, B, X in memory)
            sc.picture(img, x, y + 2 * h, gclip)


def draw_overlay(sc, o, clip):
    x, ytop, w, h, fw, fh, pitch, path = o
    raw = open(path, 'rb').read()
    ysz = pitch * fh
    Y = Image.frombytes('L', (pitch, fh), raw[:ysz]).crop((0, 0, fw, fh))
    c1 = Image.frombytes('L', (pitch // 2, fh // 2), raw[ysz:ysz + ysz // 4]).crop((0, 0, fw // 2, fh // 2))
    c2 = Image.frombytes('L', (pitch // 2, fh // 2), raw[ysz + ysz // 4:ysz + ysz // 2]).crop((0, 0, fw // 2, fh // 2))
    # (YV12: V then U; limited range, BT.601: the overlay's own conversion)
    img = Image.merge('YCbCr', (Y, c2.resize((fw, fh)), c1.resize((fw, fh))))
    r, g, b = img.convert('RGB').split()  # (Pillow's is full range: stretched after)
    rgb = Image.merge('RGB', [ch.point(lambda v: max(0, min(255, round((v - 16) * 255 / 219)))) for ch in (r, g, b)])
    sc.picture(rgb.resize((w, h)), x, ytop, clip)


def draw_menu(sc, m, items):
    x, y, w, h, title = m
    gap = 0
    n = len(items)
    width = max(w, max((sc.text_width(text_of(t)) for (_, _, _, t) in items), default=0) + 80)
    top, bottom = y + 44, y - n * h
    sc.mark(x - 2, bottom - 2, x + width + 2, top + 2)
    sc.rect(x - 2, bottom - 2, x + width + 1, top + 1, '#000000')
    sc.rect(x, y + 2, x + width - 1, top - 1, WIMP[2])
    sc.text(text_of(title), x + (width - sc.text_width(text_of(title))) // 2, y + 14, '#000000', Clip(x, y, x + width, top))
    sc.rect(x, bottom, x + width - 1, y, WIMP[0])
    for k, (flags, iflags, sub, t) in enumerate(items):
        iy = y - (k + 1) * h
        fg = '#999999' if iflags & (1 << 22) else '#000000'
        c = Clip(x, iy, x + width, iy + h)
        if flags & 1:
            sc.text('✓', x + 8, iy + 12, fg, c)
        sc.text(text_of(t), x + 36, iy + 12, fg, c)
        if sub:
            sc.text('▸', x + width - 30, iy + 12, fg, c)
        if flags & 2:  # a dotted line after
            for dx in range(x + 8, x + width - 8, 8):
                sc.rect(dx, iy, dx + 3, iy + 1, '#777777')


def render(path):
    windows = []
    menu, items = None, []
    cur = None
    for line in open(path, 'rb').read().split(b'\n'):
        if not line:
            continue
        k, _, rest = line.partition(b' ')
        k = k.decode()
        if k == 'W':
            p = rest.split(b' ', 8)
            cur = {'w': [int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6]),
                         int(p[7], 16), p[8] if len(p) > 8 else b''], 'icons': [], 'ops': [], 'ov': None}
            windows.append(cur)
        elif k == 'I':
            p = rest.split(b' ', 5)
            cur['icons'].append((int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4], 16), p[5] if len(p) > 5 else b''))
        elif k == 'C':
            cur['ops'].append(('C',) + tuple(int(v) for v in rest.split(b' ')))
        elif k == 'R':
            p = rest.split(b' ')
            cur['ops'].append(('R', int(p[0]), int(p[1]), int(p[2]), int(p[3]), p[4].decode()))
        elif k == 'T':
            p = rest.split(b' ', 5)
            cur['ops'].append(('T', int(p[0]), int(p[1]), p[2].decode(), p[3].decode(), int(p[4]),
                               p[5] if len(p) > 5 else b''))
        elif k == 'S':
            p = rest.split(b' ', 4)
            cur['ops'].append(('S', int(p[0]), int(p[1]), int(p[2]), int(p[3]), p[4].decode()))
        elif k == 'O':
            p = rest.split(b' ', 7)
            cur['ov'] = tuple(int(v) for v in p[:7]) + (p[7].decode(),)
        elif k == 'M':
            p = rest.split(b' ', 4)
            menu = (int(p[0]), int(p[1]), int(p[2]), int(p[3]), p[4] if len(p) > 4 else b'')
        elif k == 'MI':
            p = rest.split(b' ', 3)
            items.append((int(p[0], 16), int(p[1], 16), int(p[2]), p[3] if len(p) > 3 else b''))
    sc = Screen()
    for w in windows:
        draw_window(sc, w['w'], w['icons'], w['ops'])
        if w['ov']:
            x0, y0, x1, y1 = w['w'][1:5]
            draw_overlay(sc, w['ov'], Clip(x0, y0, x1 - 1, y1 - 1))
    if menu:
        draw_menu(sc, menu, items)
    # the part of the screen in use, and a margin
    if sc.used:
        x0, y0, x1, y1 = sc.used
        l, t = px(max(0, x0 - 60), min(SCREEN_H, y1 + 60))
        r, b = px(min(SCREEN_W, x1 + 60), max(0, y0 - 60))
        sc.im = sc.im.crop((l, t, r, b))
    out = path[:-4] + '.png'
    sc.im.save(out)
    return out


if __name__ == '__main__':
    d = sys.argv[1]
    for f in sorted(os.listdir(d)):
        if f.endswith('.txt'):
            print(render(os.path.join(d, f)))
