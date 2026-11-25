"""Host check of the release images' title id: tools/binary-release/make_images.sh and the two
release workflows that call it.

    python3 -I tools/check-release-images.py [--bash <bash>]

TrinityX360's source releases (PS5X360Reloaded's until 1.0.5) are title id PPSA83600 (tools/source-1.0.2/package.py), next to
the official PS5X360's PPSA50011, which the old binary releases (release-binary.yml) keep. Checks:
  * release-source.yml makes the images from stage/PPSA83600 with the id PPSA83600, attaches
    PPSA83600.zip, .zip.sha256, .ffpkg and .ffpfsc, names its draft "TrinityX360 v<version>",
    attaches the scripts archive under package.py's name (TrinityX360-<version>-scripts.tar.gz),
    and still downloads the official v1.0.2 PPSA50011.zip with the pinned SHA-256 (package.py's);
    PPSA50011 appears nowhere else in it;
  * release-binary.yml still makes PPSA50011.ffpkg/.ffpfsc from stage/PPSA50011 (the default id);
  * the stage's sce_sys/param.json as package.py writes it for the images: titleId PPSA83600 and
    contentVersion from the release version (1.0.4 -> 01.004.000), above the official
    00.001.000 that v1.0.3 shipped; a base contentVersion that is not lower, or a version that
    does not fit, is refused;
  * make_images.sh, run with stand-ins for git, dotnet (UFS2Tool) and MkPFS's python: the id
    names both images and is the UFS2 volume label; the default is PPSA50011; an id that is not
    4 capital letters and 5 digits, or a stage whose sce_sys/param.json carries another titleId,
    stops it before any image is made (skipped when no bash is found; CI has one).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'tools/binary-release/make_images.sh'
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / 'tools/source-1.0.2'))
import package  # noqa: E402

ID = package.TITLE_ID
OFFICIAL = package.BASE_PREFIX.rstrip('/')
OFFICIAL_URL = 'https://github.com/BrinooTk/PS5X360/releases/download/v1.0.2/PPSA50011.zip'


def check_workflows():
    assert ID == 'PPSA83600' and OFFICIAL == 'PPSA50011', (ID, OFFICIAL)
    text = (REPO / '.github/workflows/release-source.yml').read_text(encoding='utf-8')
    for needle in (
            f'run: bash tools/binary-release/make_images.sh dist/source-release/stage/{ID} dist/source-release {ID}\n',
            f'cp "$out/{ID}.zip" "$out/{ID}.zip.sha256" "$out/{ID}.ffpkg" "$out/{ID}.ffpfsc" "$rel/"',
            f'- name: Build {ID}.zip from the official package and the source build',
            f'- name: Make {ID}.ffpkg and {ID}.ffpfsc',
            f'name: Package {ID} and the sources',
            'python3 -I tools/source-1.0.2/package.py',
            '--out-dir dist/source-release\n',
            'title="TrinityX360 v$VERSION"',
            # The scripts archive the NOTICE and RELEASE.json name (package.source_names).
            f'-o "$rel/{package.source_names("$VERSION")["scripts"]}" "$GITHUB_SHA"',
            '--prefix="TrinityX360-$VERSION/"',
            # The official base, unchanged: its name, URL and pinned hash.
            f'curl -fsSL --retry 5 --retry-all-errors -o "$RUNNER_TEMP/PPSA50011-v1.0.2.zip" \\\n'
            f'            {OFFICIAL_URL}\n',
            f'echo "{package.BASE_ZIP_SHA256}  $RUNNER_TEMP/PPSA50011-v1.0.2.zip" | sha256sum --check --strict',
            '--base-zip "$RUNNER_TEMP/PPSA50011-v1.0.2.zip"'):
        assert needle in text, needle
    assert 'title="PS5X360 v' not in text and 'title="PS5X360Reloaded' not in text and 'PS5X360Reloaded-' not in text
    # PPSA50011 only for the official base (its download, its file and the comments saying so).
    for number, line in enumerate(text.splitlines(), 1):
        if OFFICIAL not in line:
            continue
        allowed = (line.lstrip().startswith('#') or 'PPSA50011-v1.0.2.zip' in line or OFFICIAL_URL in line
                   or line.strip() == '- name: Download the official v1.0.2 PPSA50011.zip')
        assert allowed, f'release-source.yml:{number}: {line.strip()}'

    binary = (REPO / '.github/workflows/release-binary.yml').read_text(encoding='utf-8')
    assert ('run: bash tools/binary-release/make_images.sh dist/binary-release/stage/PPSA50011 '
            'dist/binary-release\n') in binary
    assert ID not in binary
    print('  release-source.yml: PPSA83600 stage, images, assets and title; official download unchanged')
    print('  release-binary.yml: PPSA50011 images (the default id)')


def find_bash(explicit):
    if explicit:
        return explicit
    candidates = [shutil.which('bash')]
    if os.name == 'nt':  # Git for Windows' bash, not WSL's C:\Windows\System32\bash.exe
        candidates = [r'C:\Program Files\Git\bin\bash.exe', r'C:\Program Files\Git\usr\bin\bash.exe'] + \
                     [c for c in candidates if c and 'system32' not in c.lower()]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return None


STUB_GIT = '''#!/usr/bin/env bash
# git -C <dir> rev-parse HEAD: the revision the fixture wrote; nothing else is expected.
if [[ $1 == -C && $3 == rev-parse && $4 == HEAD ]]; then cat "$2/.git/HEAD"; exit 0; fi
echo "unexpected git $*" >&2
exit 3
'''
# Both write the image (the argument after the stage folder) and log their arguments.
STUB_DOTNET = '''#!/usr/bin/env bash
printf 'dotnet' >> "$STUB_LOG"; printf ' %s' "$@" >> "$STUB_LOG"; echo >> "$STUB_LOG"
[[ $DOTNET_ROLL_FORWARD == Major ]] || { echo "no DOTNET_ROLL_FORWARD" >&2; exit 3; }
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  if [[ ${args[i]} == -D ]]; then echo ufs2 > "${args[i + 2]}"; exit 0; fi
done
exit 3
'''
STUB_PYTHON = '''#!/usr/bin/env bash
printf 'mkpfs' >> "$STUB_LOG"; printf ' %s' "$@" >> "$STUB_LOG"; echo >> "$STUB_LOG"
[[ $1 == -m && $2 == mkpfs && $3 == pack && $4 == folder ]] || exit 3
echo pfs > "$6"
'''


def write_stub(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode())
    path.chmod(0o755)


def posix(bash, path):
    """The path as bash sees it (Git for Windows: /c/...)."""
    if os.name != 'nt':
        return str(path)
    result = subprocess.run([bash, '-c', 'cygpath -u "$1"', 'bash', str(path)], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 and result.stdout.strip() else path.as_posix()


def check_script(bash):
    script = SCRIPT.read_text(encoding='utf-8')
    revisions = dict(re.findall(r'fetch "\$(ufs|mkpfs)" \S+ ([0-9a-f]{40})', script))
    assert set(revisions) == {'ufs', 'mkpfs'}, revisions
    assert 'id=${3:-PPSA50011}' in script and '"$out/$id.ffpkg" "$id"' in script
    if not bash:
        print('  SKIP make_images.sh run: no bash found (CI has one)')
        return
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch)
        cache, stubs, log = root / 'cache', root / 'bin', root / 'calls.log'
        for name, key, tool in (('UFS2Tool', 'ufs', '.build/UFS2Tool.dll'), ('MkPFS', 'mkpfs', '.venv/bin/python')):
            (cache / name / '.git').mkdir(parents=True)
            (cache / name / '.git/HEAD').write_text(revisions[key] + '\n', encoding='ascii', newline='\n')
            if name == 'UFS2Tool':
                (cache / name / tool).parent.mkdir(parents=True)
                (cache / name / tool).write_bytes(b'dll')
            else:
                write_stub(cache / name / tool, STUB_PYTHON)
        write_stub(stubs / 'git', STUB_GIT)
        write_stub(stubs / 'dotnet', STUB_DOTNET)
        env = dict(os.environ, PACKAGING_CACHE=posix(bash, cache), STUB_LOG=posix(bash, log))
        # The fields of the official param.json make_images.sh and package.py read (CRLF, as there).
        official_param = ('{\r\n  "titleId": "PPSA50011",\r\n  "conceptId": "50011",\r\n'
                          '  "contentId": "UP9000-PPSA50011_00-XBOX360PS5M80001",\r\n'
                          '  "contentVersion": "00.001.000",\r\n'
                          '  "localizedParameters": {\r\n    "defaultLanguage": "en-US",\r\n'
                          '    "en-US": {\r\n      "titleName": "PS5X360"\r\n    }\r\n  },\r\n'
                          '  "masterVersion": "00.01"\r\n}\r\n').encode()

        def stage(name, param):
            folder = root / 'stage' / name
            (folder / 'sce_sys').mkdir(parents=True, exist_ok=True)
            (folder / 'eboot.bin').write_bytes(b'eboot')
            target = folder / 'sce_sys/param.json'
            if param is None:
                target.unlink(missing_ok=True)
            else:
                target.write_bytes(param)
            return folder

        def run(folder, out, *extra):
            if log.exists():
                log.unlink()
            # The stand-ins first on PATH, set inside bash: Git for Windows' bash.exe puts its own
            # git first on a PATH it is given.
            result = subprocess.run([bash, '-c', 'PATH="$1:$PATH"; shift; exec bash "$@"', 'bash',
                                     posix(bash, stubs), posix(bash, SCRIPT), posix(bash, folder),
                                     posix(bash, out), *extra],
                                    env=env, capture_output=True, text=True, timeout=120)
            calls = log.read_text(encoding='utf-8').splitlines() if log.exists() else []
            return result, calls

        def made(out):
            return sorted(p.name for p in out.iterdir()) if out.exists() else []

        # The binary release: no id, PPSA50011.
        out = root / 'out-binary'
        result, calls = run(stage(OFFICIAL, official_param), out)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert made(out) == ['PPSA50011.ffpfsc', 'PPSA50011.ffpkg'], made(out)
        assert len(calls) == 2 and calls[0].endswith('/out-binary/PPSA50011.ffpkg PPSA50011'), calls
        assert ' newfs -O 2 -b 65536 -f 8192 -D ' in calls[0], calls
        assert calls[1].startswith('mkpfs -m mkpfs pack folder ') and \
            calls[1].endswith('/PPSA50011.ffpfsc --version PS5 --verify'), calls

        # The source release: PPSA83600, with param.json as package.py writes it (CRLF) for 1.0.6:
        # contentVersion 01.006.000, above the 00.001.000 of the official package and of v1.0.3, the 01.004.000 of v1.0.4
        # and the hotfixes' 01.004.001 (v1.0.4.1) and 01.004.002 (v1.0.4.2).
        reloaded = package.param_json(official_param, '1.0.6')
        assert b'"titleId": "PPSA83600"' in reloaded and b'\r\n' in reloaded
        assert b'"contentVersion": "01.006.000",\r\n' in reloaded and b'"masterVersion": "00.01"' in reloaded
        assert package.SHIPPED_CONTENT_VERSIONS['1.0.3'] == '00.001.000' < package.content_version('1.0.6')
        assert package.SHIPPED_CONTENT_VERSIONS['1.0.4'] == '01.004.000' < package.content_version('1.0.6')
        assert package.SHIPPED_CONTENT_VERSIONS['1.0.4.1'] == '01.004.001' < package.content_version('1.0.6')
        assert package.SHIPPED_CONTENT_VERSIONS['1.0.4.2'] == '01.004.002' < package.content_version('1.0.6')
        assert package.SHIPPED_CONTENT_VERSIONS['1.0.5'] == '01.005.000' < package.content_version('1.0.6')
        for base, version, message in (
                (official_param.replace(b'"00.001.000"', b'"01.006.000"'), '1.0.6',
                 'contentVersion 01.006.000 is not higher than 01.006.000 of the base package'),
                (official_param.replace(b'"00.001.000"', b'"02.000.000"'), '1.0.6',
                 'contentVersion 01.006.000 is not higher than 02.000.000 of the base package'),
                (official_param, '1.0.4', 'contentVersion 01.004.000 is not higher than 01.004.000 of v1.0.4'),
                (official_param, '1.0.100', 'does not fit contentVersion')):
            try:
                package.param_json(base, version)
            except SystemExit as error:
                assert message in str(error.code), (message, error.code)
            else:
                raise AssertionError(f'param.json accepted: {message}')
        out = root / 'out-source'
        result, calls = run(stage(ID, reloaded), out, ID)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert made(out) == [f'{ID}.ffpfsc', f'{ID}.ffpkg'], made(out)
        assert calls[0].endswith(f'/{ID}.ffpkg {ID}') and '/stage/PPSA83600 ' in calls[0], calls
        assert calls[1].endswith(f'/{ID}.ffpfsc --version PS5 --verify'), calls
        assert not any(OFFICIAL in call for call in calls), calls
        # A rerun replaces the images (rm -f first).
        result, _ = run(stage(ID, reloaded), out, ID)
        assert result.returncode == 0 and made(out) == [f'{ID}.ffpfsc', f'{ID}.ffpkg']

        # No param.json: the id alone decides.
        out = root / 'out-no-param'
        result, calls = run(stage('plain', None), out, ID)
        assert result.returncode == 0 and made(out) == [f'{ID}.ffpfsc', f'{ID}.ffpkg'], (result.stderr, made(out))

        # Refusals, before any tool runs or any image is made.
        for folder, extra, message in (
                # The official stage named PPSA83600, and a PPSA83600 stage without the id.
                (stage(ID, official_param), [ID], "has titleId 'PPSA50011', not PPSA83600"),
                (stage('moved', reloaded), [], "has titleId 'PPSA83600', not PPSA50011"),
                (stage('moved', reloaded), ['ppsa83600'], 'is not 4 capital letters and 5 digits'),
                (stage('moved', reloaded), ['PPSA8360'], 'is not 4 capital letters and 5 digits'),
                (stage('moved', reloaded), ['PPSA836000'], 'is not 4 capital letters and 5 digits'),
                (stage('moved', reloaded), ['PPSA83600 x'], 'is not 4 capital letters and 5 digits'),
                (stage('moved', reloaded), ['../x'], 'is not 4 capital letters and 5 digits')):
            out = root / 'out-refused'
            shutil.rmtree(out, ignore_errors=True)
            result, calls = run(folder, out, *extra)
            assert result.returncode != 0 and message in result.stderr, (extra, result.returncode, result.stderr)
            assert calls == [] and made(out) == [], (extra, calls, made(out))
    print('  make_images.sh: id names the images and the volume label, PPSA50011 by default, '
          'bad ids and a param.json of another title refused; contentVersion 01.006.000 for 1.0.6')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bash', help='bash to run make_images.sh with (default: found on PATH)')
    args = parser.parse_args()
    check_workflows()
    check_script(find_bash(args.bash))
    print('PASS: the release images carry the package\'s title id')


if __name__ == '__main__':
    main()
