# TrinityX360 package docs

The official v1.0.2 package (`PPSA50011.zip`) carries user docs that name the official app's
folder, `PPSA50011`: they tell the user to copy files into `/data/homebrew/PPSA50011` and to look
there for logs, saves, DLC and updates. In TrinityX360's package (`PPSA83600/`, made by
`tools/source-1.0.2/package.py`) that would send files into the official PS5X360's folder, so
`package.py` does not ship them as they are:

| Package file | In the TrinityX360 package |
|---|---|
| `INSTALLATION.md` | `INSTALLATION.md` in this folder, with `@VERSION@`, `@REPOSITORY@` and `@COMMIT@` filled in from `--version` and `--source-url` |
| `RELEASE_NOTES.md` | the release's notes, `docs/release-notes/v<version>.md` |
| `dlc/README.md`, `updates/README.md` | the official text with the folder `PPSA50011` renamed to `PPSA83600` (each names it once; another count stops the packaging) |
| `AUTOLOG.md` | left out: the AutoLog payload of the official releases reads `/data/homebrew/PPSA50011/logs` only, and is not part of the package |

`tools/check-source-package.py` checks that no other file of the package text names `PPSA50011`
outside the notices that say where the package came from.
