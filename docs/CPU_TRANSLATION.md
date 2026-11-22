> Translated from the original author's Portuguese text.

# CPU: initial HIR components

## M4 continuation: translating synthetic instructions

The original PowerPC emitters are now linked into a probe covering ADDI, ORI,
LWZ and STW. A synthetic nine-word program runs through these emitters;
24 host checks compare registers and guest bytes, immediate sign extension,
RA=0, negative displacement and the before/after optimization states. The sixth
suite passed, bringing the total to 98 host cases. See [CPU_NATIVE_TEST.md](CPU_NATIVE_TEST.md)
for the separate native test. This path does not start the full
frontend/Processor and does not run the JIT.

## M3 continuation: frontend and backend compiled

The build now also produces `libxenia_ppc_frontend.a`,
`libxenia_cpu_compiler.a`, `libxenia_x64_backend.a` and `libxenia_capstone.a`,
both on the host and for the Prospero target. The frontend includes the scanner,
translator, PPC builder and all five emitter categories. The compiler passes and
the common/POSIX files of the x64 backend are included as upstream components.
The compiled POSIX cache is not yet a validated PS5 memory adapter.

The `base/atomic.h` blocker was resolved with an explicit PS5 overlay:
Clang `__atomic` operations with sequential consistency, exchange/fetch-add
returning the previous value, and strong CAS. No other platform was changed.
The PS5 branch is exercised on the host through an explicit define in the test target.

Five suites were run on the host with zero failures:

| Suite | Cases | What it checks |
|---|---:|---|
| PowerPC decoder | 9 | Scalar/VMX/VMX128 instructions and invalid opcode |
| HIR | 20 | Values, blocks, operands and building operations |
| PS5 atomics | 13 | Return values, CAS, wrappers and concurrent updates |
| Optimizer | 24 | Three builds/optimizations, redundant swaps and dead code, preserving the store and its operand |
| Xbyak/Capstone | 8 | x64 generation and decoding, immediate and label resolution |

The Xbyak test uses a caller-provided data buffer and does not execute the
generated code. It does not reserve executable memory. All five probes were
linked for PS5 and had their ELF structure verified; the new probes were not
run on the console. `build/host/cpu-tests.xml` stores the host result.

`python tools/package-cpu-probe.py` validates the ELF64 x86-64 objects inside the
six libraries, generates hashes/revisions in `build/ps5/cpu-receipt.json` and creates
`dist/Xbox360PS5-M3-CPU-development-probe.zip` with the probes, files and notices.
It is a development package, not an installable application.

Compiling a static archive does not resolve all of its linkage. The symbol
survey shows dependencies on the Processor, guest/host memory, config/logs,
thread state and exception handling. These need real implementations before
the frontend or the JIT can be started. They were not hidden behind stubs. Next step:
integrate these services and link a synthetic PPC -> HIR test, then validate
x64 execution inside the native runtime.

## Earlier M2 evidence

Updated on 2026-10-01. The M1 title validated on the console remains separate.

The upstream files `arena.cc`, `hir_builder.cc`, `block.cc`, `instr.cc`,
`opcodes.cc` and `value.cc` were added to the build. No CPU behavior
was replaced with a stub and the Xenia checkout was not modified.

`src/hir_smoke.cpp` links these components into an executable and checks 20
cases. In addition to constant evaluation, it builds an add operation
from a context register, stores the result, finalizes the block and
checks the translation dump. It also checks that the consumer lists are
maintained when operands are replaced and instructions are removed.

## Evidence

- `./build.ps1 Host`: two suites passed, decoder and HIR; HIR with 20 cases
  and zero failures. Run on Linux inside Docker, not on the PS5.
- `./build.ps1 PS5`: libraries and executables linked with the Prospero SDK.
- `tools/verify.py xenia-hir-smoke`: ELF64 x86-64, segments within the file,
  load segment alignment and executable entry point verified.
- SHA256 of the HIR ELF: `be5a941fe6174146c26e7c158dd75ab414c36ffebfb1fc680b92f865ec1b2cfe`.

The HIR ELF is a payload SDK artifact, not an installable native title.
It was not sent to the console. The M1 video/input/decoder evidence does not
validate this new component. The M2 package is for development only.

## What is missing in this phase

The test does not translate PowerPC bytes or execute guest code. Still to be
integrated: `PPCHIRBuilder`, scanner/frontend, compiler passes and the x64 backend.
The PPC builder pulls in Processor, flags, logging and memory; compiling the
emitters in isolation is not enough to obtain a working CPU. The x64 backend
adds Xbyak, Capstone, the code cache and host/guest transitions.

The next verification should link a PPC -> HIR path with a synthetic program,
then test x64 generation and adapt memory/TLS/threads. The executable memory
strategy must be validated specifically for the native runtime.
Building HIR neither requires nor proves that a JIT is available. Vulkan and games
remain pending.
