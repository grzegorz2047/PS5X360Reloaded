> Translated from the original author's Portuguese text.

# Porting Xbox 360 to PS5

## Selected core

[Xenia](https://github.com/xenia-project/xenia), revision
`95a5c3ee250f80c3b9d139658649d9ffb6db3eec`. The chosen version includes the x64
backend and the Vulkan renderer. The port uses these components; it does not try
to convert a Dreamcast emulator into an Xbox 360 one.

## Adaptation map

| Area | Xenia code | PS5 work |
|---|---|---|
| Platform identity | `base/platform.h` | Explicit PS5 overlay, already implemented |
| Xenon PowerPC | `cpu/ppc/`, `cpu/hir/` | CPU runtime linked; synthetic PPC/x64 execution validated on the host; PS5 test pending |
| x64 recompiler | `cpu/backend/x64/` | System V boundaries and vector helpers fixed; validate cache and exceptions on the console |
| Memory | `memory.cc`, `base/memory_posix.cc` | Guest layout, aliases, reservations and faults; do not assume POSIX compatibility |
| Xenos | `gpu/vulkan/`, `gpu/spirv_shader_translator*` | PS5 Vulkan driver, required features and native presentation |
| Audio | `apu/`, audio backend | XMA/FFmpeg and AudioOut; confirm format and pacing |
| Controller | `hid/` | DualSense backend |
| Files | `vfs/`, `cpu/xex_module*`, `kernel/util/xex2_info.h` | Reading content, saves and directories accessible to the title |
| Threads | `base/threading*`, `kernel/xthread*` | TLS, synchronization and scheduling of guest threads |
| Application | `app/`, `ui/` | PS5 startup, library, lifecycle, shutdown and logs |

## Provided references

- [Mihawk PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan): RADV driver,
  winsys and PS5 presentation; comparison with the Xenia provider in
  [PS5_VULKAN_REFERENCE.md](PS5_VULKAN_REFERENCE.md).

- [PS5SX2](https://github.com/Swordpdf/PS5SX2): native app structure,
  integration of an emulator with PS5 Vulkan, and shader cache.
- [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden): RADV linking,
  `VK_KHR_display` surface, frontend and persistence.
  Updated comparison of the adapters and the limits of reuse in
  [PROSPEROEDEN_REFERENCE.md](PROSPEROEDEN_REFERENCE.md).
- [PS5CEMU](https://github.com/premohq/PS5CEMU): organization of the platform
  adapters and separation between the verification build and the app with a real driver.
  The README consulted states that the application has not yet been tested on hardware.
- [NATIVE-EMUS-PS5](https://github.com/klortekhq/NATIVE-EMUS-PS5): runtime
  contracts and organization of milestones per component. Its matrix treats Xenia as
  research; it does not offer an Xbox 360 emulator validated on the console.

## Validation sequence

1. Compile and test the upstream decoder/disassembler (current milestone).
2. Compile the PowerPC frontend, HIR and x64 backend without the desktop frontend.
3. Implement the PS5 adapters for memory, threads and files; compare
   small synthetic PowerPC programs against the Xenia tests.
4. Integrate the Vulkan driver and check the PS5's effective features before
   starting the Xenos command processor. Compare synthetic shaders/frames.
5. Integrate XEX loading, the guest kernel, audio and input; launch test
   content with persistent logs and a complete shutdown.
6. Build a separate PS5 title and validate it on the console; measure games and resolve
   discrepancies before experimenting with upscaling.

No step should be marked as executed on the PS5 merely because it compiles on the
computer. The first build uses the payload SDK: its ELF still needs to be
integrated into the runtime and the native title format.

## Castation experience that applies here

Keep package format, file access and memory privileges separate. A
working payload does not prove that the same code has the same conditions
in a registered title. Make startup report capabilities and fail
with an understandable message before starting an unavailable backend.
Keep the build, symbols and hash so each dump can be matched to the right executable.
Avoid promising performance based solely on the hardware specification.
