# Starting a game from the PS5 home screen (forwarders)

A **forwarder** is a small extra app with its own home screen tile: its own name, icon and
title ID. Opening it starts TrinityX360 (`PPSA83600`) with launch arguments, and
TrinityX360 goes straight into that game instead of opening the library.

Since v1.0.3 this fork's app has its own title ID, **`PPSA83600`**: it was called PS5X360Reloaded
until 1.0.5 and is TrinityX360 since 1.0.6, with the same title ID, so tiles made for
PS5X360Reloaded keep working. The official PS5X360 keeps `PPSA50011`, and so do this fork's
earlier builds (`v1.0.2-opt`, r2 and the `1.0.3_candidate_N` test builds made before the first
rename). A tile starts whichever title ID its
`forwarder.json` names in `target`.

Forwarders use the [PS5 Forwarder Format](https://github.com/heydemoura/ps5-forwarder-format).
The tile cannot start another app by itself, so it asks a small resident launcher (which it
starts through `elfldr` when needed) to call `sceSystemServiceLaunchApp("PPSA83600", args)`.
The arguments are the ones in the tile's `forwarder.json`.

Supported from the source build `1.0.3_candidate_34` (fix 21,
`tools/source-1.0.2/fixes/21-launch-argument.patch`). Older builds understand only a bare
full path to a file.

## What you need

- A jailbroken PS5 with `elfldr` listening on port 9021.
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), which registers the new tiles.
- TrinityX360 (or PS5X360Reloaded, v1.0.3 to v1.0.5) installed as `PPSA83600`.
- **Forwarder Manager** (`PPSA99930`), release
  [00.001.006](https://github.com/heydemoura/ps5-forwarder-manager/releases/tag/00.001.006) or newer:
  asset `PPSA99930.zip` (SHA-256 `986eb9bce653f4cc30864d897b9da151030f41423ba7d9b870a97d8155c89a1d`).
  Its `PS5X360` target lists PS5X360's games (the darkxex fork's work, merged; the fork itself
  publishes no release), but it starts the **official** app, `PPSA50011`. Forwarder Manager has
  no TrinityX360 target yet, so a tile for it gets the target `PPSA83600` either from the
  **Generic** target or by editing `forwarder.json` (below).

## Making a tile for one game

1. Install Forwarder Manager: unzip `PPSA99930.zip`, copy the `PPSA99930` folder to
   `/data/homebrew/PPSA99930` on the console (FTP is fine), let ShadowMountPlus register it.
2. Start **Forwarder Manager** from the home screen and create a new forwarder.
3. **Target app**, one of:
   - `PS5X360`: you can pick the game from a list, and Forwarder Manager writes its full path.
     The tile's target is then `PPSA50011` (the official app); change it in step 6.
   - `Generic (enter a title ID)`, with **Target title ID** `PPSA83600`. There is no game list:
     the **ROM file** row's file browser writes `--rom` with the file's **name only**, which finds
     the game only when the file lies directly in one of the game folders (`Halo 3.iso` in
     `/data/xbox360`). For anything else, write the full path in step 6.
4. **Game** (with the `PS5X360` target): pick it from the list (games in the default folders:
   `/data/xbox360`, `xbox360` on USB and extended storage, `assets/roms`), or pick the file or
   folder with the file browser (a game in a folder added in the emulator's own settings). Any of
   these works:
   - an `.iso` file;
   - an extracted game's folder or its `default.xex`;
   - an XBLA or Games on Demand title's folder (`Limbo`, the one with `584109D1/000D0000/...`
     inside), or the package file itself;
   - a multi-disc game's folder (`Mass Effect 2` with `Disc1`, `Disc2` inside): it starts at disc 1,
     as in the library.
5. Give the tile a name and an icon (the manager can use the game's cover), generate it, and let
   ShadowMountPlus register it. With the `PS5X360` target the tile passes the game's full path as
   its only argument.
6. Over FTP, open the tile's `/data/homebrew/PPSA992xx/forwarder.json` (its folder is named after
   the tile's title ID) and make `target` and `args` say:

   ```json
   "target": "PPSA83600",
   "args": ["--rom", "/data/xbox360/Halo 3.iso", "--exit-after-game"]
   ```

   - `target` must be `PPSA83600`. A tile made with the `PS5X360` target has `PPSA50011` here
     and would start the official app.
   - `args` names the game (`--rom` and a full path, as in the table below; a bare full path also
     works). `"--exit-after-game"` is optional: it returns to the PS5 home screen when you close
     the game. Forwarder Manager has no switch for it on these tiles.

   The file is read each time the tile is opened; nothing needs registering again. Saving the tile
   again in Forwarder Manager may rewrite `target` or `args`; check the file afterwards. The same
   edit moves an existing tile from the official PS5X360 to TrinityX360; tiles left with
   `PPSA50011` keep starting the official app.
7. **Close TrinityX360 before using a tile** if it is running. Arguments reach only a new
   process: if it is already open, the console brings it to the front as it was (library or
   running game) and the tile's game does not start.

Opening the tile now shows TrinityX360's start screen and then the game.

## Arguments TrinityX360 accepts

| Argument | Meaning |
| --- | --- |
| `--rom <game>` or `--rom=<game>` | The game. A full path (`/mnt/usb0/xbox360/Halo 3.iso`), a path inside the library's game folders (Settings; by default `/app0/assets/roms`, `/data/xbox360` and `xbox360` on USB and extended storage), tried in their order (`Halo 3.iso`, `RPG/Fable 2`), or a **title ID** of a game the library lists (`4D5307E6`; with several copies, the most recently played). A folder stands for the game in it, as the library lists it. A relative path may not contain `..`. |
| `<full path>` | A bare full path, without `--rom` (what Forwarder Manager writes for its `PS5X360` target). Same meaning as `--rom`. |
| `--exit-after-game` | *Close the game* in the guide closes TrinityX360 too, so the console goes back to the home screen. Without it the library opens, as usual. |

Other arguments are ignored. `--rom` wins over a bare path. Examples of `forwarder.json`
`args`:

```json
["--rom", "Halo 3.iso"]
["--rom", "/mnt/usb0/xbox360/Fable 2", "--exit-after-game"]
["--rom", "4D5307E6"]
["/data/xbox360/Limbo", "--exit-after-game"]
```

## What happens

- The game is looked for the way the library finds games, and starts as if it had been
  chosen there: its own options apply, its cover and name are the library's, and it becomes
  the most recently played game.
- A game whose own options are only read as the emulator starts (start options, the FSI
  render-target choice) makes TrinityX360 restart once, straight into it. It is still the tile's
  game after that restart, `--exit-after-game` included.
- If the game is not found (wrong path, a folder with several games, an unknown title ID),
  the library opens and says *Could not launch the game:* and why.
- If the game is found but does not start, TrinityX360 starts again into the library, which says
  why (even with `--exit-after-game`).
- *Close the emulator* in the guide closes TrinityX360 either way. Choosing another game from the
  library over the running game starts that one normally (no longer the tile's game).
- A game that switches to another executable (a collection's menu, a second disc) restarts
  TrinityX360 from Canary's launch data; `--exit-after-game` no longer applies after that.
- Every restart TrinityX360 makes of itself runs without arguments, so the tile's game never
  starts twice. `game.txt` is not read when a tile started TrinityX360.
- The log (stream on TCP 9100 and the session log) shows `Start argument N: ...` and
  `Forwarder: ...` lines saying what was asked for and what was started.

## Troubleshooting

### "Error in the game or application" right after opening the tile

The system's error screen (Polish: *Wystąpił błąd w grze lub aplikacji*) names the **tile's** title
ID, not the emulator's, and its **Debug** box says (captured with a test build that still used
`PPSA50011`; with TrinityX360 the messages name `PPSA83600`):

```
signal: 12 (SIGSYS)  thread name: eboot.bin  RIP: 0x00000008000003ac
Forwarder: could not start PPSA50011: the system refused to start the app (0x80940033, 0x80940033)
```

The emulator never started, so its own logs have nothing new. The SIGSYS is not a second fault: the
forwarder program ends with `exit()` after the refusal, and the system reports that as a crash.

`0x80940033` is the system's launcher refusing a homebrew title. To see why, read
`/data/forwarder-manager/launcher.log` (the forwarder launcher) and `/data/shadowmount/debug.log`
(ShadowMount+, with `debug = on`). The case seen with ShadowMount+ 1.7beta12:

```
launcher.log:     sceSystemServiceLaunchApp(PPSA50011) structsize=0 rc=0x80940033   (twice)
debug.log:        [GAME] started: PPSA99758 ...                                      (the tile)
                  [SHELLCORE] launch mount unavailable: PPSA50011 status=16 (Device busy)   (twice)
```

This is ShadowMount+ bug [drakmor/ShadowMountPlus#196](https://github.com/drakmor/ShadowMountPlus/issues/196)
(open): ShadowMount+ refuses a launch that another managed app (the tile) requests **within about a
second of its own start**. The same request about 6 s later goes through ("game switch prepared:
outgoing=<tile> incoming=PPSA50011"). It does not depend on the emulator, the game, its path or the
arguments, and it happens for folder installs too, so moving the emulator out of its `.ffpkg` or
`persistent_image_mounts=1` do not fix it.

The tile asks at once when the forwarder launcher is already running (Forwarder Manager starts it
when it opens, and it stays running after a tile's first use). When the tile has to start the
launcher itself through elfldr, its request comes several seconds later and is accepted. Until
ShadowMount+ is fixed:

- open the tile when the forwarder launcher is **not** running: after a console restart and the
  payloads, without opening Forwarder Manager first; the first tile then usually works;
- after that the launcher stays resident, and later tile opens in the same session are refused
  again: restart the console (there is no command to stop the launcher);
- or start the game from the emulator's library.

Other `Forwarder: ...` texts: *no launcher is running, and it could not be started through elfldr*
(elfldr not listening on 9021); *nobody is signed in*; *the launcher did not answer* or *refused
the request* (another or an old launcher on port 10199, see `launcher.log`).

### The official PS5X360 opens instead

The tile's `forwarder.json` still has `"target": "PPSA50011"` (the `PS5X360` target of Forwarder
Manager). Change it to `PPSA83600` (step 6 above).

### TrinityX360 opens, but in the library

- TrinityX360 was already open: the system brings it to the front and the arguments are lost.
  Close it first.
- The game was not found: the library says *Could not launch the game:* and why. The log has
  `Start argument N: ...` and `Forwarder: ...` lines.

## For developers

Parsing and lookup are in `include/xbox360ps5/forward.hpp` (assembled tree; added by fix 21) and
are checked on a PC by `tools/check-forward.cpp`, which `tools/source-1.0.2/run-fix-checks.sh` builds and runs
with the other fix checks (the source-fix-checks workflow and the source build). `src/canary_game_main.cpp` scans the library
before the game starts and passes `Launcher::Entries()` to `forward::Pick`.

Ported from upstream [BrinooTk/PS5X360#62](https://github.com/BrinooTk/PS5X360/pull/62)
(MartinPham, "Adding ROM launch argument"). Differences: the game is resolved against the
library's own scan (so ISOs and packages get their title ID and per-game options, multi-disc
sets resolve, title IDs work); a bare path also counts as a forwarded game (Forwarder
Manager's format); and the start-option restart and the FSI restart both keep it.
