# GFX-02A: a presentation-side frame rate limit (fix 146)

Base: `fix/1.0.6-perf` 38020a4 (v1.0.5 + 134e, 137/137b, 141/141b, 143). Patch:
`tools/source-1.0.2/fixes/146-present-limit.patch`. Check: `tools/check-present-limit.cpp`.
Work label: `1.0.6-work-fix146`. Only GFX-02A; the GPU flags of GFX-02B are not part of it.

## 1. Three clocks, kept apart

| Clock | Where (assembled tree) | What sets it | Who waits on it |
|---|---|---|---|
| **Emulated vblank** (the game's clock) | `GraphicsSystem` "GPU Frame limiter" thread, `MarkVblank()` in `xenia/gpu/graphics_system.cc` | `cvars::framerate_limit` = 60 or 120 from "Emulated video rate" (`video_clock`, `src/launcher.cpp` `Settings::Apply`); 0 is normalised to 60 | the game: it counts vblank interrupts (Dragon Age II waits 2 of them per frame: 30 FPS at 60 Hz, 60 FPS at 120 Hz). `framegen::Tick()` counts them too (`source_ticks`) |
| **Game swaps** (frame delivery) | `VdSwap` -> `PaceSwap()` in `xenia/kernel/xboxkrnl/xboxkrnl_video.cc` (fix 10/116), then the GPU thread's `IssueSwap` -> `Presenter::RefreshGuestOutput` | the game | fix 116 sleeps the game's thread in `VdSwap` only while VSync is on and the game's own swaps come faster than about 61 Hz. `RefreshGuestOutput` never waits for the screen: it makes the new picture the "ready" one of a mailbox of three images and writes the next picture into the third |
| **Output refresh** (the screen) | `VulkanPresenter` KHR_display surface (59.94 Hz, or 119.88 Hz when the framegen file asks for 120/auto and the display offers it), swapchain FIFO with frame generation, otherwise IMMEDIATE > MAILBOX > FIFO_RELAXED > FIFO as the driver offers | the display mode | the UI thread: on PS5 the FPS overlay and the guide are UI drawers, so every paint is `PaintFromUIThread` from the main loop (`context.Tick(); window.Paint();`), and with FIFO each present waits for a refresh. Each paint takes the newest ready picture (`Presenter::ConsumeGuestOutput`) |

Looking only at the number on screen can hide a change of the game's pace: the FPS counter shows
`refreshed_output_count` (game frames that reached the presenter), not presents, and a change of
`video_clock` changes the game's own rate (DA2 30 -> 60). The limit must change none of the first
two rows.

## 2. Where the limit waits

**In `Presenter::ConsumeGuestOutput`, and it does not wait.** When a newer picture than the
acquired one is ready, `present_limit::Gate::Take()` decides whether the paint takes it now. If not,
the paint shows the picture it already holds (the acquired image stays acquired; the CAS is
skipped). The producer keeps writing the other two images and replacing the ready one, exactly as
it does today whenever the game is faster than the screen. So:

- nothing sleeps for the limit (the check rejects any sleep/wait in `present_limit.hpp`);
- the game's thread, the GPU command thread, the emulated vblank, fix 116 and the audio are not
  touched: no backpressure on the guest;
- the only waits stay the ones of today: fix 116 in `VdSwap` (guest side) and the FIFO present on
  the UI thread (host side). **No double waiting.**

Rejected places: `VdSwap`/fix 116 (a wait there is backpressure: it changes the game's own timing,
which the card forbids; a guest-side cap would be another option with its own tests), the vblank
thread (it is the game's clock), a sleep before the present on the UI thread (it would delay the
guide and could skip refreshes; with FIFO the loop is already refresh-paced).

### The grid

A picture is held a whole number of output refreshes, `k = round(output_hz / target)`, accepted
only if `|output_hz / target - k| <= 5% of k`:

- period = k refreshes of the real mode (119.88 Hz: 3 x 8.342 ms = 25.025 ms, so no drift against
  the screen), slack = half a refresh (a paint that wakes a little early is on time);
- a newer picture is taken when `now + slack >= deadline`; on time keeps the grid
  (`deadline += period`), late (the game's picture came after the slot) restarts it from now, so
  there is no short "catch-up" picture and a game slower than the target is shown as before;
- the first picture, a blank one, or one after a pause of 250 ms (loading, a menu that stops
  drawing) is shown at once and the grid starts there;
- the limit never raises a frame rate.

## 3. Coexistence with fix 116

Fix 116 owns the guest side: while VSync is on and the game's own swaps come faster than the
vblank (a 60 FPS patch, a game unlocked to 80 FPS), it paces `VdSwap` on fix 10's 16.60 ms grid.
Fix 146 owns the host side: which of the ready pictures the screen shows. They never wait for each
other: a game paced by fix 116 to 60 swaps/s with a 30 limit on the 120 Hz output still swaps 60
times a second (fix 116's line), and the screen shows every second picture for 4 refreshes. The
VSync option keeps its meaning (the limit does not force VSync or any refresh), and VSync off still
leaves fix 116 idle.

## 4. Cases

- **Frame generation on**: not applied (`not applied: frame generation is on and paces its own
  pictures`). The framegen path consumes the mailbox itself, with its own pacing (strict/adaptive)
  and FIFO; a cap in front of it would fight it. Toggling frame generation needs a restart anyway
  (the framegen file is read once), and the plan follows the flag at every paint.
- **Menus and the guide**: the guide and the FPS overlay are drawn at every paint, so they move at
  the output rate; only the game's picture under them changes at most at the target. A game menu
  that draws at 60 is limited like gameplay; a static menu (no new pictures) is unaffected.
- **A change during the session**: the option is live (`W::live`, the guide and the phone's page
  call `Settings::Apply`, which calls `SetChoice`); at the next paint the plan is made again, the
  grid restarts and a line is logged. A new display mode (surface made again) does the same.
  Per game: a change in the guide is kept for the running game (fix 143's rule, `per_game`).
- **Game below the target** (GTA IV at 35 with 40): every picture is shown when it comes, as
  before; nothing is held.
- **Return to the launcher / next game**: the gate is a process-wide object; the next game's
  `Settings::Apply` sets its own choice; the grid restarts after the pause.

## 5. Behaviour table

Game faster than the target unless said. "As before" = every new picture at the next present.

| Option | Output 59.94 Hz (FIFO) | Output 119.88 Hz (FIFO) |
|---|---|---|
| Automatic (default) | as before (at most ~59.94 new pictures/s) | as before (at most ~119.88/s) |
| No limit | as before (same as Automatic in 146) | as before |
| 30 | a new picture every 2 refreshes (33.37 ms), 29.97/s | every 4 refreshes (33.37 ms), 29.97/s |
| 40 | **not applied** (1.5 refreshes): as before; the log says "40 FPS needs the 120 Hz output" | every 3 refreshes (25.03 ms), 39.96/s |
| 60 | 1 refresh: as before (the output already limits to 59.94) | every 2 refreshes (16.68 ms), 59.94/s |
| any, frame generation on | not applied | not applied |
| any, game slower than target | as before | as before |
| any, swapchain not FIFO | held on the same timed grid, no refresh lock (tearing as today) | same |

Automatic and No limit do the same in 146: Automatic is the default that may later get a title's
own value (the card removed automatic title presets for now); No limit says "never this game".

Latency: a held picture waits at most one period minus a refresh (40 at 120 Hz: up to ~17 ms more
than today); the summary line gives the average wait of a shown picture.

Honest limit of the approach: the screen's cadence becomes even, the content's does not. A game
at 45-55 FPS capped at 40 shows its newest picture every 25 ms, but the game time between two shown
pictures varies (18-36 ms) and about one in five of its pictures is dropped. Whether that looks
better than 45-55 FPS on the 120 Hz output (2-3 refresh mix) is the console test's question. A
fully even game-time cadence needs backpressure (the game waits), which is out of this card.

## 6. Measurement (log)

At a change: `Presentation limit (fix 146): 40 FPS chosen; applied: each new picture shown for 3
refreshes of 119.88 Hz (FIFO) (the game runs as before)` or `...; not applied: <why>`.

Every 30 s (in every mode, Automatic too, as the A/B baseline):

```
Presentation limit (fix 146): target 40 FPS (3 refreshes of 119.88 Hz); 40.0 new pictures/s of
52.3 game swaps/s, 119.9 presents/s, 120.0 vblanks/s; held 21.0/s, repeated 79.9/s, dropped
12.3/s; waited 392.0 ms/s (9.8 ms a picture); interval median 25.25 ms, p95 25.50 ms
```

- new pictures/s: pictures the screen took; game swaps/s: pictures the game published
  (`RefreshGuestOutput`, the FPS counter's source); presents/s: paints; vblanks/s: the emulated
  vblank (`framegen::source_ticks`): VBlank, real swaps and presents are separate numbers.
- held: paints that kept the old picture while a newer one was ready (the limit's doing);
  repeated: paints without a new picture (held or nothing new); dropped: game pictures never shown.
- waited: from a shown picture's arrival to its paint; interval: time between new pictures
  (0.25 ms bins, upper edge).
- fix 116's own line (`Performance swap pacing (fix 116): n swaps/s ...`) stays the guest side.

## 7. Console test plan (`1.0.6-work-fix146`)

Record: PS5 model, renderer Vulkan, game + Title ID, scene, the Frame generation output setting
(60 / Up to 120 Hz) and the `Framegen output: ... selected_hz=` line, frame generation off unless
said, VSync on. Take 60 s per case on the same spot after shaders warmed; keep the log.

1. **Baseline, Automatic.** One `Presentation limit (fix 146): Automatic, not applied` summary per
   30 s; note presents/s (FIFO at ~59.9 or ~119.9; anything far above means the swapchain is not
   FIFO), game swaps/s and the median/p95.
2. **Dragon Age II (4541090B), Emulated video rate 120 Hz (game at 60), output 60 Hz**: Automatic,
   then 30 from the guide. Expect `30 FPS chosen; applied ... 2 refreshes of 59.94 Hz`, ~30 new
   pictures/s, median ~33.5 ms, p95 < 34.5; **game swaps/s, vblanks/s (~120) and fix 116's
   swaps/s unchanged** vs Automatic; dialogue lip-sync and music unchanged; the guide stays smooth.
   Back to Automatic: ~60 new pictures/s again.
3. **GTA IV (545407F2), city, output Up to 120 Hz (restart), 1x**: Automatic (expect ~45-55 new
   pictures/s, intervals mixing 16.7/25 ms), then 40: `3 refreshes of 119.88 Hz`, ~40 new
   pictures/s, median ~25.3, p95 ~25.5; game swaps/s the same as Automatic. Judge by eye: is the
   motion steadier or worse (content judder)? Then 30 (4 refreshes). Then a scene below 40 (busy
   street / fast drive): the limit must not lower it further (new pictures/s = game swaps/s).
4. **40 on the 60 Hz output** (GTA IV, output 60): `not applied: ... (40 FPS needs the 120 Hz
   output)`, numbers as Automatic.
5. **A 60 FPS game** (e.g. a 60 FPS menu or a 60 FPS title), output 120: 60 -> 2 refreshes, ~60/s,
   game unchanged; output 60: 60 = as before.
6. **Frame generation on** (any mode): `not applied: frame generation is on`, framegen's own lines
   unchanged.
7. **Live switching**: change the limit 5-6 times from the guide and the phone's page while the
   game renders; one line each, no hang, no black frame; restart the game: its own value comes back
   (kept for its Title ID); a second game starts with Automatic.
8. **A/B timing**: with the limit off and on, the in-game clock/animation speed and audio sync
   are unchanged (e.g. a cutscene's length, a timed sequence); vblanks/s identical.
