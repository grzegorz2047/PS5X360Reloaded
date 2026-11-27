#!/usr/bin/env bash
# Fix 26 (tools/source-1.0.2/fixes/26-gdfx-win1252-names.patch): GDFX names in Windows-1252.
# Handball 16 (Europe, 573607E4) never launched: "Not enough space" (utfcpp's not_enough_room)
# at DiscImageDevice::Initialize.
#
#   bash tools/check-gdfx-names.sh [assembled tree]      (default build/src102)
#
# 1. Builds tools/check-gdfx-names.cpp with the tree's xenia/base and the disc image device
#    (host clang, AddressSanitizer) and runs it: a 30,000-entry image with accented names must
#    mount, resolve, round-trip and stay within its heap bound; the game's own cp1252 paths,
#    converted and checked as the file calls do (kernel/util/guest_path.h), must resolve to
#    the same entries, never come out empty, and each failure is reported once byte for byte.
# 2. Builds the same check against the tree's xenia/base and xenia/vfs with fix 26 reversed (the
#    1.0.2 code) and requires the console's failure: the mount throws "Not enough space".
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
tree=$(cd "${1:-build/src102}" && pwd)
core=$tree/.deps/xenia-canary
fix=$repo/tools/source-1.0.2/fixes/26-gdfx-win1252-names.patch
CXX=${CXX_HOST:-clang++-18}
out=$tree/build/check-gdfx-names
rm -rf "$out"
mkdir -p "$out"
# AddressSanitizer when the compiler has its runtime (libclang-rt-18-dev), else without.
sanitize="-fsanitize=address -fno-omit-frame-pointer"
if ! echo 'int main() { return 0; }' |
    "$CXX" -x c++ $sanitize - -o "$out/asan-probe" 2>/dev/null; then
  echo "(no AddressSanitizer runtime for $CXX; building without it)"
  sanitize=""
fi

jobs=${JOBS:-$(nproc)}
launcher=""
if command -v ccache >/dev/null 2>&1 && [ "${PS5X360_NO_CCACHE:-0}" != "1" ]; then
  launcher="ccache"
fi
ldflag=""
if echo 'int main() { return 0; }' | "$CXX" -fuse-ld=lld -x c++ - -o "$out/lld-probe" 2>/dev/null; then
  ldflag="-fuse-ld=lld"
fi

# $1: a directory holding src/xenia/{base,vfs} (the tree's core, or the reversed copy).
build() {
  local src=$1 exe=$2 extra=${3:-}
  local base vfs objdir="$exe.objs"
  rm -rf "$objdir"
  mkdir -p "$objdir"
  base=$(ls "$src"/src/xenia/base/*.cc |
    grep -vE '_(win|android|mac|arm64|gnulinux_x11|test[a-z_]*)\.cc$|/main_|/testing' |
    xargs grep -L -e stbi_ -e GetConsoleAppEntryInfo)
  vfs="$src/src/xenia/vfs/device.cc $src/src/xenia/vfs/entry.cc
       $src/src/xenia/vfs/devices/disc_image_device.cc
       $src/src/xenia/vfs/devices/disc_image_entry.cc
       $src/src/xenia/vfs/devices/disc_image_file.cc"
  local cflags="-std=c++20 -O1 -g -pthread -march=znver2 -DFMT_HEADER_ONLY $extra $sanitize
    -I$src/src -I$tree/include -I$tree/reconstructed -I$core/src -I$core
    -I$core/third_party/fmt/include -I$core/third_party/utfcpp/source -I$core/third_party"
  export CXX launcher cflags
  # shellcheck disable=SC2016,SC2086
  printf '%s\n' "$repo/tools/check-gdfx-names.cpp" $base $vfs |
    xargs -P "$jobs" -n 1 sh -c 'exec $launcher "$CXX" $cflags -c "$1" -o "$0/$(basename "$1").o"' "$objdir"
  # shellcheck disable=SC2086
  "$CXX" -O1 -g -pthread $sanitize $ldflag "$objdir"/*.o -ldl -o "$exe"
}

echo "== fixed tree"
build "$core" "$out/check"
ASAN_OPTIONS=detect_leaks=0 "$out/check" | tee "$out/fixed.log"

echo "== 1.0.2 (fix 26 reversed in xenia/base and xenia/vfs)"
orig=$(mktemp -d)
trap 'rm -rf "$orig"' EXIT
mkdir -p "$orig/.deps/xenia-canary/src/xenia"
cp -r "$core/src/xenia/base" "$core/src/xenia/vfs" "$orig/.deps/xenia-canary/src/xenia/"
# Outside any repository, git apply works on the plain directory.
(cd "$orig" && git apply -R --include='.deps/xenia-canary/src/xenia/base/*' \
  --include='.deps/xenia-canary/src/xenia/vfs/*' "$fix")
if grep -q 'fix 26' "$orig/.deps/xenia-canary/src/xenia/base/string.cc"; then
  echo "FAIL: fix 26 did not reverse" >&2
  exit 1
fi
build "$orig/.deps/xenia-canary" "$out/check-102"
set +e
ASAN_OPTIONS=detect_leaks=0 "$out/check-102" > "$out/original.log" 2>&1
code=$?
set -e
tail -5 "$out/original.log"
if [ "$code" -eq 0 ]; then
  echo "FAIL: the 1.0.2 code mounted the image; the check no longer reproduces the console" >&2
  exit 1
fi
grep -q 'mount threw: Not enough space' "$out/original.log"
echo "PASS: 1.0.2 fails as on the console (Not enough space); fix 26 mounts, resolves and round-trips"
