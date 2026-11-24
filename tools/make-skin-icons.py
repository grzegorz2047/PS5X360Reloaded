"""Fix 164: draws the home screen's icons (the "Trinity rings" set) from their SVG sources.

    python -I tools/make-skin-icons.py [--edge <msedge.exe>] [--check-only]

assets/skin-src/<name>.svg (drawn in the dark theme's colours: a grey body #C5C9C7, greys #A7ABA9
and #8E9290, lime #4DE11F, white #F2F4F3, blue #1E7FE0) become
  * assets/skin-dark/<file>.png: the SVG as it is;
  * assets/skin/<file>.png (the light theme): the same SVG with the body and greys swapped for the
    light skin's near-black body and dark greys (LIGHT), as the official light icons are coloured;
    lime, white and blue stay.
<file> is FILES[name]; the sizes are the official package's (192x192, the avatar 300x560), RGBA
with transparent corners. stage-reloaded-candidate.py copies both folders over the base package's
skin pictures of the same names, as the release package does (they replace the official icons).

The SVGs are rasterised by Microsoft Edge headless (the user's Windows PC has it) at SCALE times
the size and scaled down with Pillow, with premultiplied alpha. The PNGs are committed; this script
is only needed to draw them again. tools/check-skin-icons.py checks them (in CI).
"""
import argparse
import base64
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'assets/skin-src'
DARK = ROOT / 'assets/skin-dark'
LIGHT_DIR = ROOT / 'assets/skin'
# SVG source -> the skin picture it draws (port/kinect_skin.cpp's cache names) and its size.
FILES = {
    'games': ('icon-games.png', (192, 192)),
    'disc': ('icon-disc.png', (192, 192)),
    'gamerscore': ('gamerscore.png', (192, 192)),
    'settings': ('icon-settings.png', (192, 192)),
    'network': ('icon-network.png', (192, 192)),
    'avatar': ('avatar.png', (300, 560)),
}
# The dark theme's colours (the sources) -> the light theme's, sampled from the official light
# skin's icons (body #131C19 / #111B17, greys #2C3A34 / #2D4035). #8E9290, the sources' darkest
# grey, is drawn inside #A7ABA9 parts (the sticks, the disc's hub), so it becomes a lighter step of
# the same green-grey, #3A4D44, to stay visible on #2C3A34.
LIGHT = {'#C5C9C7': '#131C19', '#A7ABA9': '#2C3A34', '#8E9290': '#3A4D44'}
KEPT = ('#4DE11F', '#F2F4F3', '#1E7FE0')
SCALE = 4
EDGE = Path(r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')


def light_svg(text):
    for dark, light in LIGHT.items():
        text = text.replace(f'"{dark}"', f'"{light}"')
    return text


def render(edge, svg_text, size, work):
    """The SVG at size (RGBA): Edge draws it SCALE times larger on a transparent page."""
    from PIL import Image
    w, h = size[0] * SCALE, size[1] * SCALE
    data = base64.b64encode(svg_text.encode('utf-8')).decode('ascii')
    page = work / 'page.html'
    page.write_text('<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;'
                    f'overflow:hidden}}img{{display:block;width:{w}px;height:{h}px}}</style></head><body>'
                    f'<img src="data:image/svg+xml;base64,{data}"></body></html>', encoding='utf-8')
    shot = work / 'shot.png'
    if shot.exists():
        shot.unlink()
    subprocess.run([str(edge), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
                    f'--user-data-dir={work / "profile"}', '--force-device-scale-factor=1',
                    '--default-background-color=00000000', f'--window-size={w},{h}', f'--screenshot={shot}',
                    page.as_uri()], check=True, capture_output=True, timeout=120)
    image = Image.open(shot).convert('RGBA')
    if image.size != (w, h):
        sys.exit(f'Edge made a {image.size} screenshot, not {(w, h)}')
    return image.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--edge', type=Path, default=EDGE)
    args = parser.parse_args()
    if not args.edge.exists():
        sys.exit(f'{args.edge} not found (--edge)')
    sources = sorted(p.stem for p in SRC.glob('*.svg'))
    if sources != sorted(FILES):
        sys.exit(f'assets/skin-src has {sources}, FILES names {sorted(FILES)}')
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for name, (file, size) in FILES.items():
            text = (SRC / f'{name}.svg').read_text(encoding='utf-8')
            for folder, svg in ((DARK, text), (LIGHT_DIR, light_svg(text))):
                folder.mkdir(parents=True, exist_ok=True)
                render(args.edge, svg, size, work).save(folder / file, optimize=True)
                print('wrote', (folder / file).relative_to(ROOT), size)
        shutil.rmtree(work / 'profile', ignore_errors=True)


if __name__ == '__main__':
    main()
