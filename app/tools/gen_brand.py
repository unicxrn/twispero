#!/usr/bin/env python3
"""
Draw Twispero's logo: the home-screen tile, the background art and the marks
the app draws on its own screens.

THE LOGO
    "twi" (the tile) and "twispero" (the wordmark) in Adwaita Sans at its
    heaviest (opsz 32, wght 900), slanted, white, with a hard dark extrusion
    down and to the right. The dot of the i is a red "live" dot, a true circle.
    The tile sits on a purple gradient square.

OUTPUT
    projects/evoplayer/sce_sys/icon0.png        512x512 tile, square: the PS5
                                                rounds the corners itself
    projects/evoplayer/sce_sys/pic0.png         1920x1080, the wordmark on a
    projects/evoplayer/sce_sys/pic1.png         purple glow; the same picture
                                                twice, as the packager wants both
    projects/evoplayer/assets/icons/icon_app.png        256 px tile, corners
    projects/evoplayer/assets/icons/icon_app_small.png  128 px tile, rounded
    projects/evoplayer/assets/icons/icon_app_mono.png   "twi" alone in white
                                                        on a 2:1 canvas, for
                                                        the app to tint

    The kit draws the tiles at 128 and about 60 px of its 1920x1080 canvas
    and the white lettering at 144 x 72 (draw_app_mark and
    draw_app_lettering in ui_kit/src/evo_hui_twitch_common.cpp), so these are
    twice that, for 4K panels. The RmlUi pages use icon_app.png and
    icon_app_small.png. After a change here, run tools/bundle_rml_assets.py
    (package-app.sh does it too) so the app gets the new icons.

NEEDS
    This runs on the host, not in the dev container (which has no Pillow).
    - Pillow built with libraqm, for the font's kerning. Check with
          python3 -c "from PIL import features; print(features.check('raqm'))"
      On Arch: pacman -S python-pillow libraqm
    - Adwaita Sans, the variable font (adwaita-fonts on Arch). It's under the
      SIL Open Font License and derived from Inter. Only its shapes end up in
      the pictures; the font itself doesn't ship.

USAGE
    python3 tools/gen_brand.py [--font /path/to/AdwaitaSans-Regular.ttf]
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, features
except ImportError:
    sys.exit("gen_brand: needs Pillow (python3 -m pip install pillow, or your distro's package)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EVO = os.path.join(ROOT, "projects", "evoplayer")
SCE_SYS = os.path.join(EVO, "sce_sys")
ICONS = os.path.join(EVO, "assets", "icons")
DEFAULT_FONT = "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf"

PURPLE = (138, 68, 245)
PURPLE_DK = (112, 48, 218)
INK = (33, 12, 74)
WHITE = (255, 255, 255)
RED = (255, 46, 77)
SLANT = 0.18          # shear: the top of a letter leans right by this much of its height
TILE_RADIUS = 0.22    # the in-app tiles' corner radius, as a share of their size

FONT_PATH = DEFAULT_FONT


def font(px, wght=900):
    f = ImageFont.truetype(FONT_PATH, px, layout_engine=ImageFont.Layout.RAQM)
    axes = {"Optical size": 32, "Weight": wght}
    f.set_variation_by_axes([axes[a["name"].decode()] for a in f.get_variation_axes()])
    return f


def lettering(word, px, track_em=-0.02):
    """The slanted letters as a mask (the i without its dot) and the live dot
    as a circle (cx, cy, r) in the mask's pixels."""
    f = font(px)
    track = track_em * px
    pad = px
    w = int(f.getlength(word) + track * len(word) + 2 * pad)
    h = int(px * 1.8)
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    dot = None
    for k, ch in enumerate(word):
        x = pad + f.getlength(word[:k]) + k * track
        if ch == "i":
            # A dotless i, and the dot measured from the difference between
            # the two glyphs, made a little bigger and a little higher.
            d.text((x, pad // 2), "ı", font=f, fill=255)
            a = Image.new("L", (w, h), 0)
            ImageDraw.Draw(a).text((x, pad // 2), "i", font=f, fill=255)
            b = Image.new("L", (w, h), 0)
            ImageDraw.Draw(b).text((x, pad // 2), "ı", font=f, fill=255)
            x0, y0, x1, y1 = ImageChops.subtract(a, b).getbbox()
            r = (x1 - x0) / 2 * 1.14
            dot = [(x0 + x1) / 2, (y0 + y1) / 2 - r * 0.10, r]
        else:
            d.text((x, pad // 2), ch, font=f, fill=255)
    # Slant: output x = input x + SLANT * (h - y), so the top leans right.
    sw = int(w + SLANT * h)
    m = m.transform((sw, h), Image.AFFINE, (1, SLANT, -SLANT * h, 0, 1, 0), resample=Image.BICUBIC)
    dot[0] = dot[0] + SLANT * (h - dot[1])
    # Crop to the letters and the dot.
    dm = Image.new("L", (sw, h), 0)
    cx, cy, r = dot
    ImageDraw.Draw(dm).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    box = ImageChops.lighter(m, dm).getbbox()
    m = m.crop(box)
    return m, (cx - box[0], cy - box[1], r)


def stamp(canvas, word, width, center, depth_em=0.05, face=WHITE):
    """Draw the extruded word on canvas, `width` px wide, its face centred on
    `center`."""
    m, (cx, cy, r) = lettering(word, 560)
    sc = width / m.width
    m = m.resize((round(m.width * sc), round(m.height * sc)), Image.LANCZOS)
    cx, cy, r = cx * sc, cy * sc, r * sc
    depth = max(1, round(560 * sc * depth_em))
    x = round(center[0] - m.width / 2 - depth / 2)
    y = round(center[1] - m.height / 2 - depth / 2)
    dot = Image.new("L", m.size, 0)
    ImageDraw.Draw(dot).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    solid = ImageChops.lighter(m, dot)
    for i in range(depth, 0, -1):
        canvas.paste(INK, (x + i, y + i), solid)
    canvas.paste(face, (x, y), m)
    canvas.paste(RED, (x, y), dot)


def purple_bg(w, h):
    im = Image.new("RGB", (w, h), PURPLE)
    g = Image.linear_gradient("L").resize((w, h))
    return Image.composite(Image.new("RGB", (w, h), PURPLE_DK), im, g)


def icon(size=512):
    """The tile, square: "twi" on the purple gradient."""
    S = 4
    N = size * S
    im = purple_bg(N, N)
    stamp(im, "twi", N * 0.60, (N * 0.5, N * 0.5))
    return im.resize((size, size), Image.LANCZOS)


def background(w=1920, h=1080):
    """pic0/pic1: the art behind the tile on the home screen. The PS5 lays its
    tiles over the top left and its buttons over the bottom left, so the
    wordmark sits right of centre."""
    S = 2
    W, H = w * S, h * S
    ramp = Image.new("L", (W, 1))
    for x in range(W):
        t = min(1.0, max(0.0, (x / W - 0.15) / 0.75))
        ramp.putpixel((x, 0), int(255 * t * t * (3 - 2 * t)))
    ramp = ramp.resize((W, H))
    im = Image.composite(Image.new("RGB", (W, H), PURPLE_DK), Image.new("RGB", (W, H), (18, 8, 42)), ramp)
    light = Image.new("L", (W, H), 0)
    ImageDraw.Draw(light).ellipse((W * 0.40, H * 0.15, W * 0.95, H * 0.85), fill=110)
    light = light.filter(ImageFilter.GaussianBlur(W * 0.07))
    im = Image.composite(Image.new("RGB", (W, H), (176, 120, 255)), im, light)
    stamp(im, "twispero", W * 0.44, (W * 0.67, H * 0.50))
    return im.resize((w, h), Image.LANCZOS)


def rounded_tile(size):
    """The tile with its corners cut round in the alpha, for the app's own
    screens (the PS5 rounds icon0 itself, nothing rounds these)."""
    S = 4
    mask = Image.new("L", (size * S, size * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size * S - 1, size * S - 1), TILE_RADIUS * size * S, fill=255)
    out = icon(size).convert("RGBA")
    out.putalpha(mask.resize((size, size), Image.LANCZOS))
    return out


def mono(word, width):
    """The lettering alone in white on clear, for the app to tint: no
    extrusion, and a thin gap cut round the dot so it still reads as the i's
    dot when it's all one colour. `width` px wide on a canvas half as tall,
    the letters centred, so the app can draw it at width x width / 2."""
    m, (cx, cy, r) = lettering(word, 560)
    gap = r * 0.30
    cut = Image.new("L", m.size, 0)
    ImageDraw.Draw(cut).ellipse((cx - r - gap, cy - r - gap, cx + r + gap, cy + r + gap), fill=255)
    m = ImageChops.subtract(m, cut)
    ImageDraw.Draw(m).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    sc = width / m.width
    letters = m.resize((width, round(m.height * sc)), Image.LANCZOS)
    alpha = Image.new("L", (width, width // 2), 0)
    alpha.paste(letters, (0, (alpha.height - letters.height) // 2))
    out = Image.new("RGBA", alpha.size, WHITE + (0,))
    out.putalpha(alpha)
    return out


def save(im, path):
    im.save(path, optimize=True)
    print("wrote %s (%dx%d, %d bytes)" % (os.path.relpath(path, ROOT), im.width, im.height, os.path.getsize(path)))


def main():
    global FONT_PATH
    ap = argparse.ArgumentParser(description="Draw Twispero's icon, backgrounds and in-app marks.")
    ap.add_argument("--font", default=DEFAULT_FONT, help="Adwaita Sans, the variable font (default %(default)s)")
    args = ap.parse_args()

    if not os.path.isfile(args.font):
        sys.exit(f"gen_brand: no font at {args.font}; install Adwaita Sans or pass --font")
    if not features.check("raqm"):
        sys.exit("gen_brand: this Pillow has no raqm layout, so the letters would be spaced wrong; "
                 "install libraqm (Arch: pacman -S libraqm)")
    FONT_PATH = args.font

    save(icon(512), os.path.join(SCE_SYS, "icon0.png"))
    pic = background()
    save(pic, os.path.join(SCE_SYS, "pic0.png"))
    save(pic, os.path.join(SCE_SYS, "pic1.png"))
    save(rounded_tile(256), os.path.join(ICONS, "icon_app.png"))
    save(rounded_tile(128), os.path.join(ICONS, "icon_app_small.png"))
    save(mono("twi", 288), os.path.join(ICONS, "icon_app_mono.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
