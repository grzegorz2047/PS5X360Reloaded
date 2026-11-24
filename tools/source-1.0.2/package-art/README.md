# TrinityX360 package art

`tools/source-1.0.2/package.py` puts these files into `PPSA83600/sce_sys/` of a release built
from source, in place of the official package's art, so the console shows TrinityX360 as an
app of its own next to the official PS5X360 (PPSA50011). `package.py` checks each file against
the SHA-256 and the size below before using it, so a changed copy stops the packaging.

Since 1.0.6 (the rename from PS5X360Reloaded to TrinityX360) the images keep the style of the
art the app had until then: a black background, a large white and green "X" of two slanted
bars, and the name in white and green, "Trinity" + "X360", in place of "PS5X360" and the red
"RELOADED" plate. The icon's name is as wide as the tile, so it reads at small sizes. They are
drawn from shapes and Noto Sans SemiBold (`assets/fonts`, SIL Open Font License) by
[`make_art.py`](make_art.py) (Pillow 13); no pixel of the official art is used, and no Xbox or
PlayStation logo, shape or typeface. Same sizes and format as the official files (8-bit RGB
PNG). The script also draws the README banner (`docs/images/banner.png`, `--banner`). Another Pillow or FreeType version can draw slightly
different bytes: after drawing them again, update the SHA-256 below, in `package.py` (`ART`)
and in `tools/check-source-package.py`. Until 1.0.5 these were the official v1.0.2 images with a
red "RELOADED" plate stamped on them.

| File | Package path | Size | SHA-256 |
|---|---|---|---|
| `icon0.png` | `sce_sys/icon0.png` | 512x512 | `12a4ccc54cff625a4576c39336a15f093ab046b5965639975d08d29a39d3a41f` |
| `pic0.png` | `sce_sys/pic0.png` | 3840x2160 | `ecd2f37967541d1e33bc995c52ba27cf983f82fe88aa8e2b6a19f6ce8e020851` |
| `pic1.png` | `sce_sys/pic1.png` | 1920x1080 | `f9c26b190a2a3d427e7ab233009835df03f3082292d04dde26ce4e827a4e9db7` |
