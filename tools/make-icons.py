#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Draw the apps' 96 x 96 icons (binary PPM, what the launcher reads).
Run from the repo root: python3 tools/make-icons.py. Needs python3-pil and
the DejaVu fonts."""
import math
from PIL import Image, ImageDraw, ImageFont

S = 96
BG = (16, 22, 34)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def save(im, app):
    im.convert("RGB").save(f"apps/{app}/icon.ppm", format="PPM")


def hello():
    im = Image.new("RGB", (S, S), BG)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((14, 14, 82, 82), 16, fill=(255, 176, 60))
    f = ImageFont.truetype(FONT, 34)
    d.text((48, 49), "Hi", font=f, fill=BG, anchor="mm")
    save(im, "hello")


def waterfall():
    im = Image.new("RGB", (S, S), (8, 10, 16))
    px = im.load()
    stops = [(8, 10, 16), (11, 16, 48), (42, 20, 80), (122, 36, 72), (217, 96, 30), (255, 159, 28), (255, 232, 184)]

    def ramp(v):
        v = max(0.0, min(1.0, v)) * (len(stops) - 1)
        a = min(int(v), len(stops) - 2)
        f = v - a
        return tuple(int(stops[a][i] * (1 - f) + stops[a + 1][i] * f) for i in range(3))

    for y in range(30, S):
        for x in range(S):
            v = 0.12 + 0.05 * math.sin(x * 1.7 + y * 0.9)
            for c, w in ((28, 5), (64, 7)):
                if (y // 12) % 3 != 1:
                    v += 0.8 * math.exp(-((x - c) / w) ** 2)
            px[x, y] = ramp(v)
    d = ImageDraw.Draw(im)
    pts = [(x, 24 - 14 * math.exp(-((x - 28) / 5) ** 2) - 12 * math.exp(-((x - 64) / 7) ** 2)) for x in range(S)]
    d.line(pts, fill=(255, 176, 60), width=2)
    save(im, "waterfall")


def links():
    im = Image.new("RGB", (S, S), (14, 20, 30))
    d = ImageDraw.Draw(im)
    hub, around = (48, 48), [(20, 22), (78, 18), (82, 70), (24, 78), (50, 86)]
    for i, p in enumerate(around):
        d.line([hub, p], fill=(110, 210, 130) if i < 3 else (70, 90, 115), width=4)
    for p in around:
        d.ellipse((p[0] - 8, p[1] - 8, p[0] + 8, p[1] + 8), fill=(225, 232, 240))
    d.ellipse((hub[0] - 14, hub[1] - 14, hub[0] + 14, hub[1] + 14), fill=(255, 190, 60))
    save(im, "links")


def nes():
    """A two-button pad."""
    im = Image.new("RGB", (S, S), BG)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((8, 26, 88, 70), 9, fill=(214, 216, 220))
    d.rounded_rectangle((13, 31, 83, 65), 5, fill=(36, 38, 46))
    d.rectangle((25, 37, 31, 59), fill=(214, 216, 220))     # the cross
    d.rectangle((17, 45, 39, 51), fill=(214, 216, 220))
    d.rounded_rectangle((44, 52, 50, 55), 1, fill=(120, 124, 134))   # Select, Start
    d.rounded_rectangle((53, 52, 59, 55), 1, fill=(120, 124, 134))
    d.ellipse((62, 42, 70, 50), fill=(226, 60, 52))         # B, A
    d.ellipse((73, 42, 81, 50), fill=(226, 60, 52))
    save(im, "nes")


hello()
waterfall()
links()
nes()
