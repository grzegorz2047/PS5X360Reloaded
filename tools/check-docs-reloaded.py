"""Host check of the user docs for the app's name and title id (v1.0.3, renamed in v1.0.6).

    python3 -I tools/check-docs-reloaded.py [--repo <repository root>]

Since v1.0.3 the fork's app has the title id PPSA83600, installed next to the official PS5X360
(PPSA50011); it was called PS5X360Reloaded until v1.0.5 and is TrinityX360 since v1.0.6, with the
same title id. README.md, docs/release-notes/v1.0.3.md and v1.0.6.md, docs/FORWARDER.md and
docs/SOURCE-1.0.2.md must give the install paths and file names, the coexistence and import
notes (the automatic first-start copy, the manual /download0 steps, Forwarder Manager's target,
removing the old PPSA50011 images), the rename (TrinityX360, the same title id, the drives'
PS5X360Reloaded/saves folders kept), and no stale install instruction for PPSA50011 or for the
old name. The upstream credits that name PS5X360, BrinooTk and Xenia Canary stay. The v1.0.3 notes
stay as they were (PS5X360Reloaded). Relative links and in-page anchors of these files
must resolve, and the README images must be the expected PNG sizes.

The check also runs itself against altered texts of the docs (a stale label, a stale install
path, a dropped credit, a broken link) and expects each to fail.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

DOCS = ('README.md', 'docs/release-notes/v1.0.3.md', 'docs/release-notes/v1.0.6.md', 'docs/FORWARDER.md',
        'docs/SOURCE-1.0.2.md')

# Strings each file must contain.
REQUIRED = {
    'README.md': [
        '# TrinityX360\n',
        # The rename (v1.0.6): the same title id and data, the drives' folders kept.
        'Until v1.0.5 this app was called **PS5X360Reloaded**.',
        '## New name: TrinityX360',
        'The title id is still **`PPSA83600`**',
        '`<drive>/PS5X360Reloaded/saves`',
        '**TrinityX360 v1.0.6**',
        '**TrinityX360 vX.Y.Z_candidate_N**',
        # The v1.0.3 section keeps its history.
        '**PS5X360Reloaded v1.0.3**',
        '`PPSA83600.ffpkg`',
        '`PPSA83600.ffpfsc`',
        '`PPSA83600.zip`',
        '`/data/homebrew/PPSA83600/eboot.bin`',
        '`/mnt/shadowmnt/PPSA83600_*`',
        '### Next to the official PS5X360',
        'official PS5X360 (`PPSA50011`)',
        '`PPSA50011.ffpkg` / `PPSA50011.ffpfsc`',
        '**Automatic import on the first start.**',
        '`/mnt/sandbox/PPSA50011_000/download0/xbox360ps5`',
        '`/mnt/sandbox/PPSA83600_000/download0/xbox360ps5/`',
        '`settings.txt`, `profile.txt`',
        '`game-settings/`',
        '`fsi.c`, `fsi.1`',
        '`covers/`, `patches/`',
        '**Generic**',
        '`<drive>/PS5X360/saves`',
        '`/data/homebrew/PPSA83600/logs`',
        'make room on the internal storage first',
        'if the copied `game_paths.txt` lists',
        '`PPSA83600/licenses/`',
        # Upstream credits stay.
        'This repository is a fork of [BrinooTk/PS5X360](https://github.com/BrinooTk/PS5X360).',
        'based on [PS5X360](https://github.com/BrinooTk/PS5X360)\nby BrinooTk and on [Xenia Canary](https://github.com/xenia-canary/xenia-canary)',
        'Based on [PS5X360](https://github.com/BrinooTk/PS5X360) by BrinooTk and on',
        '[Ko-fi](https://ko-fi.com/brinoblitz)',
        'report on [BrinooTk/PS5X360](https://github.com/BrinooTk/PS5X360/issues)',
    ],
    'docs/release-notes/v1.0.3.md': [
        '## PS5X360Reloaded v1.0.3 (first release built from source)\n',
        'The home screen shows the label `PS5X360Reloaded v1.0.3`.',
        '**`PPSA83600.ffpkg`**',
        '**`PPSA83600.ffpfsc`**',
        '**`PPSA83600.zip`**',
        '`/data/homebrew/PPSA83600/eboot.bin`',
        '`/mnt/shadowmnt/PPSA83600_*`',
        'official PS5X360 (`PPSA50011`)',
        '`PPSA50011.ffpkg` / `PPSA50011.ffpfsc`',
        '**Automatic import on the first start:**',
        'never writes to `/data/homebrew/PPSA50011`',
        '`/mnt/sandbox/PPSA50011_000/download0/xbox360ps5`',
        '`/mnt/sandbox/PPSA83600_000/download0/xbox360ps5/`',
        '`settings.txt`, `profile.txt`',
        '`game-settings/`',
        '`fsi.c`, `fsi.1`',
        '`covers/`, `patches/`',
        '**Generic**',
        '`<drive>/PS5X360/saves`',
        'BrinooTk',
    ],
    'docs/release-notes/v1.0.6.md': [
        '## TrinityX360 v1.0.6\n',
        'The home screen shows `TrinityX360 v1.0.6`.',
        '(until v1.0.5 it was called PS5X360Reloaded)',
        'the title id stays `PPSA83600`',
        '`<drive>/PS5X360Reloaded/saves`',
        '`contentVersion` is `01.006.000`',
        'based on [PS5X360](https://github.com/BrinooTk/PS5X360) by BrinooTk and on\n[Xenia Canary](https://github.com/xenia-canary/xenia-canary)',
        '`licenses/PS5X360-MIT.txt`',
        '`TrinityX360-1.0.6-scripts.tar.gz`',
    ],
    'docs/FORWARDER.md': [
        'starts TrinityX360 (`PPSA83600`)',
        'sceSystemServiceLaunchApp("PPSA83600", args)',
        'TrinityX360 (or PS5X360Reloaded, v1.0.3 to v1.0.5) installed as `PPSA83600`',
        'tiles made for\nPS5X360Reloaded keep working',
        '`Generic (enter a title ID)`, with **Target title ID** `PPSA83600`',
        '"target": "PPSA83600",',
        'Ported from upstream [BrinooTk/PS5X360#62](https://github.com/BrinooTk/PS5X360/pull/62)',
    ],
    'docs/SOURCE-1.0.2.md': [
        '(`TrinityX360 vX.Y.Z` on the home screen',
        'The released app is **TrinityX360** (named PS5X360Reloaded until 1.0.5)',
        '`TrinityX360-X.Y.Z-scripts.tar.gz`',
        'written under `PPSA83600/`',
        'title id `PPSA83600`',
        '`PPSA83600.ffpkg`',
        '`PPSA83600.zip`',
        'the whole `PPSA83600` folder has to be installed',
        'since the rename the folder is `/data/homebrew/PPSA83600/cache`',
        '`/data/homebrew/PPSA83600/skin` in TrinityX360',
        # The page is about the official 1.0.2 package; those references stay.
        'This page records whether the official PS5X360 v1.0.2 `eboot.bin` (package `PPSA50011.zip`,',
    ],
}

# Stale install instructions that must not come back in the user-facing install docs.
FORBIDDEN = {
    'README.md': ['PPSA50011.zip', '/data/homebrew/PPSA50011/eboot.bin', '`PPSA50011/logs`',
                  'PPSA50011/licenses', 'PS5X360 v1.0.3', '# PS5X360 Reloaded', '```text\nPPSA50011/',
                  # The old name as the app's present one.
                  '# PS5X360Reloaded\n', 'open **PS5X360Reloaded**', '## What PS5X360Reloaded includes',
                  '**PS5X360Reloaded v1.0.3_candidate_N**', '`<drive>/PS5X360/saves` (the folder name did not',
                  # The drives' saves folder kept its name.
                  '<drive>/TrinityX360/saves'],
    'docs/release-notes/v1.0.3.md': ['PPSA50011.zip', '/data/homebrew/PPSA50011/eboot.bin',
                                     'PPSA50011/assets/lang', 'PS5X360 v1.0.3',
                                     '/data/homebrew/PPSA50011/cache` (previously'],
    'docs/release-notes/v1.0.6.md': ['PS5X360Reloaded v1.0.6', 'PPSA50011.zip', 'PS5X360Reloaded-1.0.6',
                                     '<drive>/TrinityX360/saves'],
    'docs/FORWARDER.md': ['PS5X360 installed as `PPSA50011`', 'sceSystemServiceLaunchApp("PPSA50011", args)',
                          '**Target app**: `PS5X360` (`PPSA50011`).', 'starts PS5X360Reloaded (`PPSA83600`)',
                          'Close PS5X360Reloaded before'],
    'docs/SOURCE-1.0.2.md': ['`PS5X360 vX.Y.Z`', 'then `PPSA50011.ffpkg`',
                             'the whole `PPSA50011` folder has to be installed', '(`PS5X360Reloaded vX.Y.Z`',
                             '`PS5X360Reloaded-X.Y.Z-scripts.tar.gz`'],
}

IMAGES = {'docs/images/banner.png': (2172, 724), 'docs/images/installation.png': (1942, 809)}


def slug(heading):
    """GitHub's anchor for a Markdown heading."""
    text = re.sub(r'[`*_\[\]()]', '', heading.strip().lower())
    text = re.sub(r'[^\w\- ]', '', text)
    return text.replace(' ', '-')


def anchors(text):
    found = set()
    in_code = False
    for line in text.splitlines():
        if line.startswith('```'):
            in_code = not in_code
        elif not in_code and line.startswith('#'):
            found.add(slug(line.lstrip('#')))
    return found


def png_size(path):
    data = path.read_bytes()[:24]
    if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise AssertionError(f'{path}: not a PNG')
    return struct.unpack('>II', data[16:24])


def check(repo, overrides=None):
    """Errors in the docs under repo; overrides replaces some files' text (for the self-test)."""
    errors = []
    texts = {}
    for name in DOCS:
        path = repo / name
        if overrides and name in overrides:
            texts[name] = overrides[name]
        elif not path.is_file():
            errors.append(f'{name}: missing')
        else:
            texts[name] = path.read_text(encoding='utf-8')
    for name, text in texts.items():
        for needle in REQUIRED[name]:
            if needle not in text:
                errors.append(f'{name}: missing {needle!r}')
        for needle in FORBIDDEN[name]:
            if needle in text:
                errors.append(f'{name}: stale {needle!r}')
        # Relative links and anchors.
        own = anchors(text)
        for target in re.findall(r'\]\(([^)\s]+)\)|<img src="([^"]+)"', text):
            target = target[0] or target[1]
            if re.match(r'[a-z]+:', target):
                continue
            file_part, _, anchor = target.partition('#')
            if file_part:
                linked = (repo / name).parent / file_part
                if not linked.exists():
                    errors.append(f'{name}: broken link {target}')
                    continue
                if anchor and linked.suffix == '.md' and anchor not in anchors(linked.read_text(encoding='utf-8')):
                    errors.append(f'{name}: broken anchor {target}')
            elif anchor not in own:
                errors.append(f'{name}: broken anchor {target}')
    notes = texts.get('docs/release-notes/v1.0.3.md', '')
    sections = re.findall(r'^### (.+)$', notes, re.M)
    if not sections or sections[0] != 'New name and title id':
        errors.append(f'docs/release-notes/v1.0.3.md: the first section must be "New name and title id", not {sections[:1]}')
    sections = re.findall(r'^### (.+)$', texts.get('docs/release-notes/v1.0.6.md', ''), re.M)
    if not sections or sections[0] != 'New name: TrinityX360':
        errors.append(f'docs/release-notes/v1.0.6.md: the first section must be "New name: TrinityX360", not {sections[:1]}')
    for name, size in IMAGES.items():
        path = repo / name
        if not path.is_file():
            errors.append(f'{name}: missing')
        elif png_size(path) != size:
            errors.append(f'{name}: {png_size(path)}, expected {size}')
    return errors


def self_test(repo):
    """Each alteration of good docs must be caught."""
    alterations = [
        ('README.md', '**PS5X360Reloaded v1.0.3**', '**PS5X360 v1.0.3**'),
        ('README.md', '# TrinityX360\n', '# PS5X360Reloaded\n'),
        ('README.md', '`<drive>/PS5X360Reloaded/saves`. Do not', '`<drive>/TrinityX360/saves`. Do not'),
        ('README.md', 'by BrinooTk and on [Xenia Canary](https://github.com/xenia-canary/xenia-canary)', 'by BrinooTk'),
        ('docs/release-notes/v1.0.6.md', '### New name: TrinityX360', '### Renamed'),
        ('docs/release-notes/v1.0.6.md', 'the title id stays `PPSA83600`', 'the title id is new'),
        ('docs/FORWARDER.md', 'starts TrinityX360 (`PPSA83600`)', 'starts PS5X360Reloaded (`PPSA83600`)'),
        ('README.md', '`/data/homebrew/PPSA83600/eboot.bin`', '`/data/homebrew/PPSA50011/eboot.bin`'),
        ('README.md', 'Based on [PS5X360](https://github.com/BrinooTk/PS5X360) by BrinooTk', 'Based on PS5X360'),
        ('README.md', '(#next-to-the-official-ps5x360)', '(#next-to-the-official-app)'),
        ('docs/release-notes/v1.0.3.md', '### New name and title id', '### Renamed'),
        ('docs/release-notes/v1.0.3.md', '`/mnt/sandbox/PPSA83600_000/download0/xbox360ps5/`', '`/download0`'),
        ('docs/FORWARDER.md', '"target": "PPSA83600",', '"target": "PPSA50011",'),
        ('docs/SOURCE-1.0.2.md', '(`TrinityX360 vX.Y.Z` on the home screen', '(`PS5X360Reloaded vX.Y.Z` on the home screen'),
    ]
    for name, old, new in alterations:
        text = (repo / name).read_text(encoding='utf-8')
        assert old in text, (name, old)
        if not check(repo, {name: text.replace(old, new)}):
            raise AssertionError(f'self-test: {name}: {old!r} -> {new!r} was not caught')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    errors = check(args.repo)
    if errors:
        for error in errors:
            print('FAIL', error)
        sys.exit(1)
    self_test(args.repo)
    print('PASS: README, the v1.0.3 and v1.0.6 notes, FORWARDER and SOURCE-1.0.2 describe TrinityX360 '
          '(PPSA83600, PS5X360Reloaded until v1.0.5) next to the official PS5X360 (PPSA50011), with the '
          'upstream credits kept')


if __name__ == '__main__':
    main()
