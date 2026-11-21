# Tools you need, and how to get them

First check what is already installed (`python --version`, `git --version`, `gh --version`,
`curl --version`, `dotnet --list-sdks`). Ask the user before you install anything system-wide;
a `pip install --user` package is usually fine to propose.

| Need | Program | Get it |
|---|---|---|
| Scripts (`assemble.py`, staging, checks) | **Python 3.10+** | python.org installer (Windows/macOS), or `apt install python3` / `dnf install python3`. Run repo scripts with `python -I` (isolated mode). |
| Host compiles and syntax checks of C++20 | **zig** as a C/C++ compiler, through pip | `python -m pip install --user ziglang`, then `python -m ziglang c++ --version`. It bundles clang and libc++/glibc headers, so it works on Windows without Visual Studio or MinGW. On Linux, `clang++-18` (what CI uses) is an alternative: `apt install clang-18`. |
| Git with worktrees | **git** 2.30+ | git-scm.com, or the OS package manager. On Windows, Git for Windows brings Git Bash, `sha256sum` and `curl`. |
| CI runs, artifacts, issues, releases | **GitHub CLI `gh`** | cli.github.com (`winget install GitHub.cli`, `brew install gh`, `apt install gh`), then `gh auth login`. The user signs in. Never handle their token yourself. |
| FTP upload and download, web API calls | **curl** 7.70+ | Built into Windows 10+ (`curl.exe`), macOS and most Linux distributions. |
| UFS2 image (`.ffpkg`) | **.NET SDK 8** + **UFS2Tool** | .NET: dotnet.microsoft.com (`winget install Microsoft.DotNet.SDK.8`, `brew install dotnet-sdk`, `apt install dotnet-sdk-8.0`). UFS2Tool: build it from source at the pinned revision ([commands.md](commands.md#ufs2-image)). Run it with `DOTNET_ROLL_FORWARD=Major` if only a newer runtime is installed. |
| Verified PFS container (`.ffpfsc`, releases only) | **MkPFS** | Fetched and built by `tools/binary-release/make_images.sh` in CI. Local console tests do not need it. |
| SHA-256 | `sha256sum` (Git Bash, Linux), `shasum -a 256` (macOS), `Get-FileHash` (PowerShell) | Already present. |
| FTP on the console | an **FTP server payload** on the PS5 (the one the user already runs alongside ShadowMountPlus; it listens on `<ftp-port>`) | The user runs it. Ask for `<console-ip>` and `<ftp-port>`, and never guess them. |
| Image mounter on the console | **ShadowMountPlus** | The user's setup. It mounts `<image-drive>/<TITLE_ID>.ffpkg` when the app starts: from a USB root by itself, from the extended storage only when listed in `/data/shadowmount/manual.lst`. Its log is `/data/shadowmount/debug.log`. |
| PS5 screenshots (`.jxr`, JPEG-XR) | A JXR decoder: **Windows** WIC/WPF (built in, PowerShell snippet in [commands.md](commands.md#screenshots-jxr-to-png)); **Linux** `JxrDecApp` from jxrlib (`apt install libjxr-tools`), then any converter for the TIFF it writes; **ImageMagick** 7 when built with the JXR delegate (`magick in.jxr out.png`); **macOS** `brew install jxrlib` | |
| Node.js (some checks are `.mjs`) | **node** 18+ | nodejs.org, or the package manager. Only needed for the `.mjs` checks. |

What only CI can do: the PS5 compile (`prospero-clang`, RADV, payload SDK, about 20-30 min) and
the full `run-fix-checks.sh` with `clang++-18` on Linux. Locally you can assemble the tree, run
the Python checks, compile and run single `tools/check-*.cpp` files, and syntax-check core files
with zig.

On Windows, prefer Git Bash for the shell snippets in this skill. In PowerShell, use `curl.exe`
(not the `curl` alias) and `Get-FileHash -Algorithm SHA256`.
