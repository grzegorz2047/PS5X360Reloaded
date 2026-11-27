"""Stages a PPSA83600 console test package (TrinityX360, PS5X360Reloaded until 1.0.5) from a
build-source-1.0.2.yml artifact.

    python -I tools/stage-reloaded-candidate.py --base <work-dir>/base/PPSA50011 \
        --artifact <work-dir>/art-<label> --label <label> \
        --package-repo <checkout of a release/source-* branch> --out <work-dir>/stage-<label>

--base      the unpacked official v1.0.2 package folder (PPSA50011), the user's own copy; never
            modified, never committed. A base that already has the English-keyed language files
            works the same (they are replaced anyway).
--artifact  the downloaded artifact folder (contains src102-out/eboot.bin, eboot.elf, lang/*.txt)
--label     the label BUILD_VERSION had (1.0.5-work-fix91e, 1.0.5_candidate_62, ...); the ELF must
            carry the home-screen label of --package-repo's package.py (HOME_LABEL: "TrinityX360
            v<label>" since the rename, "PS5X360Reloaded v<label>" for a package.py without it), and
            no older name's label (its OFFICIAL_LABEL_RE)
--package-repo  a checkout with tools/source-1.0.2/package.py (the release branches have it);
            it supplies PPSA83600's param.json, art and the doc renames
--out       an empty or disposable folder; <out>/PPSA83600 is written

Then build the image (volume label and file name PPSA83600):
    DOTNET_ROLL_FORWARD=Major dotnet <UFS2Tool>/.build/UFS2Tool.dll newfs -O 2 -b 65536 -f 8192 \
        -D <out>/PPSA83600 <work-dir>/PPSA83600-<label>.ffpkg PPSA83600
"""
import argparse
import inspect
import re
import shutil
import sys
from pathlib import Path

OFFICIAL, TITLE_ID = 'PPSA50011', 'PPSA83600'
LANG_FILES = 16


def version_of(label):
    """The release version a label belongs to: 1.0.5 for 1.0.5-work-fix91e or 1.0.5_candidate_62."""
    found = re.match(r'\d+\.\d+\.\d+(?:\.\d+)?', label)
    if not found:
        sys.exit(f'cannot read a version from label {label!r}; pass --content-version X.Y.Z')
    return found.group(0)


def folder_size(folder):
    files = [f for f in folder.rglob('*') if f.is_file()]
    return sum(f.stat().st_size for f in files), len(files)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--base', type=Path, required=True)
    p.add_argument('--artifact', type=Path, required=True)
    p.add_argument('--label', required=True)
    p.add_argument('--package-repo', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--content-version', help='X.Y.Z[.W] for param.json (default: from --label); '
                   'must be above every shipped version, e.g. 1.0.4.2 for a 1.0.4.1 hotfix test')
    a = p.parse_args()

    package_dir = (a.package_repo / 'tools/source-1.0.2').resolve()
    if not (package_dir / 'package.py').is_file():
        sys.exit(f'{package_dir}/package.py not found: use a checkout of a release/source-* branch')
    sys.path.insert(0, str(package_dir))
    import package  # noqa: E402
    if package.TITLE_ID != TITLE_ID:
        sys.exit(f'package.py makes {package.TITLE_ID}, expected {TITLE_ID}')

    out_dir = a.artifact / 'src102-out'
    elf = (out_dir / 'eboot.elf').read_bytes()
    # The package's own label: a build with the rename to TrinityX360 goes with a package.py that
    # names it (HOME_LABEL), an older build with an older package.py.
    want = getattr(package, 'HOME_LABEL', b'PS5X360Reloaded v') + a.label.encode() + b'\0'
    if want not in elf:
        sys.exit(f'eboot.elf does not carry {want!r}: wrong artifact, wrong label, or a package repo '
                 'of the other name (TrinityX360 since 1.0.6, PS5X360Reloaded before)')
    found = package.OFFICIAL_LABEL_RE.search(elf)
    if found:
        sys.exit(f'eboot.elf carries an older name\'s label ({elf[found.start():found.end() + 8]!r}): '
                 'the official "PS5X360 v<digit>", or PS5X360Reloaded\'s for a TrinityX360 package')
    langs = sorted((out_dir / 'lang').glob('*.txt'))
    if len(langs) != LANG_FILES:
        sys.exit(f'{len(langs)} language files in the artifact, expected {LANG_FILES}')

    free = shutil.disk_usage(a.out.parent if a.out.parent.exists() else Path('.')).free
    base_size, base_count = folder_size(a.base)
    if free < 2 * base_size:
        sys.exit(f'only {free >> 20} MiB free next to {a.out}; need about {2 * base_size >> 20} MiB '
                 '(stage + image). Free space first: a full disk once made a broken image.')

    app = a.out / TITLE_ID
    if a.out.exists():
        shutil.rmtree(a.out)
    shutil.copytree(a.base, app)
    shutil.copy2(out_dir / 'eboot.bin', app / 'eboot.bin')
    # Fix 186d: the optional, user-loaded LGPL Lapy one-shot helper, bundled at /app0/lapy.elf.
    lapy = out_dir / 'lapy.elf'
    if lapy.exists():
        shutil.copy2(lapy, app / 'lapy.elf')
    for f in langs:
        shutil.copy2(f, app / 'assets/lang' / f.name)
    # Fix 155c: this repository's own game patches (assets/patches, e.g. GTA V's audio fix) as the
    # release packages ship them (package-release.py); the base's community patches stay.
    own_patches = sorted((Path(__file__).resolve().parents[1] / 'assets/patches').glob('*.patch.toml'))
    (app / 'assets/patches').mkdir(parents=True, exist_ok=True)
    for f in own_patches:
        shutil.copy2(f, app / 'assets/patches' / f.name)
    print(f'{len(own_patches)} game patch file(s) from the repository copied into assets/patches')
    # The repository's per-game presets and core configs, as package.py and package-release.py
    # ship them: assets/presets.txt replaces the base's (the official 1.0.2 one, which had e.g.
    # full readback for Tekken Tag Tournament 2 where the repository has fast readback), and each
    # assets/config/*.toml replaces the base's file of the same name. readback-auto.off goes, as
    # in the release packages (it would switch off the copy-back of small resolves).
    repo_assets = Path(__file__).resolve().parents[1] / 'assets'
    presets = repo_assets / 'presets.txt'
    if not presets.is_file():
        sys.exit(f'{presets} not found: run the copy of this script in the tested branch\'s checkout')
    shutil.copy2(presets, app / 'assets/presets.txt')
    own_configs = sorted((repo_assets / 'config').glob('*.toml'))
    (app / 'assets/config').mkdir(parents=True, exist_ok=True)
    for f in own_configs:
        shutil.copy2(f, app / 'assets/config' / f.name)
    (app / 'readback-auto.off').unlink(missing_ok=True)
    print(f'assets/presets.txt and {len(own_configs)} core config file(s) from the repository '
          'copied into assets')
    # Fix 164: this repository's home screen icons (assets/skin and assets/skin-dark, drawn by
    # tools/make-skin-icons.py) over the base's pictures of the same names.
    own_skin = 0
    for skin in ('skin', 'skin-dark'):
        for f in sorted((Path(__file__).resolve().parents[1] / 'assets' / skin).glob('*.png')):
            (app / 'assets' / skin).mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, app / 'assets' / skin / f.name)
            own_skin += 1
    print(f'{own_skin} home screen icon(s) from the repository copied into assets/skin and assets/skin-dark')
    param = app / 'sce_sys/param.json'
    if len(inspect.signature(package.param_json).parameters) >= 2:
        # Release-branch package.py: contentVersion from a release version X.Y.Z[.W].
        version = a.content_version or version_of(a.label)
        param.write_bytes(package.param_json(param.read_bytes(), version))
    else:
        param.write_bytes(package.param_json(param.read_bytes()))
    for target, data in package.art_files().items():
        (app / target).write_bytes(data)
    # No picture of the official package's PS5X360 logo stays in the image (package.py's
    # OFFICIAL_LOGOS; an older package.py without it checks nothing).
    logos = getattr(package, 'OFFICIAL_LOGOS', {})
    if logos:
        import hashlib
        left = sorted(str(f.relative_to(app)) for f in (app / 'assets').rglob('*')
                      if f.is_file() and hashlib.sha256(f.read_bytes()).hexdigest() in logos)
        if left:
            sys.exit(f'the stage still has the official PS5X360 logo pictures: {", ".join(left)}')
    for name, count in package.DOC_RENAMES:
        f = app / name
        data = f.read_bytes()
        if data.count(OFFICIAL.encode()) != count:
            sys.exit(f'{name} names {OFFICIAL} {data.count(OFFICIAL.encode())} times, not {count}')
        f.write_bytes(data.replace(OFFICIAL.encode(), TITLE_ID.encode()))
    for name in package.DOCS_LEFT_OUT:
        (app / name).unlink(missing_ok=True)

    size, count = folder_size(app)
    print(f'staged {app}: {size} bytes in {count} files (base {base_size} bytes in {base_count} files)')
    # The eboot differs and a few docs are left out; anything far off means a broken copy.
    if abs(size - base_size) > 64 << 20 or count < base_count - len(package.DOCS_LEFT_OUT) - 2:
        sys.exit('stage size or file count is far from the base: check the copy before building an image')


if __name__ == '__main__':
    main()
