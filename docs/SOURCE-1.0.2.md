# Rebuilding PS5X360 v1.0.2 from the published sources

This page records whether the official PS5X360 v1.0.2 `eboot.bin` (package `PPSA50011.zip`,
`eboot.bin` SHA-256 `b1d1b3cb…`) can be rebuilt from the author's public source drops, what
is missing, and how close a rebuild gets.

- Assembly: [`tools/source-1.0.2/assemble.py`](../tools/source-1.0.2/assemble.py)
- Build: [`tools/source-1.0.2/build.sh`](../tools/source-1.0.2/build.sh), workflow
  [`build-source-1.0.2.yml`](../.github/workflows/build-source-1.0.2.yml)
- Comparison: [`tools/source-1.0.2/compare.py`](../tools/source-1.0.2/compare.py)
- The shipped release's binary patches as source changes:
  [`tools/source-1.0.2/fixes`](../tools/source-1.0.2/fixes/README.md) (see
  [Release fixes](#release-fixes-ported-from-the-binary-patches))

## Result

**It builds.** [Run 44497867041](https://github.com/grzegorz2047/PS5X360/actions/runs/44497867041)
assembled the tree, compiled every core and title object and linked a signed `eboot.bin`
(37,551,301 bytes, SHA-256 `65e6e9c0…`; `eboot.elf` `9ef24b10…`). It is a Vulkan-only
reconstruction of 1.0.2, about 22 MiB smaller than the official executable, almost all of it
the OpenGL renderer stack that was never published (see
[Comparison](#comparison-with-the-official-executable)).

On the console (user's test of that build): the launcher, Modern Warfare 2, Black Ops and Dragon
Age II run like the official build; the visible difference was the missing dark home theme,
reconstructed since.

Latest build: [run 44507850129](https://github.com/grzegorz2047/PS5X360/actions/runs/44507850129),
with the `gameinfo.txt` reader ([run 44502949131](https://github.com/grzegorz2047/PS5X360/actions/runs/44502949131))
and the dark theme: `eboot.bin` 37,574,693 bytes, SHA-256 `b7525acb…` (`eboot.elf` `a251fd04…`).
Its official-only strings drop to 18,074; the dark theme's and `gameinfo.txt`'s are now in both.
Not yet tested on a console.

The workflow keeps the built RADV driver and payload SDK in the Actions cache (keyed by the
pinned reference commits) and uses ccache: with the cache, the RADV build and Mesa's packages are
skipped (dependencies 36 s instead of up to 9 min). It runs on `workflow_dispatch` only, which
GitHub offers once the workflow file is on the default branch.

Earlier runs, each fixing what the previous one found:
[44482112072](https://github.com/grzegorz2047/PS5X360/actions/runs/44482112072) (stale core
headers, missing 1.0.x language code),
[44486101760](https://github.com/grzegorz2047/PS5X360/actions/runs/44486101760) (5 files),
[44491870772](https://github.com/grzegorz2047/PS5X360/actions/runs/44491870772) (compiled; 4
undefined symbols at the link).

## Short answer

The **emulator core** of 1.0.2 is fully reproducible from public material. The **title**
(launcher, home screen, platform layer and the port's own features) is not: 1.0.2 was never
built from one source tree. The author linked it from objects of earlier private builds plus a
few recompiled ones (`port/build_online.py --link` keeps every object of the base build
`core-e6cd2e4-20261021`, eboot `0854c878…`, and replaces only the ones that changed). The
sources of many of those retained objects were published in earlier drops; some never were.
The author says so in the 0.5.9 Fix 1 drop: *"Historical source recovery does not establish a
fully reproducible clean build of every retained object."*

So a rebuild is possible, but it is a reconstruction: the newest published version of every
file, a handful of reconstructed headers, and no OpenGL renderer.

## Inputs

| Input | Where | Pinned by |
|---|---|---|
| `PS5X360-source-0.5.9-fix1.zip` | release `v0.5.9-fix1` | SHA-256 `56de622c…` |
| `PS5X360-source-0.6.0-changes.zip` | release `v0.6.0` | SHA-256 `315a9df8…` |
| `PS5X360-source-1.0.0-changes.zip` | release `v1.0.0` | SHA-256 `b8854e22…` |
| `PS5X360-source-1.0.2-changes.zip` | release `v1.0.2` | SHA-256 `d9bd0346…` |
| Xenia Canary netplay | `AdrianCassar/xenia-canary` `netplay_canary_experimental` | commit `d2cf527aa` |
| wolfSSL 5.9.4, libcurl 8.23.0-DEV, miniupnpc | netplay's submodules | `3c5eead`, `3163ec0`, `b91c025` (the commits 1.0.0's notice names) |
| Mbed TLS 3.6.7 | GitHub release archive | SHA-256 `a7e8bcbe…` (the archive the author's Helper build records) |
| `PPSA50011.zip` (official package) and its `KINECT-source-and-licenses.zip` (fix 44) | release `v1.0.2` | SHA-256 `230359c2…` (as `tools/binary-release`) and `8cff3386…`; the 1.0.x Kinect headers no drop has (`KINECT_LAYERS` in `assemble.py`) |
| PS5 toolchain, RADV, payload SDK | `mihawk-99/PS5_Vulkan`, `PS5_Mesa`, `PS5_PayloadSDK` | the commits `build-ps5.yml` pins |

## What came from which drop

### Core (`.deps/xenia-canary`) — complete

1. netplay `d2cf527aa` with its submodules.
2. **1.0.0** `src/core/ps5-port-over-netplay.diff`: applies cleanly (149 files; the tree 1.0.0
   names as `TREE-HEAD` `d8e0a65`).
3. **1.0.2** `online-core/commits-since-netplay-merge.diff`: it starts at the netplay merge
   (`5acad1590`), so it overlaps step 2. Applied hunk by hunk
   ([`layer.py`](../tools/source-1.0.2/layer.py)): every hunk is either already present or
   applies; only one is new (`xam_content_aggregate.cc`). Three new files it carries are
   identical to 1.0.0's.
4. **1.0.2** `online-core/uncommitted.diff`: 29 hunks (XLiveAPI, UPnP, curl, sockets), all apply.
5. **1.0.2** `libcurl-3163ec0.diff` in `third_party/libcurl` (non-blocking reads on PS5 sockets).
6. **1.0.2** `1.0.2-package/core/*.cc` and its diffs are 2026-10-20 snapshots of the pre-netplay
   tree; the netplay tree already contains them (`memory.cc` and `vulkan_shared_memory.cc` are
   identical, `vulkan_command_processor.cc` differs only by newer upstream Canary code), so they
   are not applied.
7. Not the author's: [`core-build-adapters.patch`](../tools/source-1.0.2/core-build-adapters.patch),
   the CMake glue this repository already uses for the PS5 (no LTO, POSIX platform sources,
   FFmpeg's config). The author compiled the core with recorded compiler commands instead of
   Canary's CMake files, so these were never needed there. This repository's own JIT change to
   `ppc_emit_memory.cc` is deliberately **not** applied (the official binary does not have it).

### Network libraries (`online/`)

1.0.0's `src/net-libs/CMakeLists.txt` and `ps5_rng.h`, unchanged, over the netplay submodules.

### Title (`include/`, `platform/`, `src/`, `port/`)

Layered oldest to newest; a later file replaces an earlier one. The exact list is
`TITLE_LAYERS` in `assemble.py`, and `SOURCES.json` in the build artifact records the origin
and hash of every file.

| Layer | Files |
|---|---|
| 0.5.9 Fix 1 `source/` | `include/`, `platform/`, `src/` (the complete 0.5.9 working snapshot) |
| 0.5.9 Fix 1 `release-overlays/` | headers of the frozen 0.5.9 builds; `title_switch.cpp`, `multi_disc.cpp` (multi-disc), `phone_camera.hpp` |
| 0.6.0 | `dualsense_input.cpp`, `socket_errno.h` (net fix); `overlay_rates.hpp`; `disc_catalog`, `disc_library`, `readonly_file_pool.hpp` (Bully loading); `web_settings.cpp`, `dlc_manager.hpp`, `phone_camera_*` (Kinect native); ps5-framegen `vendor/` and `source_clock.hpp`, `frame_pacing.hpp`, `shader_stutter_trace.hpp` (framegen.4); `update_manager.hpp`; `framegen_config.hpp`, `pacing_adapter.hpp`, `adaptive_pacing.hpp`, `presenter_sidecar.hpp`, `shared/output_metrics.hpp` (framegen-always, the last framegen folder) |
| 1.0.0 | `canary_game_main.cpp`, `launcher.cpp`, `kinect_*`, `game_art.*`, `xex_package_decoder.*`, `dashboard_mode.hpp`, `ps5_resolver.cpp` |
| 1.0.2 | `ps5_resolver.cpp` (resolver bound in the link), `phone_camera_tls.cpp` (open phone-camera folder) |

Excluded on purpose: `release-overlays/build/kinect-update-20261008/native/include/xenia/` is a
frozen copy of the *older* Canary core's headers; on the include path it hides the netplay
core's headers (the first CI run failed in ~150 core files because of it).

From this repository (build glue no drop contains): `platform/ps5/compat/`,
`tooling/native-game-symbols.map`, `tools/resolve-mesa-weaks.py`, `assets/fonts/`, licences.

## Gaps

| Gap | Used by | Filled how | Effect |
|---|---|---|---|
| OpenGL game renderer: the "glvk" Vulkan-on-OpenGL layer, 1.0.x `radv_dispatch.cpp`'s `SelectOpenGLDriver`, `xbox360ps5/vulkan_driver.hpp`, the namespaced ps5-opengl SDK (`build/dual-renderer/namespaced-gl-sdk-all`, `libPS5OpenGL.a`) | `canary_game_main.cpp`, core `vulkan_presenter.cc`, the 1.0.2 link | **open** (only 0.5.9 `research/dual-renderer/` and `SDK-1.0.1/sources/ps5-opengl.tar` exist, for an older design). Reconstructed stand-ins: `OpenGLDriverSelected()` is false, `SelectOpenGLDriver` only logs, `glvk::PictureState()` reports "only black", so choosing OpenGL falls back to Vulkan through the launcher's own recovery | Vulkan only; ~22 MiB of the official image |
| `cvars::ps5_frame_interpolation` definition | core `vulkan_presenter.cc`, `presenter_sidecar.hpp` | reconstructed `DEFINE_bool`, registered as the official 1.0.2 registers it (fix 40, eboot 0x1230a6-0x1230be): default false, the official description, category `Vulkan`; still set from `framegen.conf` as before, and every code that reads it is published | none: the definition is exact |
| `home_navigation.hpp` (`build/ui-kinect-20261020`) | `launcher.cpp` | reconstructed: `kinect_ui::FaceButton`, empty `HomeNavigation`, and `HintRgb` with the official 1.0.2 colours (fix 40: cross 0x55a329, circle 0xd84639, square 0x287ac3, triangle 0xe3b52d, as `NxeHint` 0x211f82 and `Canvas::Hint` load them) | none: the button-hint colours are exact; `launcher.cpp` uses only the include and the instance |
| `interface_choices.hpp` (same folder) | `launcher.cpp` (its `#include` only) | reconstructed: empty | none: nothing in the title reads it, so there is nothing to recover (issue #9) |
| `disc_scan.hpp` (`build/disc-20261015`) | `launcher.cpp` | reconstructed: finds no disc | games on a disc in the console's drive are not listed (the official's `DISC …` log lines are absent) |
| `disc_relay.hpp` (client of `disc-helper.elf`, port 9302; protocol unknown) | core `mapped_memory_posix.cc` | reconstructed: never connects | discs the title cannot open directly are refused |
| `xbox360ps5/game_info.hpp` (x360db reader) | `launcher.cpp` | reconstructed: reads the official package's own `/app0/assets/gameinfo.txt` (INI by title id); host-tested by `check-game-info.cpp` (fix 43) on a committed excerpt of that file, and on the whole file when `PS5X360_GAMEINFO` names it | game information tab should work like the official one; untested on hardware |
| 1.0.x `i18n.hpp` (17 interface languages) | `launcher.cpp` | 0.5.9's `i18n.hpp` (pt/en/es table) + reconstructed `i18n_ui.hpp` reading the package's `assets/lang/<code>.txt`, hooked in by three recorded edits; fix 25 keys the interface by English, so the rebuild reads the English-keyed files of [`tools/source-1.0.2/english/lang`](../tools/source-1.0.2/fixes/README.md#language-files-fix-25) instead of the package's (keyed by Portuguese). Fix 43 adds the official loader's second load (0x1c1a50): `/app0/lang/<code>.txt` is read after the package's file into the same table, adding or replacing texts (the package has no `lang/` folder, so nothing changes until one is added) | language codes host-tested against the official tables (0x1e63e48, 0x1e63e90) and every language file parsed by `check-ui-lang-overlay.cpp` (fix 43); untested on hardware |
| `usb_kinect.hpp` (physical USB Kinect and its relay, `build/kinect-relay-20261015`) | the package's 1.0.x `virtual_depth_camera.hpp`, used by core `xboxkrnl_pscam.cc` | fix 44: the 1.0.x `virtual_depth_camera.hpp`, `factory_calibration.hpp` and `virtual_skeleton.hpp` come from the official package's `KINECT-source-and-licenses.zip` (pinned in `assemble.py`); [`usb_kinect.hpp`](../tools/source-1.0.2/reconstructed/xbox360ps5/usb_kinect.hpp) is reconstructed from the official executable (va 0x746510-0x7481e4) and the package's `kinect_tracker.c`: the relay client (127.0.0.1:9301, `KNCT`), the direct `/dev/ugen` camera (VID 045e, PID 02ae/02bf), `/data/kinect-tracker/calibration.bin`, the official 12 `Kinect USB: …` lines | relay and calibration switching checked on the host against a fake relay, the direct camera against a simulated ugen device; not yet tried with a real Kinect. The `kinect-relay-20261015` folder may have held a newer `virtual_depth_camera.hpp` than the package's (the official build put it first on the include path); the official image's layout matches the package's |
| 1.0.x `canary_audio.hpp` | `canary_audio.cpp`, `launcher.cpp` | 0.5.9's, with `override` dropped from `DestroyDriver` (no longer virtual in the netplay core; it did the same as the base) | none expected |
| 1.0.2 dark home theme (`ui-dark.on`, `assets/skin-dark`) | the home and the Settings sheet | reconstructed from the official executable's code ([`dark_theme.hpp`](../tools/source-1.0.2/reconstructed/dark_theme.hpp) and recorded edits to `launcher.cpp`, `kinect_home.inc`, `kinect_state.inc`, `kinect_press.inc`): read once at start; picture folders `/data/homebrew/PPSA50011/skin`, `/app0/assets/skin-dark`, `/app0/assets/skin`; graphite panel colours (`ApplyTheme(NxeUi() && !dark)`); slot type `#eef0f2`/`#a3a9ae` instead of `#282828`/`#8c8c8c`; a "Tema: CLARO/ESCURO" slot after System Link (message, restart) and a "Tema" Settings row before "Reiniciar o emulador" (rows 40 px apart, as in the official) that remove the marker or write `1\n` to it | the console test of run 44497867041 found it missing; the slot's icon (`kSettings`) is our choice |
| `__wrap_sceKernelDebugOutText` | 1.0.2 link (`--wrap`) | the wrap is left out: it belongs to the OpenGL renderer (glvk, issue #6), whose code is the only reader of the driver message it saves | debug output goes to the system unchanged; nothing without glvk |
| Other title sources changed between 0.6.0 and 1.0.0 (input diagnostics, network self-test, cover download, …) | retained objects of `upstream-c28af11-20261020`, `kinect-home-20261021`, `dash-mode-20261020`, `input-fix-20261021`, … (`port.json`, `title-src/` there) | the newest published version is used, with what the official image shows rebuilt: the covers' Microsoft box-art fallback after XboxUnity (fix 41) and the network self-test's timed lines, resolver report and TCP probe (fix 42). The `InputDiag` lines of `input-fix-20261021`'s `xam_input.cc` are deliberately left out: a private Fable II diagnostic, on only when `/app0/input-diagnostic.once` holds `fable-input-20261009`, for one 30-second window | the input diagnostic is absent; other changes that left no strings may still be missing (see below) |
| Compiler commands of the retained objects | every object the author did not recompile | the flags of 0.5.9's `canary/CMakeLists.txt` (title `-O2 -mavx`, all `-march=znver2 -fno-stack-protector`) and Canary's Release `-O3` (the per-object commands are not published) | code differs byte for byte even where sources match; no behaviour change known. Tuning the flags belongs to issue #2 |

## Release fixes ported from the binary patches

The shipped release is the official 1.0.2 executable with binary patches
(`tools/binary-release/patches.py` on branch `fix/mmio-gpr-and-ci`). Each group is ported to
source as one patch in [`tools/source-1.0.2/fixes`](../tools/source-1.0.2/fixes/README.md),
which `assemble.py` applies last. The intent is ported, not the machine code: where the binary
patch had to borrow room (repurposed strings, an unused settings field, a diagnostic counter),
the source does it properly. [Run 44507690116](https://github.com/grzegorz2047/PS5X360/actions/runs/44507690116)
applied all 14 patches and built `eboot.bin` (37,570,981 bytes, SHA-256 `ff26258c…`; `eboot.elf`
`5bd5b0e8…`). With the dark theme merged in,
[run 44511435853](https://github.com/grzegorz2047/PS5X360/actions/runs/44511435853) applied them
after its edits: `eboot.bin` 37,577,765 bytes, SHA-256 `e15468f2…` (`eboot.elf` `e22e1cfb…`).
**Not yet tested on a console.**

| Group | What | Status | Differences from the binary patch |
|---|---|---|---|
| 1 | JIT: no `mfence` around `ldarx`/`lwarx`/`stdcx.`/`stwcx.`; `stwcx.` reserves a word | ported (`01`) | none |
| 2 | no `clock_gettime` pair around every `mprotect` | ported (`02`) | the calls remain while GPU diagnostics are on, so their protection timings stay meaningful; off, those figures read 0 (the binary skipped them always) |
| 3 | no 256-byte XMM copy for access violations | ported (`03`) | general registers always copied (as in the binary); a handler that changes an XMM register without its state copied is passed on rather than writing zeros (no handler does today) |
| 4 | GPU command thread on its own core (CPU 2) | ported (`04`) | the dedicated core is on from the start instead of two forced branches |
| 5 | FSI choice files in `/download0/xbox360ps5` | ported (`05`) | same file names as the binary release (`fsi.c`, `fsi.1`, `fsi.c.new`), so a choice survives switching builds; the official `/app0/render-target-fsi.conf` is still read when there is no `fsi.c` |
| 6 | caches in `/data/homebrew/PPSA50011/cache` (`/data/homebrew/PPSA83600/cache` since fix 46) | ported (`06`) | `/app0/cache` stays as the second candidate (the binary replaced it) |
| 7 | Vulkan cache exit checkpoint 30 s | ported (`07`) | none |
| 8 | selective pipeline waits (RT0 overwritten, ≤ 6 indices or target ≤ 256 px) | ported (`08`) | none |
| 8b | those pipelines compile first (priority 4) | ported (`08b`) | none |
| 9 | "no new frames" notice after 2 s | ported (`09`) | none; the longer banner text is in the language files (below) |
| 10 | VSync on: `VdSwap` paced on a 16.60 ms grid | ported (`10`) | the deadline has its own variable (the binary kept it in the protection-timing counter of group 2) |
| 11 | `framegen.conf` read and written in `/download0/xbox360ps5` first | ported (`11`) | while there is no such file, the packaged `/app0/framegen.conf` is read (the binary went to `/data/homebrew/PPSA50011` directly; the packaged file says off either way) |
| 12 | third `async_shaders` choice "Ligado (experimental)": invisible stand-ins, depth-tested quads do not wait | ported (`12`) | the flag is a core cvar, `async_shader_invisible_stand_ins` (the binary used a padding byte); the debug-level "Creating graphics pipeline state" log line stays (the binary removed it for room) |
| 13 | per-game `fsi` option (Desligada / Padrão / Ligada) | ported (`13`) | a field of its own, `Settings::fsi` (the binary reused the unread `resolution_scale` and forced the draw resolution scale to 1) |
| 14 | — | not applicable | reverted in the binary release |
| lang overrides | English and Polish texts of the new and changed strings (`assets/lang-overrides`) | not applicable to the executable | they are package data: `tools/binary-release/build_package.py` merges them into the package's `assets/lang/*.txt`, which the official executable reads; for the rebuild, fix 25 carries their texts into the English-keyed files of `tools/source-1.0.2/english/lang` (with the 13 other languages of the fork's overrides) |

## Fixes made in source after the port

Fixes numbered from 20 are not binary patch groups: they are changes made in source for problems
found on the console, each with a host check. [`fixes/README.md`](../tools/source-1.0.2/fixes/README.md)
lists all of them; the ones that change how the emulator behaves when something goes wrong are
also listed here.

| Fix | What | Status |
|---|---|---|
| 45 (`45-guest-crash-notice`) | a game that crashes (Minecraft in System Link mode on the v1.0.3 draft, `research/minecraft-online-hang/VERDICT.md`) no longer leaves a silent picture at 0 FPS: the core's crash handler no longer waits forever in `XmaDecoder::Pause` (an idle XMA worker is woken; without a dedicated XMA thread, as started, there is nothing to wait for), it tells the port first (weak hook `Xbox360PS5GuestCrashed`) and then leaves its own "Uh-oh!" modal out, the overlay says the game crashed and how to close it (all interface languages), the STALL report's first line names the crashed thread and its PC, the exit skips the Vulkan cache checkpoint the paused GPU worker cannot serve, and a guest thread is not suspended while it holds the log's lock | host-checked (`tools/check-guest-crash.cpp`); candidate 51 not yet tested on a console. The game's own crash is not fixed: it is the game's code |
| 46 (`46-ps5x360reloaded-identity`) | PS5X360Reloaded has a title ID (PPSA83600), a data folder (`/data/homebrew/PPSA83600`) and a name of its own (window, guide, notifications, web settings, phone camera certificate, version label `PS5X360Reloaded v<VERSION>`), so it installs beside the official PS5X360 (PPSA50011) and never writes to the official app's folder: every path goes through `include/xbox360ps5/app_identity.hpp`, and `tools/check-title-id.py` fails on any other `PPSA50011`. The first start copies the official app's saves and small settings files once, only when the disk has room for all of it (`official_import.hpp`; the official files are only read; marker `.official-import`); settings kept in `/download0` stay with each app. The official Kinect tracker's key is accepted for its two requests (the motion status, then the motion frames) from the console itself. The save folder on drives stays `<drive>/PS5X360/saves` (fix 27) | host checks (`check-app-identity.cpp`, `check-official-import.cpp`, `check-title-id.py`); candidate 53 on the console installed beside PS5X360 and made its own folder, but read the official folder as missing (its checks used `lstat` and `realpath`) and wrote no marker; `1.0.3_candidate_54` looks with `stat`, as the drive scan does there, logs each error with its errno and writes no marker when a folder could not be checked; **the import not yet tested on a console** |
| 47 (`47-hang-watch`) | a game whose process the system ends without any report (Ninja Gaiden II after its logo videos, issue #11) leaves one: a watch thread of its own notices a UI loop or a GPU wait that stands still for 4 s and writes a `HANG report` straight to boot.log and the log stream (not through the log's lock): which thread waits in which Vulkan call and for which GPU submission, the draws and resolves still in flight with their shader hashes, a verdict (GPU hang or a stop outside a GPU wait), where the UI and GPU threads are, and the in-flight shaders' microcode and disassembly in the logs folder; then a line every 5 s until the end. Since test build `1.0.4_candidate_4` a heartbeat every 2 s (loops, frames, GPU ring, last submission, the log writer's progress) goes to `logs/hang-watch.txt` with `write()` and `fsync()`, never waiting for a lock, so a process killed at once still leaves its last state; the game's own `DbgPrint` messages, a title that ends or relaunches itself (`XamLoaderTerminateTitle`/`LaunchTitle`) and the restart that follows, and `exit()`, are written there too. Signals that end a process without a fault are logged, and SIGPIPE is ignored (it ended the process silently). Since `1.0.4_candidate_11` the heartbeat comes every 250 ms with the free system memory and the guest's heaps, big or failed guest allocations are written there, and so are the crash report's lines and every way the title ends (LoadExec, the window's close, a fatal error, the guest's own exit, KeBugCheck, its main thread ending) | host-checked (`tools/check-hang-watch.cpp`); test builds `1.0.4_candidate_1` (no report with Ninja Gaiden II), `1.0.4_candidate_4`, `1.0.4_candidate_8` and `1.0.4_candidate_11` for the console. Diagnosis only: the cause of the hang is found from its report |
| 48 (`48-memexport-loss`) | data a memory export wrote and that was not read back is no longer lost twice in the same place: an upload of the GPU's copy from guest memory over such data (Call of Duty: Black Ops' skinned characters, issue #10: black skin before fix 30, wrong hair in every build) marks those 4 KiB units, and later exports into them are read back like fix 30's small ones; the 30-second summary says how many exports were read back, how many were not and how much exported data was lost | host-checked (`tools/check-memexport-loss.cpp`); test build `1.0.4_candidate_2`: hair right, textures corrupted by fix 30's refresh of the large learned ranges; `1.0.4_candidate_5` refreshes only for fix 30's own readbacks (textures clean, hair right in the bar, wrong outside); `1.0.4_candidate_7` also learns by the exporting shader, so exports that move through a ring are read back: on the console (Call of Duty: Black Ops, the Cuba bar and the street outside) no wrong hair colour and clean textures, 21.9-39.6 FPS, no lower than before; `1.0.4_candidate_10` is the same on the PS5X360Reloaded base (fix 46) |
| 49 (`49-frame-cost`) | the 30-second summary says where a frame's time goes: the GPU's busy time (timestamps per submission), and the GPU command thread's time executing, idle for the guest and waiting for the GPU by cause (frames in flight, memory export and resolve readbacks, queries) or for shader compilation, with a reading (GPU-bound, synchronisation, the guest's CPU time, the command thread) | host-checked (`tools/check-frame-cost.cpp`); test build `1.0.4_candidate_3` (the GPU mostly idle, the GPU thread 85-90% busy on its own work); `1.0.4_candidate_6` splits that work by section, and the build's artifact gets the symbol list |
| 54 (`54-measure-10s`) | the guide's "Measure performance" samples for 10 s (was 5 s), and its row says so in every language | host-checked (`tools/check-measure-10s.cpp`); test build `1.0.4_candidate_15` (with fix 49's frame-time lines) for the console |
| 55 (`55-gpu-global-lock-spin`) | the GPU command thread, which Battlefield 3 and Call of Duty wait for, no longer sleeps every time a guest thread holds the emulator's global lock for a moment: it spins on its own core for up to ~37 us first (the profile had it waiting in that lock for 12-41% of its time), and shared-memory requests for data already on the GPU are answered without the lock also without "Optimized video memory"; a summary line says how often it waited and for how long | host-checked (`tools/check-global-lock-spin.cpp`); test build `1.0.4_candidate_16` (with fix 49's frame-time lines and fix 54's 10 s measurement) for the console |
| 50 (`50-save-folder-reloaded`) | the external drives' saves folder follows the product's name, `<drive>/PS5X360Reloaded/saves`; a drive's old `<drive>/PS5X360/saves` is moved there by one rename before the drives are scanned (both present: the new one used, and what only the old one has, a profile or a title's folder, moved into it; nothing the new one has is replaced or merged into, and what stays in the old one is logged with a notice once), each move logged with its paths | host-checked (`tools/check-save-folder.cpp`, on a fake and a real temporary tree); test build `1.0.4_candidate_9` |
| 51 (`51-multidisc-source`) | in-game disc swap works in the console runtime: a game that asks for another disc (Battlefield 3's campaign on Disc 2, started from Disc 1) gets the sibling image from the same folder instead of 278 failed `XamSwapDisc` calls; the runtime tells the swap which image it launched, a swap carries over to a later title switch, backing out of the game's insert-disc screen is answered, and finding the siblings no longer depends on `realpath` (which fails in the console's sandbox) | host-checked (`tools/check-multidisc-source.cpp`); tested on the console with `1.0.4_candidate_14` (with fixes 52 and 53): Battlefield 3 started from Disc 1 mounted Disc 2 when the campaign was resumed and went on at 26-27 FPS; merge candidate for 1.0.4 |
| 52 (`52-library-disc-number`) | the library tells the discs of a game apart: an image named "... (Disc 2)" keeps " (Disc 2)" in its tile's name also after the first launch names it after the game (Battlefield 3's two tiles both became "Battlefield 3"), and the details line under the name, which is never cut, says "Disc 2" in the interface language | host-checked (`tools/check-library-disc.cpp`); tested on the console with `1.0.4_candidate_14` (with fixes 51 and 53): Battlefield 3's tiles read "(Disc 1)" and "(Disc 2)"; merge candidate for 1.0.4, built alone as `1.0.4_candidate_17` |
| 56 (`56-kinect-pairing`) | pairing a phone as the Kinect sensor is one clear flow: opened from the TV's QR code the phone page connects by itself, says to start the Kinect game (or that the game has Camera motion off) and waits, pairs when the game starts, turns the camera on by itself (or shows one Start button), reconnects after the console restarts into the game, shows one status banner with only the buttons that do something, fits a phone screen, and speaks Polish too (also the certificate page); the pairing key is shown large beside the QR code; an upper-case key is accepted | host-checked (`tools/check-kinect-pairing.cpp`, `tools/check-kinect-page.mjs` with node); test build `1.0.4_candidate_20` (with all the 1.0.4 fixes) for the console |
| 57 (`57-kinect-games-by-code`) | the virtual Kinect works with any Kinect-only game's executable instead of one traced Dragon Ball Z image: the titles are those whose XEX says skeletal tracking required, and the identity callback, the skeleton function and DBZ's hand seam are found by the shape of their code (the Europe DBZ disc, hash `C9561ED1F9AFFED4`, had its NUI start refused and never asked for a frame); the library offers to turn on Camera motion when such a game starts with it off | host-checked (`tools/check-nui-image-scan.cpp`; DBZ Europe and Kinect Sports executables checked locally); test build `1.0.4_candidate_21` for the console |
| 63 (`63-phone-qr-panel`) | the motion panel over a running game shows the camera QR code, the pairing code and how to stand while no phone sends frames (none yet, or none for 3 s), in the panel's own place; the skeleton comes back when frames arrive; with a body, what the game needs: target arms under the skeleton, each hand's reach against the game's threshold, the hold, and too close / too far / not all visible | host-checked (`tools/check-phone-qr-panel.cpp`); test build `1.0.4_candidate_25` for the console; the "Motion panel" option (top right, top left, off) in `1.0.4_candidate_28` |
| 64 (`64-native-mutant-guest-pointer`) | a mutant made for a guest KMUTANT is bound to it: before, every kernel call on it made a new mutant (fix 20 took the old one for another object's), each holding a host mutex, until Dragon Ball Z for Kinect stopped after about two minutes ("mutex lock failed: Cannot allocate memory"); the guest's lock now works | host-checked (`tools/check-native-mutant.cpp`); test build for the console |
| 58 (`58-gpu-wait-lines`) | a diagnostic: the GPU command thread's waits for the GPU that fix 49 could only call "other" are listed by the line that waited (MW2: 91-141 ms/s of them), so the next fix can target the right one | host-checked (`tools/check-gpu-wait-lines.cpp`); test build `1.0.4_candidate_20` (with all the 1.0.4 fixes) for the console |
| 59 (`59-fast-readback-no-wait`) | Call of Duty: Modern Warfare 2 and other games with the fast resolve readback no longer stall the GPU thread waiting for the previous copy (MW2: 138 ms/s of waiting); the data reaches the game a resolve later when the GPU is behind | host-checked (`tools/check-fast-readback.cpp`); console: next 1.0.4 candidate |
| 60 (`60-library-folders`) | issue #12: with an image install (`.ffpkg` / `.ffpfsc`), games copied to `/data/homebrew/PPSA83600/assets/roms` were not found although the empty library pointed there; that folder is now scanned (added once to an existing `game_paths.txt`), the same folder is never scanned twice, and the empty library also names `/data/xbox360/` and the drives' `xbox360` folders | host-checked (`tools/check-library-folders.cpp`); console: 1.0.4_candidate_23 |
| 61 (`61-fault-stack`) | a diagnostic for Ninja Gaiden II's silent end (issue #11): a fault on an exhausted thread stack, which ended the process with nothing written, is reported (an alternate signal stack per thread), with whether it is a stack overflow; the guest's stack switches and protection changes are written as they happen, with how deep the host stack is | host-checked (`tools/check-fault-stack.cpp`); console: 1.0.4_candidate_23 |
| 62 (`62-upload-cost`) | a diagnostic: the summary splits the GPU thread's upload time (Modern Warfare 2: 248 ms/s, 27 us each) into barriers, upload buffers, marking pages valid and copying, with the copy rate, so the next fix can target the right part | host-checked (`tools/check-upload-cost.cpp`); console: 1.0.4_candidate_23 |
| 65 (`65-fault-path`) | Ninja Gaiden II's silent end (issue #11) leaves a report: a fault inside the emulator's fault handler no longer ends the process at once (it is handled, or reported; NG2's: an XMA context clear, emulated inside the handler, writing a watched page), every fault no handler takes is written with the emulator's state of that memory page, and crash reports go to the game log instead of staying in boot.log | host-checked (`tools/check-fault-path.cpp`); console: 1.0.4_candidate_23 |
| 66 (`66-phone-latency`) | Kinect with a phone: the phone page shows its delay by stage (camera, detection, sending, network, hand smoothing) and the console logs it every 2 s while paired, for the end-to-end latency | host-checked (`tools/check-phone-latency.cpp`, `tools/check-phone-latency.mjs`); console: next combined candidate (26) |
| 67 (`67-xma-clear-unwatch`) | Ninja Gaiden II (issue #11): an XMA context clear no longer faults inside the fault handler (the memory watch on the context is lifted first), so the nested faults fix 65 handled, and their log lines, are gone | host-checked (`tools/check-xma-unwatch.cpp`); console: next combined candidate (26) |
| 68 (`68-deferred-watches`) | Call of Duty: Modern Warfare 2: the memory just uploaded to video is protected once per batch of work, merged into runs and checked by content, instead of one ~37 us protection call per upload (~5600 a second); a per-game setting, "Deferred memory watches" (off by default; on in MW2's preset), with a summary line of the calls saved | host-checked (`tools/check-deferred-watches.cpp`); console: 1.0.4_candidate_27 |
| 90 (`90-xctd-query`) | 1.0.5: Kinect Sports and other titles using xcompress transparent decoding get the kernel's answer to "is this file XCTD compressed" (1 for files starting with `0x0FF512ED`, 0 otherwise) instead of a failed query; titles that ask are named in the log | host-checked (`tools/check-xctd-query.cpp`); console: `1.0.5_candidate_7`; the trace of compressed files and LDI calls in `1.0.5_candidate_9` |
| 92 (`92-disc-sector-pad`) | 1.0.5: a disc read that stops at a file's end inside a 2 KiB sector gets the rest of the sector, as a console's sector read (Kinect Sports' one-segment XCTD bundle 4F45161E, after which it crashed) | host-checked (`tools/check-disc-sector-pad.cpp`); console: `1.0.5-work-fix92` |
| 99 (`99-avatar-asset-sizes`) | issue 15: Kinect Sports no longer overwrites its heap after the avatar asset calls fail: the asset sizes leave room for an empty set (the stubs answered 0 bytes, and the title read a component count from the next heap block) | host-checked (`tools/check-avatar-assets.cpp`); console: `1.0.5-work-fix99` |
| 99b (`99b-kinect-identity-stubs`) | issue 15: Kinect Sports' Kinect ID enrolment after "Pick main player" answers as a Kinect ID with nothing stored (biometric read: not found; write, enable and the identity messages: success; camera elevation accepted; title cache and Live upload queue absent) instead of undefined externs and E_FAIL; `1.0.5-work-fix99b2`: FE/2A004 (the identity database's save) and 2A009 succeed too, and the motion status counts refused frames by reason, with a log line when the title's body goes away | host-checked (`tools/check-kinect-identity.cpp`); console: `1.0.5-work-fix99b`, `1.0.5-work-fix99b2` |
| 102 (`102-kinect-tracker`) | issue 15: Kinect Sports lost its player at "Pick main player": the phone tracker (served by the title now) keeps a body while both shoulders are seen, with hips and legs inferred, keeps calibrating through a new picture size, re-centres from the body's distance, goes down to 0.5 m; the frames without a body are logged by reason with the calibration restarts | host-checked (`tools/check-kinect-tracker.mjs`, `tools/check-kinect-tracker.cpp`); console: `1.0.5-work-fix102` |

## Comparison with the official executable

[`compare.py`](../tools/source-1.0.2/compare.py) on the ELF inside each `eboot.bin` (the
official one extracted from `PPSA50011/eboot.bin`, `b1d1b3cb…`; the rebuild's `eboot.elf` of run
44497867041). Neither has section headers, so sizes are per program segment.

| | Official 1.0.2 | Rebuilt |
|---|---|---|
| `eboot.bin` | 60,204,357 B | 37,551,301 B |
| ELF | 61,544,320 B (58.69 MiB) | 38,524,304 B (36.74 MiB) |
| code (execute-only) | 26.08 MiB | 15.77 MiB |
| read-only data | 28.51 MiB | 18.12 MiB |
| data (file / in memory) | 1.35 / 6.64 MiB | 0.90 / 3.02 MiB |
| SCE dynamic-link data | 1.35 MiB | 0.95 MiB |
| printable strings (6+ chars) | 68,489 | 50,684 |
| in both | | 50,383 |
| only in the official | 18,106 | |
| only in the rebuild | | 301 |

What the 18,106 official-only strings are:

- **OpenGL stack, ~17,000**: Mesa/gallium and `opengnm-psbc` paths
  (`/home/runner/work/ps5-opengl/…`), the GLSL compiler's messages and built-in shader sources,
  GL entry point names, `glvk-*.txt` switches and `logs/OpenGL-*.log`. 8,854 match GL/GLSL/Mesa
  patterns outright; most of the other ~8,100 are the same compiler's text (ASTC/BCn decoder
  shaders, SoftFloat, lexer errors).
- **Port features from unpublished sources, about 40 log lines**: `DISC …` (games on the
  drive, 9), `Kinect USB: …` (physical camera and relay, 12; in the rebuild since fix 44), `Network test: …` (6),
  `InputDiag …` (2), `Cover {} downloaded`, `/app0/assets/skin-dark`, `ui-dark.on`,
  `/data/kinect-tracker/calibration.bin` (in the rebuild since fix 44), `/app0/lang/`.
- `/app0/assets/gameinfo.txt` was also official-only in run 44497867041; the reconstructed
  reader added after it reads that file.

The 301 rebuild-only strings are build paths (`../../src102/.deps/xenia-canary/…` from
`__FILE__`), the build's own version text and reconstructed messages.

Feature markers (`compare.py`; a marker string present, not proof the feature works):

| Feature | Official | Rebuilt |
|---|---|---|
| online play (XLiveAPI, libcurl, wolfSSL, UPnP) | yes | yes |
| frame generation (ps5-framegen) | yes | yes |
| automatic small-resolve readback | yes | yes |
| phone camera (Kinect over HTTPS) | yes | yes |
| title updates / DLC manager | yes | yes |
| per-game accurate render targets (FSI) | yes | yes |
| OpenGL game renderer | yes | **no** |
| disc drive games | yes | **no** (see gaps) |
| physical USB Kinect and its relay | yes | yes since fix 44 (reconstructed client, see gaps) |

A byte-identical rebuild is not possible: the official image was linked from objects compiled
at different times with different trees and flags, and part of its sources is unpublished.


## Licensing

PS5X360's own code is MIT and Xenia is BSD-3-Clause, but the executable as a whole is
**GPL-3.0**: it links wolfSSL (GPL-3.0-or-later) and ps5-framegen (GPL-3.0-or-later), and the
author's earlier releases already shipped GPL-3.0 runtime parts. The assembled tree keeps every
licence and notice file of the drops under `licenses/` (`drop-0.5.9`, `drop-1.0.0`,
`drop-1.0.2`, `framegen-COPYING`) next to this repository's `LICENSE` and `licenses/`.

**Anyone who distributes an `eboot.bin` rebuilt this way must distribute it under the GPL-3.0,
with its complete corresponding source** (this repository's `tools/source-1.0.2`, the drops and
the pinned commits it names, and any change made on top), and keep the licence files with it.
The CI artifact is a check, not a release, and is named `…-NOT-FOR-RELEASE`.
