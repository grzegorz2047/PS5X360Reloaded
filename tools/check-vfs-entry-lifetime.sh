#!/usr/bin/env bash
# Fix 139 (tools/source-1.0.2/fixes/139-vfs-entry-lifetime.patch): a file system entry is never
# freed while a file or an operation uses it (Halo: Reach's crash in Entry::Delete at NtClose).
# Builds tools/check-vfs-entry-lifetime.cpp against the assembled tree's xenia/base and
# xenia/vfs, under AddressSanitizer when the compiler has it, and runs it; then, with the
# sanitizer, builds it again with fix 139 taken out of xenia/vfs, where the use of a freed entry
# must be reported (the console's crash).
#
#   bash tools/check-vfs-entry-lifetime.sh <assembled tree>
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
tree=$(cd "${1:-build/src102}" && pwd)
core=$tree/.deps/xenia-canary
CXX=${CXX_HOST:-clang++-18}
out=$tree/build/check-vfs-entry-lifetime
rm -rf "$out"
mkdir -p "$out"

sanitize=""
printf 'int main() { return 0; }\n' > "$out/probe.cpp"
if "$CXX" -fsanitize=address "$out/probe.cpp" -o "$out/probe" 2>/dev/null && "$out/probe"; then
  sanitize="-fsanitize=address -fno-omit-frame-pointer"
  echo "== AddressSanitizer on"
else
  echo "== AddressSanitizer not available: the use of a freed entry may go unnoticed"
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

flags="-std=c++20 -O1 -g -pthread -march=znver2 -DFMT_HEADER_ONLY $sanitize -I$core/src -I$tree/include
  -I$tree/reconstructed -I$tree/port -I$core -I$core/third_party/fmt/include -I$core/third_party/utfcpp/source
  -I$core/third_party"
# xenia/base once, in parallel (the same for both builds); xenia/vfs and the check for each.
base=$(ls "$core"/src/xenia/base/*.cc |
  grep -vE '_(win|android|mac|arm64|gnulinux_x11|test[a-z_]*)\.cc$|/main_|/testing' |
  xargs grep -L -e stbi_ -e GetConsoleAppEntryInfo)
mkdir -p "$out/base"
export CXX launcher flags
# shellcheck disable=SC2016,SC2086
printf '%s\n' $base | xargs -P "$jobs" -n 1 sh -c 'exec $launcher "$CXX" $flags -c "$1" -o "$0/$(basename "$1" .cc).o"' "$out/base"

build() {
  local src=$1 exe=$2
  local objdir="$exe.objs"
  rm -rf "$objdir"
  mkdir -p "$objdir"
  local vfs="$src/src/xenia/vfs/device.cc $src/src/xenia/vfs/entry.cc $src/src/xenia/vfs/virtual_file_system.cc
       $src/src/xenia/vfs/devices/host_path_device.cc $src/src/xenia/vfs/devices/host_path_entry.cc
       $src/src/xenia/vfs/devices/host_path_file.cc $src/src/xenia/vfs/devices/null_device.cc
       $src/src/xenia/vfs/devices/null_entry.cc $src/src/xenia/vfs/devices/null_file.cc"
  local vfs_flags="-I$src/src $flags"
  export CXX launcher vfs_flags
  # shellcheck disable=SC2016,SC2086
  printf '%s\n' "$repo/tools/check-vfs-entry-lifetime.cpp" $vfs |
    xargs -P "$jobs" -n 1 sh -c 'exec $launcher "$CXX" $vfs_flags -c "$1" -o "$0/$(basename "$1").o"' "$objdir"
  # shellcheck disable=SC2086
  "$CXX" -O1 -g -pthread $sanitize $ldflag "$objdir"/*.o "$out"/base/*.o -ldl -o "$exe"
}

echo "== fixed tree"
build "$core" "$out/check"
ASAN_OPTIONS=detect_leaks=0 "$out/check" | tee "$out/fixed.log"

if [ -z "$sanitize" ]; then
  echo "PASS (without the sanitizer, the tree without fix 139 is not run)"
  exit 0
fi

echo "== without fix 139 in xenia/vfs"
orig=$(mktemp -d)
trap 'rm -rf "$orig"' EXIT
mkdir -p "$orig/.deps/xenia-canary/src/xenia"
cp -r "$core/src/xenia/base" "$core/src/xenia/vfs" "$orig/.deps/xenia-canary/src/xenia/"
# Fix 139, its follow-ups (139b, ...), fix 150 (the background deletes) and fix 172 (the utility
# cache's churn watch, built on 150d's entries), taken out newest first (sorted by fix_order's
# number, then letter).
for patch in $(ls "$repo"/tools/source-1.0.2/fixes/139*-*.patch "$repo"/tools/source-1.0.2/fixes/150*-*.patch \
                  "$repo"/tools/source-1.0.2/fixes/172-*.patch 2>/dev/null |
               sort -t/ -k1,1 | awk -F/ '{n=$NF; sub(/-.*/, "", n); print n "\t" $0}' | sort -k1,1Vr | cut -f2); do
  (cd "$orig" && GIT_CEILING_DIRECTORIES="$orig/.." git -c core.autocrlf=false apply -R \
    --include='.deps/xenia-canary/src/xenia/vfs/*' "$patch")
done
build "$orig/.deps/xenia-canary" "$out/check-before"
set +e
ASAN_OPTIONS=detect_leaks=0 "$out/check-before" > "$out/before.log" 2>&1
code=$?
set -e
grep -E 'ERROR: AddressSanitizer|FAIL|^ok' "$out/before.log" | head -6
if [ "$code" -eq 0 ] || ! grep -q 'heap-use-after-free' "$out/before.log"; then
  echo "FAIL: without fix 139 no use of a freed entry was reported; the check no longer reproduces the console" >&2
  exit 1
fi
echo "PASS: entries outlive their files and deletes; without fix 139 a freed entry is used, as on the console"
