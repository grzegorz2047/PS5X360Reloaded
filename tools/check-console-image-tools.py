"""Host check of the console test image tools:

- tools/stage-reloaded-candidate.py, with a fake base package, a fake CI artifact and a fake
  release-branch package.py (both param_json signatures);
- tools/upload-console-image.sh, with a fake curl on PATH that serves a folder as the console's FTP
  (uploads, a full pendrive giving curl error 55 with a partial file, DELE, RNFR/RNTO, listings).

    python3 -I tools/check-console-image-tools.py

Needs bash for the upload part (BASH names another one); nothing else outside the standard library.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STAGE = REPO / 'tools/stage-reloaded-candidate.py'
UPLOAD = REPO / 'tools/upload-console-image.sh'
LABEL = '1.0.5-work-fix99'
failures = []


def check(condition, what):
    print(('ok   ' if condition else 'FAIL ') + what)
    if not condition:
        failures.append(what)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


# ---------------------------------------------------------------- staging

PACKAGE_TWO_ARGS = '''
import json, re
TITLE_ID = 'PPSA83600'
OFFICIAL_LABEL_RE = re.compile(rb'PS5X360 v[0-9]')
DOC_RENAMES = [('dlc/README.md', 1)]
DOCS_LEFT_OUT = ['AUTOLOG.md']
def param_json(base, version):
    p = json.loads(base); p['titleId'] = TITLE_ID; p['contentVersion'] = version
    return json.dumps(p).encode()
def art_files():
    return {'sce_sys/icon0.png': b'NEW ICON'}
'''
PACKAGE_ONE_ARG = PACKAGE_TWO_ARGS.replace(
    'def param_json(base, version):\n    p = json.loads(base); p[\'titleId\'] = TITLE_ID; p[\'contentVersion\'] = version',
    'def param_json(base):\n    p = json.loads(base); p[\'titleId\'] = TITLE_ID; p[\'contentVersion\'] = \'one-arg\'')


# package.py since the rename to TrinityX360 (1.0.6): its own home-screen label, and PS5X360Reloaded's
# refused like the official one.
PACKAGE_TRINITY = PACKAGE_TWO_ARGS.replace(
    "OFFICIAL_LABEL_RE = re.compile(rb'PS5X360 v[0-9]')",
    "HOME_LABEL = b'TrinityX360 v'\nOFFICIAL_LABEL_RE = re.compile(rb'PS5X360(?:Reloaded)? v[0-9]')")
assert PACKAGE_TRINITY != PACKAGE_TWO_ARGS
# package.py with the home screen's pictures: OFFICIAL_LOGOS names the official orb's SHA-256; one
# that replaces it in art_files() stages, one that does not is refused.
_ORB = __import__('hashlib').sha256(b'OFFICIAL X ORB').hexdigest()
PACKAGE_SKIN = PACKAGE_TRINITY.replace(
    "    return {'sce_sys/icon0.png': b'NEW ICON'}",
    "    return {'sce_sys/icon0.png': b'NEW ICON', 'assets/skin/ball.png': b'TRINITY ORB'}") + \
    f"OFFICIAL_LOGOS = {{'{_ORB}': 'assets/skin/ball.png'}}\n"
PACKAGE_SKIN_MISSING = PACKAGE_TRINITY + f"OFFICIAL_LOGOS = {{'{_ORB}': 'assets/skin/ball.png'}}\n"
assert 'TRINITY ORB' in PACKAGE_SKIN


def make_stage_inputs(root, *, label=LABEL, langs=16, official_label=False, dlc_mentions=1,
                      package=PACKAGE_TWO_ARGS, name='PS5X360Reloaded', extra_elf=b''):
    base = root / 'base/PPSA50011'
    write(base / 'eboot.bin', b'OFFICIAL EBOOT' * 100)
    write(base / 'sce_sys/param.json', json.dumps({'titleId': 'PPSA50011', 'contentVersion': '01.000.000'}))
    write(base / 'sce_sys/icon0.png', b'OLD ICON')
    write(base / 'assets/lang/en.txt', 'Configuracoes = Settings\n')
    write(base / 'dlc/README.md', 'Put DLC in PPSA50011 here. ' * dlc_mentions if dlc_mentions else 'no id\n')
    write(base / 'AUTOLOG.md', 'official autolog guide\n')
    write(base / 'assets/skin/ball.png', b'OFFICIAL X ORB')
    write(base / 'assets/presets.txt', '[4E4D0859]\nreadback=2\n')
    write(base / 'assets/config/4E4D0859.config.toml', 'official config\n')
    write(base / 'assets/config/00000001.config.toml', 'base-only config\n')
    write(base / 'readback-auto.off', '1\n')
    art = root / 'art'
    elf = b'\x7fELF....' + f'{name} v{label}'.encode() + b'\0' + extra_elf
    if official_label:
        elf += b'PS5X360 v1.0.2\0'
    write(art / 'src102-out/eboot.elf', elf)
    write(art / 'src102-out/eboot.bin', b'SOURCE EBOOT' * 100)
    for i in range(langs):
        write(art / f'src102-out/lang/l{i:02d}.txt', f'Settings = S{i}\n')
    pkg = root / 'pkgrepo'
    if package is not None:
        write(pkg / 'tools/source-1.0.2/package.py', package)
    else:
        pkg.mkdir(parents=True, exist_ok=True)
    return base, art, pkg


def run_stage(root, base, art, pkg, label=LABEL, extra=()):
    out = root / 'stage'
    result = subprocess.run([sys.executable, '-I', str(STAGE), '--base', str(base), '--artifact', str(art),
                             '--label', label, '--package-repo', str(pkg), '--out', str(out), *extra],
                            capture_output=True, text=True)
    return result, out / 'PPSA83600'


def stage_checks():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base, art, pkg = make_stage_inputs(root)
        before = {p: p.read_bytes() for p in base.rglob('*') if p.is_file()}
        result, app = run_stage(root, base, art, pkg)
        check(result.returncode == 0, f'stage: a good artifact stages ({result.stderr.strip()[:200]})')
        if result.returncode == 0:
            check((app / 'eboot.bin').read_bytes() == (art / 'src102-out/eboot.bin').read_bytes(),
                  'stage: eboot.bin is the artifact\'s')
            check(len(list((app / 'assets/lang').glob('l*.txt'))) == 16, 'stage: the 16 language files are copied')
            own = sorted((REPO / 'assets/patches').glob('*.patch.toml'))
            check(own and all((app / 'assets/patches' / f.name).read_bytes() == f.read_bytes() for f in own),
                  "stage: the repository's own game patches are copied into assets/patches (fix 155c)")
            icons = [f for skin in ('skin', 'skin-dark') for f in sorted((REPO / 'assets' / skin).glob('*.png'))]
            check(len(icons) == 12 and all((app / f.relative_to(REPO)).read_bytes() == f.read_bytes() for f in icons),
                  "stage: the repository's home screen icons are copied into assets/skin and assets/skin-dark (fix 164)")
            check((app / 'assets/presets.txt').read_bytes() == (REPO / 'assets/presets.txt').read_bytes(),
                  "stage: assets/presets.txt is the repository's, not the base's (as package.py ships it)")
            configs = sorted((REPO / 'assets/config').glob('*.toml'))
            check(configs and all((app / 'assets/config' / f.name).is_file()
                                  and (app / 'assets/config' / f.name).read_bytes() == f.read_bytes() for f in configs),
                  "stage: the repository's assets/config/*.toml replace the base's files of the same name")
            check((app / 'assets/config/00000001.config.toml').read_text() == 'base-only config\n',
                  "stage: a base config the repository does not have stays")
            check(not (app / 'readback-auto.off').exists(), 'stage: readback-auto.off is dropped, as in the release packages')
            param = json.loads((app / 'sce_sys/param.json').read_text())
            check(param == {'titleId': 'PPSA83600', 'contentVersion': '1.0.5'},
                  f'stage: param.json from package.py with the label\'s version ({param})')
            check((app / 'sce_sys/icon0.png').read_bytes() == b'NEW ICON', 'stage: art from package.py')
            check(b'PPSA83600' in (app / 'dlc/README.md').read_bytes()
                  and b'PPSA50011' not in (app / 'dlc/README.md').read_bytes(), 'stage: doc renamed to PPSA83600')
            check(not (app / 'AUTOLOG.md').exists(), 'stage: docs left out are removed')
        check(before == {p: p.read_bytes() for p in base.rglob('*') if p.is_file()}, 'stage: the base is untouched')

        result, app = run_stage(root, base, art, pkg, extra=('--content-version', '1.0.4.2'))
        check(result.returncode == 0 and json.loads((app / 'sce_sys/param.json').read_text())['contentVersion'] == '1.0.4.2',
              'stage: --content-version overrides the label\'s version')

        result, _ = run_stage(root, base, art, pkg, label='1.0.5-work-fix98')
        check(result.returncode != 0 and 'does not carry' in result.stderr, 'stage: a label the ELF lacks is refused')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        result, _ = run_stage(root, *make_stage_inputs(root, label='nightly-7'), label='nightly-7')
        check(result.returncode != 0 and 'cannot read a version' in result.stderr,
              'stage: a label without a version and no --content-version is refused')

    cases = [
        (dict(official_label=True), 'official', 'stage: an ELF with the official label is refused'),
        (dict(langs=15), 'language files', 'stage: an artifact without 16 language files is refused'),
        (dict(dlc_mentions=2), 'names PPSA50011', 'stage: a doc naming the official id an unexpected number of times is refused'),
        (dict(package=None), 'package.py not found', 'stage: a package repo without package.py is refused'),
    ]
    for kwargs, text, what in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result, _ = run_stage(root, *make_stage_inputs(root, **kwargs))
            check(result.returncode != 0 and text in result.stderr, f'{what} ({result.stderr.strip()[:120]})')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        result, app = run_stage(root, *make_stage_inputs(root, package=PACKAGE_ONE_ARG))
        check(result.returncode == 0 and json.loads((app / 'sce_sys/param.json').read_text())['contentVersion'] == 'one-arg',
              'stage: an older package.py with param_json(base) works')

    # The rename to TrinityX360: the package repo's package.py says which label the ELF must carry.
    renamed = [
        (dict(package=PACKAGE_TRINITY, name='TrinityX360'), None,
         'stage: a TrinityX360 build with a TrinityX360 package.py stages'),
        (dict(package=PACKAGE_TRINITY), 'does not carry',
         'stage: a PS5X360Reloaded build with a TrinityX360 package.py is refused'),
        (dict(name='TrinityX360'), 'does not carry',
         'stage: a TrinityX360 build with an older package.py is refused'),
        (dict(package=PACKAGE_TRINITY, name='TrinityX360', extra_elf=b'PS5X360Reloaded v1.0.5\0'), 'older name',
         'stage: a TrinityX360 build that also carries PS5X360Reloaded\'s label is refused'),
        (dict(package=PACKAGE_SKIN, name='TrinityX360'), None,
         'stage: the official logo pictures replaced by package.py\'s art stage'),
        (dict(package=PACKAGE_SKIN_MISSING, name='TrinityX360'), 'official PS5X360 logo',
         'stage: a stage that keeps an official logo picture is refused'),
    ]
    for kwargs, text, what in renamed:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result, app = run_stage(root, *make_stage_inputs(root, **kwargs))
            if text is None:
                check(result.returncode == 0 and (app / 'eboot.bin').is_file(), f'{what} ({result.stderr.strip()[:120]})')
            else:
                check(result.returncode != 0 and text in result.stderr, f'{what} ({result.stderr.strip()[:120]})')


# ---------------------------------------------------------------- upload

FAKE_CURL = r'''
import os, sys
from pathlib import Path
from urllib.parse import urlparse
root = Path(os.environ['FAKE_FTP_ROOT'])
with open(os.environ['FAKE_FTP_LOG'], 'a') as log:
    log.write(' '.join(sys.argv[1:]) + '\n')
if os.environ.get('FAKE_FTP_DOWN'):
    sys.exit(7)
args, i = sys.argv[1:], 0
upload, quote, list_only, url = None, [], False, None
while i < len(args):
    a = args[i]
    if a in ('-m', '--max-time'): i += 1
    elif a == '-T': upload = args[i + 1]; i += 1
    elif a == '-Q': quote.append(args[i + 1]); i += 1
    elif a == '--list-only': list_only = True
    elif a.startswith('ftp://'): url = a
    i += 1
local = lambda path: root / path.lstrip('/')
rnfr = None
for q in quote:
    verb, _, path = q.partition(' ')
    if verb == 'DELE':
        if not local(path).is_file(): sys.exit(21)
        local(path).unlink()
    elif verb == 'RNFR':
        if not local(path).exists(): sys.exit(21)
        rnfr = local(path)
    elif verb == 'RNTO':
        rnfr.rename(local(path))
target = local(urlparse(url).path)
if upload:
    data = Path(upload).read_bytes()
    limit = int(os.environ.get('FAKE_FTP_LIMIT', '-1'))
    target.parent.mkdir(parents=True, exist_ok=True)
    if 0 <= limit < len(data):
        target.write_bytes(data[:limit])
        sys.stderr.write('curl: (55) Connection was aborted\n')
        sys.exit(55)
    target.write_bytes(data)
elif urlparse(url).path.endswith('/'):
    if not target.is_dir(): sys.exit(9)
    for p in sorted(target.iterdir()):
        if list_only: print(p.name)
        else: print(f'-rw-r--r-- 1 0 0 {p.stat().st_size} Jan 1 00:00 {p.name}')
else:
    if not target.is_file(): sys.exit(78)
    sys.stdout.buffer.write(target.read_bytes())
'''


def upload_setup(root):
    bin_dir = root / 'bin'
    write(bin_dir / 'fake_curl.py', FAKE_CURL)
    write(bin_dir / 'curl', f'#!/bin/sh\nexec "{Path(sys.executable).as_posix()}" "{(bin_dir / "fake_curl.py").as_posix()}" "$@"\n')
    os.chmod(bin_dir / 'curl', 0o755)
    ftp = root / 'ftp'
    (ftp / 'mnt/usbX').mkdir(parents=True)
    (ftp / 'mnt/shadowmnt').mkdir(parents=True)
    write(root / 'img.ffpkg', os.urandom(5000))
    return bin_dir, ftp


def run_upload(root, *args, **env):
    bin_dir = root / 'bin'
    environment = dict(os.environ, CONSOLE_IP='console.invalid', FTP_PORT='2', IMAGE_DRIVE='/mnt/usbX',
                       FAKE_FTP_ROOT=str(root / 'ftp'), FAKE_FTP_LOG=str(root / 'curl.log'))
    environment['PATH'] = str(bin_dir) + os.pathsep + environment['PATH']
    environment.update(env)
    bash = os.environ.get('BASH', 'bash')
    # bash's own PATH lookup must find the fake curl first: prepend in POSIX form inside the shell
    command = f'export PATH="{bin_dir.as_posix()}:$PATH"; exec bash "{UPLOAD.as_posix()}" "$@"'
    return subprocess.run([bash, '-c', command, 'upload', *args], capture_output=True, text=True, env=environment)


def uploads_logged(root):
    log = root / 'curl.log'
    return sum(' -T ' in f' {line}' for line in log.read_text().splitlines()) if log.exists() else 0


def upload_checks():
    if shutil.which(os.environ.get('BASH', 'bash')) is None:
        check(False, 'upload: bash not found (set BASH)')
        return
    image = lambda root: str(root / 'img.ffpkg')
    pen = lambda ftp: ftp / 'mnt/usbX'

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); upload_setup(root)
        r = run_upload(root, image(root), 'L1')
        check(r.returncode != 0 and '--app-closed' in r.stderr and uploads_logged(root) == 0,
              'upload: refused without --app-closed')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'L1', FAKE_FTP_DOWN='1')
        check(r.returncode != 0 and 'no FTP' in r.stderr, 'upload: refused when the FTP does not answer')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'bad label!')
        check(r.returncode != 0 and uploads_logged(root) == 0, 'upload: a label with odd characters is refused')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'L1')
        side = pen(ftp) / 'PPSA83600.ffpkg.vL1'
        check(r.returncode == 0 and 'OK ' in r.stdout and side.read_bytes() == (root / 'img.ffpkg').read_bytes(),
              f'upload: uploaded as a side file and verified ({r.stderr.strip()[:160]})')
        check((root / 'img.ffpkg').exists(), 'upload: the local image is kept')
        r = run_upload(root, '--app-closed', image(root), 'L1')
        check(r.returncode != 0 and 'already exists' in r.stderr and uploads_logged(root) == 1,
              'upload: an existing side file is not overwritten')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'L2', FAKE_FTP_LIMIT='1000')
        check(r.returncode != 0 and not (pen(ftp) / 'PPSA83600.ffpkg.vL2').exists() and 'full' in r.stderr,
              'upload: curl 55 (pendrive full) deletes the partial file')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(pen(ftp) / 'PPSA83600.ffpkg', b'OLD ACTIVE')
        write(ftp / 'mnt/shadowmnt/PPSA83600', b'')
        r = run_upload(root, '--app-closed', image(root), 'L3', '--activate', 'L0')
        check(r.returncode != 0 and 'mounted' in r.stderr and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == b'OLD ACTIVE'
              and (pen(ftp) / 'PPSA83600.ffpkg.vL3').exists(), 'upload: no swap while an image is mounted')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(pen(ftp) / 'PPSA83600.ffpkg', b'OLD ACTIVE')
        shutil.rmtree(ftp / 'mnt/shadowmnt')
        r = run_upload(root, '--app-closed', image(root), 'L4', '--activate', 'L0')
        check(r.returncode != 0 and 'cannot list' in r.stderr and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == b'OLD ACTIVE',
              'upload: no swap when the mount folder cannot be listed')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(pen(ftp) / 'PPSA83600.ffpkg', b'OLD ACTIVE')
        r = run_upload(root, '--app-closed', image(root), 'L5', '--activate', 'L0')
        check(r.returncode == 0 and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == (root / 'img.ffpkg').read_bytes()
              and (pen(ftp) / 'PPSA83600.ffpkg.vL0').read_bytes() == b'OLD ACTIVE'
              and not (pen(ftp) / 'PPSA83600.ffpkg.vL5').exists(),
              f'upload: --activate swaps when nothing is mounted ({r.stderr.strip()[:160]})')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(pen(ftp) / 'PPSA83600.ffpkg', b'OLD ACTIVE')
        write(pen(ftp) / 'PPSA83600.ffpkg.vL0', b'SOMETHING ELSE')
        r = run_upload(root, '--app-closed', image(root), 'L6', '--activate', 'L0')
        check(r.returncode != 0 and (pen(ftp) / 'PPSA83600.ffpkg.vL0').read_bytes() == b'SOMETHING ELSE'
              and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == b'OLD ACTIVE',
              'upload: --activate never overwrites an existing side file with the old active image')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'L7', '--activate', 'L0')
        check(r.returncode == 0 and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == (root / 'img.ffpkg').read_bytes(),
              'upload: --activate with no active image yet makes this one active')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        r = run_upload(root, '--app-closed', image(root), 'L8', IMAGE_DRIVE='', PENDRIVE='/mnt/usbX')
        check(r.returncode == 0 and (pen(ftp) / 'PPSA83600.ffpkg.vL8').exists(), 'upload: PENDRIVE still works as the old name')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(pen(ftp) / 'PPSA83600.ffpkg', b'OLD ACTIVE')
        r = run_upload(root, '--app-closed', image(root), 'L9', '--activate', 'L0', SIDE_DIR='/mnt/usbX/ps5x360-images')
        sub = pen(ftp) / 'ps5x360-images'
        check(r.returncode == 0 and (pen(ftp) / 'PPSA83600.ffpkg').read_bytes() == (root / 'img.ffpkg').read_bytes()
              and (sub / 'PPSA83600.ffpkg.vL0').read_bytes() == b'OLD ACTIVE' and not (sub / 'PPSA83600.ffpkg.vL9').exists(),
              f'upload: side images in a subfolder, the active one at the drive root ({r.stderr.strip()[:160]})')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        write(ftp / 'mnt/ext1/PPSA83600.ffpkg', b'OTHER DRIVE ACTIVE')
        r = run_upload(root, '--app-closed', image(root), 'L10', '--activate', 'L0')
        check(r.returncode != 0 and 'exists too' in r.stderr and not (pen(ftp) / 'PPSA83600.ffpkg').exists(),
              'upload: no swap while another drive holds an active image (ShadowMount would ignore the duplicate)')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); _, ftp = upload_setup(root)
        ext = ftp / 'mnt/ext1'
        ext.mkdir(parents=True)
        write(ftp / 'data/shadowmount/manual.lst', '/mnt/ext0/Other.ffpkg\n')
        r = run_upload(root, '--app-closed', image(root), 'L11', '--activate', 'L0', IMAGE_DRIVE='/mnt/ext1')
        check(r.returncode != 0 and 'manual.lst' in r.stderr and not (ext / 'PPSA83600.ffpkg').exists(),
              'upload: no swap on extended storage the image is not listed for in manual.lst')
        write(ftp / 'data/shadowmount/manual.lst', '/mnt/ext0/Other.ffpkg\r\n/mnt/ext1/PPSA83600.ffpkg\r\n')
        r = run_upload(root, '--app-closed', image(root), 'L12', '--activate', 'L0', IMAGE_DRIVE='/mnt/ext1')
        check(r.returncode == 0 and (ext / 'PPSA83600.ffpkg').read_bytes() == (root / 'img.ffpkg').read_bytes()
              and 'mount_img.lnk' in r.stdout, f'upload: swap on extended storage listed in manual.lst ({r.stderr.strip()[:160]})')


def main():
    stage_checks()
    upload_checks()
    if failures:
        print(f'{len(failures)} check(s) failed')
        sys.exit(1)
    print('all console image tool checks passed')


if __name__ == '__main__':
    main()
