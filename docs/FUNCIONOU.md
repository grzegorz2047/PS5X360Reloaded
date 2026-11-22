> Translated from the original author's Portuguese text.

# It worked: first game running on the PS5 (2026-10-02)

Xbox360PS5 (Xenia ported to the PS5, native title PPSA50011 in
`/data/homebrew/PPSA50011`) ran **Sonic the Hedgehog (2006)** on a PS5
with firmware 13.60 (etaHEN + ShadowMountPlus + kstuff).

Result reported by the user: **"funcionou por hora, mas em partes"** ("it worked
for now, but only partly"). The game's image appeared on the TV. Which parts
still fail was not detailed; this file records only what the log proves.

## What the log of this run proves

Full log: `logs/ultimo-log-ps5.log` (4.9 MB, streamed by the title itself
over TCP port 9100).

- Session of about 2 min 37 s (16:35:27 to 16:38:04 in the ShadowMount log),
  ended by the user reopening the title, with no crash recorded.
- 9265 frames delivered by the game (`VdSwap`), around 59 per second
  over that interval. This is the game's swap count, not a measurement of
  on-screen performance.
- PowerPC-to-x64 JIT executing the game's code; 26 game threads created.
- Vulkan via RADV (`PlayStation 5 GPU (RADV NAVI21)`), 3840x2160 swapchain,
  10 graphics pipelines, DXT1/DXT2_3/DXT4_5/8_8_8_8 textures and render targets
  with 4x MSAA.
- PS5 audio port opened (`AudioOut port ... open`). Whether the sound came out
  correctly was not confirmed.
- Game files read from `/app0/assets/roms` (`win32\archives`,
  `xenon\archives`).

## Problems seen in the same log

- `CreateThread failed` / `Thread creation failed: C0000017` four times: the
  game requested threads that the emulator could not create (out of memory for
  the stack, judging by the error code). This may explain part of the "em partes"
  ("only partly").
- Two texture formats unsupported by the device (signed `k_2_10_10_10`
  and `k_2_10_10_10_AS_16_16_16_16`).
- Writes to unknown GPU registers (0081, 0082, 1E4E).
- Controller, sound and stability in long sessions were not verified.

## What it took to get here

Each item was found through a crash on the console and fixed right after.

1. **C++ exception during ImGui initialization and game configuration**:
   `std::filesystem` throws an error for relative paths, because the title's
   working directory is not searchable. `stat`/`lstat` now answer
   "file does not exist" in these cases (`platform/ps5/libc_bits.c`).
2. **Memory** (`platform/ps5/memory_ps5.cpp`): anonymous `mmap` is limited to
   448 MiB, so everything uses direct memory in reserved ranges. The 4.5 GiB
   guest memory sits at `0x1000000000` along with its mirrors. PS5 pages
   are 16 KiB and Xbox pages are 4 KiB; each host page receives the union of
   the protections of its four subpages.
3. **JIT**: the title's `libc.prx` is loaded at `0x80000000`, where Xenia
   wanted the indirection table. The generated code moved to `0x40000000` and
   the table to `0x50000000` (overlays in `tools/prepare.py`).
4. **Signals**: the PS5 signal context has 48 extra bytes before the
   registers and a size of 0x480 (`platform/ps5/exception_handler.cpp`).
5. **Thread exit**: `thread_local` destructors ran after the emulated
   TLS storage had been freed; the destructor key is now
   created first (`platform/ps5/crash_report.cpp`).
6. **Audio**: the main port requires the system user (255). With audio
   registration failing, Sonic tore down its own sound engine and
   then used a null pointer. There is also a silent fallback driver
   (`platform/ps5/native_audio.cpp`).

## Debugging tools created

- `tools/console.py update | logs | watch | netlog`: swaps the `eboot.bin` on the
  console, reads the kernel log and saves the log streamed by the title.
- In-title crash reporter: signal, registers, symbolizable stack
  (`eboot+0x...` is the address in `build/native-game/llvm-pie.elf`) and, for
  a game crash, the PowerPC registers and the surrounding code.
- `xenia-engine-integration --run-xex <default.xex> <segundos>`: runs the game
  on the PC without a window, to compare its trace with the console's.
- Kernel call trace on the PS5: create `assets/debug.txt` in the title's
  folder.

## Limits

One run of one game is not general compatibility. The Vulkan backend of this
Xenia is described by the project itself as incomplete. The automated host
tests were not rerun after these changes.

## October 3, 2026

- **Black screen in fights (Burst Limit, probably Ultimate Tenkaichi and others):**
  fixed. It was a Xenia defect in the shader translator for Vulkan: the texture
  exponent adjustment was read from the wrong word of the texture constant, and
  any texture with an LOD bias came out multiplied by a wrong power of two
  (Burst Limit's depth came out divided by 65536 and the post-processing
  wiped out the scene). The fix applies to the whole library. Verified on PC: the
  Goku vs. Raditz fight appears complete.
- **Budokai HD Collection:** choosing Budokai 1 or 3 restarts the emulator straight into
  the chosen game (it used to hang). Verified on PC: Budokai 3 reaches the menu with music.
- **3D covers:** each game appears as a box with its original cover and spine.
  The covers are downloaded automatically from XboxUnity when the shelf opens (also via
  Square → Baixar capas ("Download covers")).
- The game ID is read from the executable/ISO itself: covers and patches appear before
  the first boot.
