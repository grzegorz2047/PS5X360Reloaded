"""Writes or checks the repository's sce_sys/param.json and sce_sys/icon0.png for a release.

    python3 -I tools/source-1.0.2/repo_sce_sys.py --base-zip PPSA50011-v1.0.2.zip --version 1.0.6 --write
    python3 -I tools/source-1.0.2/repo_sce_sys.py --base-zip PPSA50011-v1.0.2.zip --version 1.0.6 --check

Homebrew catalogs and stores read the title id, name, contentVersion and icon from sce_sys/ in the
repository at the release tag (github.com/darkxex/ps5-homebrew-catalog, docs/submitting.md and
docs/versioning.md). The package makes those files (tools/source-1.0.2/package.py), so the
repository carries copies at its root: --write makes them from the official PPSA50011.zip and the
release version, the same bytes the package ships; commit them in the release commit. --check
stops when they are missing or differ. The release workflow runs --check before the build and
compares the files with the package's after it.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package  # noqa: E402


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--base-zip', type=Path, required=True, help='official v1.0.2 PPSA50011.zip')
    parser.add_argument('--version', required=True, help='release version X.Y.Z')
    parser.add_argument('--repo-root', type=Path, default=package.ROOT,
                        help='repository root (default: this repository)')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--write', action='store_true', help='write sce_sys/ at the repository root')
    mode.add_argument('--check', action='store_true', help='check sce_sys/ at the repository root')
    args = parser.parse_args(argv)

    version = package.check_version(args.version)
    expected = package.repo_sce_sys(package.read_base_zip(args.base_zip), version)
    if args.write:
        for name, data in expected.items():
            path = args.repo_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    package.check_repo_sce_sys(expected, version, args.repo_root)
    param = json.loads(expected['sce_sys/param.json'])
    print(json.dumps({'version': version, 'mode': 'write' if args.write else 'check',
                      'title_id': param['titleId'], 'content_version': param['contentVersion'],
                      'files': {name: package.sha256(data) for name, data in expected.items()}}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
