# TrinityX360 package art

`tools/source-1.0.2/package.py` puts these files into `PPSA83600/sce_sys/` of a release built
from source, in place of the official package's art, so the console shows TrinityX360 as an
app of its own next to the official PS5X360 (PPSA50011). `package.py` checks each file against
the SHA-256 and the size below before using it, so a changed copy stops the packaging.

Since 1.0.6 (the rename from PS5X360Reloaded to TrinityX360) the images are original art:
three interlocking rings (the "trinity") over a dark blue background and the word mark
"TrinityX360" in Noto Sans SemiBold (`assets/fonts`, SIL Open Font License). They use no Xbox or
PlayStation logo, shape or typeface. [`make_art.py`](make_art.py) draws them (Pillow 13), at the
official files' sizes and format (8-bit RGB PNG); it also draws the README banner
(`docs/images/banner.png`, `--banner`). Another Pillow or FreeType version can draw slightly
different bytes: after drawing them again, update the SHA-256 below, in `package.py` (`ART`)
and in `tools/check-source-package.py`. Until 1.0.5 these were the official v1.0.2 images with a
red "RELOADED" plate stamped on them.

| File | Package path | Size | SHA-256 |
|---|---|---|---|
| `icon0.png` | `sce_sys/icon0.png` | 512x512 | `4228bd442a3c803668c92fa211bf39ae69f1789d770bd22325ed2a5ef6499e24` |
| `pic0.png` | `sce_sys/pic0.png` | 3840x2160 | `ad4017e8f303ae57f7d34f4fd38076ee6d7584a9d81fc2a0505f8605384ed142` |
| `pic1.png` | `sce_sys/pic1.png` | 1920x1080 | `60588bb128b50c9fb9275628f6944c1d1959eba21b1d8a5d07abba1dd5501ec7` |
