"""Renders Nitro's logo (PlayStation-style blue tile, italic N, speed lines).

Writes assets/nitro-logo.png (512), assets/nitro.ico (16-256) and the
installer bitmaps. Run from the repo root: python tools/make_logo.py
Shapes match nitro_ui::DrawLogo (512-unit coordinates).
"""
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 2048
K = S / 512
SLANT = 0.20
N = [(136, 384), (136, 128), (204, 128), (308, 282), (308, 128), (376, 128), (376, 384), (308, 384), (204, 230), (204, 384)]
LINES = [(200, 66, 0.55), (256, 34, 0.85), (312, 66, 0.55)]  # y, start x, opacity
TOP, BOTTOM = (31, 140, 235), (0, 46, 125)


def slant(p, dx=0.0, dy=0.0):
    x, y = p
    return ((x + (256 - y) * SLANT + dx) * K, (y + dy) * K)


def overlay(draw_fn):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    return layer


def logo(size=512):
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(112 * K), fill=255)
    tile = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(tile)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)) + (255,))
    shine = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shine).ellipse([-S * 0.5, -S * 0.9, S * 1.1, S * 0.42], fill=46)
    shine = shine.filter(ImageFilter.GaussianBlur(S * 0.06))
    tile = Image.composite(Image.new("RGBA", (S, S), (255, 255, 255, 255)), tile, shine)
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    im.paste(tile, (0, 0), mask)

    def lines(dr):
        for y, x0, a in LINES:
            x1 = 136 + (256 - y) * SLANT - 22
            dr.rounded_rectangle([x0 * K, (y - 10) * K, x1 * K, (y + 10) * K], radius=10 * K,
                                 fill=(255, 255, 255, int(255 * a)))
    im = Image.alpha_composite(im, overlay(lines))
    shadow = overlay(lambda dr: dr.polygon([slant(p, 10, 16) for p in N], fill=(0, 18, 60, 120)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(14 * K))
    im = Image.alpha_composite(im, Image.composite(shadow, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))
    im = Image.alpha_composite(im, overlay(lambda dr: dr.polygon([slant(p) for p in N], fill=(255, 255, 255, 255))))
    return im.resize((size, size), Image.LANCZOS)


def backdrop(w, h):
    """PlayStation-style deep blue backdrop with soft light waves."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=(int(9 - 6 * t), int(30 - 22 * t), int(82 - 58 * t)))
    import math
    glow = Image.new("L", (w, h), 0)
    g = ImageDraw.Draw(glow)
    for band, (amp, off, width, alpha) in enumerate([(0.06, 0.58, 0.05, 60), (0.08, 0.66, 0.025, 90), (0.05, 0.72, 0.012, 130)]):
        pts = []
        for x in range(0, w + 8, 4):
            y = h * (off + amp * math.sin(x / w * math.pi * 2.2 + band))
            pts.append((x, y))
        for x, y in pts:
            g.ellipse([x - 3, y - h * width, x + 3, y + h * width], fill=alpha)
    glow = glow.filter(ImageFilter.GaussianBlur(max(2, h * 0.02)))
    light = Image.new("RGB", (w, h), (120, 180, 255))
    return Image.composite(light, im, glow)


if __name__ == "__main__":
    import os
    big = logo(512)
    big.save("assets/nitro-logo.png", optimize=True)
    big.save("assets/nitro.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

    font_bold = "fonts/SourceSans3-Semibold.ttf"
    font_light = "fonts/SourceSans3-Light.ttf"
    for w, h in [(164, 314), (328, 628)]:
        im = backdrop(w, h).convert("RGBA")
        s = int(w * 0.52)
        im.alpha_composite(logo(s), ((w - s) // 2, int(h * 0.16)))
        d = ImageDraw.Draw(im)
        f = ImageFont.truetype(font_light, int(w * 0.22))
        bb = d.textbbox((0, 0), "Nitro", font=f)
        y = int(h * 0.16) + s + int(w * 0.08)
        d.text(((w - (bb[2] - bb[0])) // 2 - bb[0], y), "Nitro", font=f, fill=(255, 255, 255))
        f2 = ImageFont.truetype(font_bold, int(w * 0.068))
        sub = "FRAME GENERATION"
        bb2 = d.textbbox((0, 0), sub, font=f2)
        d.text(((w - (bb2[2] - bb2[0])) // 2 - bb2[0], y + (bb[3] - bb[1]) + int(w * 0.12)), sub, font=f2,
               fill=(150, 190, 255))
        im.convert("RGB").save(f"installer/wizard-{w}.bmp")
    for s in (55, 110):
        im = Image.new("RGB", (s, s), (255, 255, 255))
        l = logo(s)
        im.paste(l, (0, 0), l)
        im.save(f"installer/small-{s}.bmp")
    print("ok")
