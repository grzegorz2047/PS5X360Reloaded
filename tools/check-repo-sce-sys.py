"""Host check of the repository's sce_sys/ copies for homebrew catalogs (tools/source-1.0.2/repo_sce_sys.py).

    python3 -I tools/check-repo-sce-sys.py

Runs on the made-up base PPSA50011.zip of tools/check-source-package.py. It checks:
  * package.repo_sce_sys() gives sce_sys/param.json and sce_sys/icon0.png byte for byte as
    package.build() ships them under PPSA83600/;
  * repo_sce_sys.py --write writes them at the repository root, after which --check passes, and the
    summary names PPSA83600 and the release's contentVersion;
  * --check stops when a file is missing, when param.json or icon0.png differs, and when the files
    are another version's; a base zip other than the official one is refused;
  * .gitattributes keeps both files byte for byte (param.json has the official CRLF lines);
  * the release workflow checks them before the build (prepare job) and compares them with the
    package after it (package job).
"""
import importlib.util
import io
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools/source-1.0.2'))
import package  # noqa: E402
import repo_sce_sys  # noqa: E402

spec = importlib.util.spec_from_file_location('check_source_package', REPO / 'tools/check-source-package.py')
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
VERSION = fixtures.VERSION


def run(argv):
    """repo_sce_sys.main(argv): (exit status or SystemExit message, stdout)."""
    stdout, stderr = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = io.StringIO(), io.StringIO()
    try:
        try:
            status = repo_sce_sys.main(argv)
        except SystemExit as error:
            status = error.code
        return status, sys.stdout.getvalue()
    finally:
        sys.stdout, sys.stderr = stdout, stderr


def main():
    shipped = fixtures.build()
    expected = package.repo_sce_sys(fixtures.base_files(), VERSION)
    assert tuple(expected) == package.REPO_SCE_SYS
    for name, data in expected.items():
        assert shipped[package.PREFIX + name] == data, name
    assert b'\r\n' in expected['sce_sys/param.json'], 'the official CRLF lines are kept'
    print('[OK] the repository copies are the package bytes')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        zip_path = tmp / 'base.zip'
        digest = fixtures.sha(fixtures.base_zip(zip_path))
        root = tmp / 'repo'
        root.mkdir()
        argv = ['--base-zip', str(zip_path), '--version', VERSION, '--repo-root', str(root)]

        status, _ = run(argv + ['--check'])
        assert 'is not the official v1.0.2 PPSA50011.zip' in str(status), status
        official = package.BASE_ZIP_SHA256
        package.BASE_ZIP_SHA256 = digest
        try:
            status, _ = run(argv + ['--check'])
            assert 'sce_sys/param.json is missing; sce_sys/icon0.png is missing' in str(status), status
            assert f'--version {VERSION} --write' in str(status), status

            status, out = run(argv + ['--write'])
            assert status == 0, status
            summary = json.loads(out)
            assert summary['mode'] == 'write' and summary['title_id'] == 'PPSA83600', summary
            assert summary['content_version'] == fixtures.CONTENT_VERSION, summary
            for name, data in expected.items():
                assert (root / name).read_bytes() == data, name
            status, out = run(argv + ['--check'])
            assert status == 0 and json.loads(out)['mode'] == 'check', status
            print('[OK] --write, then --check passes')

            param = root / 'sce_sys/param.json'
            param.write_bytes(expected['sce_sys/param.json'].replace(b'\r\n', b'\n'))
            status, _ = run(argv + ['--check'])
            assert f"sce_sys/param.json is not the v{VERSION} package's" in str(status), status
            param.write_bytes(expected['sce_sys/param.json'])
            icon = root / 'sce_sys/icon0.png'
            icon.write_bytes(expected['sce_sys/icon0.png'] + b'\0')
            status, _ = run(argv + ['--check'])
            assert "sce_sys/icon0.png is not the v" in str(status), status
            assert 'param.json' not in str(status).split(' in the repository')[0], status
            icon.write_bytes(expected['sce_sys/icon0.png'])
            other = '1.0.99'
            status, _ = run(['--base-zip', str(zip_path), '--version', other, '--repo-root', str(root), '--check'])
            assert f"sce_sys/param.json is not the v{other} package's" in str(status), status
            status, _ = run(argv)
            assert status == 2, status  # argparse: --write or --check is required
        finally:
            package.BASE_ZIP_SHA256 = official
    print('[OK] --check refuses missing, changed and other-version files and a non-official base')

    attributes = (REPO / '.gitattributes').read_text(encoding='utf-8').splitlines()
    assert 'sce_sys/param.json -text' in attributes, 'param.json must keep its CRLF lines in git'
    assert '*.png binary' in attributes
    workflow = (REPO / '.github/workflows/release-source.yml').read_text(encoding='utf-8')
    prepare = workflow[workflow.index('\n  prepare:'):workflow.index('\n  build:')]
    packaging = workflow[workflow.index('\n  package:'):workflow.index('\n  draft:')]
    assert 'tools/source-1.0.2/repo_sce_sys.py' in prepare and '--check' in prepare
    assert 'cmp dist/source-release/stage/PPSA83600/sce_sys/' in packaging
    print('[OK] .gitattributes and the release workflow')
    print('check-repo-sce-sys: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
