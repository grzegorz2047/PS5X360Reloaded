#!/usr/bin/env bash
# TrinityX360 fix 186: build the optional, USER-loaded Lapy owned-root elevation daemon (issue #80)
# reproducibly in CI, from the pinned upstream commit, with the ps5-payload-sdk prospero-clang (the
# same SDK CI builds kinect-fake-relay.elf with) and our own MIT no-op ps5log.h stub.
#
# LICENCE: the built lapy.elf links the payload SDK's crt (GPL-3.0-or-later), so it is
# GPL-3.0-or-later as a whole; its Lapy parts keep LGPL-2.1-or-later (lapy_elevation_protocol.h)
# and MIT (the daemon sources, licenses/LAPY-MIT.txt in the package). It is a SEPARATE helper ELF, not linked into
# eboot.bin; the MIT eboot streams it to the local elfldr (127.0.0.1:9021) at startup only when the
# probe is switched on (fix 186d). See manifest.json / NOTICE.md.
#
# Usage: build-lapy-daemon.sh <ps5-payload-sdk-dir> <output-dir>
# Non-fatal by design in CI (continue-on-error): if the SDK or upstream build cannot produce the ELF,
# it prints why and exits non-zero without touching eboot.bin or the host checks.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
sdk=${1:?ps5-payload-sdk dir}
out=${2:?output dir}

REPO_URL="https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon"
PIN=$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["upstream"]["lapy_helper"]["commit"])' "$here/manifest.json")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "lapy-daemon: cloning $REPO_URL @ $PIN"
git clone --quiet "$REPO_URL" "$work/lapy"
git -C "$work/lapy" checkout --quiet "$PIN"
got=$(git -C "$work/lapy" rev-parse HEAD)
if [ "$got" != "$PIN" ]; then
  echo "lapy-daemon: FAIL pinned commit mismatch: wanted $PIN got $got" >&2
  exit 1
fi

# Our MIT ps5log stub stands in for the logging_server client the build expects. It is the
# DIAGNOSTIC stub (it appends the helper's stages to /data/homebrew/PPSA83600/lapy-helper.log), and
# it stays so in the release: with a pure no-op stub the helper's APPLY failed (fix 186k -> 186n).
logging="$here/ps5log-stub"
test -f "$logging/ps5log.h" || { echo "lapy-daemon: FAIL missing ps5log stub" >&2; exit 1; }

# Fix 186i/186j (issue #80): adapt the MIT owned_root_daemon.c -- (186i) accept our own multi-threaded
# target in the APPLY preflight, and (186j) make the elevation credential-only (UID 0, skip the sandbox
# escape) so the throttle lifts without changing the process's filesystem view. These modifications are
# MIT; their corresponding source is this script + relax-single-thread.py (public). See that header.
echo "lapy-daemon: fix 186i/186j -- relax single-thread gate + credential-only elevation (no escape)"
python3 -I "$here/relax-single-thread.py" "$work/lapy/source/owned_root_daemon.c"

echo "lapy-daemon: building owned one-shot helper (--elf-helper --title PPSA83600)"
PS5_PAYLOAD_SDK="$sdk" python3 "$work/lapy/tools/build_owned_daemon.py" \
  --sdk "$sdk" --logging-client "$logging" --elf-helper --title PPSA83600

elf="$work/lapy/build/owned_root_helper-PPSA83600/lapy.elf"
if [ ! -f "$elf" ]; then echo "lapy-daemon: FAIL no ELF produced" >&2; exit 1; fi
python3 -I - "$elf" <<'PY'
import sys
with open(sys.argv[1],'rb') as f:
    assert f.read(4)==b'\x7fELF', "output is not ELF"
print("lapy-daemon: ELF ok")
PY

mkdir -p "$out/helper"
cp "$elf" "$out/lapy.elf"              # bundled at /app0/lapy.elf by stage-reloaded-candidate.py
cp "$elf" "$out/helper/lapy.elf"
cp "$repo/licenses/LAPY-LGPL-2.1.txt" "$out/helper/LAPY-LGPL-2.1.txt"
cp "$here/manifest.json" "$out/helper/lapy-daemon-manifest.json"
cp "$here/NOTICE.md" "$out/helper/NOTICE.md"
sha=$(python3 -I -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$out/lapy.elf")
python3 -I - "$out/helper/built-manifest.json" "$PIN" "$sha" <<'PY'
import json,sys
json.dump({"component":"lapy.elf","mode":"owned-helper","title":"PPSA83600","upstream_commit":sys.argv[2],
           "elf_sha256":sys.argv[3],"license":"LGPL-2.1-or-later",
           "deployment":"bundled at /app0/lapy.elf; streamed to the local elfldr by the eboot at startup when the probe is on; not linked into eboot; LGPL isolated"},
          open(sys.argv[1],"w"), indent=2)
print("lapy-daemon: wrote", sys.argv[1], "sha256", sys.argv[3])
PY
echo "lapy-daemon: done -> $out/helper/"
