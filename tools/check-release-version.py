"""Host check of the release path of the source build: the version label, the release eboot check
and the GPL source tarball.

    python3 -I tools/check-release-version.py [--tree <assembled tree>] [--release-version X.Y.Z]
        [--candidate-elf <a test build's eboot.elf>] [--bash <GNU bash>]

1. assemble.py's version: --version wins over PS5X360_BUILD_VERSION, which wins over
   tools/source-1.0.2/BUILD_VERSION, then 1.0.2; a blank override counts as unset. A release
   (--release or PS5X360_RELEASE) takes its version only from an override, accepts only X.Y.Z
   (no candidate label, no "v", no fourth part, ASCII digits only) and leaves no fix out; a bad
   release stops assemble.py before it creates or downloads anything. Every version is a valid C
   string and CMake value. VERSION and SOURCES.json's version/version_source/release are written.
2. tools/source-1.0.2/check_release_elf.py on made-up executables: the label
   ("TrinityX360 v<version>") and boot line of exactly this version pass, RADV's own
   "candidate_" names pass, and a candidate label, another version (1.0.30 is not 1.0.3), a
   missing line, an older name's label (the official app's "PS5X360 v<version>" or
   "PS5X360Reloaded v<version>", the name until 1.0.5; alone or next to the new one) or a
   SOURCES.json leaving a fix out fail.
   With --candidate-elf, on a real test build (one labelled "TrinityX360 v..._candidate_N"):
   its candidate label fails, and the same ELF with
   the label replaced by a plain version passes (no false alarm anywhere else in 38 MB).
3. build-source-1.0.2.yml: the workflow_call inputs, the forced-empty skip list, the artifact
   names, the check step; and its "GPL source tarball" step, run with GNU tar and xz on a made-up
   tree, twice: the same bytes whatever the files' times and creation order, the top folder
   PS5X360-<version>-source, owner 0, the commit's mtime, and no .git, build/, copied
   tools/check-* or __pycache__ (skipped when no GNU bash/tar/xz is found).
With --tree: the tree's VERSION matches SOURCES.json's version; with --release-version too, the
tree is that release (release true, no fix left out).
"""
import argparse
import hashlib
import io
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / 'tools/source-1.0.2'
WORKFLOW = REPO / '.github/workflows/build-source-1.0.2.yml'
sys.path.insert(0, str(SOURCE))
import assemble  # noqa: E402
import check_release_elf  # noqa: E402

EPOCH = 1795126630


def stops(function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except SystemExit:
        return True
    return False


def check_versions(tmp):
    candidate = tmp / 'BUILD_VERSION'
    candidate.write_text('1.0.3_candidate_37\n', encoding='utf-8')
    missing = tmp / 'no-BUILD_VERSION'
    rv = assemble.resolve_version

    # Precedence: --version > PS5X360_BUILD_VERSION > BUILD_VERSION > 1.0.2.
    assert rv(None, None, candidate) == ('1.0.3_candidate_37', 'BUILD_VERSION')
    assert rv('', '  ', candidate) == ('1.0.3_candidate_37', 'BUILD_VERSION')  # blank = unset
    assert rv(None, '1.0.3', candidate) == ('1.0.3', 'PS5X360_BUILD_VERSION')
    assert rv('1.0.4', '1.0.3', candidate) == ('1.0.4', '--version')
    assert rv(' 1.0.4\n', None, candidate) == ('1.0.4', '--version')
    assert rv(None, None, missing) == ('1.0.2', 'default')
    assert rv(None, '1.0.3_candidate_40', missing) == ('1.0.3_candidate_40', 'PS5X360_BUILD_VERSION')
    # The repository's own label is still what a test build without an override gets.
    own = (SOURCE / 'BUILD_VERSION').read_text(encoding='utf-8').strip()
    assert rv(None, None) == (own, 'BUILD_VERSION'), own

    # A release: only an override, only X.Y.Z.
    assert rv('1.0.3', None, candidate, release=True) == ('1.0.3', '--version')
    assert rv(None, '1.0.3', candidate, release=True) == ('1.0.3', 'PS5X360_BUILD_VERSION')
    assert rv('1.0.3', '1.0.3_candidate_37', candidate, release=True) == ('1.0.3', '--version')
    assert rv('10.20.300', None, missing, release=True) == ('10.20.300', '--version')
    assert stops(rv, None, None, candidate, release=True)          # the candidate file is no release
    plain = tmp / 'BUILD_VERSION-plain'
    plain.write_text('1.0.3\n', encoding='utf-8')
    assert stops(rv, None, None, plain, release=True)              # nor is a plain file: say it
    assert stops(rv, None, None, missing, release=True)            # nor the 1.0.2 fallback
    assert stops(rv, '1.0.3_candidate_37', '1.0.3', candidate, release=True)  # --version wins, and fails
    # A hotfix X.Y.Z.W is a release (1.0.4.1); W = 0, a leading zero or a fifth part are not.
    assert rv('1.0.4.1', None, candidate, release=True) == ('1.0.4.1', '--version')
    for junk in ('1.0.4.0', '1.0.4.01', '1.0.4.1000', '1.0.4.1.1'):
        assert stops(rv, junk, None, missing, release=True), repr(junk)
    for junk in ('1.0.3_candidate_37', '1.0.3-candidate', 'v1.0.3', '1.0', '1', '1.0.3-rc1',
                 '1.0.3 x', '1.0.3\n1', '1.0.3\n\n1.0.4', '1..3', '.1.0.3', '1.0.3.', 'a.b.c',
                 '\uff11.0.3', '1.0.\u0663', '1.0.3"', "1.0.3'", '1.0.3;', '1.0.3/'):
        assert stops(rv, junk, None, missing, release=True), repr(junk)
        assert stops(rv, None, junk, missing, release=True), repr(junk)
    # Any build: the label goes into a C string and a CMake definition.
    for junk in ('1.0.3"', '1.0.3\\', '1.0 3', '1.0.3;x', '1.0.3$x', '\u00e91.0', '-1.0', '_1', 'x' * 65,
                 '1.0.3\n1'):
        assert stops(rv, junk, None, missing), repr(junk)
    for fine in ('1.0.3', '1.0.3_candidate_37', '1.0.2-opt', '1.0.3+build.5', 'x' * 64):
        assert rv(fine, None, missing) == (fine, '--version'), fine

    # PS5X360_RELEASE.
    for on in ('1', 'true', 'TRUE', ' yes '):
        assert assemble.is_release(False, on), on
    for off in (None, '', '0', 'false', 'no', 'release'):
        assert not assemble.is_release(False, off), off
    assert assemble.is_release(True, None)

    # settings(): the whole decision, as main() makes it.
    def args(version=None, release=False, skip_fix=None):
        return SimpleNamespace(version=version, release=release, skip_fix=skip_fix)
    st = assemble.settings
    assert st(args(), {}, candidate) == (False, '1.0.3_candidate_37', 'BUILD_VERSION', [])
    # A dispatched test build: the workflow passes empty strings for what it does not set.
    dispatch = {'PS5X360_BUILD_VERSION': '', 'PS5X360_RELEASE': '', 'PS5X360_SKIP_FIXES': '01'}
    assert st(args(), dispatch, candidate) == (False, '1.0.3_candidate_37', 'BUILD_VERSION', ['01'])
    release_env = {'PS5X360_BUILD_VERSION': '1.0.3', 'PS5X360_RELEASE': '1', 'PS5X360_SKIP_FIXES': ''}
    assert st(args(), release_env, candidate) == (True, '1.0.3', 'PS5X360_BUILD_VERSION', [])
    assert st(args('1.0.4', True), {}, candidate) == (True, '1.0.4', '--version', [])
    assert stops(st, args(), dict(release_env, PS5X360_SKIP_FIXES='01'), candidate)
    assert stops(st, args(skip_fix=['08b']), release_env, candidate)
    assert stops(st, args(release=True), {}, candidate)
    assert st(args(skip_fix=['08b']), {'PS5X360_SKIP_FIXES': '01'}, candidate)[3] == ['08b', '01']

    # write_version: VERSION for canary/CMakeLists.txt, and SOURCES.json's record.
    out = tmp / 'tree'
    out.mkdir()
    report = {}
    assemble.write_version(out, report, '1.0.3', 'PS5X360_BUILD_VERSION', True)
    assert (out / 'VERSION').read_bytes() == b'1.0.3\n'
    assert report == {'version': '1.0.3', 'version_source': 'PS5X360_BUILD_VERSION', 'release': True}
    assert json.loads(json.dumps(report, sort_keys=True))['version'] == '1.0.3'
    assemble.write_version(out, report, '1.0.3_candidate_37', 'BUILD_VERSION', False)
    assert (out / 'VERSION').read_bytes() == b'1.0.3_candidate_37\n' and report['release'] is False

    # main(): the settings are decided first, VERSION and SOURCES.json's record are written after the
    # title files (which no longer write VERSION) and before the fixes, with the checked skip list.
    import inspect
    body = inspect.getsource(assemble.main)
    steps = [body.index(s) for s in ('settings(args, os.environ)', 'out.mkdir(', 'build_title(out',
                                     'write_version(out, report, version, version_source, release)',
                                     'apply_fixes(out, report', "'SOURCES.json').write_text")]
    assert steps == sorted(steps), steps
    assert 'skip=skip)' in body and 'skip_names(' not in body, 'main() must use settings() skip list'
    assert "'VERSION'" not in inspect.getsource(assemble.build_title)

    # assemble.py itself: a bad release stops before it creates --out or downloads anything.
    env = {k: v for k, v in os.environ.items() if not k.startswith('PS5X360_')}
    for extra, extra_env, message in (
            (['--release', '--version', '1.0.3_candidate_37'], {}, 'is not X.Y.Z'),
            (['--release'], {}, 'give its version'),
            (['--version', '1.0.3'], {'PS5X360_RELEASE': '1', 'PS5X360_SKIP_FIXES': '01'}, 'no fix may be left out'),
            (['--version', '1.0.3"'], {}, 'cannot take')):
        target = tmp / 'never'
        result = subprocess.run([sys.executable, '-I', str(SOURCE / 'assemble.py'), '--out', str(target),
                                 '--cache', str(tmp / 'never-cache'), *extra],
                                env=dict(env, **extra_env), capture_output=True, text=True, timeout=60)
        assert result.returncode != 0 and message in result.stderr, (extra, result.returncode, result.stderr)
        assert not target.exists() and not (tmp / 'never-cache').exists(), extra
    print('  assemble.py: override order, release validation, skip refusal, VERSION/SOURCES.json')


GOOD = (b'\x7fELF\x00junk\x00TrinityX360 v1.0.3\x00{"version":"1.0.3"}\x00BOOT version 1.0.3\x00'
        b'intersection_candidate_aabb_opaque\x00candidate_infos\x00candidate_vertex_index\x00'
        b'TrinityX360 development build\x00ACO_DEBUG\x001.0.3\x00'
        # session_log.hpp's header "PS5X360 " + version is kept, and is no label.
        b'PS5X360 \x00')
LABEL = b'TrinityX360 v1.0.3\x00'


def check_elf_logic(tmp):
    bp = check_release_elf.binary_problems
    assert bp(GOOD, '1.0.3') == [], bp(GOOD, '1.0.3')
    cases = {
        'candidate label': GOOD.replace(LABEL, b'TrinityX360 v1.0.3_candidate_37\x00'),
        'candidate boot line': GOOD.replace(b'BOOT version 1.0.3\x00', b'BOOT version 1.0.3_candidate_37\x00'),
        'bare candidate version': GOOD + b'1.0.3_candidate_37\x00',
        'candidate with a dash': GOOD + b'1.0.3-candidate-2\x00',
        'another version too': GOOD + b'TrinityX360 v1.0.2\x00',
        'official label too': GOOD + b'PS5X360 v1.0.3\x00',
        'official label instead': GOOD.replace(LABEL, b'PS5X360 v1.0.3\x00'),
        'official candidate label too': GOOD + b'PS5X360 v1.0.3_candidate_52\x00',
        # Built without the rename to TrinityX360 (1.0.6): PS5X360Reloaded's label.
        'old name label too': GOOD + b'PS5X360Reloaded v1.0.3\x00',
        'old name label instead': GOOD.replace(LABEL, b'PS5X360Reloaded v1.0.3\x00'),
        'fallback boot line too': GOOD + b'BOOT version 1.0.2\x00',
        '1.0.30 is not 1.0.3': GOOD.replace(LABEL, b'TrinityX360 v1.0.30\x00'),
        'no label': GOOD.replace(LABEL, b'TrinityX360 development build\x00'),
        'no boot line': GOOD.replace(b'BOOT version 1.0.3\x00', b''),
        'label not a whole string': GOOD.replace(LABEL, b'TrinityX360 v1.0.3'),
        'empty': b'',
    }
    for name, data in cases.items():
        assert bp(data, '1.0.3'), name
    assert bp(GOOD, '1.0.4') and bp(GOOD, '1.0')
    assert check_release_elf.LABELS[0] == b'TrinityX360 v'
    for name in ('official label too', 'official label instead', 'official candidate label too',
                 'old name label too', 'old name label instead'):
        assert any("older name's label" in p for p in bp(cases[name], '1.0.3')), name
    assert check_release_elf.version_problems('1.0.3') == []
    for junk in ('1.0.3_candidate_37', 'v1.0.3', '', None, '1.0.3\n'):
        assert check_release_elf.version_problems(junk), repr(junk)

    sp = check_release_elf.sources_problems
    report = {'version': '1.0.3', 'release': True, 'fixes_skipped': [], 'fixes': [{'patch': 'x'}]}
    assert sp(report, '1.0.3') == []
    for change in ({'version': '1.0.3_candidate_37'}, {'version': None}, {'release': False},
                   {'fixes_skipped': ['tools/source-1.0.2/fixes/01-jit-reserved-barriers.patch']},
                   {'fixes': []}):
        assert sp(dict(report, **change), '1.0.3'), change
    assert sp({k: v for k, v in report.items() if k != 'fixes_skipped'}, '1.0.3')
    assert sp(report, '1.0.4')

    # The command line, as the workflow runs it.
    elf, binary, sources = tmp / 'eboot.elf', tmp / 'eboot.bin', tmp / 'SOURCES.json'
    elf.write_bytes(GOOD)
    binary.write_bytes(b'SELF' + GOOD)
    sources.write_text(json.dumps(report), encoding='utf-8')
    run = ['--version', '1.0.3', '--elf', str(elf), '--bin', str(binary), '--sources-json', str(sources)]
    quiet = io.StringIO()
    stdout, sys.stdout = sys.stdout, quiet
    try:
        assert check_release_elf.main(run) == 0
        assert check_release_elf.main(['--version', '1.0.3']) == 0
        assert check_release_elf.main(['--version', '1.0.3_candidate_37']) == 1
        assert check_release_elf.main(['--version', '1.0.4'] + run[2:]) == 1
        binary.write_bytes(cases['candidate label'])
        assert check_release_elf.main(run) == 1
        binary.write_bytes(GOOD)
        sources.write_text(json.dumps(dict(report, fixes_skipped=['x'])), encoding='utf-8')
        assert check_release_elf.main(run) == 1
    finally:
        sys.stdout = stdout
    assert 'FAIL eboot.bin' in quiet.getvalue() and 'FAIL SOURCES.json' in quiet.getvalue()
    result = subprocess.run([sys.executable, '-I', str(SOURCE / 'check_release_elf.py'), '--version', '1.0.3'],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and 'PASS' in result.stdout, (result.stdout, result.stderr)
    print('  check_release_elf.py: labels, candidates, other versions, SOURCES.json, exit codes')


def check_real_elf(path):
    data = Path(path).read_bytes()
    # A test build with the TrinityX360 label (TrinityX360 v1.0.3_candidate_N).
    labels = check_release_elf.label_values(data, check_release_elf.LABELS[0])
    assert len(set(labels)) == 1 and 'candidate' in labels[0], labels
    label = labels[0]
    assert check_release_elf.binary_problems(data, label.split('_', 1)[0])
    # The same executable with a plain version in place of the label (NUL-padded, same length).
    plain = '9.8.7'
    patched = data.replace(label.encode() + b'\x00', plain.encode() + b'\x00' * (len(label) - len(plain) + 1))
    problems = check_release_elf.binary_problems(patched, plain)
    assert problems == [], problems
    print(f'  {path}: {label} rejected; with a plain version in its place, nothing else is flagged')


def workflow_text():
    return WORKFLOW.read_text(encoding='utf-8')


def check_workflow_text(text):
    for needle in (
            '  workflow_dispatch:\n    inputs:\n      skip_fixes:',
            '  workflow_call:\n    inputs:\n      version:',
            "        type: string\n        required: false\n        default: ''",
            '      release:',
            '        type: boolean\n        required: false\n        default: false',
            "PS5X360_SKIP_FIXES: ${{ inputs.release != true && inputs.skip_fixes || '' }}",
            "PS5X360_BUILD_VERSION: ${{ inputs.version || '' }}",
            "PS5X360_RELEASE: ${{ inputs.release == true && '1' || '' }}",
            "RELEASE_VERSION: ${{ inputs.release == true && inputs.version || '' }}",
            'tools/source-1.0.2/check_release_elf.py --version "$RELEASE_VERSION"',
            '--elf build/src102-out/eboot.elf --bin build/src102-out/eboot.bin',
            '--sources-json build/src102/SOURCES.json',
            'name=eboot-$RELEASE_VERSION-from-source',
            'name=eboot-1.0.2-from-source-NOT-FOR-RELEASE',
            'if [[ $RELEASE == true && $JOB_STATUS == success ]]; then',
            'JOB_STATUS: ${{ job.status }}',
            'echo "artifact=source-$RELEASE_VERSION"',
            'name: ${{ steps.artifact.outputs.name ||',
            'name: ${{ steps.source.outputs.artifact }}',
            'value: ${{ jobs.rebuild.outputs.artifact }}'):
        assert needle in text, needle
    # The release checks run after the link and before anything is uploaded.
    order = [text.index(s) for s in ('- name: Check the release version', '- name: Assemble the 1.0.2',
                                     '- name: Compile and link eboot.bin', '- name: Check the release eboot',
                                     '- name: GPL source tarball', '- name: Name the eboot artifact',
                                     '- name: Upload eboot.bin', '- name: Upload the GPL source tarball')]
    assert order == sorted(order), order


def step_script(text, step_name):
    """The `run: |` block of the step called step_name, dedented."""
    lines = text.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == f'- name: {step_name}')
    run = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == 'run: |')
    indent = len(lines[run]) - len(lines[run].lstrip()) + 2
    body = []
    for line in lines[run + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        body.append(line[indent:])
    return '\n'.join(body).rstrip() + '\n'


def find_bash(explicit):
    if explicit:
        return explicit
    candidates = [shutil.which('bash')]
    if os.name == 'nt':  # Git for Windows' bash, not WSL's C:\Windows\System32\bash.exe
        candidates = [r'C:\Program Files\Git\bin\bash.exe', r'C:\Program Files\Git\usr\bin\bash.exe'] + \
                     [c for c in candidates if c and 'system32' not in c.lower()]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            probe = subprocess.run([candidate, '-c', 'tar --version | head -1; xz --version | head -1'],
                                   capture_output=True, text=True)
            if probe.returncode == 0 and 'GNU tar' in probe.stdout and 'xz' in probe.stdout:
                return candidate
    return None


def make_tree(root, reverse):
    """A made-up assembled tree, files created in one order or the other."""
    files = {
        'VERSION': '1.0.3\n', 'SOURCES.json': '{}\n', 'src/a.cpp': 'a\n', 'src/b.cpp': 'b\n',
        'tools/resolve-mesa-weaks.py': 'print()\n', 'licenses/framegen-COPYING': 'GPL\n',
        '.deps/xenia-canary/src/x.cc': 'x\n', '.deps/xenia-canary/.git/HEAD': 'ref\n',
        '.deps/xenia-canary/third_party/wolfssl/.git': 'gitdir: ../../.git/modules/wolfssl\n',
        '.deps/xenia-canary/third_party/wolfssl/wolfssl.c': 'w\n', '.deps/xenia-canary/.gitmodules': 'm\n',
        'tools/check-forward.cpp': 'copied by run-fix-checks\n', 'build/fix-checks/check-forward': 'exe\n',
        'build/check-gdfx-names/out': 'exe\n', 'tools/__pycache__/x.cpython-312.pyc': 'pyc\n',
        'src/build/keep.cpp': 'not the top build/\n',
    }
    tree = root / 'build/src102'
    for i, (rel, content) in enumerate(sorted(files.items(), reverse=reverse)):
        path = tree / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')
        os.utime(path, (EPOCH + 1000 * i + (7 if reverse else 0),) * 2)
    linked = True
    try:
        (tree / 'online').mkdir()
        os.symlink('../.deps/xenia-canary', tree / 'online/tree', target_is_directory=True)
    except (OSError, NotImplementedError):
        linked = False
    return linked


def check_tarball(text, bash):
    script = step_script(text, 'GPL source tarball of the assembled tree')
    assert 'tar --create' in script and '--sort=name' in script and '--owner=0' in script
    if not bash:
        print('  SKIP tarball step: no bash with GNU tar and xz found (CI has them)')
        return
    results = []
    with tempfile.TemporaryDirectory() as scratch:
        for reverse in (False, True):
            root = Path(scratch) / ('second' if reverse else 'first')
            linked = make_tree(root, reverse)
            output, summary = root / 'github-output', root / 'summary'
            env = dict(os.environ, RELEASE_VERSION='1.0.3', SOURCE_DATE_EPOCH=str(EPOCH),
                       GITHUB_OUTPUT=output.as_posix(), GITHUB_STEP_SUMMARY=summary.as_posix())
            result = subprocess.run([bash, '-c', script], cwd=root, env=env, capture_output=True, text=True,
                                    timeout=300)
            assert result.returncode == 0, (result.stdout, result.stderr)
            outputs = dict(line.split('=', 1) for line in output.read_text().splitlines())
            tarball = root / 'dist' / 'PS5X360-1.0.3-source.tar.xz'
            data = tarball.read_bytes()
            sha = hashlib.sha256(data).hexdigest()
            assert outputs == {'artifact': 'source-1.0.3', 'tarball': tarball.name, 'sha256': sha}, outputs
            assert (root / 'dist' / (tarball.name + '.sha256')).read_text() == f'{sha}  {tarball.name}\n'
            assert sha in summary.read_text() and 'entries' in result.stdout
            results.append((sha, linked))
            with tarfile.open(fileobj=io.BytesIO(lzma.decompress(data))) as archive:
                members = archive.getmembers()
            names = [m.name for m in members]
            top = 'PS5X360-1.0.3-source'
            assert all(n == top or n.startswith(top + '/') for n in names), names
            assert names == sorted(names, key=lambda n: n.split('/')), names  # --sort=name
            for kept in ('VERSION', 'SOURCES.json', 'src/a.cpp', 'tools/resolve-mesa-weaks.py',
                         '.deps/xenia-canary/src/x.cc', '.deps/xenia-canary/.gitmodules',
                         '.deps/xenia-canary/third_party/wolfssl/wolfssl.c', 'src/build/keep.cpp',
                         'licenses/framegen-COPYING'):
                assert f'{top}/{kept}' in names, kept
            for gone in ('.git', 'check-forward.cpp', '__pycache__'):
                assert not any(gone in n.split('/') for n in names), gone
            assert not any(n == f'{top}/build' or n.startswith(f'{top}/build/') for n in names)
            assert all(m.mtime == EPOCH and m.uid == 0 and m.gid == 0 for m in members)
            assert all(not (m.mode & 0o022) for m in members if not m.issym())
            if linked:
                link = next(m for m in members if m.name == f'{top}/online/tree')
                assert link.issym() and link.linkname == '../.deps/xenia-canary', link.linkname
    (first, linked1), (second, linked2) = results
    if linked1 == linked2:
        assert first == second, 'the tarball is not deterministic'
    print(f'  tarball step: deterministic ({first[:12]}), top folder, owner 0, commit mtime, exclusions'
          + ('' if linked1 else ' (no symlink here)'))


def check_tree(tree, release_version):
    report = json.loads((tree / 'SOURCES.json').read_text(encoding='utf-8'))
    version = (tree / 'VERSION').read_text(encoding='utf-8').strip()
    assert report.get('version') == version, (report.get('version'), version)
    assert report.get('version_source') in ('--version', 'PS5X360_BUILD_VERSION', 'BUILD_VERSION', 'default')
    if release_version:
        problems = check_release_elf.sources_problems(report, release_version)
        assert not problems, problems
        assert version == release_version
    print(f'  tree: version {version} from {report["version_source"]}, release {report.get("release")}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--release-version')
    parser.add_argument('--candidate-elf', type=Path)
    parser.add_argument('--bash', help='a bash with GNU tar and xz (default: found on PATH)')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as scratch:
        check_versions(Path(scratch))
    with tempfile.TemporaryDirectory() as scratch:
        check_elf_logic(Path(scratch))
    if args.candidate_elf:
        check_real_elf(args.candidate_elf)
    text = workflow_text()
    check_workflow_text(text)
    print('  build-source-1.0.2.yml: release inputs, skip list, artifact names, check order')
    check_tarball(text, find_bash(args.bash))
    if args.tree:
        check_tree(args.tree, args.release_version)
    print('PASS: the release version, the release eboot check and the GPL source tarball')


if __name__ == '__main__':
    main()
