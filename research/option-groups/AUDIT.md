# Option groups: audit of the launcher's and the guide's options (1.0.6)

Base: `origin/fix/1.0.6-perf` at `1.0.6_candidate_2` (77868b0), assembled title files
(`assemble.py --skip-core`), plus fix 146 (`fix/fps-limit`, not merged) for "Frame rate limit (screen)".

Trigger: a player set GTA IV's **Emulated video rate** to 120 Hz in the guide, believing it was the
TV's output rate. It is the emulated console's vblank clock (`cvars::framerate_limit`), and some
games run fast with it. The real output rate is "Frame generation output (test)", under
Settings > Performance in the launcher only.

## The two groups

1. **Picture and smoothness**: changes how the finished picture looks or how smoothly it is shown.
   The game itself runs the same (its speed, timing and what it is told do not change).
2. **Emulation (advanced, per game)**: changes how the emulated Xbox 360 behaves for the game, or how
   faithfully it is emulated. It can change the game's speed, timing or correctness.

Neither group: sound, controls and system options (Audio, Controls, System) stay where they are.

## Where the options are today

- Launcher, Settings > General: Video (0), Performance (1), Audio (2), Controls (3), System (4)
  (`kOptionCategories`, `include/xbox360ps5/launcher.hpp`). The Performance page adds three rows that
  are not in the table: Frame generation (test), Frame generation output (test), Accurate render
  targets (FSI, test).
- Launcher, a game's sheet: the per-game options by the same categories.
- Guide, Settings tab: Network, then each category with options that are `W::live`
  (`GuideItems`, `src/canary_game_main.cpp`); the Performance category starts with
  "Measure performance (10 s)". The Game tab has the quick row "Mute game sound".
- Phone page: one tab per category (`WebState` sends `kOptionCategories` in index order).

## Audit: the launcher's option table

`When`: live = at once (the guide shows it), launch = next game start, start = the emulator restarts.
`Guide`: shown in the guide today.

| Key | Label | Page now | When | Per game | Guide | Group | Reason |
|---|---|---|---|---|---|---|---|
| renderer | Renderer | Video | start | no | no | 2 | Chooses the emulator's graphics backend; compatibility and correctness differ between them. |
| image_filter | Image filter | Video | live | yes | yes | 1 | Scales the finished picture to the screen (Plain/CAS/FSR). |
| antialiasing | Anti-aliasing | Video | live | yes | yes | 1 | Post effect on the finished picture (FXAA, FXAA Extreme, SMAA 1x). |
| color_preset | Color preset | Video | live | yes | yes | 1 | Post effect on the finished picture's colours. |
| overscan_crop | Overscan crop | Video | live | yes | yes | 1 | Crops and enlarges the finished picture. |
| sharpness | Filter sharpness | Video | live | yes | yes | 1 | Strength of CAS/FSR on the finished picture. |
| dither | Smooth gradients | Video | live | no | yes | 1 | Dithers the output picture. |
| stretch | Picture proportions | Video | live | yes | yes | 1 | Letterbox or stretch of the finished picture. |
| anisotropic | Anisotropic filtering | Video | live | yes | yes | 1 | Texture sampling quality only; the game's timing is unchanged (a few games show defects). |
| present_limit (146) | Frame rate limit (screen) | Video | live | yes | yes | 1 | Holds pictures on screen for an even motion; the game runs as before (host-checked in 146). |
| draw_scale | Resolution scale (test) | Video | start | yes | no | 1 | The game is not told; sharper picture, more GPU cost (heavy; some read-back effects may look wrong). |
| internal_resolution | Emulated console resolution | Video | launch | yes | no | 2 | The video mode the game is told; some games draw more or fewer pixels. |
| video_clock | Emulated video rate | Video | live | yes | **yes** | 2 | The emulated console's vblank clock (`framerate_limit` 60/120): 120 Hz makes some games run fast. |
| vsync | VSync | Video | start | yes | no | 2 | Not the TV's vsync: it switches the emulated vblank pacing (the vblank thread waits 1000/60 ms or 1 ms), so games with 60 FPS patches speed up when off. Listed as group 1 in the brief; the code says group 2. |
| gamma | Emulated console gamma | Video | launch | yes | no | 2 | The display type the console reports to the game. |
| depth_precision | Depth precision | Video | start | yes | no | 2 | How the emulated GPU's 24-bit depth is reproduced (correctness, cost). |
| decal_bias | Decal fix | Video | launch | yes | no | 2 | Shader translation of the depth offset (correctness). |
| mulsc | Geometry rounding | Video | launch | yes | no | 2 | Shader translation rounding (correctness). |
| fast_locks | Fast locks | Performance | start | yes | no | 2 | Emulator lock behaviour; speed, can hang games. |
| memory_boost | Optimized video memory | Performance | start | yes | no | 2 | CPU-GPU memory tracking; speed, can show defects. |
| dynamic_buffers | Optimize dynamic buffers | Performance | live | yes | yes | 2 | Memory tracking; speed, can flicker. |
| memory_window | Memory update window | Performance | live | yes | yes | 2 | Memory tracking granularity; speed. |
| occlusion | Visibility queries | Performance | start | yes | no | 2 | What the game is answered for its queries; correctness and speed. |
| readback | Image readback by the game | Performance | live | yes | yes | 2 | Whether the game gets its drawn pictures back; correctness and speed. |
| memexport | Readback of data made by video | Performance | live | yes | yes | 2 | Whether the game gets GPU-written data; correctness (BF3 hangs without), speed. |
| async_shaders | Background shaders | Performance | live | yes | yes | 2 | Draws are skipped or stood in until shaders are ready; correctness and stutter. |
| clear_pages | Re-read memory written by video | Performance | live | yes | yes | 2 | Memory page state; correctness (Team Ninja games), speed. |
| released_memory | Draw from released memory | Performance | live | yes | yes | 2 | Whether draws from released memory are made; correctness. |
| guest_yield | Yield the processor while waiting | Performance | launch | yes | no | 2 | Guest thread scheduling; speed and timing. |
| threaded_driver | Video driver on a second thread (test) | Performance | start | no | no | 2 | Video driver threading; speed, can hang. |
| pipeline_threads | Shader preparation threads | Performance | start | yes | no | 2 | Competes with the game for CPU; speed. |
| fast_dot | Fast dot product (test) | Performance | launch | yes | no | 2 | Less exact CPU maths; physics and geometry correctness. |
| no_promotion | Conservative recompiler | Performance | launch | yes | no | 2 | Recompiler optimisation; correctness and speed. |
| zero_page | Allow access to address zero | Performance | start | yes | no | 2 | Guest memory behaviour; correctness. |
| fsi | FSI (per game) | Performance | launch | yes | no | 2 | Render target emulation path; correctness and cost. |
| mute | Sound | Audio | live | no | yes (+ "Mute game sound" quick row) | - | Sound output. |
| volume | Game volume | Audio | live | yes | yes | - | Sound output. |
| ui_sounds | Interface sounds | Audio | live | no | yes | - | Menu sounds. |
| ui_sound_volume | Interface volume | Audio | live | no | yes | - | Menu sounds. |
| xma_decoder | Audio decoder | Audio | start | yes | no | (2) | Emulated XMA decoder; can hang. Start only, not in the guide; stays under Audio (where players look for sound problems). |
| xma_inline | Audio without its own thread | Audio | start | yes | no | (2) | XMA decode threading; timing. Same as above. |
| touchpad_menu | Touchpad click | Controls | live | no | yes | - | Controller mapping. |
| vibration | Vibration | Controls | live | yes | yes | - | Controller. |
| deadzone_left / deadzone_right | Left / Right stick dead zone | Controls | live | yes | yes | - | Controller. |
| motion_phone | Camera motion (research) | Controls | live | yes | yes | - | Phone as Kinect. |
| motion_panel | Motion panel | Controls | live | yes | yes | - | Overlay position. |
| motion_picture | Camera picture for the game | Controls | live | yes | yes | - | Phone camera picture. |
| show_fps | Show FPS in game | System | live | no | yes | - | Overlay. |
| achievement_toasts | Achievement notifications | System | live | no | yes | - | Notifications. |
| web_page | Settings page for a phone | System | start | no | no | - | Phone page. |
| arcade_full | Arcade games | System | launch | yes | no | (2) | The licence the game is told; launch only, not in the guide; stays under System. |
| save_drive | Save drive | System | live | yes | yes | - | Where saves go. |
| detailed_logs | Detailed logs | System | live | no | no (excluded) | - | Diagnostics. |
| bug_report | Logs for a bug report | none (-1) | live | yes | no (own button) | - | Diagnostics. |

### Rows outside the table

| Row | Where | Guide | Group | Reason |
|---|---|---|---|---|
| Frame generation (test) | Launcher, Performance | no | 1 | In-between pictures from two game pictures; the game runs as before. |
| Frame generation output (test) | Launcher, Performance | no | 1 | The PS5 video output rate (60 Hz / Up to 120 Hz): the TV's rate. |
| Accurate render targets (FSI, test) | Launcher, Performance | no | 2 | General FSI switch (render target emulation). |
| Network | Guide, Settings tab | yes | - | Network mode (network.conf). |
| Measure performance (10 s) | Guide, Performance | yes | - | Measures the options of group 2; goes with them. |
| Mute game sound | Guide, Game tab | yes | - | The Sound option. |

## Today's mix-ups

- The guide's **Video** category mixes group 1 with **Emulated video rate** (group 2), the only group 2
  option there; it is live, so a change from the guide takes effect at once.
- The launcher's Video page holds 8 group 2 options next to 10 group 1 options, while the real output
  rate (Frame generation output) is on the Performance page.
- The guide shows no help text, so "Emulated video rate" / "120 Hz (test)" reads as a TV setting.

## The proposal the user chose (fix 149, `tools/source-1.0.2/fixes/149-option-groups.patch`)

1. **Two pages, Emulation last.** `kOptionCategories` 0 is "Picture and smoothness" (was Video), 1 is
   "Emulation (advanced)" (was Performance); the numbers stay, so other fixes' rows apply unchanged.
   `xbox360ps5/option_groups.hpp` holds one table with every option's group (this audit as code);
   `Options()` moves each option to its group's page and replaces the few labels and help texts below.
   Pages in the order Picture and smoothness, Audio, Controls, System, Emulation (advanced): the
   launcher's settings, a game's sheet, the guide and the phone page (its `category` is now the
   page's place in that order). Frame generation and Frame generation output move to Picture and
   smoothness; the general FSI row stays with Emulation (advanced).
2. **The guide shows Emulation (advanced) as its last page, with a warning.** The page opens with an
   amber notice row (it only tells; the focus lands on it): "These change how the emulated Xbox 360
   runs this game, not your TV. They can make it run too fast, too slow or wrong. Changes are kept for
   this game." Changing one of its options shows "Changed for this game only. If it now runs too
   fast, too slow or looks wrong, set it back." over the list's bottom for 8 s, and logs
   `Guide: <key> is an emulation option (fix 149): the game may now run faster, slower or wrong`.
3. **VSync is emulation** (the code: it switches the emulated vblank pacing).
4. **Clearer names and help:** "Emulated video rate (game clock, not your TV)" (help: not the TV's
   refresh rate, that is Frame generation output under Picture and smoothness), "VSync (emulated, game
   speed)", Emulated console resolution's help: "not your TV's resolution". New page descriptions on
   the settings sheet and "Restore recommended". All in the 15 translated languages.

Not changed: the option keys and values in `settings.txt` and `game-settings/<TITLE ID>.txt`, what each
option does, and where a guide change is kept (fix 143).

## Console test plan

Build: `1.0.6-work-option-groups` (BUILD_VERSION on the home screen).

1. **Launcher, Settings > General**: the rows read Player profile, Language, Picture and smoothness,
   Audio, Controls, System, Emulation (advanced), Download covers... Picture and smoothness lists Image
   filter, Anti-aliasing, Color preset, Overscan crop, Filter sharpness, Smooth gradients, Picture
   proportions, Anisotropic filtering, Resolution scale (test), then Frame generation (test) and Frame
   generation output (test). Emulation (advanced) starts with Renderer, Emulated console resolution,
   "Emulated video rate (game clock, not your TV)", "VSync (emulated, game speed)", ... and ends with
   "Accurate render targets (FSI, test)". Each row's help text at the bottom is the new one.
2. **A game's sheet (L1/R1 > settings)**: the subjects are in the same order, Emulation (advanced) last;
   the names are the pages' (no "Graphics").
3. **Guide over GTA IV (545407F2)**: Settings tab: Network, Picture and smoothness, Audio, Controls,
   System, Emulation (advanced). Picture and smoothness has no Emulated video rate. Open Emulation
   (advanced): the first row is the amber notice (Cross and left/right do nothing on it), then Measure
   performance (10 s), then "Emulated video rate (game clock, not your TV)" and the former Performance
   options. Change the video rate to 120 Hz (test): the amber "Changed for this game only..." box shows
   over the list's bottom for about 8 s, the log has `Guide: video_clock set to 120 Hz (test) ... kept
   for 545407F2` and `Guide: video_clock is an emulation option (fix 149)`. Set it back to 60 Hz.
   Changing Anti-aliasing on Picture and smoothness shows no warning.
4. **Phone page**: the tabs read Picture and smoothness, Audio, Controls, System, Emulation (advanced),
   Logs, Camera; each option on the tab of its page (Emulated video rate under Emulation (advanced)).
5. **Language**: switch the interface to Polish: "Obraz i płynność", "Emulacja (zaawansowane)", the
   notice and the warning in Polish.
6. **Nothing else moved**: a game's own values (game-settings) and the general settings.txt keep their
   values; the options apply as before.

## Fix 149b: the settings as a menu of several levels (#57)

Console feedback on `1.0.6-work-fix149` (the user's words): the launcher's settings and a game's
settings ("Next subject") are unreadable; there must be a menu of several levels, not lists hidden under
Triangle. The user then chose (all recommended): sub-pages only where a page is long; a visible "Game
settings" row; in the guide the same pages with the sections as headers; the library's rows grouped.

```
Settings
├─ Player profile / Language
├─ Picture and smoothness  >
│   ├─ Anti-aliasing and sharpness  >  Anti-aliasing, Image filter, Filter sharpness, Smooth gradients
│   ├─ Colors and screen fit  >        Color preset, Overscan crop, Picture proportions
│   ├─ Frame rate and 120 Hz  >        Frame rate limit (screen) [fix 146], Frame generation (test), Frame generation output (test)
│   └─ Detail and resolution  >        Anisotropic filtering, Resolution scale (test)
├─ Audio  >  /  Controls  >  /  System  >   (one list each)
├─ Emulation (advanced)  >
│   ├─ Video clock and mode  >         Emulated video rate, VSync, Emulated console resolution, Emulated console gamma
│   ├─ Renderer and accuracy  >        Renderer, Depth precision, Decal fix, Geometry rounding, Visibility queries, FSI (per game), Accurate render targets (FSI, test)
│   ├─ Video memory and readback  >    Optimized video memory, Optimize dynamic buffers, Memory update window, Image readback, Readback of data made by video, Re-read memory, Draw from released memory
│   ├─ Shaders and threads  >          Background shaders, Shader preparation threads, Video driver on a second thread
│   └─ Processor  >                    Fast locks, Yield the processor, Fast dot product, Conservative recompiler, Allow access to address zero
├─ Game settings  >                    the identified games (the one chosen in the library first) > the same tree, the game's own values
├─ Library and saves  >                Download covers, Game folders, Refresh game list, Saved data
├─ Restore recommended / Theme / Restart emulator
└─ Problem with a game? Logs for a bug report
```

### Console test plan (1.0.6-work-fix149b)

1. Library, Square (Settings): no "General / Per game" tabs, no "L1 / R1". The rows are as above; Game
   settings and Library and saves show ">".
2. Picture and smoothness: four section rows, each with ">" or "N changed  >", and under the list the
   names of the focused section's options. Above: "Settings  >  Picture and smoothness". X on Colors and
   screen fit: the title reads "Settings  >  Picture and smoothness  >  Colors and screen fit", three
   options. Circle: back to the four sections; Circle again: the top list. Square from anywhere closes.
3. Frame rate and 120 Hz holds Frame generation (test) and Frame generation output (test) (and the frame
   rate limit when fix 146 is in); Emulation (advanced) > Renderer and accuracy ends with "Accurate render
   targets (FSI, test)". Changing them works as before.
4. Audio, Controls, System: open straight to their options.
5. Game settings: the game focused in the library is first. X: the game's sheet on its Settings tab;
   subjects, then Picture and smoothness > sections > options with "General: ..."/own values; the hints
   show no "Next subject"; Circle goes back level by level; back in the list the game is first again.
6. Library and saves: Download covers (its status line), Game folders, Refresh game list, Saved data
   work; Circle from Saved data or Game folders returns to Library and saves.
7. Guide over a game, Settings > Picture and smoothness: grey section names over the rows; the focus never
   stops on them (Up from the first option goes to the tab bar). Emulation (advanced): the amber notice,
   Measure performance, then the sections' names over their options.
8. Phone page: the Picture and smoothness and Emulation (advanced) tabs show the section headings.
9. Polish interface: "Wygładzanie i ostrość", "Biblioteka i zapisy", the titles translated.
