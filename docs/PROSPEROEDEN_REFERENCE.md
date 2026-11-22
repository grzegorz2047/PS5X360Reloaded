> Translated from the original author's Portuguese text.

# Platform reference: ProsperoEden

Review of the public documentation and scripts on 2026-10-01:
https://github.com/blackbearreloaded/ProsperoEden

The local copy consulted is at `336ff39891ee924df94bc421062e2ec11144bc7f`
and does not contain the `headless` directory announced in the online repository. Do not use it
as a complete and current source for the port. No file in that copy was modified.

## Application to Xbox360PS5

| Evidence consulted | Intended use in the Xenia port |
|---|---|
| `docs/BUILDING.md`: pinned dependencies, sources derived at configure time, native toolchain | Keep a pinned Xenia revision and a generated overlay; extend adapters without editing the upstream checkout |
| `UPSTREAM.json`: Boost.Context built per target and compiler-rt emulated TLS | Audit Xenia's context transitions and TLS destructors; do not link host libraries on the PS5 |
| `tools/radv-link-eden.sh`: static RADV recipe, Vulkan loader adaptation and runtime wrappers | Compare with `ui/vulkan/vulkan_provider.cc`; only afterwards validate the extensions and features required by Xenos |
| `tools/check-native-gpu-thread.py`: creation failures, wake/stop/join, shutdown before destroying objects used by the worker | Require equivalent tests when integrating CPU/GPU threads; avoid hangs when closing or switching games |
| `tools/isolate-radv.py`: separation of internal Mesa/RADV symbols | Needs evaluation if an OpenGL launcher and Vulkan emulation coexist; do not add isolation indiscriminately |
| README: native folder, configuration separate from games, logs and shader cache | Reuse the app/data organization; frontend and cache only after the backends work |

The README states that video, audio, controls and saves work in ProsperoEden.
That is a statement by the reference project, not evidence that Xenia
already has these capabilities on the PS5. Eden emulates the Switch; Xenia needs its own
PowerPC/Xenon translation, Xbox 360 kernel and Xenos GPU.

Do not copy Eden's ARM JIT or memory offsets/layout for the Xenon CPU.
Do not enable Vulkan just because a library was linked: check the
features required by Xenia's provider/command processor and validate on the target.
The next Xbox360PS5 step remains PPC -> HIR and the x64 backend; HIR has already been
built for the PS5 but not yet run on the console.

## Verified sources

- https://github.com/blackbearreloaded/ProsperoEden
- https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/BUILDING.md
- https://github.com/blackbearreloaded/ProsperoEden/blob/main/UPSTREAM.json
- https://github.com/blackbearreloaded/ProsperoEden/blob/main/tools/radv-link-eden.sh
- https://github.com/blackbearreloaded/ProsperoEden/blob/main/tools/check-native-gpu-thread.py
- https://github.com/blackbearreloaded/ProsperoEden/blob/main/tools/isolate-radv.py
