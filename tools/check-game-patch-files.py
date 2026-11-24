"""Host check of the game patch files the package ships (assets/patches/*.patch.toml).

    python3 -I tools/check-game-patch-files.py [--official <folder of the official package's patches>]

Each file must be valid TOML (Xenia Canary reads them with toml++) and readable by the title's own
line-based reader (ReadPatchFile in src/game_patches.cpp): a title ID matching the file name, the
executable hashes, and patches with a name, an author, is_enabled, and writes that each have an
address and a value of a type the reader encodes.

A repository file replaces the official package's file of the same name (build_package.py), so
the GTA IV file must keep the official file's four community patches byte for byte in their writes.
With --official, every repository file that replaces an official one is compared to it: no patch
of the official file may be lost.

The GTA IV draw distance patches: off by default, one 4-byte write each to the game's global
level-of-detail multiplier (0x82a2bda4, 1.0 in the executable), 1.25 and 1.5, and the same address,
so the patch loader's conflict check lets only one of them apply.
"""
import argparse
import re
import struct
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TYPES = {'be8': 1, 'be16': 2, 'be32': 4, 'be64': 8, 'f32': 4, 'f64': 8, 'string': 0, 'u16string': 0, 'array': 0}

GTA4 = '545407F2 - Grand Theft Auto IV.patch.toml'
GTA4_HASH = 0x4CBE7EEB2455A159
GTA4_COMMUNITY = {  # name -> writes, from the official package's file
    'Unlock FPS': [(0x829a963c, 0x48000048)],
    'Skip Legal Screens': [(0x82123fbc, 0x39600001)],
    'Disable blur filter': [(0x822f8a08, 0x908be904), (0x822f8a4c, 0x908be904)],
    'Aspect Ratio': [(0x820ae698, 0x4018e393)],
}
GTA4_LOD_SCALE = 0x82a2bda4
GTA4_DRAW_DISTANCE = {'Draw Distance +25%': 1.25, 'Draw Distance +50%': 1.5}


def line_reader(text):
    """The structure ReadPatchFile sees: [[patch]] headers and, for each write header, whether an
    address and a value followed before the next header."""
    patches, problems, section, have = 0, [], None, set()

    def close():
        if section and section != 'patch' and have != {'address', 'value'}:
            problems.append(f'[[{section}]] entry without {sorted({"address", "value"} - have)}')

    for raw in text.splitlines():
        line = re.sub(r'#[^"\']*$', '', raw).strip()
        if line.startswith('[['):
            close()
            section, have = line[2:line.index(']]')].strip(), set()
            if section == 'patch':
                patches += 1
            elif not section.startswith('patch.'):
                problems.append(f'unknown section [[{section}]]')
            elif section[6:] not in TYPES:
                problems.append(f'write type {section[6:]} the reader does not encode')
            continue
        if '=' in line and section and section != 'patch':
            key = line.split('=', 1)[0].strip()
            if key in have:
                close()
                have = set()
            have.add(key)
    close()
    return patches, problems


def check_file(path):
    """Problems of one patch file (an empty list when it is good) and its parsed patches."""
    problems = []
    text = path.read_text(encoding='utf-8')
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as error:
        return [f'not valid TOML: {error}'], []
    title_id = data.get('title_id', '')
    if not re.fullmatch(r'[0-9A-Fa-f]{8}', title_id) or not path.name.upper().startswith(title_id.upper()):
        problems.append(f'title_id {title_id!r} does not match the file name')
    if not path.name.endswith('.patch.toml'):
        problems.append('the file name must end with .patch.toml')
    hashes = data.get('hash')
    hashes = hashes if isinstance(hashes, list) else [hashes]
    if not hashes or not all(isinstance(h, str) and re.fullmatch(r'[0-9A-Fa-f]{16}', h) for h in hashes):
        problems.append(f'hash {data.get("hash")!r} is not one or more 16-digit hex strings')
    if not isinstance(data.get('title_name'), str):
        problems.append('title_name missing')
    patches = data.get('patch', [])
    if not patches:
        problems.append('no [[patch]]')
    names = set()
    for patch in patches:
        name = patch.get('name')
        if not isinstance(name, str) or not name.strip():
            problems.append('a patch without a name')
            continue
        if name.strip().lower() in names:
            problems.append(f'{name}: name used twice (the menu keeps only the first)')
        names.add(name.strip().lower())
        if not isinstance(patch.get('author'), str):
            problems.append(f'{name}: author missing')
        if not isinstance(patch.get('is_enabled'), bool):
            problems.append(f'{name}: is_enabled must be true or false')
        writes = [(kind, entry) for kind, entries in patch.items() if kind in TYPES for entry in entries]
        if not writes:
            problems.append(f'{name}: no writes')
        for kind, entry in writes:
            address, value = entry.get('address'), entry.get('value')
            if not isinstance(address, int) or not 0x80000000 <= address <= 0xFFFFFFFF:
                problems.append(f'{name}: address {address!r} is not a guest address')
            size = TYPES[kind]
            if kind.startswith('be') and not (isinstance(value, int) and 0 <= value < 1 << (8 * size)):
                problems.append(f'{name}: {kind} value {value!r} does not fit')
            if kind.startswith('f') and not isinstance(value, (int, float)):
                problems.append(f'{name}: {kind} value {value!r} is not a number')
            if kind in ('string', 'u16string', 'array') and not isinstance(value, str):
                problems.append(f'{name}: {kind} value {value!r} is not a string')
        unknown = set(patch) - set(TYPES) - {'name', 'desc', 'author', 'is_enabled'}
        if unknown:
            problems.append(f'{name}: unknown keys {sorted(unknown)}')
    count, structure = line_reader(text)
    problems += structure
    if count != len(patches):
        problems.append(f'the line reader sees {count} patches, TOML {len(patches)}')
    return problems, patches


def be32_writes(patch):
    return [(entry['address'], entry['value']) for entry in patch.get('be32', [])]


def check_gta4(path):
    problems, patches = check_file(path)
    data = tomllib.loads(path.read_text(encoding='utf-8'))
    if GTA4_HASH not in [int(h, 16) for h in ([data['hash']] if isinstance(data['hash'], str) else data['hash'])]:
        problems.append('the base executable hash 4CBE7EEB2455A159 is not listed')
    by_name = {p['name']: p for p in patches}
    for name, writes in GTA4_COMMUNITY.items():
        if name not in by_name or be32_writes(by_name[name]) != writes:
            problems.append(f'community patch {name} lost or changed')
    for name, scale in GTA4_DRAW_DISTANCE.items():
        patch = by_name.get(name)
        if not patch:
            problems.append(f'{name} missing')
            continue
        writes = be32_writes(patch)
        if patch['is_enabled'] or set(patch) - {'name', 'desc', 'author', 'is_enabled', 'be32'}:
            problems.append(f'{name}: must be off by default with be32 writes only')
        if len(writes) != 1 or writes[0][0] != GTA4_LOD_SCALE:
            problems.append(f'{name}: must write the LOD multiplier at {GTA4_LOD_SCALE:#x} only')
            continue
        value = struct.unpack('>f', struct.pack('>I', writes[0][1]))[0]
        if value != scale:
            problems.append(f'{name}: writes {value}, expected {scale}')
        if 'vsync' in patch.get('desc', '').lower().replace('-', ''):
            problems.append(f'{name}: the description must not mention VSync (the loader acts on it)')
    extra = [p['name'] for p in patches if p['name'].lower().startswith('draw distance') and p['name'] not in GTA4_DRAW_DISTANCE]
    if extra:
        problems.append(f'unexpected draw distance levels {extra} (2x was dropped: -45% FPS)')
    others = [p for p in patches if p['name'] not in GTA4_DRAW_DISTANCE]
    if any(address == GTA4_LOD_SCALE for p in others for address, _ in be32_writes(p)):
        problems.append('another patch writes the LOD multiplier')
    return problems


def compare_official(folder):
    """Repository files that replace official ones must keep every official patch."""
    problems = []
    for path in sorted((ROOT / 'assets/patches').glob('*.patch.toml')):
        official = folder / path.name
        if not official.exists():
            continue
        ours = {p['name']: p for p in tomllib.loads(path.read_text(encoding='utf-8')).get('patch', [])}
        for patch in tomllib.loads(official.read_text(encoding='utf-8')).get('patch', []):
            mine = ours.get(patch['name'])
            strip = lambda p: {k: v for k, v in p.items() if k not in ('is_enabled', 'desc')}
            if mine is None or strip(mine) != strip(patch):
                problems.append(f'{path.name}: official patch {patch["name"]} lost or changed')
    return problems


def self_test():
    """Made-up files: what must pass and what must fail."""
    good = ('title_name = "T"\ntitle_id = "4D5308D6"\nhash = "15A2B07FFF0A2754"\n'
            '[[patch]]\n name = "A"\n author = "x"\n is_enabled = false\n'
            ' [[patch.be32]]\n  address = 0x82000000\n  value = 0x60000000 # nop\n')
    bad = {
        'missing value': good.replace('  value = 0x60000000 # nop\n', ''),
        'other title': good.replace('4D5308D6', '4D5308D7'),
        'short hash': good.replace('15A2B07FFF0A2754', '15A2'),
        'unknown type': good.replace('patch.be32', 'patch.be24'),
        'too big': good.replace('0x60000000', '0x160000000'),
        'not toml': good.replace('is_enabled = false', 'is_enabled = no'),
        'no author': good.replace(' author = "x"\n', ''),
        'twice': good + good[good.index('[[patch]]'):],
    }
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / '4D5308D6 - T.patch.toml'
        path.write_text(good, encoding='utf-8')
        assert check_file(path)[0] == [], check_file(path)[0]
        for what, text in bad.items():
            path.write_text(text, encoding='utf-8')
            assert check_file(path)[0], f'made-up file ({what}) passed'
    assert line_reader('[[patch]]\n[[patch.be32]]\naddress = 1\nvalue = 2\naddress = 3\n')[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--official', type=Path, help="the official package's assets/patches folder")
    args = parser.parse_args()
    self_test()
    problems = []
    files = sorted((ROOT / 'assets/patches').glob('*.patch.toml'))
    for path in files:
        found = check_gta4(path) if path.name == GTA4 else check_file(path)[0]
        problems += [f'{path.name}: {p}' for p in found]
    if not (ROOT / 'assets/patches' / GTA4).exists():
        problems.append(f'{GTA4} missing')
    if args.official:
        problems += compare_official(args.official)
    for problem in problems:
        print('FAIL', problem)
    if problems:
        sys.exit(1)
    print(f'PASS: {len(files)} patch files are valid TOML and readable by the title\'s reader; GTA IV keeps its '
          f'community patches and has two exclusive draw distance patches (1.25x, 1.5x)'
          + (' ; no official patch lost' if args.official else ''))


if __name__ == '__main__':
    main()
