> Translated from the original author's Portuguese text.

# M7 — Real PPC/x64 execution in the PS5 title

This test **is not a definitive game test**. It replaces the M4 HIR comparison
with execution on Xenia's real x64 backend: PPC translation, guest
reads/writes, guest/host calls, function resolution and vector preservation.
The name on the console is `Xbox360PS5 Actual CPU Runtime Test`, **PPSA50010**.
PPSA50008, PPSA50009 and the earlier packages remain preserved.

The host runs all 50 cases twice, destroying and recreating the runtime.
The PS5 application runs when X is pressed; pressing X again tests reinitialization.
It only shows `CPU JIT PASS` with status 0, exactly 50 cases and zero failures.
An error while reserving or accessing executable memory is an initialization
failure, not a passed test with zero cases.

The application writes to `/download0/xbox360ps5-m7.log`, including stdout/stderr
of the original test. The temporary memory/cache files also use
`/download0`, not `/app0`, which is read-only. A normal shutdown of the
runtime releases the files; a crash may leave backing files for analysis.
The backing is sparse and is about 4.5 GiB logical, not a game package.

## Fix specific to the native title

Linking the runtime initially introduced `kernel_mprotect`, a dependency
of the payload SDK that has no valid import in the native title. M7 uses wrappers
for `mmap`, `mprotect` and `munmap` linked to `sceKernelMmap`,
`sceKernelMprotect` and `sceKernelMunmap`, the process's own normal APIs.
It does not change privileges. An SCE error becomes an errno/POSIX return value; a denial of
executable memory remains a denial. Seven host cases check the ABI,
64-bit offsets and error propagation. These mocks do not prove the hardware.

## Build and install

In the `castation-buildenv` container with the existing SDK:

```bash
PS5_PAYLOAD_SDK=/ws/Castation/native-ps5/.deps/native/ps5-payload-sdk \
  bash tools/build-native-runtime-probe.sh
python3 tools/package-native-runtime.py
```

Output in `build/native-runtime-stage/dist/PPSA50010` and
`dist/Xbox360PS5-M7-native-runtime-test.zip`, in native folder format.
With the FTP server active and the ShadowMount API reachable, publish a new
folder without overwriting any existing title:

```bash
python3 tools/deploy-native-probe.py --runtime --host IP_DO_PS5
```

Open it from the PS5 menu and press X. Collect the log and confirm that the second
run also finishes. Record the eboot/libc hashes together with the
result. Do not record hardware validation just because the FSELF was
generated or the upload finished.

At the M7 stage, a Sonic test still lacked the guest kernel, full VFS/XEX, the RADV driver, the Xenos
command processor, audio and guest input. The M7 test does not run the
supplied XEX, does not start Vulkan and does not claim compatibility or performance.
For the later M8 integration, see [ENGINE_INTEGRATION.md](ENGINE_INTEGRATION.md).
