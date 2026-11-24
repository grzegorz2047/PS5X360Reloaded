"""Draws the TrinityX360 package art: icon0.png (512x512), pic0.png (3840x2160), pic1.png (1920x1080).

    python3 -I tools/source-1.0.2/package-art/make_art.py [--out <folder>] [--banner docs/images/banner.png]

Made for the rename to TrinityX360 (1.0.6), in the style of the art the app had until then (the
official PS5X360 art with a "RELOADED" plate): a black background, a large white and green "X" of
two slanted bars, and the name in white and green, "Trinity" + "X360", now in place of "PS5X360"
and the plate. Everything is drawn here from shapes and Noto Sans SemiBold (assets/fonts, SIL Open
Font License); no pixel of the official art is used, and no Xbox or PlayStation logo, shape or
typeface. The icon's name is set as large as the tile allows, so it reads at small sizes.
Also the README banner (docs/images/banner.png, 2172x724) with --banner.

Needs Pillow. package.py pins the committed files by SHA-256 (package-art/README.md); another
Pillow or FreeType version can draw slightly different bytes, so after running this script the
pins in package.py, package-art/README.md and tools/check-source-package.py must be updated, and
sce_sys/ written again (tools/source-1.0.2/repo_sce_sys.py --write).
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
FONT = HERE.parents[2] / 'assets/fonts/NotoSans-SemiBold.ttf'
FONT_REGULAR = HERE.parents[2] / 'assets/fonts/NotoSans-Regular.ttf'

BLACK = (3, 4, 3)
WHITE = (244, 245, 243)
WHITE_LOW = (214, 216, 214)
GREEN_HIGH = (78, 246, 22)
GREEN_LOW = (36, 176, 8)
GREEN_TEXT = (64, 238, 16)
BORDER = (52, 214, 12)

# The X in a unit box (x, y from its top-left corner, in parts of its width and height): two bars
# of parallel edges, the green one behind the white one.
WHITE_BAR = [(0.05, 0.0), (0.31, 0.0), (0.95, 1.0), (0.69, 1.0)]
GREEN_BAR = [(0.71, 0.0), (1.0, 0.0), (0.29, 1.0), (0.0, 1.0)]


def gradient(size, top, bottom):
    """A vertical gradient (RGB) from top to bottom."""
    width, height = size
    column = Image.new('RGB', (1, height))
    for y in range(height):
        t = y / max(1, height - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((width, height))


def polygon_mask(size, points):
    mask = Image.new('L', size, 0)
    ImageDraw.Draw(mask).polygon(points, fill=255)
    return mask


def draw_x(image, box, scale):
    """The X in box (left, top, width, height), drawn at `scale` times the size and scaled down."""
    left, top, width, height = box
    big = (round(width * scale), round(height * scale))
    pad = round(0.08 * big[0])
    canvas = (big[0] + 2 * pad, big[1] + 2 * pad)

    def to_px(points):
        return [(pad + x * big[0], pad + y * big[1]) for x, y in points]

    green_mask = polygon_mask(canvas, to_px(GREEN_BAR))
    white_mask = polygon_mask(canvas, to_px(WHITE_BAR))
    layer = Image.new('RGB', canvas, (0, 0, 0))
    layer.paste(gradient(canvas, GREEN_HIGH, GREEN_LOW), (0, 0), green_mask)
    # The white bar's shadow on the green one, then the white bar.
    shadow = white_mask.filter(ImageFilter.GaussianBlur(big[0] * 0.025))
    shadow = ImageChops.multiply(shadow, green_mask)
    layer.paste((0, 0, 0), (0, 0), shadow.point(lambda v: v * 3 // 4))
    layer.paste(gradient(canvas, WHITE, WHITE_LOW), (0, 0), white_mask)
    alpha = ImageChops.lighter(green_mask, white_mask)
    small = (round(canvas[0] / scale), round(canvas[1] / scale))
    layer = layer.resize(small, Image.LANCZOS)
    alpha = alpha.resize(small, Image.LANCZOS)
    image.paste(layer, (round(left - pad / scale), round(top - pad / scale)), alpha)


def fit_font(path, text, width, stroke_part):
    """The largest font size (and its stroke) whose text fits in width."""
    probe = ImageDraw.Draw(Image.new('L', (1, 1)))
    size = 8
    while True:
        font = ImageFont.truetype(str(path), size + 1)
        box = probe.textbbox((0, 0), text, font=font, stroke_width=round((size + 1) * stroke_part))
        if box[2] - box[0] > width:
            return ImageFont.truetype(str(path), size), round(size * stroke_part)
        size += 1


def draw_name(image, x, top, width, scale=4, left_aligned=False):
    """"Trinity" in white and "X360" in green, width wide, its letters' top at top; centred on x,
    or starting at x. Returns the name's height."""
    font, stroke = fit_font(FONT, 'TrinityX360', width * scale, 0.025)  # a little heavier than SemiBold
    probe = ImageDraw.Draw(Image.new('L', (1, 1)))
    first = probe.textlength('Trinity', font=font)
    box = probe.textbbox((0, 0), 'TrinityX360', font=font, stroke_width=stroke)
    margin = 4 * scale
    layer = Image.new('RGBA', (box[2] - box[0] + 2 * margin, box[3] - box[1] + 2 * margin), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    origin = (margin - box[0], margin - box[1])
    draw.text(origin, 'Trinity', font=font, fill=WHITE, stroke_width=stroke, stroke_fill=WHITE)
    draw.text((origin[0] + first, origin[1]), 'X360', font=font, fill=GREEN_TEXT, stroke_width=stroke,
              stroke_fill=GREEN_TEXT)
    layer = layer.resize((round(layer.width / scale), round(layer.height / scale)), Image.LANCZOS)
    left = x if left_aligned else x - layer.width / 2
    image.paste(layer, (round(left - 4), round(top - 4)), layer)
    return layer.height - 8


def subtitle(image, left, top, height, text, colour=(206, 210, 206), scale=4):
    font = ImageFont.truetype(str(FONT_REGULAR), round(height * scale))
    probe = ImageDraw.Draw(Image.new('L', (1, 1)))
    box = probe.textbbox((0, 0), text, font=font)
    margin = 4 * scale
    layer = Image.new('RGBA', (box[2] - box[0] + 2 * margin, box[3] - box[1] + 2 * margin), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((margin - box[0], margin - box[1]), text, font=font, fill=colour)
    layer = layer.resize((round(layer.width / scale), round(layer.height / scale)), Image.LANCZOS)
    image.paste(layer, (round(left - 4), round(top - 4)), layer)


def icon(size=512):
    """The tile: a rounded green frame on black, the X, and the name across the tile's width."""
    scale = 4
    big = Image.new('RGB', (size * scale, size * scale), (0, 0, 0))
    inset = 10 * scale
    ImageDraw.Draw(big).rounded_rectangle([inset, inset, size * scale - inset, size * scale - inset],
                                          radius=64 * scale, fill=BLACK, outline=BORDER, width=3 * scale)
    image = big.resize((size, size), Image.LANCZOS)
    draw_x(image, (size * 0.215, size * 0.105, size * 0.57, size * 0.50), scale=4)
    draw_name(image, size / 2, size * 0.70, size * 0.84)
    return image


def picture(width, height):
    """A background: the X in the middle and the name under it, on black."""
    image = Image.new('RGB', (width, height), BLACK)
    x_width, x_height = height * 0.59, height * 0.465
    draw_x(image, ((width - x_width) / 2, height * 0.185, x_width, x_height), scale=2)
    draw_name(image, width / 2, height * 0.70, height * 0.90, scale=2)
    return image


def banner(width=2172, height=724):
    """The README banner: the name and a subtitle on the left, the X on the right."""
    image = Image.new('RGB', (width, height), BLACK)
    draw_x(image, (width * 0.66, height * 0.17, height * 0.80, height * 0.66), scale=2)
    name_height = draw_name(image, width * 0.045, height * 0.33, width * 0.55, left_aligned=True)
    subtitle(image, width * 0.048, height * 0.33 + name_height + height * 0.07, height * 0.062,
             'Xbox 360 emulation on PlayStation 5')
    return image


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
