"""Builds the PPSA83600 package of a TrinityX360 release compiled from source (tools/source-1.0.2).

    python3 -I tools/source-1.0.2/package.py --base-zip PPSA50011-v1.0.2.zip \
        --eboot build/src102-out/eboot.bin --lang-dir build/src102-out/lang \
        --sources-json build/src102/SOURCES.json --version 1.0.3 \
        --source-url https://github.com/<owner>/<repo>/tree/<commit> --out-dir dist/source-release

The base is the official PS5X360 v1.0.2 PPSA50011.zip, checked against its SHA-256 as
tools/binary-release/build_package.py checks it. Unlike that script, nothing is patched or
signed again: eboot.bin is the source build's, already signed by build.sh. TrinityX360 (named
PS5X360Reloaded until 1.0.5) has its own title id, PPSA83600, so it installs next to the official
PS5X360 (PPSA50011) instead of replacing it. The rename to TrinityX360 changed only the name: the
title id, conceptId and contentId stay those of PS5X360Reloaded, so the console updates the
installed app and keeps its saves. This script:

  * reads the base's files under PPSA50011/ and writes them under PPSA83600/;
  * rewrites sce_sys/param.json: titleId PPSA83600, conceptId 83600, contentId
    UP9000-PPSA83600_00-PS5X360RELOADED0 (unchanged by the rename), the titleName TrinityX360 in every language
    entry and contentVersion from the release version (content_version(): X.Y.Z ->
    "%02d.%03d.000" % (X, Y * 100 + Z), so 1.0.4 -> 01.004.000), which must be higher than the
    base's and than every contentVersion already shipped (SHIPPED_CONTENT_VERSIONS); every other
    field (masterVersion included) and the file's layout (key order, indent, CRLF) stay the base's;
  * replaces sce_sys/icon0.png, pic0.png and pic1.png with the TrinityX360 art in
    tools/source-1.0.2/package-art (checked by SHA-256 and size, see its README.md);
  * replaces the official user docs that name its folder PPSA50011 (tools/source-1.0.2/package-docs,
    see its README.md): INSTALLATION.md with TrinityX360's own, RELEASE_NOTES.md with
    docs/release-notes/v<version>.md, dlc/README.md and updates/README.md with the folder
    renamed, and leaves out AUTOLOG.md; then refuses the package if any other text file still
    names PPSA50011 (the English-keyed language files included);
  * replaces eboot.bin, after checking the SELF magic and that the binary carries the release's
    version labels ("TrinityX360 v<version>", "BOOT version <version>"), no candidate label
    and no older name's label ("PS5X360 v<version>", "PS5X360Reloaded v<version>");
  * replaces every assets/lang/*.txt with the English-keyed files of the build (fix 25; the
    eboot reads /app0/assets/lang/). It does not merge assets/lang-overrides, because those are
    keyed by Portuguese for the official executable and their texts are already in
    tools/source-1.0.2/english/lang. A base language with no English-keyed file stops the build.
  * takes assets/presets.txt, assets/config/*.toml and assets/patches/*.patch.toml from this
    repository, as the binary release does, and drops readback-auto.off;
  * writes VERSION, RELEASE.json (a source build), licenses/PS5X360-<version>-NOTICE.txt (the
    GPL-3.0 notice), the licence texts the official package lacks (tools/source-1.0.2/licenses),
    and MANIFEST.json;
  * replaces the package's LICENSE, the official MIT License, with the GPL-3.0 text (as this
    repository's root LICENSE) and adds this repository's licenses/PS5X360-MIT.txt, after
    checking that it ends with that MIT notice word for word, so the notice of the PS5X360 code
    stays in the package;
  * writes <out-dir>/PPSA83600.zip (deterministic), <out-dir>/PPSA83600.zip.sha256 and the
    unpacked <out-dir>/stage/PPSA83600. The images come from it with
    tools/binary-release/make_images.sh <out-dir>/stage/PPSA83600 <out-dir> PPSA83600.

The modification date in the NOTICE is --date, else SOURCE_DATE_EPOCH, else today (UTC).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools/binary-release'))
import build_package  # noqa: E402  (constants and helpers only; its main() is not run)
import fself  # noqa: E402

# The official package's folder; the release's own title id and folder.
BASE_PREFIX = build_package.PREFIX
TITLE_ID = 'PPSA83600'
PREFIX = TITLE_ID + '/'
# sce_sys/param.json of the release. The contentId keeps the official package's form
# (<service id UP9000>-<titleId>_00-<16 uppercase letters or digits>) with a label of its own, so
# the console and ShadowMountPlus see a different app from the official UP9000-PPSA50011_00-...
# The rename to TrinityX360 (1.0.6) kept the title id, conceptId and contentId of PS5X360Reloaded
# (1.0.3 to 1.0.5): a new one would make the console see another app, installed beside the old one
# without its saves. Only titleName changed. tools/check-source-package.py pins all four.
CONCEPT_ID = TITLE_ID[4:]
CONTENT_ID = f'UP9000-{TITLE_ID}_00-PS5X360RELOADED0'
TITLE_NAME = 'TrinityX360'
CONTENT_ID_RE = re.compile(r'UP9000-(PPSA[0-9]{5})_00-[A-Z0-9]{16}')
# The home-screen label of the release (build_version.hpp: fix 46's product name, as the TrinityX360
# fix renames it) and the older names' labels, which a TrinityX360 eboot must not carry: the
# official app's and PS5X360Reloaded's.
HOME_LABEL = b'TrinityX360 v'
OFFICIAL_LABEL_RE = re.compile(rb'PS5X360(?:Reloaded)? v[0-9]')
BASE_ZIP_SHA256 = build_package.BASE_ZIP_SHA256
BASE_VERSION = build_package.BASE_VERSION
UPSTREAM = build_package.UPSTREAM
ZIP_TIME = build_package.ZIP_TIME
sha256 = build_package.sha256

VERSION_RE = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:\.[1-9][0-9]{0,2})?')  # X.Y.Z or a hotfix X.Y.Z.W
# sce_sys/param.json contentVersion, "NN.NNN.NNN". The console and the homebrew catalogs compare it
# with the installed app's to tell an outdated install, so each release needs a higher one.
CONTENT_VERSION_RE = re.compile(r'[0-9]{2}\.[0-9]{3}\.[0-9]{3}')
# contentVersion of every release already published (named PS5X360Reloaded then): v1.0.3 kept the official
# package's value. A new release's contentVersion must be higher than all of them.
SHIPPED_CONTENT_VERSIONS = {'1.0.3': '00.001.000', '1.0.4': '01.004.000', '1.0.4.1': '01.004.001',
                            '1.0.4.2': '01.004.002'}
# A test build's label (BUILD_VERSION 1.0.3_candidate_N). A bare "candidate_" is not enough:
# Mesa's own strings contain it (intersection_candidate_aabb_opaque, candidate_infos).
CANDIDATE_LABEL_RE = re.compile(rb'[0-9]+\.[0-9]+\.[0-9]+_candidate')
# First line of every file tools/source-1.0.2/english/rekey.py makes.
ENGLISH_HEADER_RE = re.compile(r'# PS5X360 interface text: (\S+)\. Keys are English')
SOURCE_URL_RE = re.compile(r'(https://[^/]+/[^/]+/[^/]+)/(?:tree|commit)/([0-9a-f]{40})/?')
WORKFLOW = ROOT / '.github/workflows/build-source-1.0.2.yml'

# The package's LICENSE: the GPL-3.0 text (GPL-3.0.txt below, also this repository's root
# LICENSE), in place of the official package's MIT License. That notice moves to
# licenses/PS5X360-MIT.txt: this repository's file of that name, a short header and then the
# official LICENSE word for word (checked, so a changed notice stops the packaging).
LICENSE_TEXT = 'GPL-3.0.txt'
MIT_NOTICE = 'licenses/PS5X360-MIT.txt'
# (file in tools/source-1.0.2/licenses, path in the package, SHA-256); see licenses/README.md.
LICENCES = [
    ('GPL-3.0.txt', 'licenses/GPL-3.0.txt',
     '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'),
    ('MINIUPNPC-BSD-3.txt', 'licenses/MINIUPNPC.txt',
     'ae63874b77b3d9c58ab99b2d116d83dca5f8c79d3f7d16d6bf7c23068f640fb5'),
    ('MBEDTLS-3.6.7-LICENSE.txt', 'licenses/MBEDTLS-3.6.7-LICENSE.txt',
     '9b405ef4c89342f5eae1dd828882f931747f71001cfba7d114801039b52ad09b'),
    ('MPL-2.0.txt', 'licenses/CACERT-MPL-2.0.txt',
     '3f3d9e0024b1921b067d6f7f88deb4a60cbe7a78e76c64e3f1d7fc3b779b9d04'),
]

# (file in tools/source-1.0.2/package-art, path in the package, (width, height), SHA-256); see
# package-art/README.md. The sizes are the official package's.
ART = [
    ('icon0.png', 'sce_sys/icon0.png', (512, 512),
     '7b55e75d4293b5bbb01b40c5831855bb626cab80d149a7a01f4c2adeecf7b26f'),
    ('pic0.png', 'sce_sys/pic0.png', (3840, 2160),
     'e0541c1987d983bb50ca04eb7b2c653afc4316f63d84620c7017cdacbfe5d350'),
    ('pic1.png', 'sce_sys/pic1.png', (1920, 1080),
     'd25b1bb70a158f1a07825a3b5bf0633469af8ae62e5cd791f004db9d54b90a62'),
]
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
# The package's files the repository also carries, at its root, in the commit a release tag points
# at: homebrew catalogs and stores (github.com/darkxex/ps5-homebrew-catalog, docs/versioning.md)
# read the title id, the name and contentVersion from sce_sys/param.json and the icon from
# sce_sys/icon0.png in the repository, so the committed copies must be the release's bytes.
# tools/source-1.0.2/repo_sce_sys.py writes them (--write) and checks them (--check); the release
# workflow stops when they are not the package's.
REPO_SCE_SYS = ('sce_sys/param.json', 'sce_sys/icon0.png')

# The official package's user docs name its folder PPSA50011 (copy the folder there, put DLC and
# updates in PPSA50011/..., logs in /data/homebrew/PPSA50011/logs); shipped as they are, they
# would send TrinityX360's files into the official app's folder. See package-docs/README.md.
# INSTALLATION.md: TrinityX360's own (package-docs/INSTALLATION.md, placeholders filled in).
# RELEASE_NOTES.md: the release's notes, docs/release-notes/v<version>.md.
# (path, times the official text names PPSA50011): that text with the folder renamed.
DOC_RENAMES = [('dlc/README.md', 1), ('updates/README.md', 1)]
# Left out: the official AutoLog payload's guide (it reads /data/homebrew/PPSA50011/logs only).
DOCS_LEFT_OUT = ['AUTOLOG.md']
# The package's files that may name the official title id: the notices of where the package came
# from, the docs that tell TrinityX360 apart from it, and binaries (eboot.bin reads the
# official folder for the first start's import; the official Kinect tracker and its source).
# Every other text file naming it stops the packaging.
# (licenses/PS5X360-<version>-NOTICE.txt too.)
OFFICIAL_ID_ALLOWED = {'INSTALLATION.md', 'RELEASE_NOTES.md', 'RELEASE.json', 'eboot.bin',
                       'tools/kinect-tracker.elf', 'KINECT-source-and-licenses.zip'}
TEXT_SUFFIXES = ('.md', '.txt', '.json', '.toml', '.conf', '.ini', '.cfg', '.html', '.xml')


def fail(message):
    raise SystemExit('package.py: ' + message)


def check_version(version):
    if not VERSION_RE.fullmatch(version or ''):
        fail(f'version {version!r} is not X.Y.Z or X.Y.Z.W (a release, not a candidate label)')
    return version


def content_version(version):
    """sce_sys/param.json contentVersion of release X.Y.Z: "%02d.%03d.000" % (X, Y * 100 + Z), so
    1.0.4 is 01.004.000 and 1.2.15 is 01.215.000; of a hotfix X.Y.Z.W the third field is W, so
    1.0.4.1 is 01.004.001, above 1.0.4 and below 1.0.5. Strictly increasing with the version while
    X <= 99, Y <= 9, Z <= 99 and W <= 999; another version stops the packaging instead of
    wrapping around."""
    parts = [int(part) for part in check_version(version).split('.')]
    major, minor, patch = parts[:3]
    hotfix = parts[3] if len(parts) == 4 else 0
    if major > 99 or minor > 9 or patch > 99 or hotfix > 999:
        fail(f'version {version} does not fit contentVersion NN.NNN.NNN (X <= 99, Y <= 9, Z <= 99, W <= 999)')
    return f'{major:02d}.{minor * 100 + patch:03d}.{hotfix:03d}'


def check_content_version(value, base_value):
    """The derived contentVersion must be higher than the base package's and every shipped one
    (SHIPPED_CONTENT_VERSIONS), or an installed older release would not show as outdated.
    The fixed-width form compares as text."""
    others = [('the base package', base_value)]
    others += [(f'v{shipped}', other) for shipped, other in sorted(SHIPPED_CONTENT_VERSIONS.items())]
    for name, other in others:
        if not CONTENT_VERSION_RE.fullmatch(other if isinstance(other, str) else ''):
            fail(f'contentVersion {other!r} of {name} is not NN.NNN.NNN')
        if value <= other:
            fail(f'contentVersion {value} is not higher than {other} of {name}')
    return value


def source_names(version):
    """The GPL source assets release-source.yml attaches: the assembled tree that was compiled
    (made by build-source-1.0.2.yml), the toolchain repositories at the build's pinned commits
    and the git archive of this repository at the commit."""
    return {'tarball': f'PS5X360-{version}-source.tar.xz',
            'toolchain': f'PS5X360-{version}-toolchain-source.tar.xz',
            'scripts': f'TrinityX360-{version}-scripts.tar.gz'}


def parse_source_url(url):
    """(repository URL, 40-hex commit) of https://<host>/<owner>/<repo>/tree|commit/<sha>."""
    match = SOURCE_URL_RE.fullmatch(url or '')
    if not match:
        fail(f'--source-url {url!r} is not https://<host>/<owner>/<repo>/tree/<40-hex commit>')
    return match.group(1), match.group(2)


def modification_date(value=None, environ=os.environ):
    if value:
        try:
            datetime.date.fromisoformat(value)
        except ValueError:
            fail(f'--date {value!r} is not YYYY-MM-DD')
        return value
    epoch = environ.get('SOURCE_DATE_EPOCH')
    if epoch:
        if not epoch.isdigit():
            fail(f'SOURCE_DATE_EPOCH {epoch!r} is not a number of seconds')
        stamp = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
    else:
        stamp = datetime.datetime.now(datetime.timezone.utc)
    return stamp.date().isoformat()


def toolchain_pins(workflow_text):
    """The pins of build-source-1.0.2.yml: the three mihawk-99 reference commits (required) and
    the Vulkan SDK and zlib archives (when found)."""
    pins = {}
    for key, name in (('PS5_VULKAN_REV', 'PS5_Vulkan'), ('PS5_MESA_REV', 'PS5_Mesa'),
                      ('PS5_PAYLOAD_SDK_REV', 'PS5_PayloadSDK')):
        match = re.search(rf'^\s*{key}:\s*([0-9a-f]{{40}})\s*$', workflow_text, re.M)
        if not match:
            fail(f'{WORKFLOW.name}: no {key} pin')
        pins[name] = {'url': f'https://github.com/mihawk-99/{name}', 'commit': match.group(1)}
    sdk = re.search(r'\bv=([0-9.]+)\s*$', workflow_text, re.M)
    sdk_sha = re.search(r'"([0-9a-f]{64})  \$RUNNER_TEMP/\$file"', workflow_text)
    if sdk and sdk_sha:
        pins['Vulkan SDK'] = {'version': sdk.group(1), 'sha256': sdk_sha.group(1)}
    zlib = re.search(r'zlib-([0-9.]+)\.tar\.gz"\s', workflow_text)
    zlib_sha = re.search(r'"([0-9a-f]{64})  \$zlib_dir/', workflow_text)
    if zlib and zlib_sha:
        pins['zlib'] = {'version': zlib.group(1), 'sha256': zlib_sha.group(1)}
    runner = re.search(r'^\s*runs-on:\s*(\S+)', workflow_text, re.M)
    pins['host'] = {'runner': runner.group(1) if runner else 'ubuntu-24.04',
                    'compiler': 'clang-18 / lld-18 through the payload SDK prospero-clang wrappers'}
    return pins


def check_eboot(eboot, version):
    if len(eboot) < 4 or int.from_bytes(eboot[:4], 'little') not in fself.FSELF_MAGICS:
        fail('eboot.bin is not a signed PS5 SELF (build.sh output expected)')
    for label in (HOME_LABEL + version.encode(), b'BOOT version ' + version.encode()):
        if label + b'\0' not in eboot:
            fail(f'eboot.bin does not carry the label {label.decode()!r}: built with another version')
    found = CANDIDATE_LABEL_RE.search(eboot)
    if found:
        fail(f'eboot.bin carries a candidate label ({eboot[found.start():found.end() + 8]!r})')
    found = OFFICIAL_LABEL_RE.search(eboot)
    if found:
        fail(f'eboot.bin carries an older name\'s label ({eboot[found.start():found.end() + 8]!r}): '
             'not built as TrinityX360')


def param_json(base, version):
    """The release's sce_sys/param.json from the official one: new titleId, conceptId, contentId,
    titleName in every localizedParameters language and contentVersion (content_version()); the
    rest, masterVersion included, the key order, the indent and the line endings stay the base's."""
    try:
        param = json.loads(base)
    except ValueError as error:
        fail(f'base sce_sys/param.json is not JSON ({error})')
    if not isinstance(param, dict):
        fail('base sce_sys/param.json is not a JSON object')
    if param.get('titleId') != BASE_PREFIX.rstrip('/'):
        fail(f'base sce_sys/param.json titleId {param.get("titleId")!r} is not {BASE_PREFIX.rstrip("/")}')
    for key in ('conceptId', 'contentId', 'contentVersion'):
        if not isinstance(param.get(key), str):
            fail(f'base sce_sys/param.json has no {key}')
    value = check_content_version(content_version(version), param['contentVersion'])
    localized = param.get('localizedParameters')
    languages = [code for code, entry in (localized or {}).items() if isinstance(entry, dict)] \
        if isinstance(localized, dict) else []
    if not languages or localized.get('defaultLanguage') not in languages:
        fail('base sce_sys/param.json has no localizedParameters entry for its defaultLanguage')
    match = CONTENT_ID_RE.fullmatch(CONTENT_ID)
    if not match or match.group(1) != TITLE_ID or not CONCEPT_ID.isdigit() or TITLE_ID[4:] != CONCEPT_ID:
        fail(f'contentId {CONTENT_ID!r} / conceptId {CONCEPT_ID!r} do not fit titleId {TITLE_ID}')
    param['titleId'] = TITLE_ID
    param['conceptId'] = CONCEPT_ID
    param['contentId'] = CONTENT_ID
    param['contentVersion'] = value
    for code in languages:
        localized[code]['titleName'] = TITLE_NAME
    newline = '\r\n' if b'\r\n' in base else '\n'
    text = json.dumps(param, indent=2).replace('\n', newline)
    if base.rstrip(b' \t').endswith(b'\n'):
        text += newline
    return text.encode()


def png_size(data):
    """(width, height) of a PNG, from its IHDR chunk."""
    if len(data) < 24 or data[:8] != PNG_SIGNATURE or data[12:16] != b'IHDR':
        return None
    return int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')


def art_files():
    files = {}
    for name, target, size, digest in ART:
        data = (HERE / 'package-art' / name).read_bytes()
        if sha256(data) != digest:
            fail(f'tools/source-1.0.2/package-art/{name} is not the reviewed file (SHA-256 differs)')
        if png_size(data) != size:
            fail(f'tools/source-1.0.2/package-art/{name} is not a {size[0]}x{size[1]} PNG')
        files[target] = data
    return files


def read_base_zip(path):
    """The official PPSA50011.zip's files ({name: bytes}), after its SHA-256 check."""
    base = Path(path).read_bytes()
    if sha256(base) != BASE_ZIP_SHA256:
        fail(f'{path} is not the official v{BASE_VERSION} PPSA50011.zip')
    with zipfile.ZipFile(path) as zin:
        return {i.filename: zin.read(i) for i in zin.infolist() if not i.is_dir()}


def repo_sce_sys(base_files, version):
    """{path at the repository root: bytes} of REPO_SCE_SYS for release `version`: the same bytes
    build() puts under PPSA83600/ (param_json() of the base's param.json, the reviewed icon0.png)."""
    check_version(version)
    param = base_files.get(BASE_PREFIX + 'sce_sys/param.json')
    if param is None:
        fail('base package has no sce_sys/param.json')
    files = {'sce_sys/param.json': param_json(param, version),
             'sce_sys/icon0.png': art_files()['sce_sys/icon0.png']}
    assert tuple(files) == REPO_SCE_SYS
    return files


def check_repo_sce_sys(expected, version, repo_root=ROOT):
    """Stops when a REPO_SCE_SYS file at the repository root is missing or is not `expected`'s."""
    wrong = []
    for name, data in expected.items():
        path = Path(repo_root) / name
        if not path.is_file():
            wrong.append(f'{name} is missing')
        elif path.read_bytes() != data:
            wrong.append(f"{name} is not the v{version} package's")
    if wrong:
        fail('; '.join(wrong) + ' in the repository: run python3 -I tools/source-1.0.2/repo_sce_sys.py '
             f'--base-zip <official PPSA50011.zip> --version {version} --write and commit sce_sys/')


def package_docs(files, version, repo, commit, repo_root=ROOT):
    """Replaces the official user docs that name PPSA50011 (DOC_RENAMES, DOCS_LEFT_OUT, see
    package-docs/README.md) in `files` ({name under PREFIX: bytes})."""
    official = BASE_PREFIX.rstrip('/')
    for name, count in DOC_RENAMES:
        data = files.get(PREFIX + name)
        if data is None:
            fail(f'base package has no {name}')
        if data.count(official.encode()) != count:
            fail(f'base package {name} names {official} {data.count(official.encode())} times, not {count}')
        files[PREFIX + name] = data.replace(official.encode(), TITLE_ID.encode())
    for name in DOCS_LEFT_OUT:
        files.pop(PREFIX + name, None)
    template = (HERE / 'package-docs/INSTALLATION.md').read_text(encoding='utf-8')
    for key in ('@VERSION@', '@REPOSITORY@', '@COMMIT@'):
        if key not in template:
            fail(f'tools/source-1.0.2/package-docs/INSTALLATION.md has no {key}')
    text = template.replace('@VERSION@', version).replace('@REPOSITORY@', repo).replace('@COMMIT@', commit)
    files[PREFIX + 'INSTALLATION.md'] = text.encode()
    notes = repo_root / f'docs/release-notes/v{version}.md'
    if not notes.is_file():
        fail(f'no release notes {notes.relative_to(repo_root).as_posix()} for RELEASE_NOTES.md')
    files[PREFIX + 'RELEASE_NOTES.md'] = notes.read_bytes()


def official_id_problems(files, version):
    """Text files of the package (not OFFICIAL_ID_ALLOWED or the release's NOTICE) that still name
    the official title id."""
    official = BASE_PREFIX.rstrip('/').encode()
    allowed = OFFICIAL_ID_ALLOWED | {f'licenses/PS5X360-{version}-NOTICE.txt'}
    return sorted(name[len(PREFIX):] for name, data in files.items()
                  if name[len(PREFIX):] not in allowed
                  and name.lower().endswith(TEXT_SUFFIXES) and official in data)


def check_sources(sources, version):
    if sources.get('version') != version:
        fail(f'SOURCES.json version {sources.get("version")!r} is not {version!r}')
    if sources.get('release') is not True:
        fail('SOURCES.json is not from a release assembly (assemble.py --release)')
    if 'fixes_skipped' not in sources:
        fail('SOURCES.json has no fixes_skipped list')
    if sources['fixes_skipped'] != []:
        fail(f'SOURCES.json lists skipped fixes: {sources["fixes_skipped"]}')
    for key in ('drops', 'core', 'fixes'):
        if not sources.get(key):
            fail(f'SOURCES.json has no {key!r} (not a full assembly)')


def english_lang(lang):
    """Checks {name: bytes} are the English-keyed files of tools/source-1.0.2/english/lang."""
    if not lang:
        fail('no language files (*.txt) in --lang-dir')
    for name, data in sorted(lang.items()):
        first = data.split(b'\n', 1)[0].decode('utf-8', 'replace')
        match = ENGLISH_HEADER_RE.match(first)
        if not match or f'{match.group(1)}.txt' != name:
            fail(f'{name} is not an English-keyed language file of tools/source-1.0.2/english/lang')
    return lang


def licence_texts():
    texts = {}
    for name, target, digest in LICENCES:
        data = (HERE / 'licenses' / name).read_bytes()
        if sha256(data) != digest:
            fail(f'tools/source-1.0.2/licenses/{name} is not the verbatim text (SHA-256 differs)')
        texts[target] = data
    return texts


def licence_files(base_license, repo_root=ROOT):
    """{package path: bytes}: LICENSE, the GPL-3.0 text, and licenses/PS5X360-MIT.txt, this
    repository's file. The official package's LICENSE, the MIT notice of the PS5X360 code, must
    be the end of licenses/PS5X360-MIT.txt, after its header, word for word (line ends aside)."""
    if base_license is None:
        fail('base package has no LICENSE')
    gpl = licence_texts()['licenses/' + LICENSE_TEXT]
    path = repo_root / MIT_NOTICE
    if not path.is_file():
        fail(f'no {MIT_NOTICE} in the repository (the MIT notice of the PS5X360 code)')
    mit = path.read_bytes().replace(b'\r\n', b'\n')
    official = base_license.replace(b'\r\n', b'\n')
    if not official.startswith(b'MIT License\n') or b'Permission is hereby granted' not in official:
        fail('base package LICENSE is not the MIT License of PS5X360')
    header = mit[:len(mit) - len(official)]
    if not mit.endswith(b'\n\n' + official) or b'Permission is hereby granted' in header:
        fail(f'{MIT_NOTICE} does not end with the base package\'s LICENSE (the MIT notice) word for word')
    return {'LICENSE': gpl, MIT_NOTICE: mit}


def notice(version, date, repo, commit, eboot_sha, sources, toolchain, sources_sha):
    release = f'{repo}/releases/tag/v{version}'
    names = source_names(version)
    tarball, toolchain_tar, scripts = names['tarball'], names['toolchain'], names['scripts']
    drops = '\n'.join(f'        {info["asset"]} ({tag})  sha256 {info["sha256"]}'
                      for tag, info in sorted(sources['drops'].items()))
    online = sources.get('online', {})
    net_libs = '\n'.join(f'        {path}  {rev}'
                         for path, rev in sorted(online.get('net_libs', {}).items()))
    mbedtls = online.get('mbedtls', {})
    fixes = '\n'.join(f'        {Path(fix["patch"]).name}  sha256 {fix["sha256"]}' for fix in sources['fixes'])
    pins = []
    for name, pin in toolchain.items():
        if 'commit' in pin:
            pins.append(f'        {name}  {pin["url"]} commit {pin["commit"]}')
        elif 'sha256' in pin:
            pins.append(f'        {name} {pin["version"]}  sha256 {pin["sha256"]}')
        else:
            pins.append(f'        build host  {pin["runner"]}, {pin["compiler"]}')
    pins = '\n'.join(pins)
    return f"""TrinityX360 {version} ({TITLE_ID}) - built from source, distributed under the GNU GPL version 3

Licence of eboot.bin
    eboot.bin as a whole is distributed under the terms of the GNU General Public License
    version 3 (licenses/GPL-3.0.txt). It links wolfSSL 5.9.4 (GPL-3.0-or-later,
    licenses/WOLFSSL-GPL-3.txt), ps5-framegen (GPL-3.0-or-later, licenses/PS5-FRAMEGEN-GPL-3.txt)
    and the GPL-3.0 PS5 runtime of PS5_Vulkan and the payload SDK (licenses/PS5-VULKAN-GPL-3.txt,
    licenses/NATIVE-RUNTIME-GPL-3.txt); LICENSE is the GPL-3.0 text too. Each part keeps its own
    licence as well: the PS5X360 code came under the MIT License and keeps its notice
    (licenses/PS5X360-MIT.txt), Xenia is BSD-3-Clause (licenses/XENIA-BSD.txt,
    licenses/XENIA-NETPLAY-BSD.txt), RADV/Mesa is MIT (licenses/MESA-LICENSE.txt), libcurl is
    under the curl licence (licenses/CURL.txt), miniupnpc is BSD-3-Clause (licenses/MINIUPNPC.txt), Mbed TLS 3.6.7 is
    used under Apache-2.0 (licenses/MBEDTLS-3.6.7-LICENSE.txt), and the embedded Mozilla CA
    certificate bundle (cacert.pem, from curl.se) is MPL-2.0 (licenses/CACERT-MPL-2.0.txt).
    The other licence files in licenses/, kept from the official package, cover further
    third-party code of Xenia Canary and of the package, and apply to the parts they name.
    There is NO WARRANTY, to the extent permitted by law (GPL-3.0 sections 15 and 16).

Modified version (GPL-3.0 section 5)
    This is a modified version of PS5X360 {BASE_VERSION} by BrinooTk ({UPSTREAM}) and of
    Xenia Canary netplay, modified by the contributors of {repo}; modified {date}.
    eboot.bin was compiled from source: the author's public source drops, assembled by
    tools/source-1.0.2/assemble.py, with reconstructed headers for parts that were never
    published and the fixes listed below. It is not the official {BASE_VERSION} executable.

Built from
    Repository   {repo} commit {commit}
    eboot.bin    sha256 {eboot_sha}
    SOURCES.json sha256 {sources_sha} (where every source file came from)
    Source drops (BrinooTk/PS5X360 releases):
{drops}
    Core         {sources['core'].get('base', '')}
    Network libraries (netplay submodules):
{net_libs}
    Mbed TLS     {mbedtls.get('url', '')}  sha256 {mbedtls.get('sha256', '')}
    Fixes applied (tools/source-1.0.2/fixes):
{fixes}
    Toolchain:
{pins}

Corresponding source (GPL-3.0 section 6)
    Attached to the release {release}:
        {tarball}
            the assembled tree build.sh compiled (tools/source-1.0.2/assemble.py): the title,
            the Xenia Canary netplay core with the submodules it uses, the network libraries,
            Mbed TLS, the fixes applied and SOURCES.json
        {scripts}
            the scripts that control compilation and installation: this repository
            ({repo}) at commit {commit}, with tools/source-1.0.2 (assemble.py,
            build.sh, package.py), tools/build-radv-platform.sh,
            tools/binary-release/make_images.sh and .github/workflows
    GitHub's source archive of the tag v{version} on the release page is the same commit.
    Not in {tarball}: the parts linked into eboot.bin from the toolchain repositories, RADV
    (Mesa, from PS5_Mesa, built by tools/build-radv-platform.sh), app_crt.cpp and the AGC import
    stubs (PS5_Vulkan) and the payload SDK runtime (PS5_PayloadSDK), and the tools that link and
    sign it (radv-link.sh and ps5-native-tool from PS5_Vulkan). Their source is those public
    repositories at the commits listed under Toolchain above, attached as {toolchain_tar}
    (PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK without .git, and PINS.txt).

Not linked into this eboot.bin
    The OpenGL renderer stack (Vulkan only), the physical USB Kinect and its relay, and the disc
    helper (disc-helper.elf relay) are NOT part of this executable. licenses/PS5-OpenGL,
    KINECT-source-and-licenses.zip and tools/kinect-tracker.elf are kept from the official
    package unchanged.

Other files
    Every other file is the official {BASE_VERSION} PPSA50011.zip (sha256 {BASE_ZIP_SHA256})
    unchanged, moved from PPSA50011/ to {PREFIX}, except: sce_sys/param.json (title id
    {TITLE_ID}, content id {CONTENT_ID}, name {TITLE_NAME}, so this
    app installs next to the official PS5X360 instead of replacing it; content version
    {content_version(version)}, so an older install shows as outdated), sce_sys/icon0.png,
    pic0.png and pic1.png (TrinityX360's own art, tools/source-1.0.2/package-art),
    the user docs that named the official app's folder (tools/source-1.0.2/package-docs:
    INSTALLATION.md is TrinityX360's own, RELEASE_NOTES.md is this release's notes,
    dlc/README.md and updates/README.md name {PREFIX} instead, AUTOLOG.md is left out),
    assets/lang/*.txt (English-keyed, tools/source-1.0.2/english/lang),
    assets/presets.txt, assets/config/*.toml and assets/patches/*.patch.toml (from the
    repository), VERSION, RELEASE.json, MANIFEST.json, this notice, the licence files above
    (tools/source-1.0.2/licenses/README.md says where each came from) and LICENSE: the official
    LICENSE, the MIT License, is in licenses/PS5X360-MIT.txt under a header, and LICENSE is the
    GPL-3.0 text.
    licenses/PS5X360-NOTICE.txt is the official 1.0.0 notice, kept as provenance.
""".encode()


def build(base_files, eboot, lang, sources, version, source_url, date, toolchain, sources_sha,
          repo_root=ROOT):
    """The package's files ({zip name under PPSA83600/: bytes}) from the official base's files
    (under PPSA50011/). `sources` is the parsed SOURCES.json and `sources_sha` the SHA-256 of its
    bytes."""
    check_version(version)
    repo, commit = parse_source_url(source_url)
    check_eboot(eboot, version)
    check_sources(sources, version)
    english_lang(lang)
    outside = sorted(name for name in base_files if not name.startswith(BASE_PREFIX))
    if outside:
        fail(f'base package files outside {BASE_PREFIX}: {", ".join(outside[:5])}')
    files = {PREFIX + name[len(BASE_PREFIX):]: data for name, data in base_files.items()}
    if PREFIX + 'sce_sys/param.json' not in files:
        fail('base package has no sce_sys/param.json')
    files[PREFIX + 'sce_sys/param.json'] = param_json(files[PREFIX + 'sce_sys/param.json'], version)
    for target, data in art_files().items():
        files[PREFIX + target] = data
    if files.get(PREFIX + 'VERSION') != (BASE_VERSION + '\n').encode():
        fail('base package VERSION is not ' + BASE_VERSION)
    if PREFIX + 'eboot.bin' not in files:
        fail('base package has no eboot.bin')
    base_lang = sorted(name for name in files
                       if name.startswith(PREFIX + 'assets/lang/') and name.endswith('.txt'))
    if not base_lang:
        fail('base package has no assets/lang/*.txt')
    missing = [name for name in base_lang if name.rsplit('/', 1)[1] not in lang]
    if missing:
        fail(f'no English-keyed file for {", ".join(missing)}')

    eboot_sha = sha256(eboot)
    files[PREFIX + 'eboot.bin'] = eboot
    for name in base_lang:
        del files[name]
    for name, data in sorted(lang.items()):
        files[PREFIX + 'assets/lang/' + name] = data
    files[PREFIX + 'assets/presets.txt'] = (repo_root / 'assets/presets.txt').read_bytes()
    for path in sorted((repo_root / 'assets/config').glob('*.toml')):
        files[PREFIX + 'assets/config/' + path.name] = path.read_bytes()
    for path in sorted((repo_root / 'assets/patches').glob('*.patch.toml')):
        files[PREFIX + 'assets/patches/' + path.name] = path.read_bytes()
    # As in the binary release: this marker would switch off the copy-back of small resolves.
    files.pop(PREFIX + 'readback-auto.off', None)
    files.pop(PREFIX + 'licenses/PS5X360-OPT-NOTICE.txt', None)
    for target, data in licence_texts().items():
        files[PREFIX + target] = data
    for target, data in licence_files(files.get(PREFIX + 'LICENSE'), repo_root).items():
        files[PREFIX + target] = data

    package_docs(files, version, repo, commit, repo_root)

    files[PREFIX + 'VERSION'] = (version + '\n').encode()
    files[PREFIX + f'licenses/PS5X360-{version}-NOTICE.txt'] = notice(
        version, date, repo, commit, eboot_sha, sources, toolchain, sources_sha)
    files[PREFIX + 'RELEASE.json'] = (json.dumps({
        'version': version,
        'title_id': TITLE_ID,
        'title_name': TITLE_NAME,
        'content_version': content_version(version),
        'executable_sha256': eboot_sha,
        'candidate': False,
        'published': True,
        'candidate_id': f'src-{eboot_sha[:8]}',
        'build': 'source',
        'base_package': {'name': f'PS5X360 v{BASE_VERSION} PPSA50011.zip', 'sha256': BASE_ZIP_SHA256},
        'source_repository': repo,
        'source_commit': commit,
        'source_url': source_url,
        'source_assets': sorted(source_names(version).values()),
        'drops': {tag: info['sha256'] for tag, info in sorted(sources['drops'].items())},
        'core': sources['core'].get('base', ''),
        'fixes': [Path(fix['patch']).name for fix in sources['fixes']],
        'fixes_skipped': [],
        'sources_json_sha256': sources_sha,
        'toolchain': toolchain,
    }, indent=2) + '\n').encode()
    files.pop(PREFIX + 'MANIFEST.json', None)
    manifest = {name[len(PREFIX):]: {'bytes': len(data), 'sha256': sha256(data)}
                for name, data in sorted(files.items())}
    files[PREFIX + 'MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    named = official_id_problems(files, version)
    if named:
        fail(f'files that still name the official title id {BASE_PREFIX.rstrip("/")}: {", ".join(named)} '
             '(English-keyed language files: tools/source-1.0.2/english/rekey.py RENAMES)')
    return files


def write_package(files, out_dir):
    """Writes the deterministic PPSA83600.zip (as build_package.py does), checks it, extracts
    <out_dir>/stage and writes PPSA83600.zip.sha256. Returns the zip's SHA-256."""
    out_dir.mkdir(parents=True, exist_ok=True)
    out_zip = out_dir / f'{TITLE_ID}.zip'
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED) as zout:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            # Unix (3) on every host: zipfile's default is 0 on Windows, which changes the bytes
            # and makes unzip ignore the 0644 mode.
            info.create_system = 3
            info.external_attr = 0o644 << 16
            zout.writestr(info, data)
    manifest = json.loads(files[PREFIX + 'MANIFEST.json'])
    with zipfile.ZipFile(out_zip) as zcheck:
        if zcheck.testzip() is not None:
            fail('ZIP CRC check failed')
        if sorted(zcheck.namelist()) != sorted(files):
            fail('ZIP entries differ from the package files')
        for name, record in manifest.items():
            data = zcheck.read(PREFIX + name)
            if len(data) != record['bytes'] or sha256(data) != record['sha256']:
                fail(f'MANIFEST check failed: {name}')
        stage = out_dir / 'stage'
        shutil.rmtree(stage, ignore_errors=True)
        zcheck.extractall(stage)
    digest = sha256(out_zip.read_bytes())
    (out_dir / f'{TITLE_ID}.zip.sha256').write_text(f'{digest}  {TITLE_ID}.zip\n', encoding='utf-8')
    return digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--base-zip', type=Path, required=True, help='official v1.0.2 PPSA50011.zip')
    parser.add_argument('--eboot', type=Path, required=True, help='eboot.bin of the source build')
    parser.add_argument('--lang-dir', type=Path, required=True,
                        help='English-keyed *.txt of the build (src102-out/lang)')
    parser.add_argument('--sources-json', type=Path, required=True, help='SOURCES.json of the build')
    parser.add_argument('--version', required=True, help='release version X.Y.Z')
    parser.add_argument('--source-url', required=True,
                        help='https://<host>/<owner>/<repo>/tree/<commit> the build was made from')
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--date', help='modification date YYYY-MM-DD for the notice '
                                       '(default: SOURCE_DATE_EPOCH, else today in UTC)')
    args = parser.parse_args(argv)

    version = check_version(args.version)
    base_files = read_base_zip(args.base_zip)
    sources_bytes = args.sources_json.read_bytes()
    sources = json.loads(sources_bytes)
    lang = {path.name: path.read_bytes() for path in sorted(args.lang_dir.glob('*.txt'))}
    files = build(base_files, args.eboot.read_bytes(), lang, sources, version, args.source_url,
                  modification_date(args.date), toolchain_pins(WORKFLOW.read_text(encoding='utf-8')),
                  sha256(sources_bytes))
    digest = write_package(files, args.out_dir)
    out_zip = args.out_dir / f'{TITLE_ID}.zip'
    print(json.dumps({'version': version, 'title_id': TITLE_ID, 'content_version': content_version(version),
                      'package': str(out_zip), 'files': len(files),
                      'bytes': out_zip.stat().st_size, 'sha256': digest,
                      'eboot_sha256': sha256(files[PREFIX + 'eboot.bin'])}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
