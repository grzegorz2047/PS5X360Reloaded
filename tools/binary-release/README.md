# Binary releases from the official v1.0.2 package (superseded)

> **Superseded since v1.0.3.** The `v1.0.2-opt` and `v1.0.2-opt-r2` releases were made with
> these tools. From v1.0.3 on, releases are built from source instead
> ([Releases built from source](#releases-built-from-source-103-and-later)): the `eboot.bin` is
> compiled, not patched, and every binary patch group here (1-13) is ported to source in
> `tools/source-1.0.2/fixes`. No new `v*-opt*` release is planned. To update from a binary
> release, install the whole v1.0.3 package, not only its `eboot.bin`. These tools stay to
> rebuild the old releases, and `make_images.sh` also makes the source releases' images.

When these releases were made, the C++ in this repository stopped at v0.5.7-fix.1: the official
v1.0.2 executable was built from code its author publishes only as archives with the releases
(`PS5X360-source-*-changes.zip`). An `eboot.bin` compiled from this repository's own C++ lacks
online play, per-game FSI, frame generation and Kinect, so it was **not** released. The
`v1.0.2-opt*` releases instead take the official v1.0.2 package and change it:

| Step | Where |
| --- | --- |
| Check the official `PPSA50011.zip` (SHA-256) and its `eboot.bin` ELF | `build_package.py` |
| Patch the ELF (each patch checks the bytes it replaces) and sign it again | `patches.py`, `fself.py` |
| Replace `assets/presets.txt`, `assets/config/*.toml`, `assets/patches/*.patch.toml` from this repository | `build_package.py` |
| Write `RELEASE.json`, `MANIFEST.json`, `licenses/PS5X360-OPT-NOTICE.txt` | `build_package.py` |
| Make `PPSA50011.ffpkg` (UFS2Tool) and `PPSA50011.ffpfsc` (MkPFS, verified) | `make_images.sh` |

GitHub Actions runs it in `.github/workflows/release-binary.yml`: by hand
(*Run workflow*, artifacts only) or by pushing a tag `v*-opt*`, which also
publishes the GitHub release.

Locally (Python 3, plus the .NET 8 SDK for the images):

```
curl -LO https://github.com/BrinooTk/PS5X360/releases/download/v1.0.2/PPSA50011.zip
python3 tools/binary-release/build_package.py --base-zip PPSA50011.zip
bash tools/binary-release/make_images.sh dist/binary-release/stage/PPSA50011 dist/binary-release
```

A change to the C++ of this repository reached a binary release only when it
could be written as a binary patch in `patches.py`;
`.github/workflows/build-ps5.yml` only checks that the repository's C++ still
compiles.

## Releases built from source (1.0.3 and later)

The 1.0.2 sources reassembled from the author's drops (`tools/source-1.0.2`,
[docs/SOURCE-1.0.2.md](../../docs/SOURCE-1.0.2.md)) do build a complete
`eboot.bin`, with these patch groups ported to source and later fixes on top.
That executable **is** released (first as v1.0.3), by `.github/workflows/release-source.yml`, as a
draft GitHub release `vX.Y.Z` with its GPL-3.0 corresponding source: it is not
patched here. Its package comes from the same official v1.0.2 `PPSA50011.zip`
through `tools/source-1.0.2/package.py` (the source-built `eboot.bin` as signed,
English-keyed language files, no `patches.py`, no `assets/lang-overrides`
merge). That package is TrinityX360 (PS5X360Reloaded until 1.0.5) with its own title id `PPSA83600` (its own
`sce_sys/param.json` and art, and its own `INSTALLATION.md` and release notes in place of the
official docs that name `PPSA50011`, see `tools/source-1.0.2/package-docs`), so it installs next to the official PS5X360
(`PPSA50011`): the files move to `PPSA83600/` and the images come from
`make_images.sh <stage/PPSA83600> <out-dir> PPSA83600` (`PPSA83600.ffpkg`,
`PPSA83600.ffpfsc`). Without the third argument the title id is `PPSA50011`, as for the
binary releases above. A source build's test
artifact (`…-NOT-FOR-RELEASE`) is still not for distribution.
