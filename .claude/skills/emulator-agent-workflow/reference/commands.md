# Command cookbook

The shell is bash (Git Bash on Windows) unless marked PowerShell. Replace every `<placeholder>`.
Set these once per shell:

```bash
REPO=grzegorz2047/TrinityX360
FTP="ftp://<console-ip>:<ftp-port>"
TREE=<work-dir>/tree          # assembled 1.0.2 tree (~2.6 GB)
```

## Find the branches

Never hardcode a branch name from an earlier session or from this skill. Look the branches up:
```bash
git fetch -q origin
# <integration-branch>: the non-glob entry of source-fix-checks.yml's push trigger, read from the
# most recently updated remote branch that has that workflow (the default branch may not have it)
for b in $(git branch -r --sort=-committerdate --format='%(refname:short)' | grep -v HEAD | head -20); do
  git show "$b:.github/workflows/source-fix-checks.yml" 2>/dev/null | sed -n '/^  push:/,/paths:/p' \
    | grep -m1 branches && echo "  (read from $b)" && break
done
# <release-branch>: the highest release/source-* (version sort, hotfixes included)
git branch -r --list 'origin/release/source-*' --sort=-v:refname | head -1
# the repository's default branch (docs landing page; it does not carry the patch stack)
gh repo view $REPO --json defaultBranchRef --jq .defaultBranchRef.name
# candidates for <line-branch>: recent combined branches. Confirm with the line's owner or the shared notes.
git branch -r --sort=-committerdate | head -15
# which version a branch builds: its label
git show origin/<branch>:tools/source-1.0.2/BUILD_VERSION
```
When two sessions disagree about a branch, the line's owner decides.

## Assemble the source tree

```bash
cd <repo>
python -I tools/source-1.0.2/assemble.py --out "$TREE" --cache <work-dir>/src102-cache
# title files only, no core clone (fast, enough for launcher/UI work):
python -I tools/source-1.0.2/assemble.py --out "$TREE" --cache <work-dir>/src102-cache --skip-core
# leave fixes out (number, stem or file name; repeatable):
python -I tools/source-1.0.2/assemble.py --out "$TREE" --skip-fix 59 --skip-fix 35
```

`$TREE/SOURCES.json` records every drop, fix and edit (and `fixes_skipped`). `$TREE/VERSION` is
what `BUILD_VERSION` said: the label the home screen shows as `TrinityX360 v<label>` (`PS5X360Reloaded v<label>` on builds without `fixes/trinityx360-name.patch`).

## Make or change a patch

```bash
# 1. assemble with the fixes up to yours (move later ones aside, or --skip-fix them)
# 2. make the tree a scratch git repo: the title and the core separately
cd "$TREE" && git init -q && git -c core.autocrlf=false add -A -- . ':!.deps' && git commit -qm base
cd "$TREE/.deps/xenia-canary" && git init -q 2>/dev/null; git -c core.autocrlf=false add -A && git commit -qm base
# 3. edit, then write the patch (title part + core part with the .deps prefix)
cd "$TREE" && git diff > <work-dir>/title.diff
cd "$TREE/.deps/xenia-canary" && git diff --src-prefix=a/.deps/xenia-canary/ --dst-prefix=b/.deps/xenia-canary/ > <work-dir>/core.diff
cat <work-dir>/title.diff <work-dir>/core.diff > <repo>/tools/source-1.0.2/fixes/NN-slug.patch
# 4. prove it applies on a fresh tree (assemble.py applies every patch; it stops on failure)
python -I <repo>/tools/source-1.0.2/assemble.py --out <work-dir>/tree-check --cache <work-dir>/src102-cache
```

If the core is already a git checkout in the tree, commit a baseline there instead of
re-initialising. Files with CRLF lines: check that the hunks keep `\r` (`grep -c $'\r' NN-slug.patch`).
A later patch may depend on an earlier one; never edit an earlier patch to absorb a later one.

Interface text (fix 25 keyed the code by English):
```bash
# add translations for new Tr("...") keys to tools/source-1.0.2/english/extra.txt ([added <code>] sections)
python -I tools/source-1.0.2/english/rekey.py --tree "$TREE"      # writes english/lang/*.txt
python -I tools/check-english-source.py --tree "$TREE"             # fails until every language has the key
```

## Local checks before a push

Touched core and title files, read from your patch:
```bash
grep -h '^+++ b/' tools/source-1.0.2/fixes/NN-slug.patch | sed 's#^+++ b/##' | sort -u
```

**Every translation unit that includes a touched header** (do not skip this):
```bash
h=xenia/gpu/shared_memory.h      # the include path as written in #include "..."
grep -rl --include='*.cc' --include='*.cpp' "#include \"$h\"" "$TREE/src" "$TREE/platform" "$TREE/port" "$TREE/.deps/xenia-canary/src"
```

Syntax check (zig, Linux target, the tree's include roots; run from anywhere):
```bash
INC="-I$TREE/include -I$TREE/reconstructed -I$TREE/.deps/xenia-canary -I$TREE/.deps/xenia-canary/src"
for d in third_party third_party/fmt/include third_party/date/include third_party/xxhash \
         third_party/aes_128 third_party/xbyak third_party/snappy third_party/tomlplusplus/include \
         third_party/utfcpp/source third_party/zarchive/include third_party/Vulkan-Headers/include \
         third_party/VulkanMemoryAllocator/include third_party/SPIRV-Tools/include \
         third_party/capstone/include third_party/pugixml/src third_party/disruptorplus/include; do
  [ -d "$TREE/.deps/xenia-canary/$d" ] && INC="$INC -I$TREE/.deps/xenia-canary/$d"
done
for f in <each touched or including .cc/.cpp>; do
  python -m ziglang c++ -target x86_64-linux-gnu -std=c++20 -fsyntax-only \
    -Wall -Wno-unused-function -Wno-unused-command-line-argument $INC "$TREE/$f" || echo "FAIL $f"
done
```
Files that need PS5, X11 or glslang headers (e.g. `src/canary_game_main.cpp`) and code under
`#if XE_PLATFORM_PS5` only build in CI. Name them in your report as "CI only".

Host check with zig (built from the tree's root, with the flags `run-fix-checks.sh` uses):
```bash
cp tools/check-slug.cpp "$TREE/tools/"
(cd "$TREE" && python -m ziglang c++ -std=c++20 -O1 -g -pthread -Wall -Wno-unused-function \
   -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
   tools/check-slug.cpp -o <work-dir>/check-slug && <work-dir>/check-slug)
```
On Windows the native zig target runs most checks. Checks that fork, use signals or POSIX
sockets need Linux: run them in CI, or add `-target x86_64-linux-gnu -c` for a compile-only check.

Python checks and the patch syntax check:
```bash
python -I tools/check-patch-syntax.py
python -I tools/check-assemble-skip.py --tree "$TREE"
```

Full runner (Linux with clang-18; CI runs exactly this):
```bash
bash tools/source-1.0.2/run-fix-checks.sh "$TREE"        # CXX_HOST / PYTHON_HOST override the tools
```
`CXX_HOST` must be a single executable. To use zig, point it at a wrapper script that runs
`exec python -m ziglang c++ "$@"`.

## CI

```bash
git push -u origin fix/<slug>                                   # runs source-fix-checks.yml (host checks)
gh workflow run build-source-1.0.2.yml --repo $REPO --ref fix/<slug>
gh workflow run build-source-1.0.2.yml --repo $REPO --ref fix/<slug> -f skip_fixes=35,59   # bisect build
gh run list --repo $REPO --workflow build-source-1.0.2.yml --branch fix/<slug> --limit 3 \
   --json databaseId,status,conclusion,headSha,createdAt
gh run watch <run-id> --repo $REPO --interval 120 > /dev/null; gh run view <run-id> --repo $REPO --json conclusion --jq .conclusion
gh run view <run-id> --repo $REPO --log-failed | tail -80
gh run download <run-id> --repo $REPO -n eboot-1.0.2-from-source-NOT-FOR-RELEASE -D <work-dir>/art-<label>
```
Other inputs of the build: `lto=thin`, `title_opt=-O3` (test builds only).

Check the label the artifact carries **before** staging:
```bash
grep -aoE '(TrinityX360|PS5X360Reloaded) v[0-9][0-9A-Za-z._-]*' <work-dir>/art-<label>/src102-out/eboot.elf | sort -u
```

## Stage a test package

```bash
git worktree add <work-dir>/release-tools origin/<release-branch>     # once; refresh with git -C ... checkout
python -I tools/stage-reloaded-candidate.py \
  --base <work-dir>/base/PPSA50011 \        # unpacked official v1.0.2 package (the user's copy)
  --artifact <work-dir>/art-<label> \
  --label <label> \
  --package-repo <work-dir>/release-tools \
  --out <work-dir>/stage-<label>
```
It refuses an eboot without the label, or one that still carries the official label. It also
refuses an artifact without exactly 16 language files, refuses when there is too little free
space, and stops when the stage's size or file count is far from the base's.
`param.json`'s `contentVersion` comes from the label (`1.0.5-work-fix91e` → `1.0.5`). A test of
a hotfix line needs `--content-version` above every shipped version (e.g. `1.0.4.2`).

## UFS2 image

One-time UFS2Tool build (the revision `tools/binary-release/make_images.sh` pins):
```bash
git clone https://github.com/SvenGDK/UFS2Tool.git <work-dir>/UFS2Tool
git -C <work-dir>/UFS2Tool checkout b5307a60d5b4e3a68ba680e0e33cfadf05017c77
dotnet build <work-dir>/UFS2Tool/UFS2Tool.csproj --configuration Release --output <work-dir>/UFS2Tool/.build
```
Image (volume label = title id; the image's file name is what ShadowMountPlus mounts):
```bash
DOTNET_ROLL_FORWARD=Major dotnet <work-dir>/UFS2Tool/.build/UFS2Tool.dll newfs -O 2 -b 65536 -f 8192 \
  -D <work-dir>/stage-<label>/PPSA83600 <work-dir>/PPSA83600-<label>.ffpkg PPSA83600
```

## FTP on the console

The tool (one image, only after the user said the app is closed or idle):
```bash
CONSOLE_IP=<console-ip> FTP_PORT=<ftp-port> IMAGE_DRIVE=<image-drive> SIDE_DIR=<side-dir> \
  bash tools/upload-console-image.sh --app-closed <work-dir>/PPSA83600-<label>.ffpkg <label>
# also make it the active image:
CONSOLE_IP=<console-ip> FTP_PORT=<ftp-port> IMAGE_DRIVE=<image-drive> SIDE_DIR=<side-dir> \
  bash tools/upload-console-image.sh --app-closed <image> <label> --activate <label of the current active image>
```
It lists `<side-dir>` first and refuses an existing name. It deletes a partial file at once when
an upload fails (curl 55 = USB stick full) and verifies SHA-256 by downloading the file back.
`--activate` swaps only when:
- `/mnt/shadowmnt/` lists nothing;
- no other drive (`/mnt/usb0-7`, `/mnt/ext0-1`; `OTHER_DRIVES` overrides the list) holds a `PPSA83600.ffpkg`;
- on `/mnt/extN`, `/data/shadowmount/manual.lst` lists the image's path.
`SIDE_DIR` defaults to `IMAGE_DRIVE`; `PENDRIVE` is still accepted for `IMAGE_DRIVE`.

By hand:
```bash
curl -s --list-only "$FTP<side-dir>/"                              # names
curl -s "$FTP<side-dir>/" | grep -i ppsa                           # sizes (spot partial files)
curl -s "$FTP/mnt/shadowmnt/"                                      # empty = nothing mounted
for d in /mnt/usb0 /mnt/usb1 /mnt/usb2 /mnt/usb3 /mnt/ext0 /mnt/ext1; do   # exactly ONE active image
  curl -s --list-only "$FTP$d/" 2>/dev/null | grep -qx 'PPSA83600.ffpkg' && echo "active image on $d"; done
# upload as a side file (creating the subfolder if needed), then verify by downloading it back
curl -sS --max-time 900 --ftp-create-dirs -T <img> "$FTP<side-dir>/PPSA83600.ffpkg.v<label>"
a=$(sha256sum <img> | cut -c1-64); b=$(curl -s --max-time 900 "$FTP<side-dir>/PPSA83600.ffpkg.v<label>" | sha256sum | cut -c1-64); [ "$a" = "$b" ] && echo OK || echo MISMATCH
# delete (a partial or superseded image; never the mounted one)
curl -s "$FTP/" -Q "DELE <side-dir>/PPSA83600.ffpkg.v<old-label>" > /dev/null
# swap the active image (only when /mnt/shadowmnt/ is empty, the app is closed and no other drive has one)
curl -s "$FTP/" -Q "RNFR <image-drive>/PPSA83600.ffpkg" -Q "RNTO <side-dir>/PPSA83600.ffpkg.v<current-label>" \
               -Q "RNFR <side-dir>/PPSA83600.ffpkg.v<new-label>" -Q "RNTO <image-drive>/PPSA83600.ffpkg" > /dev/null
```

ShadowMountPlus (images on the extended storage, and checking what it mounted):
```bash
curl -s "$FTP/data/shadowmount/manual.lst"                          # one image path per line (ext storage only)
# add <image-drive>/PPSA83600.ffpkg there only with the user's yes: download, append the line, upload back
curl -s "$FTP/data/shadowmount/debug.log" | grep 'mount_img.lnk created' | tail -3   # after the app starts
curl -s "$FTP/data/shadowmount/debug.log" | grep -i 'duplicate' | tail -3            # a second active image was ignored
```
Wait for the console to come back after a reboot or crash (gently, in the background):
```bash
until curl -s -m 8 "$FTP/" > /dev/null; do sleep 60; done
```

## Logs from the console

```bash
curl -s --list-only "$FTP<data-root>/logs/" | sort | tail -20
curl -s "$FTP<data-root>/logs/<file>" -o <work-dir>/logs/<run-name>/<file>
curl -s "$FTP<data-root>/logs/" | grep -E 'hang-watch|Game-' | tail      # sizes and times
```
Without FTP, the settings page serves logs too (key below): `GET /api/logs` lists them,
`/logs/<name>?k=<key>` downloads one, and `/logs.zip?k=<key>&game=<TITLEID>` zips one game's.
Keep each run's logs in their own folder named after the label and scene.

## Web settings API

```bash
KEY=$(curl -s "$FTP<data-root>/web-settings.key" | tr -d '\r\n')    # 6 hex digits, also shown on the TV
W=http://<console-ip>:<web-port>
curl -s -H "X-Key: $KEY" "$W/api/state"                               # options, values, games (ids)
# ONLY with the user's approval:
curl -s -H "X-Key: $KEY" --data "scope=<game id or empty for every game>&key=<option key>&value=<n>" "$W/api/set"
```
`value` is an integer from -1 to 64 (the option's choice index); `-1` on a game scope returns the
option to the general/recommended value. The game log confirms the change with
`Web settings: <key> set to <n> for <scope>`. Read the option keys and choices from
`/api/state`, never guess them.

## Console-side diagnostic file (bisect without rebuilding)

Fix 91's `readback-keys.txt`, in `<data-root>/` (or `/download0/xbox360ps5/`), is re-read when it
changes (checked once a second):
```
# default for keys not listed
default fast
0F05C000 full            # every length at this address
0E338000/4194304 full    # this address and length (wins over an address-only line)
```
Upload it with `curl -T readback-keys.txt "$FTP<data-root>/readback-keys.txt"`. The log echoes
`Readback keys: <file>: default ..., N keys listed, M lines not understood`. Bisect by halving the
listed keys between runs. Delete the file when done.

## Screenshots (JXR to PNG)

PowerShell (Windows, WIC/WPF built in):
```powershell
Add-Type -AssemblyName PresentationCore
$in = "<shot>.jxr"; $out = "<shot>.png"
$s = [IO.File]::OpenRead($in)
try {
  $d = New-Object System.Windows.Media.Imaging.WmpBitmapDecoder($s, 'PreservePixelFormat', 'OnLoad')
  $c = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap($d.Frames[0], [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
  $e = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
  $e.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($c))
  $o = [IO.File]::Create($out); $e.Save($o); $o.Close()
} finally { $s.Close() }
```
Linux/macOS: `JxrDecApp -i <shot>.jxr -o <shot>.tif` (jxrlib), then `magick <shot>.tif <shot>.png`,
or `magick <shot>.jxr <shot>.png` when ImageMagick has the JXR delegate. Then open the PNG and
look at it yourself (a 4K shot is about 12 MB; crop the region of interest when you compare).

## Releases

```bash
git worktree add -b release/source-X.Y.Z <work-dir>/rel-X.Y.Z origin/<release-branch>   # the previous release
# merge <line-branch>; add the previous version to package.py SHIPPED_CONTENT_VERSIONS;
# write docs/release-notes/vX.Y.Z.md (English); commit; push (with the user's yes)
gh run list --repo $REPO --workflow release-source.yml --branch release/source-X.Y.Z --limit 1
gh release view vX.Y.Z --repo $REPO --json isDraft,targetCommitish,assets --jq '.isDraft, .targetCommitish, (.assets[].name)'
gh release download vX.Y.Z --repo $REPO -p SHA256SUMS.txt -D <work-dir>/rel-check
# publish ONLY after the user's explicit yes:
gh release edit vX.Y.Z --repo $REPO --draft=false
```
`release_guard.py` never touches a published release. A hotfix is `X.Y.Z.W`, and its
`contentVersion` comes from `package.py`.

## Issues

```bash
gh issue view <n> --repo $REPO --comments
gh issue list --repo $REPO --state open --limit 30
# after the user approved the draft text:
gh issue comment <n> --repo $REPO --body-file <work-dir>/issue-<n>-reply.md
```
