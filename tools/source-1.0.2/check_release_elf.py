"""Check that a source-built eboot is the release it claims to be, before it is packaged.

    python3 -I tools/source-1.0.2/check_release_elf.py --version 1.0.3 \
        [--elf build/src102-out/eboot.elf] [--bin build/src102-out/eboot.bin] \
        [--sources-json build/src102/SOURCES.json]

With --version alone it only checks that the version is a release's (X.Y.Z, assemble.py's
RELEASE_VERSION), so a workflow can stop before it compiles anything.

In each executable (the ELF, and the signed eboot.bin, which keeps .rodata readable):
  * the home-screen label "TrinityX360 v<version>" and the boot.log line "BOOT version <version>"
    are there as whole C strings (NUL-terminated, so 1.0.30 does not pass for 1.0.3);
  * every such label carries exactly this version (no 1.0.2 fallback, no other build's label);
  * no older name's label: not the official app's "PS5X360 v<version>" (a build without fix 46,
    title id PPSA83600) and not "PS5X360Reloaded v<version>" (the name until 1.0.5: a build
    without the rename to TrinityX360);
  * no candidate label: nothing like 1.0.3_candidate_37 anywhere, and no label containing
    "candidate". A bare "candidate_" is not an error: RADV's ray-tracing shaders carry names such
    as candidate_infos and intersection_candidate_aabb_opaque.
In SOURCES.json: version is this one, release is true, fixes were applied and none was left out.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from assemble import RELEASE_VERSION  # noqa: E402

LABELS = (b'TrinityX360 v', b'BOOT version ')
# The home-screen labels of the older names: the official PS5X360's (and this fork's before fix 46)
# and PS5X360Reloaded's (1.0.3 to 1.0.5, before the rename to TrinityX360).
OLD_LABELS = (b'PS5X360 v', b'PS5X360Reloaded v')
# A candidate label wherever it is: the web status JSON and game-log headers carry the bare version.
CANDIDATE_VERSION = re.compile(rb'[0-9]+\.[0-9]+\.[0-9]+[._-]?candidate[0-9A-Za-z_]*', re.IGNORECASE)


def version_problems(version):
    if not RELEASE_VERSION.fullmatch(version or ''):
        return [f'version {version!r} is not a release version X.Y.Z or X.Y.Z.W']
    return []


def label_values(data, prefix):
    """The versions every "<prefix><digit>...\\0" string in data carries ("PS5X360 development
    build", the fallback label, starts with a letter and is not one)."""
    return [m.group(1).decode('latin-1')
            for m in re.finditer(re.escape(prefix) + rb'([0-9][^\x00\n"]{0,63})\x00', data)]


def binary_problems(data, version, name='eboot'):
    problems = []
    for prefix in LABELS:
        label = prefix.decode()
        values = label_values(data, prefix)
        if version not in values:
            problems.append(f'{name}: no "{label}{version}" string (found: {sorted(set(values)) or "none"})')
        other = sorted({v for v in values if v != version})
        if other:
            problems.append(f'{name}: "{label}..." strings of another version: {other}')
        candidates = sorted({m.group(0).decode('latin-1') for m in
                             re.finditer(re.escape(prefix) + rb'[^\x00]{0,63}candidate[^\x00]{0,63}', data,
                                         re.IGNORECASE)})
        if candidates:
            problems.append(f'{name}: candidate labels: {candidates}')
    for old_label in OLD_LABELS:
        old = sorted(set(label_values(data, old_label)))
        if old:
            problems.append(f'{name}: an older name\'s label "{old_label.decode()}..." '
                            f'(not built as TrinityX360): {old}')
    found = sorted({m.group(0).decode('latin-1') for m in CANDIDATE_VERSION.finditer(data)})
    if found:
        problems.append(f'{name}: candidate version strings: {found}')
    return problems


def sources_problems(report, version):
    problems = []
    if report.get('version') != version:
        problems.append(f'SOURCES.json: version {report.get("version")!r}, expected {version!r}')
    if report.get('release') is not True:
        problems.append(f'SOURCES.json: release is {report.get("release")!r}, not true')
    if 'fixes_skipped' not in report:
        problems.append('SOURCES.json: no fixes_skipped (not from the current assemble.py)')
    elif report['fixes_skipped']:
        problems.append(f'SOURCES.json: fixes left out of a release: {report["fixes_skipped"]}')
    if not report.get('fixes'):
        problems.append('SOURCES.json: no fixes applied')
    return problems


def check(version, elf=None, binary=None, sources_json=None):
    problems = version_problems(version)
    if problems:
        return problems
    for path in (elf, binary):
        if path is not None:
            problems += binary_problems(Path(path).read_bytes(), version, Path(path).name)
    if sources_json is not None:
        problems += sources_problems(json.loads(Path(sources_json).read_text(encoding='utf-8')), version)
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--version', required=True)
    parser.add_argument('--elf', type=Path)
    parser.add_argument('--bin', type=Path)
    parser.add_argument('--sources-json', type=Path)
    args = parser.parse_args(argv)
    problems = check(args.version, args.elf, args.bin, args.sources_json)
    for problem in problems:
        print('FAIL', problem)
    if problems:
        return 1
    checked = [str(p) for p in (args.elf, args.bin, args.sources_json) if p is not None]
    print(f'PASS: release {args.version}' + (f' ({", ".join(checked)})' if checked else ' (version only)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
