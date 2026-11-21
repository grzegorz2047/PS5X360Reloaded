#!/usr/bin/env bash
# Uploads one console test image as a side file and verifies it by downloading it back.
# Optionally makes it the active image. One upload at a time, ONLY while the emulator app is
# closed or idle in the launcher. Never while the user tests a game or is quitting one: a parallel
# upload during a game's exit once crashed the console. So you must pass --app-closed, which
# means the user told you so.
#
#   CONSOLE_IP=<console-ip> FTP_PORT=<ftp-port> IMAGE_DRIVE=<image-drive> \
#     bash tools/upload-console-image.sh --app-closed <image.ffpkg> <label> [--activate <label-of-current-active>]
#
# Environment:
#   CONSOLE_IP, FTP_PORT  the console's FTP server payload (required)
#   IMAGE_DRIVE   where the active <TITLE_ID>.ffpkg lives: a USB root such as /mnt/usbN, or the
#                 extended storage /mnt/extN (required; PENDRIVE is accepted as its old name)
#   SIDE_DIR      where side images <TITLE_ID>.ffpkg.v<label> go (default: IMAGE_DRIVE; a subfolder
#                 such as <image-drive>/ps5x360-images keeps the drive root tidy)
#   TITLE_ID      default PPSA83600
#   MOUNT_DIR     ShadowMountPlus's mount folder, default /mnt/shadowmnt
#   SMP_DIR       ShadowMountPlus's data folder, default /data/shadowmount (manual.lst, debug.log)
#   OTHER_DRIVES  drives checked for a second active image before --activate
#                 (default /mnt/usb0../mnt/usb7 /mnt/ext0 /mnt/ext1, IMAGE_DRIVE left out)
#
# On a failed upload the partial file is deleted at once, because a partial image can crash the
# mounter (curl 55 on a USB stick usually means it is full).
# With --activate:
#   - the current active image is renamed to SIDE_DIR/<TITLE_ID>.ffpkg.v<label-of-current-active>
#     and this one to IMAGE_DRIVE/<TITLE_ID>.ffpkg;
#   - this happens only when MOUNT_DIR lists nothing mounted, no other drive has a <TITLE_ID>.ffpkg
#     (ShadowMountPlus ignores the duplicate and keeps the old registration), and, on extended
#     storage, SMP_DIR/manual.lst lists the image's path (images there are mounted only when listed).
# The local image is NOT deleted; delete it after you have noted the SHA.
set -euo pipefail

die() { echo "upload-console-image: $*" >&2; exit 1; }
[[ ${1:-} == --app-closed ]] || die "first argument must be --app-closed (ask the user: is the app closed or idle?)"
shift
image=${1:?image path}; label=${2:?label}; shift 2
activate=""; old_label=""
if [[ ${1:-} == --activate ]]; then activate=1; old_label=${2:?label of the current active image}; fi
IMAGE_DRIVE=${IMAGE_DRIVE:-${PENDRIVE:-}}
: "${CONSOLE_IP:?set CONSOLE_IP}" "${FTP_PORT:?set FTP_PORT}" "${IMAGE_DRIVE:?set IMAGE_DRIVE (e.g. /mnt/usbN or /mnt/extN)}"
IMAGE_DRIVE=${IMAGE_DRIVE%/}
SIDE_DIR=${SIDE_DIR:-$IMAGE_DRIVE}; SIDE_DIR=${SIDE_DIR%/}
TITLE_ID=${TITLE_ID:-PPSA83600}
MOUNT_DIR=${MOUNT_DIR:-/mnt/shadowmnt}
SMP_DIR=${SMP_DIR:-/data/shadowmount}
if [[ -z ${OTHER_DRIVES+set} ]]; then
  OTHER_DRIVES="/mnt/usb0 /mnt/usb1 /mnt/usb2 /mnt/usb3 /mnt/usb4 /mnt/usb5 /mnt/usb6 /mnt/usb7 /mnt/ext0 /mnt/ext1"
fi
ftp="ftp://$CONSOLE_IP:$FTP_PORT"
active="$TITLE_ID.ffpkg"
side="$active.v$label"
[[ -s $image ]] || die "$image is missing or empty"
[[ $label =~ ^[0-9A-Za-z._-]+$ ]] || die "label '$label' has characters FTP names should not have"

sha() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }
ftp_cmd() { curl -sS --max-time 30 "$ftp/" "$@" > /dev/null; }
names_in() { curl -s --max-time 30 --list-only "$ftp$1/" | tr -d '\r'; }

curl -s -m 8 "$ftp/" > /dev/null || die "no FTP at $CONSOLE_IP:$FTP_PORT (console off, or the FTP payload not running)"

echo "== $SIDE_DIR before upload (look for partial or superseded images; prune with DELE first):"
curl -s --max-time 30 "$ftp$SIDE_DIR/" | grep -i "$TITLE_ID" || true
if names_in "$SIDE_DIR" 2>/dev/null | grep -qxF "$side"; then
  die "$SIDE_DIR/$side already exists; delete it first or use another label"
fi

local_sha=$(sha < "$image")
echo "== uploading $image -> $SIDE_DIR/$side ($(wc -c < "$image") bytes, sha256 $local_sha)"
status=0
curl -sS --max-time 1800 --ftp-create-dirs -T "$image" "$ftp$SIDE_DIR/$side" || status=$?
if (( status != 0 )); then
  echo "upload failed (curl $status; 55 on a USB stick usually means it is full): deleting the partial file" >&2
  ftp_cmd -Q "DELE $SIDE_DIR/$side" || echo "could not delete $SIDE_DIR/$side: DELETE IT BY HAND" >&2
  exit 1
fi
remote_sha=$(curl -s --max-time 1800 "$ftp$SIDE_DIR/$side" | sha) || remote_sha="download failed"
if [[ $remote_sha != "$local_sha" ]]; then
  echo "SHA MISMATCH ($remote_sha): deleting the remote copy" >&2
  ftp_cmd -Q "DELE $SIDE_DIR/$side" || echo "could not delete $SIDE_DIR/$side: DELETE IT BY HAND" >&2
  exit 1
fi
echo "OK $SIDE_DIR/$side sha256 $local_sha"

if [[ -n $activate ]]; then
  # An unreadable mount folder is not proof that nothing is mounted: refuse then too.
  listing=$(curl -s --max-time 30 --list-only "$ftp$MOUNT_DIR/") ||
    die "cannot list $MOUNT_DIR: not swapping (is ShadowMountPlus running? set MOUNT_DIR)"
  mounted=$(printf '%s\n' "$listing" | tr -d '\r' | grep -vxE '\.{0,2}' || true)
  [[ -z $mounted ]] || die "something is mounted in $MOUNT_DIR ($mounted): not swapping; ask the user to close the app"
  # A second active image on another drive: ShadowMountPlus logs "Duplicate ... ignored" and keeps
  # the old registration, so the console would run the old build.
  for drive in $OTHER_DRIVES; do
    drive=${drive%/}
    [[ $drive == "$IMAGE_DRIVE" ]] && continue
    if names_in "$drive" 2>/dev/null | grep -qxF "$active"; then
      die "$drive/$active exists too: rename it to a side file first (two active images = the old one stays registered)"
    fi
  done
  if [[ $IMAGE_DRIVE == /mnt/ext* ]]; then
    lst=$(curl -s --max-time 30 "$ftp$SMP_DIR/manual.lst" | tr -d '\r') ||
      die "cannot read $SMP_DIR/manual.lst: images on extended storage mount only when listed there"
    printf '%s\n' "$lst" | grep -qxF "$IMAGE_DRIVE/$active" ||
      die "$SMP_DIR/manual.lst does not list $IMAGE_DRIVE/$active: ask the user before adding it (one path per line)"
  fi
  if names_in "$IMAGE_DRIVE" | grep -qxF "$active"; then
    names_in "$SIDE_DIR" | grep -qxF "$active.v$old_label" &&
      die "$SIDE_DIR/$active.v$old_label already exists: not overwriting it; check the current active image's label"
    ftp_cmd -Q "RNFR $IMAGE_DRIVE/$active" -Q "RNTO $SIDE_DIR/$active.v$old_label"
    echo "previous active image kept as $SIDE_DIR/$active.v$old_label"
  fi
  ftp_cmd -Q "RNFR $SIDE_DIR/$side" -Q "RNTO $IMAGE_DRIVE/$active"
  echo "active image is now $label ($IMAGE_DRIVE/$active)"
  echo "after the user starts the app, check: curl -s $ftp$SMP_DIR/debug.log | grep 'mount_img.lnk created' | tail -1"
fi

echo "== $SIDE_DIR after:"
curl -s --max-time 30 "$ftp$SIDE_DIR/" | grep -i "$TITLE_ID" || true
