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

## The home screen's pictures (`skin/`)

The official package's `assets/skin` has four pictures with the PS5X360 logo, which the home screen
draws (`port/kinect_home.inc`, `port/kinect_state.inc`; `assets/skin-dark` has none of them, so the
dark theme draws these too). TrinityX360 shows no logo of the project it is based on, so
`package.py` (`ART`) replaces them with these, drawn by `make_art.py` (into `<out>/skin`) in the
same style, at the official names, sizes and formats; `stage-reloaded-candidate.py` copies the same
`ART` files into console test images. `package.py` refuses a package with any file that is one of
the official logo pictures (`OFFICIAL_LOGOS`, by SHA-256). The other skin pictures (icons, slots,
background, Kinect wave) carry no logo and stay the official ones.

| File | Package path | Size | Was | SHA-256 |
|---|---|---|---|---|
| `skin/xbox-logo.png` | `assets/skin/xbox-logo.png` | 300x196 RGBA PNG | the PS5X360 logo (the "Online" slot) | `df0939abce9b60cef25586051b050830ee0dac83c31f5db46b1412e01f0fb534` |
| `skin/basics.jpg` | `assets/skin/basics.jpg` | 420x200 JPEG | PS5X360's X on a plate (the "Original dashboard" slot) | `8ce47a0083e137829dfd0a937b013fce92c549e928eeb765687dee131525bafa` |
| `skin/ball.png` | `assets/skin/ball.png` | 96x96 RGBA PNG | the X in a green ring (the home's corner orb) | `f02d6eefdd69c58dca4d925ecbd8a77fcb2045a4f70456444b209f639f973aa3` |
| `skin/ball-glow.png` | `assets/skin/ball-glow.png` | 96x96 RGBA PNG | the orb's glow with the X | `ac14d7d75fe9c0715768e0e3882d971340b3870fa0372fab4a9dbafcb69c4aaf` |
