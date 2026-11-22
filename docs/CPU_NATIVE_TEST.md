> Translated from the original author's Portuguese text.

# M4: native PowerPC translation test

Separate application: **Xbox360PS5 CPU Translation Test**, title **PPSA50009**.
Build with `./build.ps1 NativeCPU`. The folder package is output to
`dist/Xbox360PS5-M4-native-cpu-test.zip`.

Copy the PPSA50009 folder to `/data/homebrew/PPSA50009` and register/open it through
the homebrew manager already used on the console. Do not replace PPSA50008.

On the TV, expect the nine decoder cases with PASS, DUALSENSE READY and:

- `PPC HIR CASES 24`
- `PPC FAILURES 0`

Pressing X repeats the tests. The log is at
`/mnt/sandbox/PPSA50009_000/download0/xbox360ps5-m4.log`, viewable over FTP after
opening the application. The summary records cases, failures and runs.

## Actual scope

The test uses Xenia's original emitters for ADDI, ORI, LWZ and STW. A small
test HIR evaluator checks registers and memory bytes before/after
the real simplification and dead-code-elimination passes. It rejects
operations outside the subset instead of ignoring them. This evaluator is not a
production interpreter, nor does it prove JIT execution.

The only method added to the builder's generated header is a reset of the
per-instruction trace information, matching what upstream Emit does in its loop. Linking
by sections retains only the path used by the test; Processor, guest memory,
thread scheduling, faults, scanner and the full Emit are not initialized.
There are no stubs replacing these services; they remain pending.

The upstream configuration and UTF-8 files needed to link this path
were integrated. They compile with `-fno-char8_t`, preserving the C++17 UTF-8 strings
expected by the pinned code. The native application enables exceptions and
RTTI to match Xenia's libraries and to catch evaluator failures.

This variant links `libc++.a`, `libc++abi.a`, `libunwind.a` and the helpers from
`libc.a` of the installed SDK. The M1 minimal allocation-operator runtime
is not part of this link. The renderer's allocation observation hook remains
present; it does not replace the allocator. The generated linker script defines the
`.eh_frame` and `.eh_frame_hdr` bounds required by the unwinder. The hashes of the libraries,
the build script and the linker script are kept in the receipt. These changes are exclusive
to the M4 stage; the SDK, Castation and the installed M1 folder are not modified.

Compilation alone does not demonstrate that TLS and unwinding work on the PS5.
The normal test does not deliberately trigger an exception; exception paths and
full runtime services require additional validation before using the JIT.

The test does not open games, does not initialize Vulkan and does not allocate executable memory.
Only mark the hardware as validated after observing this version's screen/log on the console.

## Result on the console on 2026-10-01

The user confirmed 24 cases, zero failures and the counter incrementing when pressing
X. The log collected over FTP recorded 41 runs, all with 9 decoder cases
and 24 translation cases without failures. The evidence in `evidence/M4_HARDWARE.json` ties
this result to the SHA256 of the uploaded executable. This validates the synthetic
subset and normal use of the C++ runtime in this test; it does not validate games, execution
of generated x64 code, deliberately thrown exceptions or Vulkan.
