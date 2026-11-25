"""Assemble a source tree for PS5X360 v1.0.2 from the author's public source drops.

    python3 -I tools/source-1.0.2/assemble.py --out build/src102 [--cache .deps/src102-cache]
        [--version X.Y.Z] [--release]

The version shown on the home screen ("PS5X360Reloaded v<version>", fix 46) is --version, else
PS5X360_BUILD_VERSION, else tools/source-1.0.2/BUILD_VERSION (the test builds' candidate label),
else 1.0.2. --release (or PS5X360_RELEASE=1) is a release build: its version must be given by
--version or PS5X360_BUILD_VERSION and be plain X.Y.Z, no fix may be left out, and SOURCES.json
records version, version_source and release.

Inputs (all public; every archive is checked against its SHA-256 below):
  * BrinooTk/PS5X360 release assets PS5X360-source-{0.5.9-fix1,0.6.0-changes,1.0.0-changes,
    1.0.2-changes}.zip
  * AdrianCassar/xenia-canary netplay_canary_experimental d2cf527aa (the core's base, with its
    submodules; wolfSSL, libcurl and miniupnpc are the commits the author names)
  * Mbed TLS 3.6.7 (phone-camera TLS server; the archive the author records)
  * the official v1.0.2 package PPSA50011.zip (BrinooTk/PS5X360 release v1.0.2), for its
    KINECT-source-and-licenses.zip: the 1.0.x Kinect headers no source drop has (fix 44)

Output (--out): a project root laid out like this repository, so the repository's toolchain
steps can build it:
  .deps/xenia-canary/   the core: netplay d2cf527aa + 1.0.0 ps5-port-over-netplay.diff
                        + 1.0.2 commits-since-netplay-merge.diff + uncommitted.diff (hunk by hunk,
                        see layer.py) + core-build-adapters.patch (CMake glue, not the author's)
  include/ platform/ src/   the port's sources, newest drop wins (see TITLE_LAYERS)
  port/                 loose port files the author built from build/ folders (framegen, disc
                        library, update and DLC managers, phone camera, title files of 1.0.0/1.0.2)
  reconstructed/        headers no drop contains, rebuilt from their uses (marked as such)
  include/xbox360ps5/{virtual_skeleton,virtual_depth_camera,factory_calibration}.hpp
                        the 1.0.x Kinect headers from the package's KINECT-source-and-licenses.zip
  online/               net-libs/CMakeLists.txt of 1.0.0 with deps/ (wolfSSL, libcurl, miniupnp)
  mbedtls/              Mbed TLS 3.6.7
  SOURCES.json          where every file came from, and every core hunk's fate
Then fixes/*.patch: the shipped release's binary patches as source changes (fixes/README.md).

Downloaded material is data: nothing from it is executed by this script.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from layer import layer  # noqa: E402

RELEASES = 'https://github.com/BrinooTk/PS5X360/releases/download'
DROPS = {
    'v0.5.9-fix1': ('PS5X360-source-0.5.9-fix1.zip',
                    '56de622c2596116812c5136b5f9b765c4ac18d015d71a8a3c9bb28138fdf948d'),
    'v0.6.0': ('PS5X360-source-0.6.0-changes.zip',
               '315a9df892995511403d2198e95058b850e470d6879d6c31d9e3397746412a93'),
    'v1.0.0': ('PS5X360-source-1.0.0-changes.zip',
               'b8854e2212d19b47a3ca6bcd8e21f15b236998bbc55bb25ac64afbf3af35f2ed'),
    'v1.0.2': ('PS5X360-source-1.0.2-changes.zip',
               'd9bd03469de0f858d6d3ef8ebde04771b26c6fbcad68f765b0f33bdd0f10d606'),
}
NETPLAY_URL = 'https://github.com/AdrianCassar/xenia-canary.git'
NETPLAY_COMMIT = 'd2cf527aa2bcc7e6720aeeaab9e06bcc231f67fa'
# Not needed for the PS5 title (Windows shader compiler, lint, tests, desktop-only libraries).
SUBMODULES_EXCLUDED = {'third_party/DirectXShaderCompiler', 'third_party/DirectX-Headers',
                       'third_party/cpplint', 'third_party/catch'}
# The commits 1.0.0's LICENSE-NOTICE names; checked against the netplay submodules.
NET_LIBS = {'third_party/wolfssl': '3c5eead44904df64e6a5a1f4ebdce377d35a849a',
            'third_party/libcurl': '3163ec065643ba20709f190111f42c3e1fe95273',
            'third_party/miniupnp': 'b91c025446d3ec3d9161cd56c91e200484af7906'}
MBEDTLS = ('https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2',
           'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6')

# The official package (tools/binary-release/build_package.py's BASE_ZIP_SHA256) and the Kinect
# sources inside it (fix 44): the author shipped them with the release, not in a source drop.
OFFICIAL_PACKAGE = ('v1.0.2', 'PPSA50011.zip',
                    '230359c2638f99add3972147e1690e62d8984a37c00181501680bdda7c1096c8')
KINECT_ZIP = ('PPSA50011/KINECT-source-and-licenses.zip',
              '8cff3386817fcfbc04d2c2dd7620ab61e5f4e9319592465050e8bd9a8a4d67c9')
KINECT = 'kinect-1.0.2'  # where the Kinect zip is extracted, next to the drops

S59 = 'v0.5.9-fix1/source'
O59 = S59 + '/release-overlays/build'
V60 = 'v0.6.0'
# (destination in --out, source in the extracted drops). Directories are merged; a later entry
# replaces an earlier file. Order = the author's own chronology (release notes, folder dates).
TITLE_LAYERS = [
    # v0.5.9 Fix 1: the complete working snapshot, then the frozen changes linked into 0.5.9.
    ('include', f'{S59}/include'),
    ('platform', f'{S59}/platform'),
    ('src', f'{S59}/src'),
    ('include', f'{O59}/baseline-cache-20261008/include'),
    ('include', f'{O59}/kinect-update-20261008/native/include'),
    ('include', f'{O59}/kinect-update-20261008/web/include'),
    ('include', f'{O59}/multidisc-20261008/include'),
    ('src/title_switch.cpp', f'{O59}/multidisc-20261008/title_switch.cpp'),
    ('port/multi_disc.cpp', f'{O59}/multidisc-20261008/multi_disc.cpp'),
    ('port/phone_camera.hpp', f'{O59}/kinect-update-20261008/web/phone_camera.hpp'),
    # v0.6.0 (2026-10-09/10), in the order of its README.
    ('platform/ps5/dualsense_input.cpp', f'{V60}/net-fix-20261009/input/dualsense_input.cpp'),
    ('port/socket_errno.h', f'{V60}/net-fix-20261009/socket_errno.h'),
    ('port/overlay_rates.hpp', f'{V60}/framegen-counter-20261009/overlay/overlay_rates.hpp'),
    ('port/disc_catalog.cpp', f'{V60}/bully-loading-20261009/candidate/disc_catalog.cpp'),
    ('port/disc_catalog.hpp', f'{V60}/bully-loading-20261009/candidate/disc_catalog.hpp'),
    ('port/disc_library.cpp', f'{V60}/bully-loading-20261009/candidate/disc_library.cpp'),
    ('port/disc_library.hpp', f'{V60}/bully-loading-20261009/candidate/disc_library.hpp'),
    ('port/readonly_file_pool.hpp', f'{V60}/bully-loading-20261009/readonly-pool/readonly_file_pool.hpp'),
    ('platform/ps5/web_settings.cpp', f'{V60}/kinect-native-20261009/web_settings.cpp'),
    ('port/dlc_manager.hpp', f'{V60}/kinect-native-20261009/dlc_manager.hpp'),
    ('port/phone_camera_console.hpp', f'{V60}/kinect-native-20261009/phone_camera_console.hpp'),
    ('port/phone_camera_tls.hpp', f'{V60}/kinect-native-20261009/phone_camera_tls.hpp'),
    ('port/phone_camera_tls.cpp', f'{V60}/kinect-native-20261009/phone_camera_tls.cpp'),
    ('port/mbedtls_ps5x360_config.h', f'{V60}/kinect-native-20261009/mbedtls_ps5x360_config.h'),
    ('port/vendor', f'{V60}/0.5.9-framegen.4-local-SOURCE/vendor'),
    ('port/source_clock.hpp', f'{V60}/0.5.9-framegen.4-local-SOURCE/source_clock.hpp'),
    ('port/frame_pacing.hpp', f'{V60}/0.5.9-framegen.4-local-SOURCE/presenter/frame_pacing.hpp'),
    ('port/shader_stutter_trace.hpp', f'{V60}/0.5.9-framegen.4-local-SOURCE/shader-agent/shader_stutter_trace.hpp'),
    ('port/update_manager.hpp', f'{V60}/title-updates-20261010/update_manager.hpp'),
    ('port/framegen_config.hpp', f'{V60}/framegen-always-20261010/framegen_config.hpp'),
    ('port/pacing_adapter.hpp', f'{V60}/framegen-always-20261010/pacing_adapter.hpp'),
    ('port/adaptive_pacing.hpp', f'{V60}/framegen-always-20261010/adaptive_pacing.hpp'),
    ('port/presenter_sidecar.hpp', f'{V60}/framegen-always-20261010/presenter/presenter_sidecar.hpp'),
    ('port/shared/output_metrics.hpp', f'{V60}/framegen-always-20261010/shared/output_metrics.hpp'),
    # v1.0.0 (2026-10-22): the title of the release.
    ('include/xbox360ps5/dashboard_mode.hpp', 'v1.0.0/src/include/xbox360ps5/dashboard_mode.hpp'),
    ('src/canary_game_main.cpp', 'v1.0.0/src/title/canary_game_main.cpp'),
    ('src/launcher.cpp', 'v1.0.0/src/title/launcher.cpp'),
    ('port/kinect_skin.cpp', 'v1.0.0/src/title/kinect_skin.cpp'),
    ('port/kinect_skin.hpp', 'v1.0.0/src/title/kinect_skin.hpp'),
    ('port/kinect_home.inc', 'v1.0.0/src/title/kinect_home.inc'),
    ('port/kinect_press.inc', 'v1.0.0/src/title/kinect_press.inc'),
    ('port/kinect_shelf.inc', 'v1.0.0/src/title/kinect_shelf.inc'),
    ('port/kinect_state.inc', 'v1.0.0/src/title/kinect_state.inc'),
    ('port/game_art.cpp', 'v1.0.0/src/title/game_art.cpp'),
    ('port/game_art.hpp', 'v1.0.0/src/title/game_art.hpp'),
    ('port/xex_package_decoder.cpp', 'v1.0.0/src/title/xex_package_decoder.cpp'),
    ('port/xex_package_decoder.hpp', 'v1.0.0/src/title/xex_package_decoder.hpp'),
    ('port/ps5_resolver.cpp', 'v1.0.0/src/title/ps5_resolver.cpp'),
    ('online/net-libs', 'v1.0.0/src/net-libs'),
    # v1.0.2 (2026-10-22): the name resolver and the open phone-camera folder.
    ('port/ps5_resolver.cpp', 'v1.0.2/port/port-src/ps5_resolver.cpp'),
    ('port/phone_camera_tls.cpp', 'v1.0.2/port/phone-folder/phone_camera_tls.cpp'),
]
# A release's version: plain X.Y.Z, ASCII digits (no candidate label, no "v", nothing after it), or
# X.Y.Z.W for a hotfix of X.Y.Z (1.0.4.1; W from 1 to 999, no leading zero).
RELEASE_VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:\.[1-9][0-9]{0,2})?')
# Any version: it becomes a C string literal and a CMake compile definition, so no quotes,
# backslashes, spaces or semicolons (a test build's 1.0.3_candidate_37 is fine).
ANY_VERSION = re.compile(r'[0-9A-Za-z][0-9A-Za-z._+-]{0,63}')

# Fix 44: the 1.0.x Kinect headers over the 0.5.9 ones (unified-20261012 is the folder the
# official build_online.py builds the core from; kinect-usb-20261014's virtual_depth_camera.hpp uses
# the real camera through reconstructed/xbox360ps5/usb_kinect.hpp), and the licences with them.
KINECT_LAYERS = [
    ('include/xbox360ps5/virtual_skeleton.hpp',
     f'{KINECT}/build/unified-20261012/port-include/xbox360ps5/virtual_skeleton.hpp'),
    ('include/xbox360ps5/factory_calibration.hpp',
     f'{KINECT}/build/kinect-usb-20261014/emu-include/xbox360ps5/factory_calibration.hpp'),
    ('include/xbox360ps5/virtual_depth_camera.hpp',
     f'{KINECT}/build/kinect-usb-20261014/emu-include/xbox360ps5/virtual_depth_camera.hpp'),
    ('licenses/kinect-1.0.2/LICENSE', f'{KINECT}/LICENSE'),
    ('licenses/kinect-1.0.2/OPENKINECT-APACHE20', f'{KINECT}/native/kinect/OPENKINECT-APACHE20'),
    ('licenses/kinect-1.0.2/OPENKINECT-CONTRIB', f'{KINECT}/native/kinect/OPENKINECT-CONTRIB'),
]
# Not taken from the layers: kinect-update-20261008/native/include/xenia is a frozen copy of the
# older Canary core's headers (for that overlay's isolated compiles); on the include path it
# would hide the netplay core's own headers.
EXCLUDED = ('include/xenia/',)
EXCLUDED_SEEN = []
# Edits to drop files, each with its reason. Every `old` must occur exactly once.
EDITS = [
    ('include/xbox360ps5/i18n.hpp',
     'inline int SupportedUiLanguage(int id) { return id == 9 || id == 5 ? id : 1; }',
     '// 1.0.x interface languages (reconstructed/xbox360ps5/i18n_ui.hpp; 0.5.9 had pt, en, es).\n'
     'inline int SupportedUiLanguage(int id) {\n'
     '  switch (id) { case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 11:\n'
     '    case 12: case 14: case 16: case 17: case 18: case 19: return id; default: return 1; }\n'
     '}',
     '1.0.0 launcher.cpp offers 17 interface languages; 0.5.9 accepted only pt, en and es'),
    ('include/xbox360ps5/i18n.hpp',
     '#include <cstring>\n',
     '#include <cstring>\n#include "xbox360ps5/i18n_ui.hpp"  // reconstructed: 1.0.x language files\n',
     'the 1.0.x language functions launcher.cpp calls (not in any drop)'),
    ('include/xbox360ps5/i18n.hpp',
     '  if (language == 9) return key;\n  for (const auto& entry : entries) {',
     '  if (language == 9) return key;\n  if (const char* loaded = LoadedUiText(key)) return loaded;\n'
     '  for (const auto& entry : entries) {',
     'Tr() reads /app0/assets/lang/<code>.txt first, as launcher.cpp\'s FramegenText does'),
    ('include/xbox360ps5/canary_audio.hpp',
     '  void DestroyDriver(xe::apu::AudioDriver* driver) override;',
     '  void DestroyDriver(xe::apu::AudioDriver* driver);  // not virtual in the netplay core',
     'netplay\'s AudioSystem::DestroyDriver is no longer virtual (it shuts down and deletes the '
     'driver, as this one does); 1.0.x\'s adapted header is not published'),
    # 1.0.2's dark home theme (reconstructed/dark_theme.hpp has what the official executable does).
    ('src/launcher.cpp',
     '#include "game_art.hpp"\n',
     '#include "game_art.hpp"\n#include "dark_theme.hpp"  // reconstructed: 1.0.2 dark home theme\n',
     'dark theme (reconstructed): header'),
    ('port/kinect_home.inc',
     'if (assets.LoadOwn({kKinectUserSkin, kKinectOwnSkin})) {',
     'if (assets.LoadOwn(dark_theme::SkinFolders(kKinectUserSkin, kKinectOwnSkin))) {',
     'dark theme (reconstructed): assets/skin-dark between the user\'s and the package\'s pictures'),
    ('port/kinect_state.inc',
     'dashboard, online, systemlink };',
     'dashboard, online, systemlink, theme };',
     'dark theme (reconstructed): the home\'s Tema slot'),
    ('port/kinect_state.inc',
     '        KinectAction::systemlink);\n  }\n  return slots;',
     '        KinectAction::systemlink);\n'
     '    // Reconstructed (1.0.2): the theme, as the official home shows it after System Link.\n'
     '    add({0, Asset::kSettings,\n'
     '         std::string(Tr("Tema")) + ": " + (dark_theme::AtStart() ? Tr("ESCURO") : Tr("CLARO")),\n'
     '         Tr("X troca o tema; o emulador reinicia")},\n'
     '        KinectAction::theme);\n'
     '  }\n  return slots;',
     'dark theme (reconstructed): the home\'s Tema slot'),
    ('port/kinect_press.inc',
     '        case KinectAction::none:\n          break;',
     '        case KinectAction::theme: {\n'
     '          // Reconstructed (1.0.2): the theme is read at start, so the emulator starts again.\n'
     '          const bool done = dark_theme::Toggle();\n'
     '          message_ = Tr(dark_theme::ToggleMessage(done));\n'
     '          if (done) restart_ = true;\n'
     '          break;\n'
     '        }\n'
     '        case KinectAction::none:\n          break;',
     'dark theme (reconstructed): the home\'s Tema slot toggles the theme'),
    ('src/launcher.cpp',
     '      {Tr("Reiniciar o emulador"), "", Tr("Fecha e abre o emulador de novo.")}};',
     '      {Tr("Tema"), dark_theme::AtStart() ? Tr("ESCURO") : Tr("CLARO"), Tr("X troca o tema; o emulador reinicia")},\n'
     '      {Tr("Reiniciar o emulador"), "", Tr("Fecha e abre o emulador de novo.")}};',
     'dark theme (reconstructed): Settings row Tema before Reiniciar o emulador, as in the official'),
    ('src/launcher.cpp',
     '  for (int n = 0; n < 13; ++n) {\n    const float y = 122 + n * 42.0f;',
     '  for (int n = 0; n < 14; ++n) {\n    const float y = 122 + n * 40.0f;',
     'dark theme (reconstructed): 14 Settings rows, 40 px apart as in the official'),
    ('src/launcher.cpp',
     '    c.Fill(x + 40, y, 780, 40, focused ? kSurfaceHigh : kSurface, focused ? 1.0f : 0.6f, 10);\n'
     '    if (focused) c.Edge(x + 40, y, 780, 40, kAccent, 0.9f, 10, 2.0f);\n'
     '    c.Text(rows[n].label, x + 64, y + 8, 24, focused ? kText : kBody);\n'
     '    c.Text(rows[n].value, x + 796, y + 8,',
     '    c.Fill(x + 40, y, 780, 38, focused ? kSurfaceHigh : kSurface, focused ? 1.0f : 0.6f, 10);\n'
     '    if (focused) c.Edge(x + 40, y, 780, 38, kAccent, 0.9f, 10, 2.0f);\n'
     '    c.Text(rows[n].label, x + 64, y + 7, 24, focused ? kText : kBody);\n'
     '    c.Text(rows[n].value, x + 796, y + 7,',
     'dark theme (reconstructed): Settings rows 38 px high, text 7 px down, as in the official'),
    ('src/launcher.cpp',
     'void ApplyTheme(bool dashboard) {\n',
     'void ApplyTheme(bool dashboard) {\n'
     '  // Reconstructed (1.0.2): the dark theme keeps the graphite colours (the official executable\n'
     '  // calls ApplyTheme(NxeUi() && !dark) at each of its three call sites).\n'
     '  dashboard = dashboard && !dark_theme::AtStart();\n',
     'dark theme (reconstructed): graphite panel colours with the dark theme'),
    ('port/kinect_home.inc',
     'constexpr uint32_t kKinectInk = 0x282828, kKinectGrey = 0x8c8c8c, kKinectBand = 0x6cb33f;',
     '// Reconstructed (1.0.2): the slots\' type, light over the dark theme\'s pictures (official:\n'
     '// #eef0f2 for #282828, #a3a9ae for #8c8c8c).\n'
     'struct KinectThemeColour {\n'
     '  uint32_t light, dark;\n'
     '  operator uint32_t() const { return dark_theme::AtStart() ? dark : light; }\n'
     '};\n'
     'constexpr KinectThemeColour kKinectInk{0x282828, 0xeef0f2}, kKinectGrey{0x8c8c8c, 0xa3a9ae};\n'
     'constexpr uint32_t kKinectBand = 0x6cb33f;',
     'dark theme (reconstructed): home slot text colours'),
    ('src/launcher.cpp',
     '      const int rows = 13;',
     '      const int rows = 14;',
     'dark theme (reconstructed): 14 Settings rows'),
    ('src/launcher.cpp',
     '        else if (settings_row_ == 12 && key == Key::cross) { settings_.Save(); restart_ = true; }',
     '        else if (settings_row_ == 12 && key == Key::cross) {\n'
     '          // Reconstructed (1.0.2): the theme; it is read at start, so the emulator starts again.\n'
     '          if (dark_theme::Toggle()) { settings_.Save(); restart_ = true; }\n'
     '        }\n'
     '        else if (settings_row_ == 13 && key == Key::cross) { settings_.Save(); restart_ = true; }',
     'dark theme (reconstructed): Settings row 12 toggles the theme, row 13 restarts'),
]
# The author's licence and notice files, kept with the tree.
NOTICES = [
    ('licenses/drop-1.0.0', 'v1.0.0/licenses'),
    ('licenses/drop-1.0.0/LICENSE-NOTICE.txt', 'v1.0.0/LICENSE-NOTICE.txt'),
    ('licenses/drop-1.0.2/LICENSE-NOTICE.txt', 'v1.0.2/1.0.2-package/LICENSE-NOTICE.txt'),
    ('licenses/drop-0.5.9', f'{S59}/licenses'),
    ('licenses/drop-0.5.9/LICENSE', f'{S59}/LICENSE'),
    ('licenses/framegen-COPYING', f'{V60}/0.5.9-framegen.4-local-SOURCE/COPYING'),
]
# The release's binary patches ported to source, applied last (fixes/README.md).
FIXES = HERE / 'fixes'
# From this repository: build glue that is not in any drop (the author's build/ folders are not
# published). Each is the repository's own file, used unchanged.
FROM_REPO = ['platform/ps5/compat', 'tooling/native-game-symbols.map', 'tools/resolve-mesa-weaks.py',
             'assets/fonts', 'LICENSE', 'licenses']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, dest, digest):
    if not dest.exists() or sha256(dest) != digest:
        dest.parent.mkdir(parents=True, exist_ok=True)
        print('download', url, flush=True)
        with urllib.request.urlopen(url) as r, open(dest, 'wb') as f:
            shutil.copyfileobj(r, f)
    actual = sha256(dest)
    if actual != digest:
        sys.exit(f'{dest.name}: SHA-256 {actual}, expected {digest}')
    return dest


def run(*cmd, cwd=None):
    print('+', ' '.join(map(str, cmd)), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def git(tree, *args, capture=False):
    cmd = ['git', '-C', str(tree), '-c', 'core.autocrlf=false', '-c', 'advice.detachedHead=false', *args]
    if capture:
        return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    run(*cmd)


def extract_drops(cache):
    root = cache / 'x'
    for tag, (name, digest) in DROPS.items():
        archive = fetch(f'{RELEASES}/{tag}/{name}', cache / tag / name, digest)
        target = root / tag
        stamp = target / '.sha256'
        if stamp.exists() and stamp.read_text() == digest:
            continue
        if target.exists():
            shutil.rmtree(target)
        with zipfile.ZipFile(archive) as z:
            safe_members(z, name)
            z.extractall(target)
        stamp.write_text(digest)
    return root


def safe_members(z, name):
    """Untrusted archive: no absolute paths or parent references."""
    for info in z.infolist():
        parts = Path(info.filename).parts
        if info.filename.startswith(('/', '\\')) or '..' in parts:
            sys.exit(f'{name}: unsafe member {info.filename}')


def extract_kinect(cache, root):
    """The official package's KINECT-source-and-licenses.zip, checked and extracted to
    <root>/kinect-1.0.2 (the package is downloaded once into the cache)."""
    tag, name, digest = OFFICIAL_PACKAGE
    member, member_digest = KINECT_ZIP
    target = root / KINECT
    stamp = target / '.sha256'
    if stamp.exists() and stamp.read_text() == member_digest:
        return
    package = fetch(f'{RELEASES}/{tag}/{name}', cache / tag / name, digest)
    inner = cache / tag / Path(member).name
    with zipfile.ZipFile(package) as z:
        inner.write_bytes(z.read(member))
    actual = sha256(inner)
    if actual != member_digest:
        sys.exit(f'{member}: SHA-256 {actual}, expected {member_digest}')
    if target.exists():
        shutil.rmtree(target)
    with zipfile.ZipFile(inner) as z:
        safe_members(z, member)
        z.extractall(target)
    stamp.write_text(member_digest)


def build_core(out, drops, report):
    core = out / '.deps/xenia-canary'
    if not (core / '.git').exists():
        core.mkdir(parents=True, exist_ok=True)
        git(core, 'init', '-q')
        git(core, 'remote', 'add', 'origin', NETPLAY_URL)
    git(core, 'fetch', '-q', '--depth', '1', 'origin', NETPLAY_COMMIT)
    git(core, 'checkout', '-q', '-f', '--detach', NETPLAY_COMMIT)
    git(core, 'clean', '-qfdx')
    paths = git(core, 'config', '-f', '.gitmodules', '--get-regexp', 'path', capture=True).split()[1::2]
    wanted = [p for p in paths if p not in SUBMODULES_EXCLUDED]
    git(core, 'submodule', 'update', '--init', '--depth', '1', '--', *wanted)
    for path, commit in NET_LIBS.items():
        actual = git(core / path, 'rev-parse', 'HEAD', capture=True).strip()
        assert actual == commit, (path, actual, commit)
    # 1.0.0: the PS5 port over netplay, whole (TREE-HEAD d8e0a65).
    git(core, 'apply', '--whitespace=nowarn', drops / 'v1.0.0/src/core/ps5-port-over-netplay.diff')
    # 1.0.2: the online core's commits since the netplay merge (partly in 1.0.0 already), then
    # its uncommitted changes.
    for name in ('commits-since-netplay-merge.diff', 'uncommitted.diff'):
        result = layer(core, (drops / 'v1.0.2/online-core' / name).read_text(encoding='utf-8'))
        report['core_layers'][name] = result
        bad = {k: v for k, v in result.items() if v.get('conflict') or v.get('failed_whole')}
        if bad:
            sys.exit(f'{name}: hunks that neither apply nor are present: {bad}')
    # libcurl: the author's one change (blocking PS5 sockets).
    git(core / 'third_party/libcurl', 'apply', '--whitespace=nowarn', drops / 'v1.0.2/libcurl-3163ec0.diff')
    # Not the author's: the CMake glue this repository already uses for the PS5 (no LTO for the
    # PS5 link, POSIX platform sources, FFmpeg's config) so Canary's CMake files build the tree.
    git(core, 'apply', '--whitespace=nowarn', HERE / 'core-build-adapters.patch')
    report['core'] = {
        'base': f'{NETPLAY_URL} {NETPLAY_COMMIT}',
        'applied': ['v1.0.0/src/core/ps5-port-over-netplay.diff',
                    'v1.0.2/online-core/commits-since-netplay-merge.diff (hunk by hunk)',
                    'v1.0.2/online-core/uncommitted.diff (hunk by hunk)',
                    'v1.0.2/libcurl-3163ec0.diff (third_party/libcurl)',
                    'tools/source-1.0.2/core-build-adapters.patch (this repository)'],
        'submodules_excluded': sorted(SUBMODULES_EXCLUDED),
        'changed_files': git(core, 'status', '--porcelain', capture=True).count('\n'),
    }
    return core


def copy_entry(src, dest, origin, manifest, out):
    if src.is_dir():
        for item in sorted(src.rglob('*')):
            if item.is_file():
                copy_entry(item, dest / item.relative_to(src), f'{origin}/{item.relative_to(src).as_posix()}',
                           manifest, out)
        return
    rel = dest.relative_to(out).as_posix()
    if rel.startswith(EXCLUDED):
        EXCLUDED_SEEN.append(origin)
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    entry = {'from': origin, 'sha256': sha256(dest)}
    if rel in manifest:
        entry['replaces'] = manifest[rel]['from']
    manifest[rel] = entry


def build_title(out, drops, report):
    manifest = report['files']
    for dest, source in TITLE_LAYERS + NOTICES:
        copy_entry(drops / source, out / dest, 'drop:' + source, manifest, out)
    for dest, source in KINECT_LAYERS:
        copy_entry(drops / source, out / dest,
                   f'package:{KINECT_ZIP[0]}:{source[len(KINECT) + 1:]}', manifest, out)
    for rel in FROM_REPO:
        copy_entry(REPO / rel, out / rel, 'repo:' + rel, manifest, out)
    copy_entry(HERE / 'reconstructed', out / 'reconstructed', 'reconstructed (this repository)', manifest, out)
    copy_entry(HERE / 'canary-CMakeLists.txt', out / 'canary/CMakeLists.txt',
               'repo:tools/source-1.0.2/canary-CMakeLists.txt', manifest, out)
    for rel, old, new, why in EDITS:
        path = out / rel
        text = path.read_text(encoding='utf-8')
        assert text.count(old) == 1, (rel, old[:60], text.count(old))
        path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
        manifest[rel].setdefault('edits', []).append(why)
        manifest[rel]['sha256'] = sha256(path)


def is_release(argument, environment):
    """--release, or PS5X360_RELEASE set to 1/true/yes (any case)."""
    return bool(argument) or (environment or '').strip().lower() in ('1', 'true', 'yes')


def resolve_version(argument, environment, build_version=HERE / 'BUILD_VERSION', release=False):
    """The version label of this build and where it came from, as (version, source).

    The label on the home screen ("PS5X360Reloaded v<VERSION>") is --version, else PS5X360_BUILD_VERSION,
    else tools/source-1.0.2/BUILD_VERSION, bumped for every console test build
    (1.0.3_candidate_N) so the running build is visible, else 1.0.2. A blank override counts as
    unset. A release takes its version only from --version or PS5X360_BUILD_VERSION, never from
    the candidate file or the fallback, and it must be plain X.Y.Z. Anything else stops the
    assembly before it downloads anything."""
    overrides = (((argument or '').strip(), '--version'),
                 ((environment or '').strip(), 'PS5X360_BUILD_VERSION'))
    version, source = next(((v, s) for v, s in overrides if v), ('', ''))
    if not version and release:
        sys.exit('release build: give its version with --version or PS5X360_BUILD_VERSION')
    if not version and build_version.exists():
        version, source = build_version.read_text(encoding='utf-8').strip(), 'BUILD_VERSION'
    if not version:
        version, source = '1.0.2', 'default'
    if release and not RELEASE_VERSION.fullmatch(version):
        sys.exit(f'release build: version {version!r} ({source}) is not X.Y.Z or X.Y.Z.W')
    if not ANY_VERSION.fullmatch(version):
        sys.exit(f'version {version!r} ({source}) has characters a C string or CMake cannot take')
    return version, source


def write_version(out, report, version, source, release):
    """<out>/VERSION (canary/CMakeLists.txt makes it XBOX360PS5_VERSION) and SOURCES.json's record."""
    (out / 'VERSION').write_text(version + '\n', newline='\n')
    report['version'] = version
    report['version_source'] = source
    report['release'] = bool(release)


def skip_names(arguments, environment):
    """The fixes to leave out: --skip-fix arguments and PS5X360_SKIP_FIXES (comma-separated)."""
    names = [name.strip() for name in (arguments or [])]
    names += [name.strip() for name in (environment or '').split(',')]
    return [name for name in names if name]


def select_fixes(patches, skip):
    """Splits the fixes into those applied and those left out. A skip name is a patch's file
    name, its stem, or its number ("01", "08b"); one that names no patch, or more than one,
    stops the assembly, so a typo cannot build something else than asked for."""
    left_out = set()
    for name in skip:
        matches = [patch for patch in patches
                   if name in (patch.name, patch.stem, patch.stem.split('-', 1)[0])]
        if len(matches) != 1:
            sys.exit(f'--skip-fix {name}: matches {len(matches)} fixes ({", ".join(p.name for p in matches)})')
        left_out.add(matches[0])
    return [p for p in patches if p not in left_out], [p for p in patches if p in left_out]


FIX_NAME = re.compile(r'(\d+)([a-z]*)-')


def unordered_fixes(patches):
    """The fix patches whose names fix_order cannot read (a number, letters, then a dash: 148k-...,
    not 148j2-...); such a name would sort after every other fix whatever its number."""
    return [patch.name for patch in patches if not FIX_NAME.match(patch.name)]


def fix_order(patch):
    """A fix's place in the order the fixes apply: by number, then letter (08, 08b, 09, ..., 99,
    100). Name order alone would put 100-... before 11-...; names without a number go last."""
    match = FIX_NAME.match(patch.name)
    return (int(match.group(1)), match.group(2), patch.name) if match else (1 << 30, '', patch.name)


def apply_fixes(out, report, with_core, skip=()):
    """The shipped release's binary patches, ported to source (fixes/README.md), in number order
    (fix_order).

    Applied after everything else: each patch is a git diff against the assembled tree, core
    paths under .deps/xenia-canary/. git apply runs with the tree as a plain folder (the
    ceiling stops it from finding this repository when --out is inside it). `skip` leaves fixes
    out (a test build that tells whether one of them causes a problem); SOURCES.json lists them
    under fixes_skipped."""
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(out.parent))
    manifest = report['files']
    applied = []
    unordered = unordered_fixes(FIXES.glob('*.patch'))
    if unordered:
        # Fix 148k: 148j2-... sorted after 155c; a fix's name must say its place.
        sys.exit(f'fix patches named outside the number order (<number><letters>-...): {", ".join(sorted(unordered))}')
    selected, left_out = select_fixes(sorted(FIXES.glob('*.patch'), key=fix_order), skip)
    for patch in left_out:
        print('skipped', patch.name, flush=True)
    report['fixes_skipped'] = [f'tools/source-1.0.2/fixes/{patch.name}' for patch in left_out]
    for patch in selected:
        cmd = ['git', '-c', 'core.autocrlf=false', 'apply', '--whitespace=nowarn']
        if not with_core:
            cmd.append('--exclude=.deps/*')
        print('+', ' '.join(cmd), patch.name, flush=True)
        subprocess.run(cmd + [str(patch)], cwd=out, env=env, check=True)
        # Only the file names (ASCII) are read; a fix may add an upstream file byte for byte that is
        # not UTF-8 (fix 145: iryoku/smaa's SMAA.hlsl has Latin-1 box drawing in a comment), as
        # rekey.py reads the patches.
        files = sorted({line[6:].strip()
                        for line in patch.read_text(encoding='utf-8', errors='replace').splitlines()
                        if line.startswith('+++ b/')})
        for rel in files:
            if rel in manifest:
                manifest[rel].setdefault('edits', []).append(f'tools/source-1.0.2/fixes/{patch.name}')
                manifest[rel]['sha256'] = sha256(out / rel)
        applied.append({'patch': f'tools/source-1.0.2/fixes/{patch.name}', 'sha256': sha256(patch),
                        'files': [f for f in files if with_core or not f.startswith('.deps/')]})
    report['fixes'] = applied


def build_online(out, core, cache, report):
    # The layout 1.0.0's net-libs/CMakeLists.txt expects: ../deps/<lib> and ../tree.
    deps = out / 'online/deps'
    deps.mkdir(parents=True, exist_ok=True)
    for name, path in (('wolfssl', 'third_party/wolfssl'), ('libcurl', 'third_party/libcurl'),
                       ('miniupnp', 'third_party/miniupnp')):
        link = deps / name
        if link.is_symlink() or link.exists():
            link.unlink() if link.is_symlink() else shutil.rmtree(link)
        os.symlink(os.path.relpath(core / path, deps), link, target_is_directory=True)
    tree = out / 'online/tree'
    if tree.is_symlink():
        tree.unlink()
    os.symlink(os.path.relpath(core, tree.parent), tree, target_is_directory=True)
    archive = fetch(MBEDTLS[0], cache / 'mbedtls-3.6.7.tar.bz2', MBEDTLS[1])
    target = out / 'mbedtls'
    if target.exists():
        shutil.rmtree(target)
    with tarfile.open(archive) as t:
        t.extractall(out, filter='data')
    (out / 'mbedtls-3.6.7').rename(target)
    report['online'] = {'net_libs': NET_LIBS, 'mbedtls': {'url': MBEDTLS[0], 'sha256': MBEDTLS[1]}}


def settings(args, environ, build_version=HERE / 'BUILD_VERSION'):
    """(release, version, version_source, skip) from the arguments and the environment, checked
    before anything is downloaded: a release with a fix left out stops here."""
    release = is_release(args.release, environ.get('PS5X360_RELEASE'))
    version, source = resolve_version(args.version, environ.get('PS5X360_BUILD_VERSION'),
                                      build_version=build_version, release=release)
    skip = skip_names(args.skip_fix, environ.get('PS5X360_SKIP_FIXES'))
    if release and skip:
        sys.exit(f'release build: no fix may be left out (asked: {", ".join(skip)})')
    return release, version, source, skip


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--out', type=Path, default=REPO / 'build/src102')
    parser.add_argument('--cache', type=Path, default=REPO / '.deps/src102-cache')
    parser.add_argument('--skip-core', action='store_true', help='title files only (no clone)')
    parser.add_argument('--skip-fix', action='append', metavar='NAME',
                        help='leave out a fix (file name, stem or number, e.g. 01); repeatable. '
                             'PS5X360_SKIP_FIXES adds a comma-separated list')
    parser.add_argument('--version', metavar='X.Y.Z',
                        help='the version label (wins over PS5X360_BUILD_VERSION and BUILD_VERSION)')
    parser.add_argument('--release', action='store_true',
                        help='a release build: --version or PS5X360_BUILD_VERSION must be X.Y.Z and '
                             'no fix may be left out (also PS5X360_RELEASE=1)')
    args = parser.parse_args()
    release, version, version_source, skip = settings(args, os.environ)
    print(f'version {version} (from {version_source}){", release build" if release else ""}', flush=True)
    out, cache = args.out.resolve(), args.cache.resolve()
    for sub in ('include', 'platform', 'src', 'port', 'reconstructed', 'online', 'canary', 'licenses'):
        if (out / sub).exists():
            shutil.rmtree(out / sub)
    out.mkdir(parents=True, exist_ok=True)
    drops = extract_drops(cache)
    extract_kinect(cache, drops)
    report = {'drops': {t: {'asset': n, 'sha256': d} for t, (n, d) in DROPS.items()},
              'official_package': {'asset': f'{OFFICIAL_PACKAGE[0]}/{OFFICIAL_PACKAGE[1]}',
                                   'sha256': OFFICIAL_PACKAGE[2],
                                   'kinect_sources': {'member': KINECT_ZIP[0],
                                                      'sha256': KINECT_ZIP[1]}},
              'core_layers': {}, 'files': {}}
    if not args.skip_core:
        core = build_core(out, drops, report)
        build_online(out, core, cache, report)
    build_title(out, drops, report)
    write_version(out, report, version, version_source, release)
    apply_fixes(out, report, with_core=not args.skip_core, skip=skip)
    report['excluded'] = {'paths': list(EXCLUDED), 'files': len(EXCLUDED_SEEN)}
    (out / 'SOURCES.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n')
    counts = {}
    for info in report['files'].values():
        kind = info['from'].split(':', 1)[0].split(' ')[0]
        counts[kind] = counts.get(kind, 0) + 1
    print('assembled', out, counts)


if __name__ == '__main__':
    main()
