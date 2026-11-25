#!/usr/bin/env bash
# Makes <ID>.ffpkg (UFS2 image, UFS2Tool) and <ID>.ffpfsc (verified PFS container, MkPFS) from
# an unpacked package folder whose title id is <ID>.
#   bash tools/binary-release/make_images.sh <stage/ID> <out-dir> [<ID>]
# <ID> is the title id (4 capital letters and 5 digits), PPSA50011 when left out: the binary
# releases of the official package. The source releases (release-source.yml) pass PPSA83600,
# TrinityX360's own id (PS5X360Reloaded's until 1.0.5). It names both images and is the UFS2 volume label; the folder's
# sce_sys/param.json, when there is one, must carry the same titleId, because ShadowMountPlus
# registers the app by that titleId and mounts it by the image's file name.
# Both tools are fetched at pinned revisions into .deps/packaging (or $PACKAGING_CACHE).
set -euo pipefail
id=${3:-PPSA50011}
if [[ ! $id =~ ^[A-Z]{4}[0-9]{5}$ ]]; then
  echo "make_images.sh: title id '$id' is not 4 capital letters and 5 digits" >&2
  exit 1
fi
app=$(cd -- "$1" && pwd)
param="$app/sce_sys/param.json"
if [[ -f $param ]]; then
  title=$(tr -d '\r' < "$param" | sed -n 's/^[[:space:]]*"titleId"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
  if [[ $title != "$id" ]]; then
    echo "make_images.sh: $param has titleId '$title', not $id" >&2
    exit 1
  fi
fi
out=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${PACKAGING_CACHE:-$root/.deps/packaging}
mkdir -p "$cache"

fetch() {  # fetch <dir> <url> <revision>
  local dir=$1 url=$2 revision=$3
  if [[ ! -d $dir/.git ]]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$revision"
    git -C "$dir" checkout -q --detach FETCH_HEAD
  fi
  [[ $(git -C "$dir" rev-parse HEAD) == "$revision" ]] || { echo "$dir is not at $revision" >&2; exit 1; }
}

# UFS2Tool: the revision PS5_Vulkan's packaging script pins.
ufs="$cache/UFS2Tool"
fetch "$ufs" https://github.com/SvenGDK/UFS2Tool.git b5307a60d5b4e3a68ba680e0e33cfadf05017c77
if [[ ! -f $ufs/.build/UFS2Tool.dll ]]; then
  dotnet build "$ufs/UFS2Tool.csproj" --configuration Release --output "$ufs/.build" --nologo --verbosity quiet
fi
rm -f "$out/$id.ffpkg"
DOTNET_ROLL_FORWARD=Major dotnet "$ufs/.build/UFS2Tool.dll" newfs -O 2 -b 65536 -f 8192 \
  -D "$app" "$out/$id.ffpkg" "$id"

# MkPFS
mkpfs="$cache/MkPFS"
fetch "$mkpfs" https://github.com/PSBrew/MkPFS.git 6cb8313dfe0c988ac52617794553f343243d3a56
if [[ ! -x $mkpfs/.venv/bin/python ]]; then
  python3 -m venv "$mkpfs/.venv"
  "$mkpfs/.venv/bin/python" -m pip install --disable-pip-version-check --quiet "$mkpfs"
fi
rm -f "$out/$id.ffpfsc"
"$mkpfs/.venv/bin/python" -m mkpfs pack folder "$app" "$out/$id.ffpfsc" --version PS5 --verify

ls -l "$out/$id.ffpkg" "$out/$id.ffpfsc"
