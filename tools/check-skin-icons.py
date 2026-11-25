"""Host check of fix 164, the home screen's icons ("Trinity rings"), drawn by tools/make-skin-icons.py.

    python3 -I tools/check-skin-icons.py

  (a) assets/skin-src has one SVG per icon of make-skin-icons.py's FILES and nothing else; each has
      the viewBox of its icon's size and uses only the sources' palette (the colours the light theme
      swaps, LIGHT, and those both themes keep, KEPT);
  (b) assets/skin and assets/skin-dark have exactly FILES' PNGs: 8-bit RGBA, at the official sizes
      (192x192, the avatar 300x560), with transparent corners and something drawn;
  (c) each dark icon is the source's colours (its most frequent opaque colour is the grey body
      #C5C9C7) and each light icon the light theme's (the body #131C19; the grey body under 1% of
      its opaque pixels: only antialiased edges);
  (d) both themes draw the same shape: the same pixels are covered (alpha), as one SVG drew both.
A self-test alters a copy of the files (an opaque corner, a missing icon, an extra SVG, a colour
outside the palette, a light icon left grey) and expects each to be caught. Standard library only
(a small PNG reader), so it runs where Pillow is not installed.
"""
import importlib.util
import re
import shutil
import struct
import sys
import tempfile
import zlib
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('make_skin_icons', REPO / 'tools/make-skin-icons.py')
make = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make)
FILES, LIGHT, KEPT = make.FILES, make.LIGHT, make.KEPT
SKINS = ('skin-dark', 'skin')
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def read_png(path):
    """(width, height, RGBA bytes) of an 8-bit RGBA, non-interlaced PNG."""
    data = Path(path).read_bytes()
    if data[:8] != PNG_SIGNATURE:
        raise ValueError('not a PNG')
    at, idat, header = 8, b'', None
    while at < len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        at += 12 + length
    width, height, depth, colour, _, _, interlace = header
    if (depth, colour, interlace) != (8, 6, 0):
        raise ValueError(f'depth {depth}, colour type {colour}, interlace {interlace}: not an 8-bit RGBA PNG')
    raw, stride = zlib.decompress(idat), width * 4
    out, prev = bytearray(), bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    """An 8-bit RGBA PNG (filter 0 rows), for the self-test."""
    rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    Path(path).write_bytes(PNG_SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def hex_rgb(value):
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


def near(a, b, tolerance=6):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def check(root):
    errors = []
    src = root / 'assets/skin-src'
    svgs = sorted(p.stem for p in src.glob('*.svg'))
    if svgs != sorted(FILES):
        errors.append(f'(a) assets/skin-src has {svgs}, want {sorted(FILES)}')
    palette = {c.upper() for c in LIGHT} | {c.upper() for c in KEPT}
    for name in svgs:
        text = (src / f'{name}.svg').read_text(encoding='utf-8')
        if name in FILES:
            w, h = FILES[name][1]
            if f'viewBox="0 0 {w} {h}"' not in text:
                errors.append(f'(a) {name}.svg: viewBox is not 0 0 {w} {h}')
        colours = {c.upper() for c in re.findall(r'(?:fill|stroke)="(#[0-9A-Fa-f]{6})"', text)}
        if colours - palette:
            errors.append(f'(a) {name}.svg: colours outside the palette {sorted(colours - palette)}')
    masks = {}
    for skin in SKINS:
        folder = root / 'assets' / skin
        pngs = sorted(p.name for p in folder.glob('*.png'))
        want = sorted(file for file, _ in FILES.values())
        if pngs != want:
            errors.append(f'(b) assets/{skin} has {pngs}, want {want}')
        for name, (file, size) in FILES.items():
            path = folder / file
            if not path.is_file():
                continue
            try:
                w, h, rgba = read_png(path)
            except ValueError as error:
                errors.append(f'(b) assets/{skin}/{file}: {error}')
                continue
            if (w, h) != size:
                errors.append(f'(b) assets/{skin}/{file}: {(w, h)}, want {size}')
                continue
            alpha = rgba[3::4]
            corners = [alpha[y * w + x] for x in (0, 1, w - 2, w - 1) for y in (0, 1, h - 2, h - 1)]
            if any(corners):
                errors.append(f'(b) assets/{skin}/{file}: the corners are not transparent')
            if max(alpha) < 255:
                errors.append(f'(b) assets/{skin}/{file}: nothing opaque drawn')
            masks[skin, file] = alpha
            opaque = Counter(rgba[i:i + 3] for i in range(0, len(rgba), 4) if rgba[i + 3] == 255)
            if not opaque:
                continue
            top = tuple(opaque.most_common(1)[0][0])
            body = hex_rgb('#C5C9C7') if skin == 'skin-dark' else hex_rgb(LIGHT['#C5C9C7'])
            if not near(top, body):
                errors.append(f'(c) assets/{skin}/{file}: the most frequent colour is #{bytes(top).hex().upper()}, '
                              f'not the body #{bytes(body).hex().upper()}')
            # (A few edge pixels between the white highlight and the black body pass through that grey.)
            grey = sum(n for c, n in opaque.items() if near(tuple(c), hex_rgb('#C5C9C7'), 3))
            if skin == 'skin' and grey * 100 > sum(opaque.values()):
                errors.append(f'(c) assets/skin/{file}: the dark theme\'s grey body is left in the light icon '
                              f'({grey} pixels)')
    for file, _ in FILES.values():
        a, b = masks.get(('skin-dark', file)), masks.get(('skin', file))
        if a is not None and b is not None:
            differing = sum(1 for x, y in zip(a, b) if abs(x - y) > 8)
            if differing > len(a) // 500:
                errors.append(f'(d) {file}: the two themes cover different pixels ({differing})')
    return errors


def self_test():
    def altered(change):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for part in ('assets/skin-src', 'assets/skin', 'assets/skin-dark'):
                shutil.copytree(REPO / part, root / part)
            change(root)
            return check(root)

    def opaque_corner(root):
        path = root / 'assets/skin-dark/icon-games.png'
        w, h, rgba = read_png(path)
        write_png(path, w, h, bytes([255, 0, 0, 255]) + rgba[4:])

    def grey_light(root):
        shutil.copy2(root / 'assets/skin-dark/avatar.png', root / 'assets/skin/avatar.png')

    def odd_colour(root):
        path = root / 'assets/skin-src/disc.svg'
        path.write_text(path.read_text(encoding='utf-8').replace('#A7ABA9', '#FF00FF'), encoding='utf-8')

    cases = [
        ('an opaque corner', opaque_corner, '(b) assets/skin-dark/icon-games.png: the corners'),
        ('a missing icon', lambda r: (r / 'assets/skin/gamerscore.png').unlink(), '(b) assets/skin has'),
        ('an extra SVG', lambda r: (r / 'assets/skin-src/extra.svg').write_text('<svg/>'), '(a) assets/skin-src has'),
        ('a colour outside the palette', odd_colour, '(a) disc.svg: colours outside the palette'),
        ('a light icon left grey', grey_light, '(c) assets/skin/avatar.png'),
    ]
    for what, change, expected in cases:
        errors = altered(change)
        if not any(expected in e for e in errors):
            raise AssertionError(f'self-test: {what} was not caught ({errors})')
    print(f'self-test: {len(cases)} alterations caught')


def main():
    errors = check(REPO)
    for error in errors:
        print('FAIL', error)
    if errors:
        sys.exit(1)
    self_test()
    print(f'PASS: {len(FILES)} home screen icons from assets/skin-src, in assets/skin and assets/skin-dark '
          '(RGBA, official sizes, transparent corners, each theme\'s colours, the same shapes) (fix 164)')


if __name__ == '__main__':
    main()
