> Translated from the original author's Portuguese text.

# TrinityX360 — user guide

Experimental Xbox 360 emulator for PlayStation 5, based on PS5X360 by BrinooTk and on Xenia
Canary (named PS5X360Reloaded until v1.0.5; title id `PPSA83600`).
No games, BIOS or keys come with the emulator: use copies of your own games.

## What you need

- A PS5 with homebrew access (tested on firmware 13.60 with etaHEN, ShadowMountPlus and kstuff).
- An FTP client on your computer (a PS5 running etaHEN opens FTP on port 2121).

## Installation

1. Copy the `PPSA83600` folder to `/data/homebrew/PPSA83600` on the PS5.
2. ShadowMountPlus registers the title on its own; it shows up in the menu as
   **TrinityX360**.

## Games

Each game goes in its own folder, inside
`/data/homebrew/PPSA83600/assets/roms/`:

```
roms/Sonic the Hedgehog (2006)/default.xex
roms/Outro Jogo/default.xex
roms/Mais Um/jogo.iso
```

Supported formats:

- **Extracted folder**: `default.xex` together with the game's data folders.
- **Disc image** `.iso`.
- **GOD/STFS package** (Games on Demand, Xbox Live Arcade): the header file
  with the `.data` folder next to it, as they come from the console.

The emulator only sees `assets/roms` inside the title folder. Folders elsewhere
on the PS5 or on USB do not show up.

After copying, open the emulator, or use **Square → Refresh game list**.

## Shelf controls

| Button | Action |
| --- | --- |
| D-pad / stick | Choose a game |
| X | Play |
| Triangle | Game details and patches |
| Square | Settings |
| L1 / R1 | Filters (All, Recent, Folders, ISO, Arcade & GOD) |
| Circle | Close the open panel |

While playing, **click the touchpad** to open the emulator guide (styled
after the Xbox 360 guide). It has two pages, switched with L1/R1:

- **Game**: resume, return to the game library (to switch games),
  press the Xbox BACK button in the game, and exit the emulator.
- **Settings**: show FPS, sound, image filter (Basic, CAS or FSR)
  and what the touchpad click does.

If you prefer the touchpad as the BACK button, change it in "Touchpad click"; the guide
then opens with OPTIONS + touchpad. The game keeps running underneath the
guide, but does not receive button presses while the guide is open. The PS button closes the
emulator like any game.

## Profiles

Xbox 360 games save progress and achievements to the signed-in profile (gamertag).
The first time, the emulator creates the profile "Player". In **Settings → Player
profile** you choose which profile to use or create another one (up to 15 letters and
digits, starting with a letter). The chosen profile stays in effect for
subsequent games, and each profile has its own saves.

In game, the DualSense becomes an Xbox 360 controller: X = A, Circle = B,
Square = X, Triangle = Y, OPTIONS = Start, touchpad click = Back.

## Covers

With **Square → Download covers**, the emulator downloads from XboxUnity (the same service
Aurora uses) the cover for each game that does not have one yet. The PS5 must be
connected to the internet. Covers are stored and appear as soon as the download
finishes.

Your own cover always takes priority: put `cover.png` (or `.jpg`) in the game's
folder, or `nome-do-arquivo.png` (file-name.png) next to an `.iso`.

## Collections

Games that launch another game from their own menu (for example, the Dragon Ball Z
Budokai HD Collection, which launches Budokai 1 or 3) make the emulator restart
straight into the chosen game. The screen flickers and comes back already in the game; this is expected.
When the game returns to the console dashboard, the emulator returns to the shelf.

## Patches

The emulator ships with the Xenia Canary community patch pack (60 FPS,
resolution, disabling effects and others), in `assets/patches`. All of them come
disabled. To enable one: Triangle on the game → choose the patch → X.

- The emulator identifies the game by its own executable (or by the `.iso` image),
  so patches show up even before the game is launched for the first time.
- A patch is only applied if it was made for the same executable version as your
  game; if not, the panel warns you.
- 60 FPS patches can change the game speed or cause problems in
  some scenes. If something goes wrong, disable the patch.

## Settings (Square)

- **Language**: Automatic uses the PS5 language. The interface has Portuguese, English and Spanish; when no translation is available, it uses English. You can also choose one manually. The selection passes the language to the next game started, as long as the game has that translation. Older per-game language preferences are kept until a new selection is made.
- **Sound**: on or muted.
- **Detailed logs**: records every system call the game makes. Makes the game
  slower; use it only to investigate a problem.
- **Show FPS in game**: frames-per-second counter in the corner of the screen.
- **VSync**: on/off, saved in the preferences. When you close the settings,
  the emulator restarts to apply the change before initializing video. It controls the emulated
  VSync; it does not turn off the PS5's display synchronization. On by default.
- **Game folders**: add several locations, including external devices.
  Square opens the picker; X enters the folder; Circle goes up one level; Triangle
  adds the current folder. In the list of locations, Triangle removes the location without deleting
  games. Disconnected folders stay saved and only accessible locations are read.

## When something goes wrong

- Each run creates a file named after the game and the date in
  `/download0/xbox360ps5/LOGS`. Older runs are preserved. Large
  sessions also use three `.partN.log` files; send the whole set for the session.
- With the app open, FTP normally reaches
  `/mnt/sandbox/PPSA83600_000/download0/xbox360ps5/LOGS`. The suffix may vary.
- `python tools/console.py game-logs --host IP_DO_PS5` downloads the logs and builds
  a ZIP in `build`, ready to share (`IP_DO_PS5` = the PS5's IP address).
- On Windows with Python installed, you can also open
  `tools/Download PS5 logs.bat` in the source tree and enter the PS5's IP address.
- Every 30 seconds of gameplay, the log records the average frames per second.
- When reporting a problem, state the game, the version (region), what appears on the screen and
  whether any patch was enabled.

Compatibility varies from game to game. Some games may not open, may freeze
or may have graphics and sound glitches.
