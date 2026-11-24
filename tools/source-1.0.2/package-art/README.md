# TrinityX360 package art

`tools/source-1.0.2/package.py` puts these files into `PPSA83600/sce_sys/` of a release built
from source, in place of the official package's art, so the console shows TrinityX360 as an
app of its own next to the official PS5X360 (PPSA50011). `package.py` checks each file against
the SHA-256 and the size below before using it, so a changed copy stops the packaging.

Since 1.0.6 (the rename from PS5X360Reloaded to TrinityX360) the images are the user's choice of
two drafts (`research/rename-trinityx360` in the workspace): three interlocking rings, the
"trinity", in a light grey, a blue and a green (colours only: no PlayStation or Xbox shape or
mark), woven over and under each other on a near-black navy gradient, and the name, "Trinity" in
white and "X360" in green, in Noto Sans SemiBold (`assets/fonts`, SIL Open Font License).
[`make_art.py`](make_art.py) draws them (Pillow 13) at the official files' sizes and format (8-bit
RGB PNG), and the README banner in the same style (`docs/images/banner.png`, `--banner`);
`--variant a --out <folder>` draws the other draft (a white and green "X" on black), a preview
only. Another Pillow or FreeType version can draw slightly different bytes: after drawing them
again, update the SHA-256 below, in `package.py` (`ART`) and in `tools/check-source-package.py`,
and write `sce_sys/` again (`repo_sce_sys.py --write`). Until 1.0.5 these were the official v1.0.2
images with a red "RELOADED" plate stamped on them.

| File | Package path | Size | SHA-256 |
|---|---|---|---|
| `icon0.png` | `sce_sys/icon0.png` | 512x512 | `7b55e75d4293b5bbb01b40c5831855bb626cab80d149a7a01f4c2adeecf7b26f` |
| `pic0.png` | `sce_sys/pic0.png` | 3840x2160 | `e0541c1987d983bb50ca04eb7b2c653afc4316f63d84620c7017cdacbfe5d350` |
| `pic1.png` | `sce_sys/pic1.png` | 1920x1080 | `d25b1bb70a158f1a07825a3b5bf0633469af8ae62e5cd791f004db9d54b90a62` |
