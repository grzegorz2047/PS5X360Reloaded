"""Host check of tools/source-1.0.2/package.py, the packager of a release built from source.

    python3 -I tools/check-source-package.py

Runs on a small made-up base PPSA50011.zip (the official one is 86 MB and pinned by SHA-256;
the check swaps the pinned hash for the fixture's to run the command line end to end) that
carries the official sce_sys/param.json verbatim. It checks:
  * the layout is the base's, moved from PPSA50011/ to PPSA83600/ (TrinityX360's title id, the
    one it had as PS5X360Reloaded until 1.0.5), with eboot.bin replaced byte for byte (not
    patched, not re-signed);
  * sce_sys/param.json differs from the base in exactly titleId PPSA83600, conceptId 83600,
    contentId UP9000-PPSA83600_00-PS5X360RELOADED0, titleName TrinityX360 in every language and
    contentVersion, keeping the base's key order, indent, CRLF and masterVersion; a base param
    that is not the official app's is refused. The rename to TrinityX360 kept the title id,
    conceptId and contentId of PS5X360Reloaded (pinned with package.py's constants), so the
    console updates the installed app and keeps its saves;
  * contentVersion comes from the release version, X.Y.Z -> "%02d.%03d.000" % (X, Y * 100 + Z)
    (1.0.4 -> 01.004.000), is strictly increasing over a run of versions, is above v1.0.3's
    shipped 00.001.000, and a version that does not fit, a base or shipped value that is not
    lower or not NN.NNN.NNN, is refused;
  * LICENSE is the GPL-3.0 text (the repository's root LICENSE too), and
    licenses/PS5X360-MIT.txt is the repository's file, ending with the official package's
    LICENSE (the MIT notice) verbatim; a base without that LICENSE, a changed MIT notice or a
    repository without the file is refused;
  * sce_sys/icon0.png, pic0.png and pic1.png are the reviewed TrinityX360 art (by SHA-256)
    from tools/source-1.0.2/package-art, at the official sizes;
  * the official user docs that name PPSA50011 are not shipped as they are
    (tools/source-1.0.2/package-docs): INSTALLATION.md is TrinityX360's own with the version,
    repository and commit filled in, RELEASE_NOTES.md is docs/release-notes/v<version>.md,
    dlc/README.md and updates/README.md name PPSA83600/ instead, AUTOLOG.md is left out, and no
    other file names PPSA50011 except the notices of where the package came from;
  * every assets/lang/*.txt is the build's English-keyed file, pt-BR.txt is added, and nothing of
    the Portuguese-keyed base files or assets/lang-overrides is merged in;
  * presets, configs and patches come from the repository; readback-auto.off and the binary
    release's OPT notice are absent;
  * VERSION, RELEASE.json, the GPL notice and the added licence texts (verbatim, by SHA-256);
  * MANIFEST.json lists every other file, the stage matches the zip, the outputs are
    PPSA83600.zip, PPSA83600.zip.sha256 and stage/PPSA83600, and two runs give the same zip byte
    for byte;
  * the home-screen label package.py wants is check_release_elf.py's ("TrinityX360 v");
  * it refuses: a candidate or malformed version, an eboot with another or a candidate label, with
    an older name's label (the official app's "PS5X360 v", "PS5X360Reloaded v") or without the
    SELF magic, base files outside PPSA50011/, a base without param.json, art that is not the reviewed files, an official doc
    that changed or is missing, a text file (a language file, a repository preset) still naming
    PPSA50011, a release without its notes, skipped fixes, a SOURCES.json of another version, a base language
    without an English-keyed file, a Portuguese-keyed file, a source URL without a commit and a
    base zip other than the official one.
"""
import hashlib
import io
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools/source-1.0.2'))
import package  # noqa: E402
import check_release_elf  # noqa: E402

B = package.BASE_PREFIX
P = package.PREFIX
assert B == 'PPSA50011/' and P == 'PPSA83600/', (B, P)
VERSION = '1.0.6'
# sce_sys/param.json of the official v1.0.2 PPSA50011.zip, verbatim (881 bytes, CRLF).
OFFICIAL_PARAM = ('{\r\n  "ageLevel": {\r\n    "default": 0\r\n  },\r\n  "applicationCategoryType": 0,\r\n'
                  '  "applicationDrmType": "free",\r\n  "attribute": 0,\r\n  "attribute2": 0,\r\n'
                  '  "attribute3": 0,\r\n  "conceptId": "50011",\r\n  "contentBadgeType": 1,\r\n'
                  '  "contentId": "UP9000-PPSA50011_00-XBOX360PS5M80001",\r\n  "contentVersion": "00.001.000",\r\n'
                  '  "downloadDataSize": 256,\r\n  "gameIntent": {\r\n    "permittedIntents": [\r\n      {\r\n'
                  '        "intentType": "launchActivity"\r\n      }\r\n    ]\r\n  },\r\n'
                  '  "localizedParameters": {\r\n    "defaultLanguage": "en-US",\r\n    "en-US": {\r\n'
                  '      "titleName": "PS5X360"\r\n    }\r\n  },\r\n  "masterVersion": "00.01",\r\n'
                  '  "pubtools": {\r\n    "creationDate": "2026-08-22 00:00:00",\r\n'
                  '    "loudnessSnd0": "-28.00",\r\n    "toolVersion": "2.00"\r\n  },\r\n'
                  '  "requiredSystemSoftwareVersion": "0x0000000000000000",\r\n'
                  '  "sdkVersion": "0x0000000000000000",\r\n  "titleId": "PPSA50011",\r\n'
                  '  "versionFileUri": ""\r\n}\r\n').encode()
# contentVersion of VERSION (1.0.6): 01.006.000, above the 00.001.000 v1.0.3 shipped, v1.0.4's 01.004.000, the
# hotfixes' 01.004.001 (v1.0.4.1) and 01.004.002 (v1.0.4.2), and v1.0.5's 01.005.000.
CONTENT_VERSION = '01.006.000'
# What package.py must make of it: these five lines differ, nothing else.
RELEASE_PARAM = (OFFICIAL_PARAM.replace(b'"conceptId": "50011"', b'"conceptId": "83600"')
                  .replace(b'"contentVersion": "00.001.000"', f'"contentVersion": "{CONTENT_VERSION}"'.encode())
                  .replace(b'"UP9000-PPSA50011_00-XBOX360PS5M80001"', b'"UP9000-PPSA83600_00-PS5X360RELOADED0"')
                  .replace(b'"titleName": "PS5X360"', b'"titleName": "TrinityX360"')
                  .replace(b'"titleId": "PPSA50011"', b'"titleId": "PPSA83600"'))
# The home screen's pictures that carried the PS5X360 logo, replaced by TrinityX360's
# (package-art/skin): file, path in the package, (width, height), SHA-256 of the replacement, and the
# official file's SHA-256 that must never ship.
SKIN = [
    ('skin/xbox-logo.png', 'assets/skin/xbox-logo.png', (300, 196),
     'df0939abce9b60cef25586051b050830ee0dac83c31f5db46b1412e01f0fb534',
     '4ac0244cee08eaa882b5757dd276c746f25aedb1d77c19b739cfedc589eb8ca5'),
    ('skin/basics.jpg', 'assets/skin/basics.jpg', (420, 200),
     '8ce47a0083e137829dfd0a937b013fce92c549e928eeb765687dee131525bafa',
     'b4ce6c522ae9f6dc591ab195319b5d0bb21ecdced746b495526d4e68664d1a4e'),
    ('skin/ball.png', 'assets/skin/ball.png', (96, 96),
     'f02d6eefdd69c58dca4d925ecbd8a77fcb2045a4f70456444b209f639f973aa3',
     'e527495051d09032e624431288a7c0a5e8f4e6c4ca25975460d4c409a34d9654'),
    ('skin/ball-glow.png', 'assets/skin/ball-glow.png', (96, 96),
     'ac14d7d75fe9c0715768e0e3882d971340b3870fa0372fab4a9dbafcb69c4aaf',
     'a04d79843ac04e7417b336bc6e3a7a788ebc335cee776d91778703aa33545d89'),
]
# The reviewed TrinityX360 art (package-art/make_art.py): file, (width, height), SHA-256.
ART = {'icon0.png': ((512, 512), '7b55e75d4293b5bbb01b40c5831855bb626cab80d149a7a01f4c2adeecf7b26f'),
       'pic0.png': ((3840, 2160), 'e0541c1987d983bb50ca04eb7b2c653afc4316f63d84620c7017cdacbfe5d350'),
       'pic1.png': ((1920, 1080), 'd25b1bb70a158f1a07825a3b5bf0633469af8ae62e5cd791f004db9d54b90a62')}
# The official package's user docs that name its folder (shortened; the lines naming it verbatim).
OFFICIAL_DOCS = {
    B + 'INSTALLATION.md': b'# Installation and usage\n\n4. Copy the **entire** `PPSA50011` folder to `/data/homebrew/`. '
                           b'The final executable path must be `/data/homebrew/PPSA50011/eboot.bin`.\n',
    B + 'RELEASE_NOTES.md': b"# PS5X360 v1.0.2\n\nPlease send the game's log from `PPSA50011/logs` with your results.\n",
    B + 'AUTOLOG.md': b'# PS5X360 AutoLog ELF 1.0.9-preview\n\n`/data/homebrew/PPSA50011/no-log-upload`\n',
    B + 'dlc/README.md': b'# DLC installation\n\n1. Copy complete Xbox 360 DLC STFS package files to `PPSA50011/dlc/`. '
                         b'Alternatively use `dlc/` beside the game executable/image.\n',
    B + 'updates/README.md': b'# Title updates\n\n1. Copy the Xbox 360 title update package for your game into this '
                             b'folder (`PPSA50011/updates`).\n',
    B + 'CREDITS.md': b'# Credits\n\nPS5X360 by BrinooTk.\n',
}
# The official package's LICENSE (the MIT notice of the PS5X360 code, as this repository's root
# LICENSE was before it became the GPL-3.0 text): the end of licenses/PS5X360-MIT.txt.
MIT_FILE = (REPO / 'licenses/PS5X360-MIT.txt').read_bytes()
OFFICIAL_LICENSE = MIT_FILE[MIT_FILE.find(b'\n\nMIT License\n') + 2:]
assert hashlib.sha256(OFFICIAL_LICENSE).hexdigest() == \
    '31278a4a718a1e625be83294ce66c51d56a38e6981b772f1f7f142f486d26275', OFFICIAL_LICENSE[:40]
GPL = (REPO / 'tools/source-1.0.2/licenses/GPL-3.0.txt').read_bytes()
COMMIT = 'f9140f1' + '0' * 33
SOURCE_URL = f'https://github.com/example/TrinityX360/tree/{COMMIT}'
DATE = '2026-11-20'
MAGIC = (0x1D3D154F).to_bytes(4, 'little')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def eboot(label=VERSION, extra=b''):
    # Mesa's own strings contain "candidate_" and must not count as a candidate label.
    return (MAGIC + b'\0' * 60 + b'intersection_candidate_aabb_opaque\0candidate_infos\0'
            + f'TrinityX360 v{label}\0\0\0BOOT version {label}\0'.encode()
            # session_log.hpp's header "PS5X360 " is no label.
            + b'PS5X360 \0' + extra)


def english(code, body='Settings = Ustawienia\n'):
    return (f'# PS5X360 interface text: {code}. Keys are English (fix 25); generated by\n'
            f'# tools/source-1.0.2/english/rekey.py. Do not edit by hand.\n{body}').encode()


LANG = {'en.txt': english('en', 'Settings = Settings\n'), 'pl.txt': english('pl'),
        'pt.txt': english('pt', 'Settings = Configurações\n'),
        'pt-BR.txt': english('pt-BR', 'Settings = Configurações\n')}
PORTUGUESE = {'en.txt': b'# PS5X360 UI translations: en\nConfiguracoes = Settings\n',
              'pl.txt': b'# PS5X360 UI translations: pl\nConfiguracoes = Ustawienia\n',
              'pt.txt': b'# PS5X360 UI translations: pt\nConfiguracoes = Configuracoes\n'}
SOURCES = {
    'version': VERSION,
    'release': True,
    'drops': {'v1.0.2': {'asset': 'PS5X360-source-1.0.2-changes.zip', 'sha256': 'd9bd' + '0' * 60}},
    'core': {'base': 'https://github.com/AdrianCassar/xenia-canary.git d2cf527aa' + '0' * 31},
    'online': {'net_libs': {'third_party/wolfssl': '3c5eead4' + '0' * 32},
               'mbedtls': {'url': 'https://example.invalid/mbedtls-3.6.7.tar.bz2', 'sha256': 'a7e8' + '0' * 60}},
    'fixes': [{'patch': 'tools/source-1.0.2/fixes/01-jit-reserved-barriers.patch', 'sha256': '1' * 64},
              {'patch': 'tools/source-1.0.2/fixes/25-english-source-text.patch', 'sha256': '2' * 64}],
    'fixes_skipped': [],
    'files': {},
}
SOURCES_BYTES = (json.dumps(SOURCES, indent=1, sort_keys=True) + '\n').encode()
TOOLCHAIN = package.toolchain_pins(package.WORKFLOW.read_text(encoding='utf-8'))


def base_files():
    files = {
        B + 'VERSION': b'1.0.2\n',
        B + 'eboot.bin': MAGIC + b'official 1.0.2 executable',
        B + 'RELEASE.json': b'{"version": "1.0.2"}\n',
        B + 'MANIFEST.json': b'{}\n',
        B + 'LICENSE': OFFICIAL_LICENSE,
        B + 'readback-auto.off': b'',
        # Not in the official zip; a base made from the binary release's stage would carry it.
        B + 'licenses/PS5X360-OPT-NOTICE.txt': b'eboot.bin is the official 1.0.2 executable\n',
        B + 'assets/presets.txt': b'old presets\n',
        B + 'assets/fonts/font.ttf': b'\0font',
        B + 'licenses/MINIUPNPC.txt': b'../LICENSE',
        B + 'licenses/WOLFSSL-GPL-3.txt': b'GPL-3\r\n',
        B + 'licenses/PS5X360-NOTICE.txt': b'PS5X360 1.0.0 - source of the parts\n',
        B + 'KINECT-source-and-licenses.zip': b'PK official kinect sources',
        B + 'tools/kinect-tracker.elf': b'\x7fELF tracker',
        B + 'sce_sys/param.json': OFFICIAL_PARAM,
        B + 'sce_sys/icon0.png': b'\x89PNG official icon0',
        B + 'sce_sys/pic0.png': b'\x89PNG official pic0',
        B + 'sce_sys/pic1.png': b'\x89PNG official pic1',
        # The home screen's pictures: the four with the PS5X360 logo are replaced (SKIN), the
        # others (an icon, the dark theme's) stay as they are.
        B + 'assets/skin/xbox-logo.png': b'\x89PNG official PS5X360 logo',
        B + 'assets/skin/basics.jpg': b'\xff\xd8\xff official X plate',
        B + 'assets/skin/ball.png': b'\x89PNG official X orb',
        B + 'assets/skin/ball-glow.png': b'\x89PNG official X glow',
        B + 'assets/skin/icon-games.png': b'\x89PNG official controller icon',
        B + 'assets/skin-dark/slot.jpg': b'\xff\xd8\xff official dark slot',
    }
    files.update(OFFICIAL_DOCS)
    for name, data in PORTUGUESE.items():
        files[B + 'assets/lang/' + name] = data
    return files


def base_zip(path):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(base_files().items()):
            z.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 22, 20, 26, 16)), data)
    return path.read_bytes()


def build(**changes):
    args = dict(base_files=base_files(), eboot=eboot(), lang=dict(LANG), sources=json.loads(SOURCES_BYTES),
                version=VERSION, source_url=SOURCE_URL, date=DATE, toolchain=TOOLCHAIN,
                sources_sha=sha(SOURCES_BYTES))
    args.update(changes)
    return package.build(**args)


def refused(contains, **changes):
    try:
        build(**changes)
    except SystemExit as error:
        assert contains in str(error.code), (contains, error.code)
        return
    raise AssertionError(f'not refused: {contains} ({list(changes)})')


def check_files(files, base_sha=package.BASE_ZIP_SHA256):
    base = base_files()
    names = {name[len(P):] for name in files}
    repo_assets = {'assets/presets.txt'}
    repo_assets |= {f'assets/config/{p.name}' for p in (REPO / 'assets/config').glob('*.toml')}
    repo_assets |= {f'assets/patches/{p.name}' for p in (REPO / 'assets/patches').glob('*.patch.toml')}
    added = {f'licenses/PS5X360-{VERSION}-NOTICE.txt', 'licenses/GPL-3.0.txt',
             'licenses/MBEDTLS-3.6.7-LICENSE.txt', 'licenses/CACERT-MPL-2.0.txt', 'assets/lang/pt-BR.txt',
             'licenses/PS5X360-MIT.txt'}
    expected = ({name[len(B):] for name in base}
                - {'readback-auto.off', 'licenses/PS5X360-OPT-NOTICE.txt', 'AUTOLOG.md'}
                | repo_assets | added)
    assert names == expected, sorted(names ^ expected)
    assert all(name.startswith(P) for name in files)
    assert not any(name.startswith(B) or B.rstrip('/') in name for name in files)

    # eboot.bin byte for byte; untouched files byte for byte.
    assert files[P + 'eboot.bin'] == eboot()
    for name in ('assets/fonts/font.ttf', 'licenses/WOLFSSL-GPL-3.txt', 'CREDITS.md',
                 'licenses/PS5X360-NOTICE.txt', 'KINECT-source-and-licenses.zip', 'tools/kinect-tracker.elf'):
        assert files[P + name] == base[B + name], name

    # LICENSE: the GPL-3.0 text, as the repository's root LICENSE (which GitHub reads as GPL-3.0);
    # the official MIT LICENSE kept, word for word, at the end of licenses/PS5X360-MIT.txt.
    assert files[P + 'LICENSE'] == GPL == files[P + 'licenses/GPL-3.0.txt']
    assert (REPO / 'LICENSE').read_bytes().replace(b'\r\n', b'\n') == GPL
    assert GPL.startswith(b'                    GNU GENERAL PUBLIC LICENSE\n')
    mit = files[P + 'licenses/PS5X360-MIT.txt']
    assert mit == MIT_FILE.replace(b'\r\n', b'\n') and mit.endswith(b'\n\n' + base[B + 'LICENSE'])
    header = mit[:-len(base[B + 'LICENSE'])].decode()
    for needle in ('MIT License', 'PS5X360 by BrinooTk', 'https://github.com/BrinooTk/PS5X360',
                   'General Public License version 3 (LICENSE)', 'keeps this notice'):
        assert needle in header.replace('\n', ' '), needle
    assert mit.count(b'Copyright (c) 2026 Xbox360PS5 contributors') == 1

    # param.json: the official file with the new id, content id, concept id, name and content
    # version, nothing else.
    assert sha(OFFICIAL_PARAM) == '68f56aa52bfdf3b73f8c8f41294b29da9d9f4d0b49fe5da953ea433123178313'
    param_bytes = files[P + 'sce_sys/param.json']
    assert param_bytes == RELEASE_PARAM, param_bytes
    assert param_bytes != base[B + 'sce_sys/param.json']
    assert param_bytes.count(b'\r\n') == OFFICIAL_PARAM.count(b'\r\n') and b'\n' not in param_bytes.replace(b'\r\n', b'')
    param, official = json.loads(param_bytes), json.loads(OFFICIAL_PARAM)
    assert list(param) == list(official)
    assert param['titleId'] == 'PPSA83600' and param['conceptId'] == '83600'
    assert param['contentId'] == 'UP9000-PPSA83600_00-PS5X360RELOADED0'
    assert re.fullmatch(r'UP9000-PPSA[0-9]{5}_00-[A-Z0-9]{16}', param['contentId'])
    assert param['contentId'] != official['contentId'] and param['conceptId'] == param['titleId'][4:]
    assert param['localizedParameters'] == {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'TrinityX360'}}
    assert param['contentVersion'] == CONTENT_VERSION == package.content_version(VERSION)
    assert param['contentVersion'] > official['contentVersion'] == package.SHIPPED_CONTENT_VERSIONS['1.0.3']
    assert param['masterVersion'] == official['masterVersion'] == '00.01'
    changed = {key for key in official if official[key] != param[key]}
    assert changed == {'titleId', 'conceptId', 'contentId', 'contentVersion', 'localizedParameters'}, changed

    # The official user docs that name PPSA50011: TrinityX360's INSTALLATION.md, this
    # release's notes, the DLC and updates READMEs with the folder renamed, no AutoLog guide.
    template = (REPO / 'tools/source-1.0.2/package-docs/INSTALLATION.md').read_text(encoding='utf-8')
    installation = files[P + 'INSTALLATION.md'].decode()
    assert installation == (template.replace('@VERSION@', VERSION)
                            .replace('@REPOSITORY@', 'https://github.com/example/TrinityX360')
                            .replace('@COMMIT@', COMMIT)), installation
    for needle in ('# TrinityX360 1.0.6: installation', '`/data/homebrew/PPSA83600/eboot.bin`',
                   '`/data/homebrew/PPSA83600/assets/roms/`', '`/data/homebrew/PPSA83600/logs`',
                   'Never copy this folder or its files into\n`/data/homebrew/PPSA50011`',
                   f'https://github.com/example/TrinityX360/blob/{COMMIT}/README.md',
                   'licenses/PS5X360-1.0.6-NOTICE.txt'):
        assert needle in installation, needle
    assert not re.search(r'@[A-Z]+@', installation) and 'PPSA50011.zip' not in installation
    # Each paragraph naming the official folder says it is the official app's.
    for paragraph in installation.split('\n\n'):
        if 'PPSA50011' in paragraph:
            assert 'official' in paragraph, paragraph
    assert files[P + 'RELEASE_NOTES.md'] == (REPO / f'docs/release-notes/v{VERSION}.md').read_bytes()
    assert files[P + 'RELEASE_NOTES.md'] != base[B + 'RELEASE_NOTES.md']
    for name in ('dlc/README.md', 'updates/README.md'):
        assert files[P + name] == base[B + name].replace(b'PPSA50011', b'PPSA83600'), name
        assert b'PPSA83600/' in files[P + name] and b'PPSA50011' not in files[P + name], name
    assert P + 'AUTOLOG.md' not in files
    # No other file names the official title id.
    assert package.official_id_problems(files, VERSION) == []
    for name, data in files.items():
        if b'PPSA50011' in data:
            assert name[len(P):] in {'INSTALLATION.md', 'RELEASE_NOTES.md', 'RELEASE.json',
                                     f'licenses/PS5X360-{VERSION}-NOTICE.txt'}, name

    # The TrinityX360 art, never the official images.
    readme = (REPO / 'tools/source-1.0.2/package-art/README.md').read_text(encoding='utf-8')
    for name, (size, digest) in ART.items():
        data = files[P + 'sce_sys/' + name]
        assert data == (REPO / 'tools/source-1.0.2/package-art' / name).read_bytes(), name
        assert sha(data) == digest and data != base[B + 'sce_sys/' + name], name
        assert data[:8] == b'\x89PNG\r\n\x1a\n' and data[12:16] == b'IHDR', name
        assert (int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')) == size, name
        assert data[24:26] == b'\x08\x02', name  # 8-bit RGB, as the official files
        assert f'`{name}`' in readme and digest in readme, name
    # The home screen's pictures with the PS5X360 logo: TrinityX360's, at the official sizes and
    # formats; never the official bytes; the other skin pictures as the base has them.
    for name, target, size, digest, official in SKIN:
        data = files[P + target]
        assert data == (REPO / 'tools/source-1.0.2/package-art' / name).read_bytes(), name
        assert sha(data) == digest and data != base[B + target], name
        if name.endswith('.jpg'):
            assert data[:3] == b'\xff\xd8\xff' and package.jpeg_size(data) == size, name
        else:
            assert package.png_size(data) == size and data[25] == 6, name  # RGBA, as the official files
        assert package.OFFICIAL_LOGOS[official] == target and digest not in package.OFFICIAL_LOGOS, name
        assert f'`{name}`' in readme and digest in readme, name
    assert files[P + 'assets/skin/icon-games.png'] == base[B + 'assets/skin/icon-games.png']
    assert files[P + 'assets/skin-dark/slot.jpg'] == base[B + 'assets/skin-dark/slot.jpg']
    assert set(package.OFFICIAL_LOGOS) == {official for *_, official in SKIN}
    assert package.official_logo_problems(files) == []
    assert [(n, s, d) for n, _, s, d in package.ART] == \
        [(n, s, d) for n, (s, d) in ART.items()] + [(n, s, d) for n, _, s, d, _ in SKIN]
    assert [t for n, t, s, d in package.ART if n.startswith('skin/')] == [t for _, t, *_ in SKIN]
    # English-keyed language files, as built; nothing Portuguese-keyed merged in.
    for name, data in LANG.items():
        assert files[P + 'assets/lang/' + name] == data, name
    overrides = b''.join(p.read_bytes() for p in (REPO / 'assets/lang-overrides').glob('*.txt'))
    for name in LANG:
        data = files[P + 'assets/lang/' + name]
        assert b'Configuracoes' not in data and b'UI translations' not in data, name
        if overrides:
            assert overrides not in data
    # Repository package data.
    for name in repo_assets:
        assert files[P + name] == (REPO / name).read_bytes(), name
    assert P + 'licenses/PS5X360-OPT-NOTICE.txt' not in files
    assert P + 'readback-auto.off' not in files

    assert files[P + 'VERSION'] == b'1.0.6\n'
    release = json.loads(files[P + 'RELEASE.json'])
    assert release['version'] == VERSION
    assert release['title_id'] == 'PPSA83600' and release['title_name'] == 'TrinityX360'
    assert release['content_version'] == CONTENT_VERSION
    assert release['executable_sha256'] == sha(eboot())
    assert release['candidate'] is False and release['published'] is True
    assert release['candidate_id'] == 'src-' + sha(eboot())[:8]
    assert release['build'] == 'source'
    assert release['source_commit'] == COMMIT and release['source_url'] == SOURCE_URL
    assert release['source_repository'] == 'https://github.com/example/TrinityX360'
    assert release['base_package']['sha256'] == base_sha
    assert release['drops'] == {'v1.0.2': 'd9bd' + '0' * 60}
    assert release['fixes'] == ['01-jit-reserved-barriers.patch', '25-english-source-text.patch']
    assert release['fixes_skipped'] == []
    assert release['sources_json_sha256'] == sha(SOURCES_BYTES)
    assert release['source_assets'] == [f'PS5X360-{VERSION}-source.tar.xz',
                                        f'PS5X360-{VERSION}-toolchain-source.tar.xz',
                                        f'TrinityX360-{VERSION}-scripts.tar.gz']
    assert release['toolchain']['PS5_Vulkan']['commit'] == TOOLCHAIN['PS5_Vulkan']['commit']

    text = files[P + f'licenses/PS5X360-{VERSION}-NOTICE.txt'].decode()
    for needle in ('GNU General Public License', 'version 3', 'licenses/GPL-3.0.txt',
                   'Modified version (GPL-3.0 section 5)', f'modified {DATE}',
                   f'https://github.com/example/TrinityX360 commit {COMMIT}',
                   'https://github.com/example/TrinityX360/releases/tag/v1.0.6',
                   sha(eboot()), sha(SOURCES_BYTES),
                   'PS5X360-source-1.0.2-changes.zip (v1.0.2)  sha256 d9bd' + '0' * 60,
                   'xenia-canary.git d2cf527aa', '01-jit-reserved-barriers.patch  sha256 ' + '1' * 64,
                   f'PS5X360-{VERSION}-source.tar.xz', f'TrinityX360-{VERSION}-scripts.tar.gz',
                   f'PS5X360-{VERSION}-toolchain-source.tar.xz',
                   'PS5_Vulkan  https://github.com/mihawk-99/PS5_Vulkan commit ' + TOOLCHAIN['PS5_Vulkan']['commit'],
                   'PS5_Mesa', 'PS5_PayloadSDK', 'OpenGL renderer stack', 'USB Kinect', 'disc',
                   'NOT part of this executable', 'wolfSSL', 'ps5-framegen',
                   'licenses/MBEDTLS-3.6.7-LICENSE.txt', 'licenses/CACERT-MPL-2.0.txt', 'MPL-2.0',
                   'Apache-2.0', base_sha,
                   # The toolchain parts linked into eboot.bin are named, with where their source is.
                   'RADV', 'app_crt.cpp', 'payload SDK runtime', 'ps5-native-tool',
                   'at the commits listed under Toolchain',
                   # The new identity, and the upstream attribution kept as it is.
                   'TrinityX360 1.0.6 (PPSA83600)', 'UP9000-PPSA83600_00-PS5X360RELOADED0',
                   'moved from PPSA50011/ to PPSA83600/', 'tools/source-1.0.2/package-art',
                   'tools/source-1.0.2/package-docs', 'AUTOLOG.md is left out',
                   'This is a modified version of PS5X360 1.0.2 by BrinooTk (https://github.com/BrinooTk/PS5X360/releases)',
                   'Source drops (BrinooTk/PS5X360 releases)',
                   # The licence layout: LICENSE is GPL-3.0, the MIT notice is kept in its own file.
                   'LICENSE is the GPL-3.0 text too',
                   'the PS5X360 code came under the MIT License and keeps its notice\n    (licenses/PS5X360-MIT.txt)',
                   'the official\n    LICENSE, the MIT License, is in licenses/PS5X360-MIT.txt',
                   f'content version\n    {CONTENT_VERSION}, so an older install shows as outdated'):
        assert needle in text, needle
    assert 'code is MIT (LICENSE)' not in text
    assert 'complete source tree' not in text
    assert 'candidate' not in text.lower()

    # Licence texts verbatim from tools/source-1.0.2/licenses, checked by SHA-256.
    readme = (REPO / 'tools/source-1.0.2/licenses/README.md').read_text(encoding='utf-8')
    for name, target, digest in package.LICENCES:
        data = (REPO / 'tools/source-1.0.2/licenses' / name).read_bytes()
        assert sha(data) == digest and b'\r' not in data, name
        assert files[P + target] == data, target
        assert f'`{name}`' in readme and digest in readme, name
    assert files[P + 'licenses/GPL-3.0.txt'].startswith(b'                    GNU GENERAL PUBLIC LICENSE\n'
                                                       b'                       Version 3, 29 June 2007')
    assert files[P + 'licenses/MINIUPNPC.txt'].startswith(b'BSD 3-Clause License')
    assert b'Apache License' in files[P + 'licenses/MBEDTLS-3.6.7-LICENSE.txt']
    assert files[P + 'licenses/CACERT-MPL-2.0.txt'].startswith(b'Mozilla Public License Version 2.0')

    manifest = json.loads(files[P + 'MANIFEST.json'])
    assert set(manifest) == names - {'MANIFEST.json'}
    for name, record in manifest.items():
        assert record == {'bytes': len(files[P + name]), 'sha256': sha(files[P + name])}, name


def check_output(out, files):
    out_zip = out / 'PPSA83600.zip'
    with zipfile.ZipFile(out_zip) as z:
        assert z.testzip() is None
        infos = z.infolist()
        assert [i.filename for i in infos] == sorted(files)
        for info in infos:
            assert info.date_time == package.ZIP_TIME and info.external_attr == 0o644 << 16, info
            assert info.create_system == 3, info  # the same bytes on Windows and Linux
            assert z.read(info) == files[info.filename], info.filename
    stage = out / 'stage'
    staged = {p.relative_to(stage).as_posix(): p.read_bytes() for p in stage.rglob('*') if p.is_file()}
    assert staged == files
    assert (stage / 'PPSA83600' / 'eboot.bin').is_file() and not (stage / 'PPSA50011').exists()
    assert sorted(p.name for p in stage.iterdir()) == ['PPSA83600']
    assert (out / 'PPSA83600.zip.sha256').read_text(encoding='utf-8') == \
        f'{sha(out_zip.read_bytes())}  PPSA83600.zip\n'
    assert not (out / 'PPSA50011.zip').exists() and not (out / 'PPSA50011.zip.sha256').exists()


def main():
    files = build()
    check_files(files)
    print('[OK] layout, eboot, English-keyed lang, repository assets, VERSION, RELEASE.json, NOTICE, licences')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        first = package.write_package(files, tmp / 'a')
        second = package.write_package(build(), tmp / 'b')
        check_output(tmp / 'a', files)
        assert first == second
        assert (tmp / 'a/PPSA83600.zip').read_bytes() == (tmp / 'b/PPSA83600.zip').read_bytes()
        print('[OK] deterministic zip, stage and .sha256')

        # The command line, end to end, with the fixture's hash in place of the official one.
        zip_path = tmp / 'base.zip'
        digest = sha(base_zip(zip_path))
        (tmp / 'eboot.bin').write_bytes(eboot())
        (tmp / 'SOURCES.json').write_bytes(SOURCES_BYTES)
        (tmp / 'lang').mkdir()
        for name, data in LANG.items():
            (tmp / 'lang' / name).write_bytes(data)
        argv = ['--base-zip', str(zip_path), '--eboot', str(tmp / 'eboot.bin'), '--lang-dir', str(tmp / 'lang'),
                '--sources-json', str(tmp / 'SOURCES.json'), '--version', VERSION,
                '--source-url', SOURCE_URL, '--date', DATE]
        stdout = sys.stdout
        try:
            sys.stdout = io.StringIO()
            try:
                package.main(argv + ['--out-dir', str(tmp / 'cli-official')])
            except SystemExit as error:
                assert 'is not the official v1.0.2 PPSA50011.zip' in str(error.code), error.code
            else:
                raise AssertionError('a base zip other than the official one was accepted')
            official = package.BASE_ZIP_SHA256
            package.BASE_ZIP_SHA256 = digest
            try:
                assert package.main(argv + ['--out-dir', str(tmp / 'cli')]) == 0
                assert package.main(argv + ['--out-dir', str(tmp / 'cli2')]) == 0
            finally:
                package.BASE_ZIP_SHA256 = official
            summary = json.loads(sys.stdout.getvalue().splitlines()[0])
        finally:
            sys.stdout = stdout
        cli_files = {p.relative_to(tmp / 'cli/stage').as_posix(): p.read_bytes()
                     for p in (tmp / 'cli/stage').rglob('*') if p.is_file()}
        check_files(cli_files, base_sha=digest)
        check_output(tmp / 'cli', cli_files)
        assert (tmp / 'cli/PPSA83600.zip').read_bytes() == (tmp / 'cli2/PPSA83600.zip').read_bytes()
        assert summary['version'] == VERSION and summary['eboot_sha256'] == sha(eboot())
        assert summary['title_id'] == 'PPSA83600' and summary['package'].endswith('PPSA83600.zip')
        assert summary['content_version'] == CONTENT_VERSION
        print('[OK] command line: official zip gate, package, same zip twice')

    for version in ('1.0.3_candidate_37', '', 'v1.0.3', '1.0', '1.0.3.0', '1.0.3.1.1', '1.0.3 '):
        refused('is not X.Y.Z', version=version)
    refused('carries a candidate label', eboot=eboot(extra=b'1.0.3_candidate_37\0'))
    refused("label 'TrinityX360 v1.0.6'", eboot=eboot('1.0.3_candidate_37'))
    refused("label 'TrinityX360 v1.0.6'", eboot=eboot('1.0.30'))
    # Built without fix 46 or without the rename to TrinityX360: an older name's label, alone or
    # next to the new one.
    refused("label 'TrinityX360 v1.0.6'", eboot=eboot().replace(b'TrinityX360 v', b'PS5X360 v'))
    refused("label 'TrinityX360 v1.0.6'", eboot=eboot().replace(b'TrinityX360 v', b'PS5X360Reloaded v'))
    refused("older name's label", eboot=eboot(extra=b'PS5X360 v1.0.3\0'))
    refused("older name's label", eboot=eboot(extra=b'PS5X360 v1.0.2\0'))
    refused("older name's label", eboot=eboot(extra=b'PS5X360Reloaded v1.0.6\0'))
    refused("label 'BOOT version 1.0.6'", eboot=eboot().replace(b'BOOT version', b'BOOT versio_'))
    refused('not a signed PS5 SELF', eboot=b'\x7fELF' + eboot()[4:])
    refused('lists skipped fixes', sources=dict(SOURCES, fixes_skipped=['tools/source-1.0.2/fixes/01-x.patch']))
    refused("SOURCES.json version '1.0.3_candidate_37'", sources=dict(SOURCES, version='1.0.3_candidate_37'))
    refused('SOURCES.json version None', sources={k: v for k, v in SOURCES.items() if k != 'version'})
    refused('not from a release assembly', sources=dict(SOURCES, release=False))
    refused('not from a release assembly', sources={k: v for k, v in SOURCES.items() if k != 'release'})
    refused('has no fixes_skipped list', sources={k: v for k, v in SOURCES.items() if k != 'fixes_skipped'})
    refused('lists skipped fixes', sources=dict(SOURCES, fixes_skipped=None))
    refused("has no 'fixes'", sources=dict(SOURCES, fixes=[]))
    refused('no English-keyed file for PPSA83600/assets/lang/pl.txt',
            lang={k: v for k, v in LANG.items() if k != 'pl.txt'})
    refused('pl.txt is not an English-keyed', lang=dict(LANG, **{'pl.txt': PORTUGUESE['pl.txt']}))
    refused('de.txt is not an English-keyed', lang=dict(LANG, **{'de.txt': english('pl')}))
    refused('no language files', lang={})
    refused('pl.txt is not an English-keyed', lang=dict(LANG, **{'pl.txt': b'\xff\xfe#\0 \0P\0'}))
    refused('base package VERSION is not 1.0.2', base_files=dict(base_files(), **{B + 'VERSION': b'1.0.3\n'}))
    # The official LICENSE is the MIT notice kept in licenses/PS5X360-MIT.txt: missing or changed,
    # the packaging stops rather than drop or misstate it.
    refused('base package has no LICENSE', base_files={k: v for k, v in base_files().items() if k != B + 'LICENSE'})
    refused('base package LICENSE is not the MIT License', base_files=dict(base_files(), **{B + 'LICENSE': GPL}))
    refused("licenses/PS5X360-MIT.txt does not end with the base package's LICENSE",
            base_files=dict(base_files(), **{B + 'LICENSE': OFFICIAL_LICENSE.replace(b'2026', b'2025')}))
    # The base is read under PPSA50011/ only: a stage already moved, or a stray file, is refused.
    refused('base package files outside PPSA50011/: PPSA83600/VERSION',
            base_files=dict(base_files(), **{P + 'VERSION': b'1.0.2\n'}))
    refused('base package files outside PPSA50011/: README.txt', base_files=dict(base_files(), **{'README.txt': b'x'}))
    refused('base package has no sce_sys/param.json',
            base_files={k: v for k, v in base_files().items() if k != B + 'sce_sys/param.json'})
    # param.json must be the official app's: a param of another title (e.g. a package made by
    # this script) or a malformed one stops the packaging.
    for param, message in ((RELEASE_PARAM, "titleId 'PPSA83600' is not PPSA50011"),
                           (b'{"titleId": "PPSA50011"', 'is not JSON'),
                           (b'[]', 'is not a JSON object'),
                           (OFFICIAL_PARAM.replace(b'  "conceptId": "50011",\r\n', b''), 'has no conceptId'),
                           (OFFICIAL_PARAM.replace(b'  "contentVersion": "00.001.000",\r\n', b''),
                            'has no contentVersion'),
                           (OFFICIAL_PARAM.replace(b'"00.001.000"', b'"1.0.2"'),
                            "contentVersion '1.0.2' of the base package is not NN.NNN.NNN"),
                           # A base already at or above the release's value: not increasing.
                           (OFFICIAL_PARAM.replace(b'"00.001.000"', f'"{CONTENT_VERSION}"'.encode()),
                            f'contentVersion {CONTENT_VERSION} is not higher than {CONTENT_VERSION} of the base package'),
                           (OFFICIAL_PARAM.replace(b'"00.001.000"', b'"99.999.000"'),
                            f'contentVersion {CONTENT_VERSION} is not higher than 99.999.000 of the base package'),
                           (OFFICIAL_PARAM.replace(b'"defaultLanguage": "en-US"', b'"defaultLanguage": "fr-FR"'),
                            'no localizedParameters entry for its defaultLanguage'),
                           (b'{"titleId": "PPSA50011", "conceptId": "50011", "contentId": "x",'
                            b' "contentVersion": "00.001.000"}',
                            'no localizedParameters entry')):
        refused(message, base_files=dict(base_files(), **{B + 'sce_sys/param.json': param}))
    # Every language entry gets the new name; LF files stay LF, other fields stay as they are.
    several = {'titleId': 'PPSA50011', 'conceptId': '50011', 'contentId': 'UP9000-PPSA50011_00-XBOX360PS5M80001',
               'contentVersion': '00.001.000', 'masterVersion': '00.01',
               'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5X360'},
                                       'pl-PL': {'titleName': 'PS5X360', 'titleNameSort': 'x'}, 'ja-JP': {}},
               'downloadDataSize': 256}
    made = package.param_json(json.dumps(several, indent=2).encode() + b'\n', '1.0.6')
    assert b'\r' not in made and made.endswith(b'}\n'), made
    made = json.loads(made)
    assert made['localizedParameters'] == {
        'defaultLanguage': 'en-US', 'en-US': {'titleName': 'TrinityX360'},
        'pl-PL': {'titleName': 'TrinityX360', 'titleNameSort': 'x'}, 'ja-JP': {'titleName': 'TrinityX360'}}
    assert made['downloadDataSize'] == 256 and made['titleId'] == 'PPSA83600'
    assert made['contentVersion'] == '01.006.000' and made['masterVersion'] == '00.01'
    assert package.param_json(OFFICIAL_PARAM, VERSION) == RELEASE_PARAM
    assert package.param_json(OFFICIAL_PARAM, '1.0.6') == \
        RELEASE_PARAM.replace(f'"{CONTENT_VERSION}"'.encode(), b'"01.006.000"')
    # The official docs: a changed text (the folder named another number of times) or a missing
    # one stops the packaging, and so does any other text file still naming PPSA50011.
    dlc = OFFICIAL_DOCS[B + 'dlc/README.md']
    refused('dlc/README.md names PPSA50011 2 times, not 1',
            base_files=dict(base_files(), **{B + 'dlc/README.md': dlc + b'See PPSA50011/dlc.\n'}))
    refused('base package has no updates/README.md',
            base_files={k: v for k, v in base_files().items() if k != B + 'updates/README.md'})
    refused('still name the official title id PPSA50011: MULTI-DISC.md',
            base_files=dict(base_files(), **{B + 'MULTI-DISC.md': b'Copy discs to PPSA50011/assets/roms.\n'}))
    refused('still name the official title id PPSA50011: assets/lang/pl.txt (English-keyed language files',
            lang=dict(LANG, **{'pl.txt': english('pl', 'Copy to PPSA50011/dlc = Kopiuj do PPSA50011/dlc\n')}))
    with tempfile.TemporaryDirectory() as other:
        other = Path(other)
        (other / 'assets').mkdir()
        (other / 'assets/presets.txt').write_bytes(b'presets\n')
        refused('no licenses/PS5X360-MIT.txt in the repository', repo_root=other)
        (other / 'licenses').mkdir()
        # The MIT notice changed (here: another copyright line) or cut short stops the packaging.
        (other / 'licenses/PS5X360-MIT.txt').write_bytes(MIT_FILE.replace(b'Xbox360PS5 contributors', b'someone else'))
        refused('licenses/PS5X360-MIT.txt does not end with the base package\'s LICENSE', repo_root=other)
        (other / 'licenses/PS5X360-MIT.txt').write_bytes(OFFICIAL_LICENSE)
        refused('licenses/PS5X360-MIT.txt does not end with the base package\'s LICENSE', repo_root=other)
        (other / 'licenses/PS5X360-MIT.txt').write_bytes(MIT_FILE.replace(b'\n', b'\r\n'))
        refused('no release notes docs/release-notes/v1.0.6.md', repo_root=other)
        (other / 'docs/release-notes').mkdir(parents=True)
        (other / 'docs/release-notes/v1.0.6.md').write_bytes(b'## TrinityX360 v1.0.6\n')
        assert build(repo_root=other)[P + 'RELEASE_NOTES.md'] == b'## TrinityX360 v1.0.6\n'
        (other / 'assets/presets.txt').write_bytes(b'# /data/homebrew/PPSA50011/assets/roms\n')
        refused('still name the official title id PPSA50011: assets/presets.txt', repo_root=other)
    # Art that is not the reviewed files (here: wrong pinned digest or size) stops the packaging.
    art = package.ART
    try:
        package.ART = [(n, t, s, '0' * 64 if n == 'pic0.png' else d) for n, t, s, d in art]
        refused('package-art/pic0.png is not the reviewed file')
        package.ART = [(n, t, (256, 256) if n == 'icon0.png' else s, d) for n, t, s, d in art]
        refused('package-art/icon0.png is not a 256x256 PNG')
        package.ART = [(n, t, (420, 201) if n == 'skin/basics.jpg' else s, d) for n, t, s, d in art]
        refused('package-art/skin/basics.jpg is not a 420x201 JPEG')
    finally:
        package.ART = art
    # A logo picture anywhere in the package (here: the official bytes under another name, as a
    # base with a copy in skin-dark would have) stops the packaging; so does an ART file that is one.
    logos = package.OFFICIAL_LOGOS
    try:
        package.OFFICIAL_LOGOS = dict(logos, **{sha(b'\xff\xd8\xff official dark slot'): 'assets/skin/basics.jpg'})
        refused('files that are the official PS5X360 logo pictures: PPSA83600/assets/skin-dark/slot.jpg')
        package.OFFICIAL_LOGOS = dict(logos, **{SKIN[2][3]: 'assets/skin/ball.png'})
        refused('package-art/skin/ball.png is the official PS5X360 logo picture')
    finally:
        package.OFFICIAL_LOGOS = logos
    # The label package.py wants is the one check_release_elf.py checks after the link.
    assert package.HOME_LABEL == check_release_elf.LABELS[0] == b'TrinityX360 v'
    assert check_release_elf.OLD_LABELS == (b'PS5X360 v', b'PS5X360Reloaded v')
    # The console identity the rename kept: the installed PS5X360Reloaded (1.0.3 to 1.0.5) is
    # PPSA83600 with this content id; another one would be another app, without its saves.
    assert (package.TITLE_ID, package.CONCEPT_ID, package.CONTENT_ID, package.TITLE_NAME) == \
        ('PPSA83600', '83600', 'UP9000-PPSA83600_00-PS5X360RELOADED0', 'TrinityX360')
    assert package.PREFIX == 'PPSA83600/'
    for url in ('https://github.com/example/TrinityX360', 'https://github.com/example/repo/tree/main',
                f'https://github.com/example/repo/tree/{COMMIT[:7]}', ''):
        refused('is not https://', source_url=url)
    # A licence text that is not the verbatim one (here: a wrong pinned digest) stops the packaging.
    licences = package.LICENCES
    package.LICENCES = [(name, target, '0' * 64) if name == 'GPL-3.0.txt' else (name, target, digest)
                        for name, target, digest in licences]
    try:
        refused('licenses/GPL-3.0.txt is not the verbatim text')
    finally:
        package.LICENCES = licences
    print('[OK] refusals')

    # contentVersion from the release version: fixed, strictly increasing, above what shipped.
    cv = package.content_version
    assert cv('1.0.4') == '01.004.000' and cv('1.0.3') == '01.003.000' and cv('1.2.15') == '01.215.000'
    assert cv('0.0.0') == '00.000.000' and cv('99.9.99') == '99.999.000' and cv('2.0.0') == '02.000.000'
    # A hotfix X.Y.Z.W: the third field (1.0.4.1 -> 01.004.001, between 1.0.4 and 1.0.5).
    assert cv('1.0.4.1') == '01.004.001' and cv('1.0.4.999') == '01.004.999'
    run = ['0.9.99', '1.0.3', '1.0.4', '1.0.4.1', '1.0.4.2', '1.0.4.999', '1.0.6', '1.0.10', '1.0.99', '1.1.0',
           '1.1.5', '1.9.99', '2.0.0', '10.0.1', '99.9.99']
    values = [cv(version) for version in run]
    assert all(package.CONTENT_VERSION_RE.fullmatch(value) for value in values), values
    assert values == sorted(values) and len(set(values)) == len(values), values
    # Text order is version order (the console and catalogs may compare either way).
    assert [tuple(int(x) for x in v.split('.')) for v in values] == \
        sorted(tuple(int(x) for x in v.split('.')) for v in values)
    shipped = package.SHIPPED_CONTENT_VERSIONS
    assert shipped == {'1.0.3': '00.001.000', '1.0.4': '01.004.000', '1.0.4.1': '01.004.001',
                       '1.0.4.2': '01.004.002', '1.0.5': '01.005.000'}, shipped
    # v1.0.5 shipped (as PS5X360Reloaded): packaging it again is refused, 1.0.6 goes above it.
    assert cv('1.0.5') == shipped['1.0.5'] == '01.005.000' < cv('1.0.6')
    try:
        package.param_json(OFFICIAL_PARAM, '1.0.5')
    except SystemExit as error:
        assert 'contentVersion 01.005.000 is not higher than 01.005.000 of v1.0.5' in str(error.code), error.code
    else:
        raise AssertionError('1.0.5 accepted after it shipped')
    assert cv('1.0.6') > max(shipped.values()) and cv('1.0.4.2') > cv('1.0.4.1') > cv('1.0.4') > cv('1.0.3')
    assert package.check_content_version('01.006.000', '00.001.000') == '01.006.000'
    for version in ('1.10.0', '1.0.100', '100.0.0'):
        try:
            cv(version)
        except SystemExit as error:
            assert 'does not fit contentVersion NN.NNN.NNN' in str(error.code), error.code
        else:
            raise AssertionError(f'{version} accepted')
        refused('does not fit contentVersion', version=version, eboot=eboot(version),
                sources=dict(SOURCES, version=version))
    # A release whose contentVersion is not above one already shipped (as 1.0.4 would be after a
    # release that shipped 01.004.000) is refused.
    try:
        package.SHIPPED_CONTENT_VERSIONS = dict(shipped, **{'1.0.4': '01.004.000'})
        assert package.param_json(OFFICIAL_PARAM, '1.0.6').count(b'"01.006.000"') == 1
        for version, message in (('1.0.4', 'contentVersion 01.004.000 is not higher than 01.004.000 of v1.0.4'),
                                 ('1.0.3', 'contentVersion 01.003.000 is not higher than 01.004.000 of v1.0.4')):
            try:
                package.param_json(OFFICIAL_PARAM, version)
            except SystemExit as error:
                assert message in str(error.code), error.code
            else:
                raise AssertionError(f'{version} accepted after 01.004.000 shipped')
        # The same release packaged again after it shipped (VERSION is 1.0.6): refused.
        package.SHIPPED_CONTENT_VERSIONS = dict(shipped, **{'1.0.6': '01.006.000'})
        refused('contentVersion 01.006.000 is not higher than 01.006.000 of v1.0.6')
        package.SHIPPED_CONTENT_VERSIONS = dict(shipped, **{'1.0.4': '1.4'})
        refused("contentVersion '1.4' of v1.0.4 is not NN.NNN.NNN")
    finally:
        package.SHIPPED_CONTENT_VERSIONS = shipped
    print('[OK] contentVersion: 1.0.4 -> 01.004.000, 1.0.6 -> 01.006.000, increasing, above the shipped 01.005.000')

    try:
        package.toolchain_pins('env:\n  PS5_MESA_REV: ' + '0' * 40 + '\n')
    except SystemExit as error:
        assert 'PS5_VULKAN_REV' in str(error.code)
    else:
        raise AssertionError('missing toolchain pin accepted')
    for key in ('PS5_Vulkan', 'PS5_Mesa', 'PS5_PayloadSDK'):
        assert len(TOOLCHAIN[key]['commit']) == 40, key
    # The optional pins must still be found in build-source-1.0.2.yml, or the NOTICE loses them.
    for key in ('Vulkan SDK', 'zlib'):
        assert len(TOOLCHAIN[key]['sha256']) == 64 and TOOLCHAIN[key]['version'], key
    try:
        package.modification_date(None, {'SOURCE_DATE_EPOCH': '17950OOOOO'})
    except SystemExit as error:
        assert 'SOURCE_DATE_EPOCH' in str(error.code), error.code
    else:
        raise AssertionError('malformed SOURCE_DATE_EPOCH accepted')
    assert package.modification_date(None, {'SOURCE_DATE_EPOCH': '1795000000'}) == '2026-11-18'
    assert package.modification_date('2026-11-20', {}) == '2026-11-20'
    try:
        package.modification_date('20.11.2026', {})
    except SystemExit:
        pass
    else:
        raise AssertionError('malformed date accepted')
    print('[OK] toolchain pins and modification date')
    print('check-source-package: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
