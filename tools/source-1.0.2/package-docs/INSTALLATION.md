# TrinityX360 @VERSION@: installation

TrinityX360 is an Xbox 360 emulator for the PS5, based on PS5X360 by BrinooTk and on Xenia
Canary, built from the sources BrinooTk published. Until 1.0.5 it was called PS5X360Reloaded.
Its title id is `PPSA83600`, so it is an app of its own: it installs next to the official PS5X360
(`PPSA50011`), which stays as it is, and it keeps its data in `/data/homebrew/PPSA83600`.

You need a PS5 with a working homebrew setup. This fork is developed on firmware 13.60 with
etaHEN, ShadowMountPlus and kstuff; other setups have not been verified. No games, BIOS or keys
are included.

## Install

Install **one** of the release files, not both (two copies of `PPSA83600` are a duplicate for
ShadowMountPlus):

- **Image** (`PPSA83600.ffpkg` or `PPSA83600.ffpfsc`): copy it, keeping its file name, to the
  place ShadowMountPlus mounts images from, then open **TrinityX360** from the PS5 home
  screen. With an image, `/data/homebrew/PPSA83600` holds only the emulator's data (saves, cache,
  logs): do not put `eboot.bin` or `sce_sys` there.
- **Folder** (this `PPSA83600` folder, from `PPSA83600.zip`): copy the whole folder over FTP to
  `/data/homebrew/`, so that the executable is `/data/homebrew/PPSA83600/eboot.bin`. When you
  update, keep the `saves`, `cache` and `logs` subfolders of the previous install. Let
  ShadowMountPlus register the title, then open **TrinityX360** from the home screen.

Always install the whole package, not only `eboot.bin`. Never copy this folder or its files into
`/data/homebrew/PPSA50011`: that folder is the official PS5X360's.

## Coming from PS5X360Reloaded

TrinityX360 is the same app with a new name: the title id (`PPSA83600`) and the data folder
(`/data/homebrew/PPSA83600`) did not change. Install it over PS5X360Reloaded as you would any
update (replace the image, or copy the folder over the old one): your saves, settings, game folders
and caches stay. The saves folder on each external drive keeps its name,
`<drive>/PS5X360Reloaded/saves`; do not rename it. Forwarder tiles that start `PPSA83600` keep
working.

## Games

Put each game in its own folder under `/data/homebrew/PPSA83600/assets/roms/` (folder install),
or in a folder on another drive added in **Settings > Game folders** (an image is read-only).
DLC: `dlc/README.md`. Title updates: `updates/README.md`.

## Coming from the official PS5X360

On its first start TrinityX360 copies, once, the saves and the small settings files of the
official PS5X360 from `/data/homebrew/PPSA50011` into `/data/homebrew/PPSA83600`; the saves only
when there is enough free space, and never over saves TrinityX360 already has. The official
folder is only read: nothing in it is changed, moved or deleted. Settings kept in `/download0`
(settings, per-game settings, FSI choices and others) belong to each title id and are not copied;
the repository's README says how to move them over FTP.

## Logs

Game-session logs are written to `/data/homebrew/PPSA83600/logs`. While TrinityX360 is open,
its `/download0` is reachable over FTP under `/mnt/sandbox/PPSA83600_000/download0/xbox360ps5/`
(the `_000` suffix can differ).

## More

The full guide is the repository's README at the commit this package was built from:
@REPOSITORY@/blob/@COMMIT@/README.md

Licences: `licenses/`, with `licenses/PS5X360-@VERSION@-NOTICE.txt` for this build. TrinityX360
keeps the credits and licence notices of PS5X360 (MIT, `licenses/PS5X360-MIT.txt`) and of Xenia
Canary (BSD-3-Clause).
