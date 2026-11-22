> Translated from the original author's Portuguese text.

# M5: real execution of the CPU core

This stage adds execution of synthetic PowerPC programs through Xenia's original
x64 recompiler. The previous M4 test evaluated HIR; it did not execute the
x64 code produced. **M5 does not start games yet and has not been run on the PS5.**

## Verified result

- Eight suites on the computer, with no failures: the six previous ones (98 checks),
  the memory and file contract (39) and real PPC/x64 execution (50 checks, including
  repeated calls to test stability).
- The execution compares registers and big-endian memory, performs 32 calls
  to a native handler through a synthetic import thunk, resolves a guest function
  that has not been compiled yet, exercises eight vector shifts and keeps fifteen
  computed vectors live across native calls.
- The vector backend is limited to extensions up to MOVBE, including AVX2/FMA,
  without GFNI/AVX512 shortcuts that do not match the PS5's Zen 2.
- The same runtime and its memory test were compiled and linked with the PS5
  SDK. Validation confirms ELF64 x86-64, segment bounds, alignment,
  an executable entry point and the format of every object in the ten libraries.
- The Python tests validate XEX metadata and the verification of files received
  over FTP. The host reports and the hashes are included in the development package.

Do not confuse the development ELF with a registered native application.
The M4 application PPSA50009 and its working packages remain preserved.

## Fixing the call boundary

In this Xenia revision, the generated thunks received target/context/return address
in RCX/RDX/R8 and wrote to the caller's home area. However, the
Linux/PS5 compiler passes those arguments in RDI/RSI/RDX and does not reserve that home area.
The first real test jumped to a vtable instead of the recompiled code.

The overlays fix the host entry without changing the internal guest convention.
On exit, the thunk converts the context and three integer/pointer arguments to
the System V registers, preserves RSI/RDI and XMM1..15 and keeps RAX/XMM0 as
return values. Lazy function resolution uses the same appropriate boundary.

The vector helpers receive addresses of values stored on the stack. Their
SIMD arguments are explicit references under System V; their results
remain SIMD values. This also applies to the tracing declarations and implementations.
The thunk slots were enlarged to save all the required
vectors, while keeping the stack aligned.

`tools/prepare.py` generates the overlays in `build/generated` and
`build/generated-sources`. The Xenia checkout pinned at
`95a5c3ee250f80c3b9d139658649d9ffb6db3eec` remains unmodified.
CMake registers the new headers as dependencies so that it does not reuse stale objects
that still pointed to the upstream headers.

## Memory and platform services

`platform/ps5/memory.cpp` uses mmap/mprotect and real shared files:

- reservations do not replace foreign mappings; a displaced hint is rejected;
- recommit preserves the bytes and the aliases instead of remapping anonymous memory;
- protection, query and release verify ownership and region bounds;
- backing files are exclusive, sized, and removed on close;
- anonymous decommit uses the normal APIs; decommit of shared backing is still
  explicitly rejected, without faking a successful operation;
- a refusal of executable memory is propagated, without changing privileges.

The guest backing was enlarged by one 4 KiB page: the E0000000 alias includes
that offset and the last range did not fit in the original size. The
upstream already adjusts that alias when the allocation granularity is larger than
4 KiB; that path and the page protections need validation on the PS5.

`thread_primitives.cpp` implements TLS, thread identification, yield, sleep
and barriers. It does not implement Thread/Wait/APC or the full guest scheduler.
Logging is limited and synchronous. The POSIX overlays use 64-bit off_t,
handle MAP_FAILED and file offsets, and keep the monotonic clock frequency
in nanoseconds consistent with the counter unit.

`exception_handler.cpp` carries SIGSEGV/SIGILL through the SDK's BSD structure,
validating the size, the registers and the format of the SIMD state. An
incompatible context is rejected; an unhandled fault goes on to the previous handler
or to normal termination. The layout **has not yet been validated on hardware**.
Handlers can only be removed after stopping the threads that may call them.
Fault and logging callbacks also need a reentrancy audit
before serving a game's texture cache and MMIO.

## Chosen content

The `default.xex` supplied for Sonic the Hedgehog was inspected for
metadata only. No game file was modified, uploaded or executed.

| Field | Value |
|---|---|
| Title ID | 534507D6 |
| XEX size | 14,102,528 bytes |
| Image base | 0x82000000 |
| Entry point | 0x82537AE0 |
| Image size | 14,286,848 bytes |
| Imported libraries | xam.xex; xboxkrnl.exe |
| Import records | 66; 256 |

These 322 records do not mean 322 distinct or implemented APIs.
The SHA-256 of the XEX is
`373689b7c8bc839fbc00f55f756da7a1ff55d786d043423606db8bf7f3905c94`.
The local report `build/sonic-preflight.json` includes the count of files
in the folder. The development package does not contain the game or keys.

## Reproducible builds

```powershell
./build.ps1 RuntimeHost
./build.ps1 RuntimePS5
```

In the Linux container already used by the project:

```bash
bash tools/build-runtime.sh host
PS5_PAYLOAD_SDK=/ws/Castation/native-ps5/.deps/native/ps5-payload-sdk \
  bash tools/build-runtime.sh ps5
python3 tools/package-runtime.py
```

Outputs: `build/runtime-host/runtime-tests.xml`,
`build/runtime-ps5/{memory,runtime,development}-receipt.json` and
`dist/Xbox360PS5-M5-runtime-development.zip`. That ZIP is for development;
it is neither an installer nor a game test. The ELFs still require integration
into a native application and validation of execution on the console.

## What is missing to start Sonic

1. Validate, in the PS5 title, execution of the x64 cache, the reservations, memory
   protection, TLS and exception recovery, keeping logs and failures clear.
2. Integrate the real guest kernel, the xam/xboxkrnl import resolver, objects,
   threads, waits/APCs, VFS and lifecycle. The XexModule source already compiles, but
   its full loading and import resolution are not yet wired in
   and exercised in the M5 test.
3. Build the compatible RADV/PS5 driver, validate features and presentation, and
   integrate Xenia's Vulkan provider and Xenos command processor.
4. Integrate audio, guest input, shutdown and game loading through the VFS.
5. Produce and install an application variant that actually starts the XEX,
   follow its first frame and diagnose execution before announcing
   a playable test. Compatibility and performance still need to be measured.

The PS5SX2, ProsperoEden and PS5_Vulkan projects remain references for
runtime, driver and presentation. They neither provide the Xbox 360 guest kernel nor
replace the Xenos backend. See `PS5_VULKAN_REFERENCE.md` and
`PROSPEROEDEN_REFERENCE.md` for the observed differences.
