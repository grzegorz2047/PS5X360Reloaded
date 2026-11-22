> Translated from the original author's Portuguese text.

# M8: core integration and native game entry

Status as of 2026-10-02. Project separate from Castation. The M4 test confirmed
on the console and the M7 package remain preserved. M8 has no confirmation of
game execution on the PS5; it is not a promise of compatibility or performance.

## Implemented components

- CPU: Xenia's real Processor, PPC/VMX128 frontend, HIR passes and x64 backend,
  with the System V boundary and guest virtual memory in place since M5.
- Kernel: original xboxkrnl/xam/xbdm modules, objects, user modules, exports,
  thread management and Emulator coordinator.
- VFS: upstream host paths, disc images, STFS and null device.
  Game files are mounted read-only; saves are kept separate.
- Threads: POSIX implementation adapted to BSD, events, semaphores, timers,
  initial suspension and a per-thread APC queue. APCs run during alertable
  waits outside of locks and signal handlers, allowing reentrant enqueue.
- Controller: real InputDriver/XInput driver, buttons, sticks, triggers, packets,
  connection and keystrokes. The native entry acquires scePad and sends samples
  to the driver. Rumble and keystroke repeat are not implemented yet.
- GPU: original CommandProcessor, Xenos/SPIR-V translator, texture/render target
  caches, VulkanGraphicsSystem, VulkanProvider and Presenter; static RADV dispatch
  and a KHR_display surface from the same instance/device used by Xenia.
- Audio: FFmpeg pinned to the Xenia fork, including the XMAFRAMES decoder actually
  consumed by the core; XMA1/XMA2 are enabled as well. Conversion from 6-channel
  big-endian float to stereo S16 and native AudioOut at 48 kHz, bounded queue,
  semaphore-synchronized consumption and release of clients/ports at shutdown.
- Application: native UI context, fullscreen window, ImGui for dialogs,
  GPU/audio/input factories, real loading via LaunchXexFile, pad loop,
  painting, guest-requested title change and shutdown.

## Fixes found through integration

The upstream VFS opened read/write as O_WRONLY, allowed certain write accesses
on readonly devices and published a negative count on I/O errors.
The overlay fixes these three boundaries. Memory initialization in
Emulator returned false, which is equivalent to X_STATUS_SUCCESS: it now returns an error.

APCs do not execute callbacks inside a signal handler. Thread creation
publishes the ID and the suspension state before waking the creating thread.
The sigaction call had an inverted success check in the initial adaptation.

Linking everything together surfaced AES, LZX/mspack, bitmaps, bitstreams, ringbuffer,
UI and the PPC frontend, which the separate probes did not require. They now
take part in the integration, without replacing the kernel or the GPU with mocks.

Enabling XMA1/XMA2 alone is not enough: XmaContext looks for AV_CODEC_ID_XMAFRAMES.
The codec was enabled, the test checks all three decoders, and a context Setup
failure is propagated in release. FFmpeg packets are freed on destruction.

Thread::current_thread_ is TLS with constant initialization to nullptr. constinit
makes this explicit to the other translation units, avoiding the import of a weak
initialization thunk that does not exist. The objects depend explicitly on this
overlay, including when old depfiles pointed to the upstream header.

The application uses the native title CRT and the process's own ordinary
mmap/mprotect/munmap. The recipe rejects privilege-changing helpers.
The AGC stubs are only link metadata for the real console modules;
they are not GPU implementations and are not included in the distributed folder.

## Evidence and limits

`build/kernel-host/integration-tests.xml`: 16 suites, zero failures, including
PPC/x64 execution, VFS, threads/APCs, input, SPIR-V modules accepted by Vulkan,
real Xenia device, codecs/PCM and the combined Emulator lifecycle.
`build/integration-vulkan-transfer.txt`: real transfer on the PC's Vulkan.
The 12 Python tests of the earlier tools also passed.
The four new weak-linking tests bring the Python suite to 16 tests:
strong imports and symbols external to Mesa are never converted to null.

`build/game-module-load.txt`: the Sonic default.xex supplied by the user
was mounted readonly, loaded by the real UserModule with imports resolved,
and entry 82537AE0. No guest entry point was called in this diagnostic.
The upstream dump points to five imports without a complete implementation: XamShowMessageBoxUIEx,
RtlUnwind, RtlCaptureContext, ExThreadObjectType and __C_specific_handler. Therefore,
loading the module does not prove that all services used by the game are ready.

RADV uses Mesa 0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8, PS5_Vulkan reference
3f3ee69607013b345d2baa6d6a37c86745649a08 and SDK fork
95c08f27386fc698f6bbe21dde3030140a41d10b. Xenia remains at
95a5c3ee250f80c3b9d139658649d9ffb6db3eec, FFmpeg at
15ece0882e8d5875051ff5b73c5a8326f7cee9f5. Recipes do not modify the Xenia checkout.

Still needing testing on the PS5: title startup and permissions, signal
context/MMIO layout, JIT memory, presentation of guest frames, access to the
files in app0, AudioOut, controller in game and teardown with a game open.
The Vulkan backend of this Xenia is described upstream as incomplete; the port does not
automatically make all of its features/exports compatible. The native entry
maps D-pad/X/O/square to navigation, confirm, cancel
and Tab in the ImGui dialogs; this path still needs confirmation on hardware.

FTP 192.168.0.19:2121 and API 10101 refused connections during this turn. No new
installation or execution on the console was done. A hardware result must not
be inferred from the builds, from passing on Lavapipe or from loading the XEX.

## Reproduction

`./build.ps1 IntegrationHost`: builds and tests the integration on the PC.
`./build.ps1 IntegrationPS5`: builds the PS5 archives and probes.
`./build.ps1 RADV`: real driver in its own Docker image and Linux cache.
`./build.ps1 NativeGame`: builds the native entry objects and generates
`dist/PPSA50011` and `dist/Xbox360PS5-M8-experimental-game-test.zip` if all
structural checks pass. The recipe depends on the pinned SDK and references
and on the reproducible libc runtime already validated in M7. It does not include games.

The extracted game must be placed in `assets/roms`, preserving default.xex and its data.
The default path is `/app0/assets/roms/default.xex`. For a subfolder, write
the full path in `assets/game.txt`. A persistent override can be placed
in `/download0/xbox360ps5/game.txt`, and arguments are also accepted.
Logs are written to `/download0/xbox360ps5/engine.log`.
To publish the new folder after the checks: `python tools/deploy-native-probe.py
--game --host IP_DO_PS5`. The script compares local/remote bytes before registering
the new title and refuses to overwrite an existing folder. It does not send game data.

## Sources and licenses

Xenia is BSD; the native title tools/startup and PS5_Vulkan are GPL-3.0
or later. FFmpeg and mspack keep LGPL and their original notices. The combined package
includes the notices in licenses. The port's code, overlays and recipes provide
the modifications; the exact commits of the upstream components above identify
their sources. The libraries were not edited inside the checkouts, except
for the build overlays explicitly generated by the port's tools.
No games, BIOS, keys or proprietary implementations are distributed.
