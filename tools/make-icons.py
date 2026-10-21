#!/usr/bin/env python3
"""Draw the icons as PNGs (app/icons/), for package/png2sprite.py.

   tools/make-icons.py

!mlt: three tracks of a timeline (video, video, sound) with clips on them
and the playhead across them, at 34 x 34 and 17 x 17. !reeledit: a film
reel above two tracks of clips, the same sizes. Drawn at 4 times
the size and scaled down, so the edges are smooth.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'app', 'icons')


def timeline(size):
    s = 4 * size
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = s / 34.0                       # one icon pixel
    # the panel
    d.rounded_rectangle([1 * u, 3 * u, 33 * u, 31 * u], radius=4 * u,
                        fill=(52, 58, 72, 255), outline=(20, 22, 30, 255), width=max(1, int(u)))
    # three tracks of clips: (top, height, [(start, end, colour)])
    tracks = [
        (7, 6, [(3, 14, (233, 120, 60)), (15, 31, (240, 176, 64))]),
        (15, 6, [(6, 22, (78, 160, 220)), (23, 31, (110, 200, 140))]),
        (23, 5, [(3, 31, (150, 120, 210))]),
    ]
    for top, h, clips in tracks:
        for a, b, c in clips:
            d.rounded_rectangle([a * u, top * u, b * u - u * 0.8, (top + h) * u], radius=1.2 * u,
                                fill=c + (255,), outline=(20, 22, 30, 255), width=max(1, int(u * 0.6)))
    # the playhead
    x = 19 * u
    d.rectangle([x - 0.7 * u, 4 * u, x + 0.7 * u, 30 * u], fill=(250, 250, 250, 255))
    d.polygon([(x - 2.5 * u, 3 * u), (x + 2.5 * u, 3 * u), (x, 6.5 * u)], fill=(250, 250, 250, 255))
    return im.resize((size, size), Image.LANCZOS)


def reeledit(size):
    s = 4 * size
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = s / 34.0
    dark = (20, 22, 30, 255)
    # the reel: a disc with five holes round a hub
    cx, cy, rr = 17 * u, 12 * u, 11 * u
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(70, 78, 96, 255), outline=dark, width=max(1, int(u)))
    import math
    for i in range(5):
        a = math.pi / 2 + i * 2 * math.pi / 5
        hx, hy, hr = cx + 6 * u * math.cos(a), cy - 6 * u * math.sin(a), 2.6 * u
        d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=(20, 22, 30, 255))
    d.ellipse([cx - 1.6 * u, cy - 1.6 * u, cx + 1.6 * u, cy + 1.6 * u], fill=(200, 205, 215, 255))
    # two tracks of clips below it
    for top, clips in ((25, [(1, 13, (233, 120, 60)), (14, 33, (78, 160, 220))]),
                       (30, [(4, 24, (110, 200, 140)), (25, 33, (240, 176, 64))])):
        for a, b, c in clips:
            d.rounded_rectangle([a * u, top * u, b * u - u * 0.8, (top + 4) * u], radius=1.2 * u,
                                fill=c + (255,), outline=dark, width=max(1, int(u * 0.6)))
    return im.resize((size, size), Image.LANCZOS)


os.makedirs(OUT, exist_ok=True)
reeledit(34).save(os.path.join(OUT, 'reeledit.png'))
reeledit(17).save(os.path.join(OUT, 'sm-reeledit.png'))
timeline(34).save(os.path.join(OUT, 'mlt.png'))
timeline(17).save(os.path.join(OUT, 'sm-mlt.png'))
