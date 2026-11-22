> Translated from the original author's Portuguese text.

# PS5 references and Vulkan connection — M6

Code analysis performed on 2026-10-02. The revisions below are snapshots,
not guarantees of compatibility with the firmware or with the user's games.

| Project and revision consulted | What helps in this port | Limit |
| --- | --- | --- |
| [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden/tree/46ed1e8991684528aaeb927d1e6a8547f1db0506), `46ed1e8991684528aaeb927d1e6a8547f1db0506` | `headless/vulkan.cmake`, `headless/vulkan_surface.cpp`, `tools/build-radv-dependencies.sh`, `tools/radv-link-eden.sh`: static driver, KHR_display surface, symbol isolation, TLS and C++ runtime | Its Switch core and its shader translation do not replace PowerPC/Xenos. The Mesa driver pinned by that snapshot differs from the current PS5_Vulkan. |
| [XPSemu](https://github.com/ZiZc3/XPSemu/tree/a1db5328af84aa651709b2004c1645a65ba57182), `a1db5328af84aa651709b2004c1645a65ba57182`; [releases](https://github.com/ZiZc3/XPSemu/releases) | `ps5/configure-ps5.sh`, `ps5/ps5-cross.ini`, `ps5/deps/build-deps.sh`, `ps5/build-title.sh`: FreeBSD cross build, static dependencies and sharing of the RADV recipe | Emulates the original Xbox: QEMU/TCG, x86 CPU and NV2A. It does not provide the Xbox 360 kernel, PowerPC or GPU. Reading the releases is not the same as testing their binaries. |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan/tree/3f3ee69607013b345d2baa6d6a37c86745649a08), `3f3ee69607013b345d2baa6d6a37c86745649a08` | `tools/build-radv.sh`, `tools/radv-link.sh`, `tooling/radv/ps5-cross.ini`: RADV/ACO/NIR, PS5 winsys and VideoOut presentation via KHR_display | The current recipe pins Mesa `0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8` and requires an SDK fork with `libps5platform.a`. The old SDK from our CPU test is not equivalent. The published CTS results belong to the reference project, not to Xbox360PS5. |
| [PS5CEMU](https://github.com/premohq/PS5CEMU/tree/c8f1291a15b1499cb8be881953d3d80dce8567b0), `c8f1291a15b1499cb8be881953d3d80dce8567b0` | `port/ps5/vulkan_display.cpp`, `port/cemu/VulkanPS5.cpp`, `tools/build-radv.sh`: connecting an existing renderer to a static ICD and to VideoOut | The surface code consulted assumes the first device/display and does not check all return values/capabilities. Our adapter handles these cases explicitly. GX2 and the Wii U APIs do not serve as Xenos/Xbox kernel. |
| [PS5SX2](https://github.com/Swordpdf/PS5SX2/tree/d96a7031615734cbe4efb1b3953ddfc23e0dd3aa), `d96a7031615734cbe4efb1b3953ddfc23e0dd3aa` | `ps5/frontend/fe_vk.cpp`, `ps5/coreorbis/orbis-shims/orbis_vk.cpp`, frontend structure and native runtime: loading global/instance/device dispatch with verification and detecting RADV by its linked entry point | Uses PCSX2, with ps5vk and RADV paths; do not mix binaries, CRTs or SDKs from different builds. The PS2 recompilers do not translate PowerPC. |

The repositories were consulted as architecture references. None of their
privilege-escalation helpers, kernel patches or installers were run or
incorporated. The new Vulkan connection code was written in this project under BSD;
it does not copy GPL implementations from the emulators.

## What was implemented

`platform/ps5/vulkan_platform.cpp` receives `PFN_vkGetInstanceProcAddr`.
This will later allow using the entry point of the statically linked RADV ICD,
without `dlopen`, XCB or Win32 on the PS5. The host test uses the Linux loader.

The class owns the instance, logical device, queue and surface. It requests Vulkan
1.1 and, for VideoOut, requires `VK_KHR_surface`, `VK_KHR_display` and
`VK_KHR_swapchain`. It enumerates all devices, modes and planes; it validates
capabilities, plane association, transform, alpha, size and position.
It selects the mode closest to the requested size and then to the refresh rate;
59.94 Hz is accepted for a 60 Hz request. It uses the plane's real stack index.
The queue must support graphics, compute and, when there is a surface, presentation.

Enumerations may return `VK_INCOMPLETE`: there is a limit of eight attempts and
16384 entries, avoiding runaway loops and allocations. Reset/destructor
wait for the device and release resources in order; reinitialization and partial
failures also clean up. As with any Vulkan consumer, the driver
must honor the basic contract: a successfully created instance must
provide its destructor. An invalid ICD without that command does not allow the
instance to be released. The driver must remain loaded until after the class destructor.

The resources belong to a single session; concurrent calls to Initialize/Reset are not
supported. The class is neither a scheduler nor a swapchain implementation.

## Validation and builds

`./build.ps1 VulkanHost` creates a separate container with the Vulkan loader and Mesa
Lavapipe. It runs the eight existing CPU/memory suites, the Vulkan contract
suite and a real transfer test: fill a buffer, barrier, copy,
wait on a fence, invalidate mapped memory and check 1024 words.
Lavapipe is a software driver on the computer: this validates calls,
synchronization and readback, **it does not measure performance or prove the PS5 GPU**.

`./build.ps1 VulkanPS5` compiles the same library and the contract test for
`x86_64-sie-ps5`; the test uses simulated dispatch and does not need RADV. The ELF is
validated structurally. `tools/package-vulkan-platform.py` generates the development
package and records hashes, sources and results. It neither generates an
installable title nor replaces PPSA50009.

## What is missing for a game test

1. Build RADV and its SDK/runtime **at the same revision**, in isolated directories;
   link its ICD entry point and verify the connection on the PS5. The driver is not yet
   included in this build. Importing only the RADV archive without its
   heap/TLS/thread/AGC dependencies and without whole-archive is not the same as integrating Vulkan.
2. Adapt Xenia's real `VulkanInstance`, `VulkanDevice` and presenter objects
   to the static ICD and KHR_display; preserve the feature checks
   that Xenia requires for GPU emulation. This class is not yet called by
   Xenos, and its minimal test device does not enable those features.
3. Integrate the Xenos command processor, memory/EDRAM, SPIR-V, descriptors,
   pipelines, fences and swapchain; test drawing and readback on hardware.
4. Complete Xenia's guest kernel, filesystem, threads and XEX loading.
   The synthetic PPC/x64 translation that passed on the host is not execution of a game.

Do not declare a game ready, the GPU active, upscaling or 60 FPS until these have been proven.
The supplied Sonic game remains in its original directory, unmodified.
