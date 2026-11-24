"""Draws the TrinityX360 package art: icon0.png (512x512), pic0.png (3840x2160), pic1.png (1920x1080).

    python3 -I tools/source-1.0.2/package-art/make_art.py [--out <folder>] [--banner docs/images/banner.png]

Also the README banner (docs/images/banner.png, 2172x724) with --banner.
Original art, made for the rename to TrinityX360 (1.0.6): three interlocking rings (the "trinity")
over a dark blue background and the word mark "TrinityX360" in Noto Sans SemiBold
(assets/fonts, SIL Open Font License). No Xbox or PlayStation logo, shape or typeface is used.
Needs Pillow. package.py pins the committed files by SHA-256 (package-art/README.md); another
Pillow or FreeType version can draw slightly different bytes, so after running this script the
pins in package.py, package-art/README.md and tools/check-source-package.py must be updated.
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
FONT = HERE.parents[2] / 'assets/fonts/NotoSans-SemiBold.ttf'
SCALE = 4  # supersampling, for smooth edges

TOP = (16, 24, 48)
BOTTOM = (4, 6, 14)
RINGS = [(236, 240, 248), (45, 212, 191), (139, 92, 246)]  # silver, teal, violet
WORD = (236, 240, 248)
ACCENT = (45, 212, 191)


def background(width, height):
    """A vertical gradient with a soft light behind the rings."""
    image = Image.new('RGB', (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(1, height - 1)
        draw.line([(0, y), (width, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)))
    return image


def glow(size, centre, radius, colour, strength):
    layer = Image.new('RGB', size, (0, 0, 0))
    ImageDraw.Draw(layer).ellipse([centre[0] - radius, centre[1] - radius, centre[0] + radius, centre[1] + radius],
                                  fill=tuple(round(c * strength) for c in colour))
    return layer.filter(ImageFilter.GaussianBlur(radius * 0.6))


def rings(draw, centre, ring_radius, stroke):
    """Three rings on the corners of a triangle around centre, each through the middle."""
    offset = ring_radius * 0.62
    for index, colour in enumerate(RINGS):
        angle = math.radians(-90 + 120 * index)
        x = centre[0] + offset * math.cos(angle)
        y = centre[1] + offset * math.sin(angle)
        draw.ellipse([x - ring_radius, y - ring_radius, x + ring_radius, y + ring_radius],
                     outline=colour, width=stroke)


def word_mark(draw, x, top, height, centred=True):
    """"Trinity" in white and "X360" in teal, centred on x or starting at x."""
    font = ImageFont.truetype(str(FONT), height)
    left_text, right_text = 'Trinity', 'X360'
    left_width = draw.textlength(left_text, font=font)
    if centred:
        x -= (left_width + draw.textlength(right_text, font=font)) / 2
    draw.text((x, top), left_text, font=font, fill=WORD)
    draw.text((x + left_width, top), right_text, font=font, fill=ACCENT)


def compose(width, height, ring_centre, ring_radius, text):
    """The background and its light at the final size; the rings and the text drawn
    supersampled on a transparent layer and scaled down over it. text(draw, scale) draws the
    text at the supersampled size."""
    image = background(width, height)
    image = ImageChops.add(image, glow((width, height), ring_centre, ring_radius * 1.9, ACCENT, 0.28))
    scale = SCALE if width <= 1024 else 2
    layer = Image.new('RGBA', (width * scale, height * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    rings(draw, (ring_centre[0] * scale, ring_centre[1] * scale), ring_radius * scale,
          max(scale, round(ring_radius * scale * 0.13)))
    text(draw, scale)
    layer = layer.resize((width, height), Image.LANCZOS)
    image.paste(layer, (0, 0), layer)
    return image


def centred(width, height, ring_radius, ring_y, word_top, word_height):
    """The rings in the middle and the word mark under them (icon0, pic0, pic1)."""
    return compose(width, height, (width / 2, ring_y), ring_radius,
                   lambda draw, s: word_mark(draw, width * s / 2, word_top * s, round(word_height * s)))


def icon(size=512):
    return centred(size, size, size * 0.19, size * 0.40, size * 0.70, size * 0.135)


def picture(width, height):
    return centred(width, height, height * 0.17, height * 0.40, height * 0.66, height * 0.12)


def banner(width=2172, height=724):
    """The README banner: the word mark and a subtitle on the left, the rings on the right."""
    def text(draw, s):
        word_mark(draw, width * 0.045 * s, height * 0.30 * s, round(height * 0.22 * s), centred=False)
        font = ImageFont.truetype(str(FONT.with_name('NotoSans-Regular.ttf')), round(height * 0.068 * s))
        draw.text((width * 0.048 * s, height * 0.60 * s), 'Xbox 360 emulation on PlayStation 5',
                  font=font, fill=(200, 208, 224))
    return compose(width, height, (width * 0.76, height * 0.50), height * 0.25, text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=HERE)
    parser.add_argument('--banner', type=Path, help='also write the README banner (docs/images/banner.png)')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, image in (('icon0.png', icon()), ('pic0.png', picture(3840, 2160)),
                        ('pic1.png', picture(1920, 1080))):
        image.convert('RGB').save(args.out / name, optimize=True)
        print('wrote', args.out / name, image.size)
    if args.banner:
        banner().convert('RGB').save(args.banner, optimize=True)
        print('wrote', args.banner)


if __name__ == '__main__':
    main()
