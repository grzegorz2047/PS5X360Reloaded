#!/usr/bin/env bash
# Fix 101 (tools/source-1.0.2/fixes/101-content-folders.patch): a file created through a package
# folder mounted at \Device\..., as Battlefield 3 creates its HD content. Builds
# tools/check-content-create.cpp against the assembled tree's xenia/base and xenia/vfs, runs it,
# then again with VirtualFileSystem::OpenFile creating through CreatePath as before (the
# console's C0000022), which must fail.
#
#   bash tools/check-content-create.sh <assembled tree>
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
tree=$(cd "${1:-build/src102}" && pwd)
core=$tree/.deps/xenia-canary
CXX=${CXX_HOST:-clang++-18}
out=$tree/build/check-content-create
rm -rf "$out"
mkdir -p "$out"

build() {
  local src=$1 exe=$2
  local base vfs
  base=$(ls "$src"/src/xenia/base/*.cc |
    grep -vE '_(win|android|mac|arm64|gnulinux_x11|test[a-z_]*)\.cc$|/main_|/testing' |
    xargs grep -L -e stbi_ -e GetConsoleAppEntryInfo)
  vfs="$src/src/xenia/vfs/device.cc $src/src/xenia/vfs/entry.cc $src/src/xenia/vfs/virtual_file_system.cc
       $src/src/xenia/vfs/devices/host_path_device.cc $src/src/xenia/vfs/devices/host_path_entry.cc
       $src/src/xenia/vfs/devices/host_path_file.cc $src/src/xenia/vfs/devices/null_device.cc
       $src/src/xenia/vfs/devices/null_entry.cc $src/src/xenia/vfs/devices/null_file.cc"
  "$CXX" -std=c++20 -O1 -g -pthread -march=znver2 -DFMT_HEADER_ONLY \
    -I"$src/src" -I"$tree/include" -I"$tree/reconstructed" -I"$tree/port" -I"$core/src" -I"$core" \
    -I"$core/third_party/fmt/include" -I"$core/third_party/utfcpp/source" -I"$core/third_party" \
    "$repo/tools/check-content-create.cpp" $base $vfs -ldl -o "$exe"
}

echo "== fixed tree"
build "$core" "$out/check"
"$out/check" | tee "$out/fixed.log"

echo "== OpenFile creating through CreatePath (before fix 101's second part)"
orig=$(mktemp -d)
trap 'rm -rf "$orig"' EXIT
mkdir -p "$orig/src/xenia"
cp -r "$core/src/xenia/base" "$core/src/xenia/vfs" "$orig/src/xenia/"
python3 -I - "$orig/src/xenia/vfs/virtual_file_system.cc" <<'EOF'
import sys
path = sys.argv[1]
text = open(path, encoding='utf-8', newline='').read()
new = """    entry = parent_entry
                ? parent_entry->CreateEntry(
                      xe::utf8::find_name_from_guest_path(path), attributes)
                : CreatePath(path, attributes);"""
old = """    entry = CreatePath(path, attributes);"""
for eol in ('\r\n', '\n'):
    if new.replace('\n', eol) in text:
        text = text.replace(new.replace('\n', eol), old)
        break
else:
    sys.exit('the fix 101 create is not in OpenFile')
open(path, 'w', encoding='utf-8', newline='').write(text)
EOF
build "$orig" "$out/check-before"
set +e
"$out/check-before" > "$out/before.log" 2>&1
code=$?
set -e
grep -E 'FAIL|check-content-create' "$out/before.log" | head -4
if [ "$code" -eq 0 ]; then
  echo "FAIL: the create through CreatePath succeeded; the check no longer reproduces the console" >&2
  exit 1
fi
grep -q 'status C0000022' "$out/before.log"
echo "PASS: a file is created through a mounted \\Device\\... package folder; through CreatePath it fails with C0000022 as on the console"
