> Translated from the original author's Portuguese text.

# Xbox360PS5

Experimental port of Xenia to the PS5, started on 2026-10-01. Project separate
from Castation. The core comes from Xenia; the work here adapts the PS5 platform.

**The emulator already runs games on the PS5; compatibility is under development.**
The current version uses Xenia Canary with the Xenos/Vulkan renderer through RADV,
DualSense controller, audio and a game library. The user confirmed execution
on the console and sent a video showing regressions: a black screen in Sonic when advancing
into the game and green flashes in the Budokai intro. The GPU upload fix
and its validation limits are in
[docs/GRAPHICS_2026-10-03.md](docs/GRAPHICS_2026-10-03.md).
The M0–M8 milestones below record the history of the port, not the current state.
Builds and limits in [docs/ENGINE_INTEGRATION.md](docs/ENGINE_INTEGRATION.md).
The analysis of the five reference emulators is in
[docs/PS5_REFERENCES.md](docs/PS5_REFERENCES.md).

M7 produces **Xbox360PS5 Actual CPU Runtime Test**, PPSA50010, in a native folder,
with the real PPC/x64 runtime and persistent logs. It fixes the title's memory
linking to use the process's own normal APIs. On the host, the 50 cases
run twice with cleanup and reinitialization. The application still awaits
validation on the PS5 and does not load games. Build: `./build.ps1 NativeRuntime`, after
`./build.ps1 RuntimeHost`. See
[docs/ACTUAL_CPU_NATIVE_TEST.md](docs/ACTUAL_CPU_NATIVE_TEST.md).

The current Canary core is pinned at `b083312b8b18e07e6e410b82104191f126722794`;
the original core of milestones M0–M8 uses `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`.
`tools/prepare.py` generates a `platform.h` overlay that recognizes
`__PROSPERO__` as `XE_PLATFORM_PS5`. The platform does not present itself as Linux.
The Xenia checkout stays untouched in `.deps/xenia`.

## Building

On Windows, with Docker and the local `castation-buildenv` image:

```powershell
python tools/prepare.py --fetch
./build.ps1 Host
./build.ps1 PS5
./build.ps1 Native
./build.ps1 NativeCPU
```

The PS5 script uses the SDK already present in the workspace. Another path can be given
with `-Sdk`. On Linux:

```bash
python3 tools/prepare.py --fetch
bash tools/build.sh host
PS5_PAYLOAD_SDK=/caminho/ps5-payload-sdk bash tools/build.sh ps5
```

Outputs:

- `build/host/xenia-platform-smoke`: executable test on the computer.
- `build/ps5/libxenia_ppc_decoder.a`: first real components of the core.
- `build/ps5/xenia-platform-smoke`: PS5 SDK test ELF.
- `build/ps5/receipt.json`: hash, revision, format and validation status.
- `build/ps5/libxenia_hir_values.a`: upstream HIR builder, blocks, instructions and values.
- `build/ps5/xenia-hir-smoke`: development test for these components.
- `build/ps5/hir-receipt.json`: structural validation and hash of the new ELF.

The HIR test has 20 cases passing on the host: arithmetic, byte order,
vectors, operand management, construction and cleanup of translation blocks.
The same components and the test were compiled and linked for the PS5.
This HIR test has not yet been run on the console. See
[docs/CPU_TRANSLATION.md](docs/CPU_TRANSLATION.md).

The M3 continuation compiles the PPC frontend, optimization passes and x64 backend
as libraries for the PS5. Five host suites (74 cases in total) passed.
They include tests of the PS5 atomics and an Xbyak/Capstone comparison; there is no
guest/JIT execution yet. The development package can be generated with
`python tools/package-cpu-probe.py`, after the Host and PS5 builds.

M4 adds a sixth suite, with 24 translation checks of the original PPC
emitters and comparison through a test HIR evaluator (98 host cases in
total). `NativeCPU` produces **Xbox360PS5 CPU Translation Test**, PPSA50009,
separate from M1. Instructions and limits in [docs/CPU_NATIVE_TEST.md](docs/CPU_NATIVE_TEST.md).
M4 was also confirmed on the PS5: the collected log records 41 runs with
24 translation cases and zero failures. The evidence pins the hashes of the executable
and of the libc module. That M4 application has no generated x64 execution and no Vulkan renderer.

M5 integrates the original CPU runtime and fixes the recompiler's System V
boundaries. Eight host suites passed: 98 earlier checks,
39 memory/file checks and 50 real PPC/x64 execution checks. The runtime was also compiled
and linked for the PS5, but has not yet been run on the console and does not start games.
`RuntimeHost` and `RuntimePS5` reproduce the builds; see
[docs/RUNTIME_INTEGRATION.md](docs/RUNTIME_INTEGRATION.md) for evidence,
artifacts, the Sonic inspection and dependencies still required.

The ELF is a component probe, not an installable package nor a playable
emulator. The standalone M0 ELF was not run on the console.

The `Native` target produces the first hardware test in
`dist/Xbox360PS5-M1-native-platform-test.zip`, with a `PPSA50008` folder
that can be registered in ShadowMountPlus. It shows the results of the Xenia
decoder and the DualSense values on the TV. See [docs/FIRST_TEST.md](docs/FIRST_TEST.md).
The title runtime is generated from Castation's local toolchain at
revision `94dfef7`; the screen uses the minimal renderer of the PS5 Native App Boilerplate
at revision `470695e0c557f99ff2df0e36e4df713c5e636526`, with buffer updates.

## Architecture and next milestones

See [docs/PORTING.md](docs/PORTING.md). Each component enters the build with
its own validation before being integrated into the application. The graphics goal is
to use Xenia's Vulkan renderer on the PS5 driver, with AudioOut and DualSense
as native services.
The M1 native test was validated on the PS5: image and controller confirmed by the
user; the log recorded 53 runs of nine cases with zero failures. This
validates the initial components, not game emulation.

Xenia is experimental and its compatibility varies between games. The PS5's
power alone does not guarantee compatibility, resolution or 60 FPS. These metrics
need to be measured after the CPU and GPU work correctly.

## Licenses

Original code of this project: MIT. Xenia: three-clause BSD; fmt: MIT
license and the exception stated in its license. The renderer and the native runtime tools
keep GPL-3.0-or-later; the combined M1 and M4 applications are distributed under
that license, keeping the notices of their components. Copies of the notices
are in `licenses/`, including the LLVM license of the C++ runtime of the SDK used in
M4. No game, BIOS or key is distributed.
