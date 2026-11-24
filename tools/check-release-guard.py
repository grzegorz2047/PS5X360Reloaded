"""Host check of the source release workflow's decisions (tools/source-1.0.2/release_guard.py).

    python3 -I tools/check-release-guard.py

- the version comes only from a release/source-X.Y.Z branch: tags, other branches and labels
  that are not X.Y.Z (1.0.3_candidate_37, 1.0.3-rc1, empty) are refused;
- SOURCES.json must record that version and leave no fix out;
- SHA256SUMS.txt lists every asset once, sorted, never itself, with the right hashes, and verify
  catches a changed, missing, extra or duplicated entry; odd names and empty files are refused;
- an existing release is updated only while it is a draft: published or immutable releases, an
  unreadable gh answer, an answer for another tag and a tag already at another commit are refused;
  "release not found" means create;
- the command line gives exit 2 with the reason on a refusal.
"""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'tools/source-1.0.2/release_guard.py'
sys.dont_write_bytecode = True
sys.path.insert(0, str(SCRIPT.parent))
import release_guard as guard  # noqa: E402

SHA = 'f9140f1' + '0' * 33
OTHER = 'a' * 40


def refused(function, *args):
    try:
        function(*args)
    except guard.Refused:
        return True
    return False


def check_version():
    assert guard.version_from_ref('refs/heads/release/source-1.0.3') == '1.0.3'
    assert guard.version_from_ref('release/source-1.0.3') == '1.0.3'
    assert guard.version_from_ref('refs/heads/release/source-10.20.300') == '10.20.300'
    for ref in ('refs/tags/v1.0.3', 'refs/tags/release/source-1.0.3', 'refs/heads/main',
                'refs/heads/source-1.0.2-patches', 'refs/heads/release/source-1.0.3_candidate_37',
                'refs/heads/release/source-1.0.3-rc1', 'refs/heads/release/source-v1.0.3',
                'refs/heads/release/source-1.0', 'refs/heads/release/source-', 'refs/pull/5/merge',
                'refs/heads/release/source-1.0.3\n', 'refs/heads/release/source-1.0.3/x',
                'refs/heads/xrelease/source-1.0.3', ''):
        if ref == 'refs/heads/release/source-1.0.3\n':
            # A trailing newline from a shell is stripped, not taken as part of the version.
            assert guard.version_from_ref(ref) == '1.0.3'
            continue
        assert refused(guard.version_from_ref, ref), ref
    # A hotfix: X.Y.Z.W (1.0.4.1); W from 1 to 999 without a leading zero, nothing after it.
    assert guard.version_from_ref('refs/heads/release/source-1.0.4.1') == '1.0.4.1'
    assert guard.check_version('1.0.4.999') == '1.0.4.999'
    for bad in ('1.0.4.0', '1.0.4.01', '1.0.4.1000', '1.0.4.1.1', '1.0.4.', '1.0.4.x'):
        assert refused(guard.check_version, bad), bad
    assert refused(guard.check_version, '1.0.3 ')
    assert refused(guard.check_version, None)
    # re's $ matches before a trailing newline: a version, tag or sha with one is refused.
    for bad in ('1.0.3' + chr(10), chr(10) + '1.0.3', '1.0.3' + chr(13), chr(0x661) + ".0.3", 1.0):
        assert refused(guard.check_version, bad), repr(bad)


def check_sources():
    good = {'version': '1.0.3', 'fixes': [{'patch': 'tools/source-1.0.2/fixes/01-x.patch'}],
            'fixes_skipped': []}
    guard.check_sources(good, '1.0.3')
    assert refused(guard.check_sources, good, '1.0.4')
    assert refused(guard.check_sources, dict(good, version='1.0.3_candidate_37'), '1.0.3')
    assert refused(guard.check_sources, {k: v for k, v in good.items() if k != 'version'}, '1.0.3')
    assert refused(guard.check_sources, dict(good, fixes_skipped=['tools/source-1.0.2/fixes/01-x.patch']), '1.0.3')
    assert refused(guard.check_sources, {k: v for k, v in good.items() if k != 'fixes_skipped'}, '1.0.3')
    assert refused(guard.check_sources, dict(good, fixes=[]), '1.0.3')
    assert refused(guard.check_sources, dict(good, version='1.0.3_candidate_37'), '1.0.3_candidate_37')


def check_sums(root):
    folder = root / 'release'
    folder.mkdir()
    # The source release's assets: TrinityX360's PPSA83600 package and images.
    files = {'PPSA83600.zip': b'zip', 'PPSA83600.zip.sha256': b'sum', 'PPSA83600.ffpkg': b'image',
             'PPSA83600.ffpfsc': b'pfs', 'PS5X360-1.0.3-source.tar.xz': b'tree',
             'Fable_III.patch.toml': b'toml', 'SOURCES.json': b'{}'}
    for name, data in files.items():
        (folder / name).write_bytes(data)
    lines = guard.write_sums(folder)
    text = (folder / guard.SUMS).read_bytes().decode('ascii')
    assert text == ''.join(lines)
    names = [line.split('  ', 1)[1].rstrip('\n') for line in lines]
    assert names == sorted(files), names
    assert guard.SUMS not in names
    for line in lines:
        digest, name = line.rstrip('\n').split('  ')
        assert digest == hashlib.sha256(files[name]).hexdigest()
    assert guard.verify_sums(folder) == sorted(files)
    # Writing again (SHA256SUMS.txt now present) gives the same file: it never lists itself.
    guard.write_sums(folder)
    assert (folder / guard.SUMS).read_bytes().decode('ascii') == text

    (folder / 'PPSA83600.zip').write_bytes(b'changed')
    assert refused(guard.verify_sums, folder)
    (folder / 'PPSA83600.zip').write_bytes(b'zip')
    guard.verify_sums(folder)
    (folder / 'extra.bin').write_bytes(b'x')
    assert refused(guard.verify_sums, folder)  # not listed
    (folder / 'extra.bin').unlink()
    (folder / guard.SUMS).write_bytes((text + text.splitlines(True)[0]).encode('ascii'))
    assert refused(guard.verify_sums, folder)  # listed twice
    (folder / guard.SUMS).write_bytes((text + f'{"0" * 64}  {guard.SUMS}\n').encode('ascii'))
    assert refused(guard.verify_sums, folder)  # lists itself
    (folder / guard.SUMS).write_bytes((text + f'{"0" * 64}  gone.bin\n').encode('ascii'))
    assert refused(guard.verify_sums, folder)  # no such file
    (folder / guard.SUMS).write_bytes((text.replace('  ', ' ', 1)).encode('ascii'))
    assert refused(guard.verify_sums, folder)  # malformed line
    (folder / guard.SUMS).write_bytes(text.replace(chr(10), chr(13) + chr(10)).encode('ascii'))
    assert refused(guard.verify_sums, folder)  # CRLF lines
    (folder / guard.SUMS).write_bytes(text.rstrip(chr(10)).encode('ascii'))
    assert refused(guard.verify_sums, folder)  # no final newline
    (folder / guard.SUMS).write_bytes((text + chr(10)).encode('ascii'))
    assert refused(guard.verify_sums, folder)  # empty line
    (folder / guard.SUMS).write_bytes(text.encode('ascii') + bytes([0xFF, 10]))
    assert refused(guard.verify_sums, folder)  # not ASCII
    (folder / guard.SUMS).unlink()
    assert refused(guard.verify_sums, folder)
    guard.write_sums(folder)

    # A name with a newline cannot be made on every host: the pattern itself is checked.
    assert not guard.ASSET_NAME_RE.fullmatch('name.zip' + chr(10))
    assert guard.ASSET_NAME_RE.fullmatch('4D5308D6.-.Fable.III.patch.toml')
    for bad in ('with space.zip', 'ąę.txt'):
        odd = root / ('odd-' + str(abs(hash(bad))))
        odd.mkdir()
        (odd / bad).write_bytes(b'x')
        assert refused(guard.write_sums, odd), bad
    empty = root / 'empty-file'
    empty.mkdir()
    (empty / 'a.bin').write_bytes(b'')
    assert refused(guard.write_sums, empty)
    nested = root / 'nested'
    (nested / 'sub').mkdir(parents=True)
    (nested / 'a.bin').write_bytes(b'a')
    assert refused(guard.write_sums, nested)
    nothing = root / 'nothing'
    nothing.mkdir()
    assert refused(guard.write_sums, nothing)
    assert refused(guard.write_sums, root / 'missing')
    return folder


def view(**fields):
    release = {'tagName': 'v1.0.3', 'isDraft': True, 'isImmutable': False,
               'targetCommitish': SHA,
               # A draft uploaded before the rename still has the old PPSA50011 package.
               'assets': [{'name': 'PPSA83600.zip'}, {'name': 'PPSA50011.zip'}]}
    release.update(fields)
    return json.dumps(release)


def check_decide():
    d = guard.decide
    assert d('v1.0.3', SHA, 1, '', 'release not found\n') == 'create'
    assert d('v1.0.3', SHA, 0, view(), '') == 'update'
    # Published, immutable, or not plainly a draft: never touched.
    assert refused(d, 'v1.0.3', SHA, 0, view(isDraft=False), '')
    assert refused(d, 'v1.0.3', SHA, 0, view(isDraft=None), '')
    assert refused(d, 'v1.0.3', SHA, 0, view(isDraft='true'), '')
    assert refused(d, 'v1.0.3', SHA, 0, json.dumps({'tagName': 'v1.0.3'}), '')
    assert refused(d, 'v1.0.3', SHA, 0, view(isDraft=False, isImmutable=True), '')
    assert refused(d, 'v1.0.3', SHA, 0, view(isImmutable=True), '')
    # An answer that cannot be read, or for another tag.
    assert refused(d, 'v1.0.3', SHA, 0, 'not json', '')
    assert refused(d, 'v1.0.3', SHA, 0, '[]', '')
    assert refused(d, 'v1.0.3', SHA, 0, view(tagName='v1.0.2-opt-r2'), '')
    # gh failed for another reason: no decision is taken from it.
    assert refused(d, 'v1.0.3', SHA, 1, '', 'HTTP 401: Bad credentials\n')
    assert refused(d, 'v1.0.3', SHA, 4, '', '')
    # Bad inputs.
    assert refused(d, '1.0.3', SHA, 1, '', 'release not found')
    assert refused(d, 'v1.0.3_candidate_37', SHA, 1, '', 'release not found')
    assert refused(d, 'v1.0.3', 'f9140f1', 1, '', 'release not found')
    assert refused(d, 'v1.0.3', SHA.upper(), 1, '', 'release not found')
    assert refused(d, 'v1.0.3' + chr(10), SHA, 1, '', 'release not found')
    assert refused(d, 'v1.0.3', SHA + chr(10), 1, '', 'release not found')
    assert refused(d, 'v1.0.3', None, 1, '', 'release not found')
    # A tag that already exists: only at this commit (lightweight or annotated, peeled).
    light = f'{SHA}\trefs/tags/v1.0.3\n'
    annotated = f'{OTHER}\trefs/tags/v1.0.3\n{SHA}\trefs/tags/v1.0.3^{{}}\n'
    elsewhere = f'{OTHER}\trefs/tags/v1.0.3\n'
    elsewhere_annotated = f'{SHA}\trefs/tags/v1.0.3\n{OTHER}\trefs/tags/v1.0.3^{{}}\n'
    assert d('v1.0.3', SHA, 1, '', 'release not found', light) == 'create'
    assert d('v1.0.3', SHA, 0, view(), '', annotated) == 'update'
    assert refused(d, 'v1.0.3', SHA, 1, '', 'release not found', elsewhere)
    assert refused(d, 'v1.0.3', SHA, 0, view(), '', elsewhere_annotated)
    # Other tags in the listing (v1.0.30, v1.0.3-opt) do not count.
    others = f'{OTHER}\trefs/tags/v1.0.30\n{OTHER}\trefs/tags/v1.0.3-opt\n'
    assert d('v1.0.3', SHA, 1, '', 'release not found', others) == 'create'
    assert guard.remote_tag_commit(others, 'v1.0.3') is None
    assert guard.remote_tag_commit('', 'v1.0.3') is None


def run(*args):
    return subprocess.run([sys.executable, '-I', str(SCRIPT), *map(str, args)],
                          capture_output=True, text=True)


def check_cli(root, folder):
    result = run('version', '--ref', 'refs/heads/release/source-1.0.3')
    assert result.returncode == 0 and result.stdout == '1.0.3\n', result
    result = run('version', '--ref', 'refs/tags/v1.0.3')
    assert result.returncode == 2 and 'refused' in result.stderr and not result.stdout, result

    sources = root / 'SOURCES.json'
    sources.write_text(json.dumps({'version': '1.0.3', 'fixes': [{'patch': 'x'}], 'fixes_skipped': []}),
                       encoding='utf-8')
    assert run('check-sources', '--sources-json', sources, '--version', '1.0.3').returncode == 0
    assert run('check-sources', '--sources-json', sources, '--version', '1.0.4').returncode == 2

    assert run('verify', folder).returncode == 0
    result = run('sums', folder)
    assert result.returncode == 0 and result.stdout == (folder / guard.SUMS).read_text(encoding='ascii')

    out, err, tags = root / 'view.json', root / 'view.err', root / 'ls-remote.txt'
    out.write_text('', encoding='utf-8')
    err.write_text('release not found\n', encoding='utf-8')
    tags.write_text('', encoding='utf-8')
    common = ('--tag', 'v1.0.3', '--sha', SHA, '--view-stdout', out, '--view-stderr', err, '--remote-tag', tags)
    result = run('decide', '--view-exit', 1, *common)
    assert result.returncode == 0 and result.stdout == 'create\n', result
    out.write_text(view(), encoding='utf-8')
    err.write_text('', encoding='utf-8')
    result = run('decide', '--view-exit', 0, *common)
    assert result.returncode == 0 and result.stdout == 'update\n', result
    out.write_text(view(isDraft=False), encoding='utf-8')
    result = run('decide', '--view-exit', 0, *common)
    assert result.returncode == 2 and 'published' in result.stderr and not result.stdout, result

    out.write_text(view(), encoding='utf-8')
    result = run('stale', folder, '--view-stdout', out)
    assert result.returncode == 0 and result.stdout == 'PPSA50011.zip\n', result


def main():
    check_version()
    check_sources()
    check_decide()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        folder = check_sums(root)
        check_cli(root, folder)
    print('PASS: release_guard takes the version from the branch, sums every asset once and '
          'updates only a draft release')


if __name__ == '__main__':
    main()
