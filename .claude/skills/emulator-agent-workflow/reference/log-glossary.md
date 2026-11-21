# Console logs: files and lines

Which lines exist depends on the build: diagnostics are fixes, and some exist only on one version
line. For the exact wording of a line, grep the fix's row in `tools/source-1.0.2/fixes/README.md`
on the branch the image was built from (`git show <branch>:tools/source-1.0.2/fixes/README.md`).

## Files

All under `<data-root>/logs/` (Reloaded: `/data/homebrew/PPSA83600/logs/`). The fallbacks when
that folder is not writable are `/app0/logs/`, then `/download0/xbox360ps5/LOGS/`.

| File | What it is | Read it for |
|---|---|---|
| `Game-<name>-<TITLEID>-<date>-<time>-UTC-<n>.log` | One game session. The header has the version label, title id and executable path. Verbose sessions roll over into `.part1.log` .. `.part3.log` after the first 8 MiB, so fetch all files with the same prefix. Native crash reports are appended to it. | Everything in a game: settings, performance summaries, crashes. |
| `Game-Launcher-<...>.log` | The launcher (library, boot, before a game is chosen). | Library scans, multi-disc grouping, start options, storage devices. |
| `boot.log` | Early boot and anything before game selection, plus hang reports (`HANG report` is also written here). | A build that does not reach the launcher. Confirm `BOOT version <label>`. |
| `hang-watch-<TITLEID>-<YYYYMMDD-HHMMSS>-UTC.txt` | Fix 47's watch output for a session whose process the system ended. The newest few are kept. | Silent exits: no `[X360] CRASH`, no `TERMINATE`, no STALL. |
| `hang-<hash>-vs.ucode` / `-ps.ucode` (+ `.txt`) | Shaders of the draws in flight at a hang. | GPU hangs. |
| `texture-capture-<n>/` | Texture captures (diagnostic builds). | Texture or colour bugs. |
| `<data-root>/web-settings.key` | The settings server's key (not a log). | Web API calls. |
| `<data-root>/readback-keys.txt` | Fix 91's console-side switch (you write it). | Per-key readback bisects. |

**Confirm the build first.** The header or `BOOT version ...` must show the label you expect.
ShadowMountPlus once kept an older image mounted, and a whole test ran the wrong build.

## Crash and hang lines

| Line (prefix) | Meaning |
|---|---|
| `[X360] CRASH` | A guest crash with its dump (registers, guest function, thread). Fix 45 shows the player a notice and logs the crashed thread. |
| `TERMINATE` | The guest ended itself. |
| `STALL` report | The frame watch saw no frames for 15 s. It lists threads, the waits on each kernel object (fix 24's history), the GPU ring pointers and any `WAIT_REG_MEM` it is in. "no output is not proof of deadlock". |
| `[X360] FAULT ...` | A host fault no handler took (fix 65), with the state of the page. `a fault inside the fault handler` = a nested fault. `STACK OVERFLOW` (fix 61) = a host stack ran out. |
| `[X360] SIGNAL <n> from pid ...` | A signal reached the crash reporter. |
| `HANG report` / `HANG still +N s` / `HANG over` | Fix 47: the UI loop stood still for 4 s, or a fence, submit or acquire wait lasted 4 s (pipelines 20 s). It gives heartbeats, open submissions, a verdict (`the GPU does not finish a submission ...` = GPU hang; `the UI loop is stopped outside a GPU wait`) and `WHERE` lines (eboot offsets). |
| Alloc failure lines (fix 76) | Failed guest allocations: the first 16, then a count. A game searching for room makes many. |
| `System heap ...` (fix 79) | The emulator's own allocations in the guest 4 KiB heap, by holder, with growth since the previous line. A steadily growing holder = a leak (the BF3 leak was found this way). |

Hang-watch "beats": the heartbeats of the UI loop (input, paint, sampling, measuring) and of the
GPU command thread (executing, idle). The beat that stopped first tells you which side hung. The
guest beat also gives the deepest host stack sampled, and heap lines show the heaps at that time.

## Performance summaries (every 30 s in a game)

Read them as rates (`/s`) and milliseconds of each second (`ms/s`) for one thread, mostly the
**GPU command thread**. 1000 ms/s means fully busy. Compare the same scene across A/B runs.

| Line | Fix | What it says |
|---|---|---|
| `Performance: ...` | base | FPS and the summary headline. The 120-frame performance line adds counters such as `N watched again unchanged` (fix 36). |
| `Frame time: GPU timestamps on/off (...)` | 49 | Whether GPU busy time is measured, logged at start. |
| `Frame time ...` (detail) | 49 | The GPU's busy time, the GPU thread's time executing the ring vs idle, and fence waits by cause (memexport, resolve, query, frames in flight, pipelines, other). |
| `Frame time other waits (GPU thread) by the line of vulkan_command_processor.cc that waited: line N ...` | 58 | Uncaused GPU waits by source line. |
| late readbacks count | 59 | Fast readback no longer waits for the previous copy. How many copies came late. |
| `Frame time uploads (GPU thread): n/s, pieces/s ..., MB/s; barriers, upload buffers, marking valid, copying ms/s` | 62 | Where upload time goes. "Marking valid" is write-watch arming under the global lock. |
| `Frame time draws (GPU thread, own time ...)` | 73 | Draw cost by step: primitives, render targets, pipeline, dynamic state, bindings, vertex/memexport buffers, recording. |
| `Frame time watch arming (GPU thread) by view: A0000000 n calls/s, p protections/s, x ms ...` | 74 | Write-watch arming per physical view (A0000000, C0000000, E0000000, 7F000000) plus write notices. |
| packets / "rest of executing" by PM4 kind | 75 | Type 0 register writes, `SET_CONSTANT`, `DRAW_INDX`, `EVENT_WRITE*`, `WAIT_REG_MEM`, `XE_SWAP`, `INDIRECT_BUFFER`, ... (sampled since fix 77). |
| `Frame time WAIT_REG_MEM (GPU thread): ...` | 77 | How the GPU thread waits on registers or memory, by wait field. |
| `Frame time hot memory (64 KiB physical blocks by watch arming): <addr> n protections/s, m write notices/s (<kind>); ...` | 78 | The 5 blocks armed most, and by what (vertex, memexport, texture, other). |
| global lock spin summary | 55 | The GPU thread spins for the global critical region before sleeping: time taken and contended. |
| `Performance: global lock callers (GPU thread, 1 in 64 sampled and scaled): 0x... n/s taken, c/s contended, w ms/s waiting; ...; anchor 0x...` | 88 | Who takes the global lock. |
| `Performance events (GPU thread, every EVENT_WRITE* packet): ...` | 96 | EVENT_WRITE* by opcode and event type. |
| `Vulkan pipelines: a draw needed a pipeline still queued (...)` | 34 | Stutters from pipelines created on the GPU thread. |

**Placing a caller address** (fixes 79 and 88 print run-time addresses plus an anchor):
`address in the build = caller - anchor (run time) + anchor's address in eboot-symbols.txt`.
`eboot-symbols.txt` comes from the **same** CI artifact as the image, so keep artifacts of
images under investigation until you have done this.

## Other useful lines

| Line | Meaning |
|---|---|
| `BOOT version <label>` | Which build ran. |
| `BOOT title switch to <path> (<id>)`, `Title switch: <path> uses the options of <id>` | A game launched another executable (fix 33). |
| `Disc swap: mounted disc N of M` | Multi-disc games (fixes 51, 52, 89). |
| `Storage: ...` | Save devices found or refused (fix 27). |
| `Web settings: <key> set to <n> for <scope>` | A change made through `/api/set`. |
| `Readback keys: <file>: default ..., N keys listed, M lines not understood` | `readback-keys.txt` was loaded (fix 91 diagnostic). Then come the keys as first seen (address, length, format, endian, mode) and every 30 s the keys of the last 30 s with counts and modes. |
| `Readback: the key ... is resolved N times in a frame` | A key resolved several times per frame (fix 91). |

## Reading order for a report

1. Header and boot lines: label, title id, settings, start options.
2. The last 200 lines before the end, and any `CRASH`, `FAULT`, `STALL` or `HANG` block in full.
3. For performance: three consecutive 30 s summaries from the same scene, not the first one
   (the first includes loading).
4. Compare with the same lines from the A run. Write the comparison as a small table for the
   user.
