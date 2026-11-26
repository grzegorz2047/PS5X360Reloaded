# Memory export readback without the GPU thread's wait: design note

Status: **design only**. No code, no build. Fix numbers come from the coordinator when a build is
needed; the steps below are M0 to M3.

Sources:
- fixes 30, 48 and 133 (`tools/source-1.0.2/fixes/README.md`);
- the code of the assembled tree (1.0.6 line): `IssueDrawInner` in
  `xenia/gpu/vulkan/vulkan_command_processor.cc`, `xenia/gpu/memexport_merge.h`,
  `xbox360ps5/memexport_loss.hpp`, `xenia/gpu/vulkan/readback_batching.h`;
- fix 160's console session (2026-11-25);
- fix 157's design and results for resolve readbacks.

Numbers are marked **measured** (a console log, with its build) or **estimate**.

## 1. The problem

Three of the four GPU-thread-bound games lose more to readback waits than any binding work
could give back:

| Game | GPU thread waiting for memory export readbacks | Source |
|---|---|---|
| Battlefield 3 | 532-566 ms/s (executing 434-469) | measured, 1.0.6-work-fix160 |
| Modern Warfare 2 | 307-515 ms/s of GPU waits, memexport and resolve readback, with the option on from its compatibility config | measured, 1.0.6-work-fix160 |
| Modern Warfare 2 | 191-232 memexport readback waits a second (memexport=1) | measured, fix 133's line, 1.0.5 |
| Kinect Sports (boxing) | about 870 memexport readback waits a second | measured, fix 133's line |

## 2. What happens today

For every draw whose memory exports are read back, `IssueDrawInner` does the following. The
readback happens for three reasons: the option, fix 30's small exports of 64 KiB or less, or fix
48's learned exports.

1. **Before the draw:**
   - It copies the GPU's copy of the export ranges into the readback buffer: the "before".
   - For fix 30's readbacks only, it invalidates the ranges in the GPU's copy so they are uploaded
     again from guest memory (`RefreshRanges`).
2. **After the draw:**
   - It copies the ranges again: the "after".
   - **It waits for the whole queue** (`AwaitAllQueueOperationsCompletion`, cause `kMemexport`).
3. **The merge:** `ApplyExports` stores into guest memory every dword where "after" differs from
   "before". It does this even where the CPU changed the dword since the draw: that is only
   counted, as "differed".

So each such draw costs one full drain of the GPU. The GPU then sits idle until the GPU thread
feeds it again. That is the same pattern fix 157 removed for resolve readbacks.

Fix 133 already counts what a cheaper scheme would save. It records:
- **w**, the memexport readback waits;
- **p**, the periods between the points where the CPU can learn that the GPU went on:
  - the command processor's MEM_WRITE, EVENT_WRITE_SHD, COND_WRITE, REG_TO_MEM and scratch
    write-backs;
  - a new part of the ring;
  - a swap.

Its 30 s line reads: `Readback batching, last 30 s (fix 133, ...): ...; memexport readbacks waited
for w times in p periods; ...`.

## 3. Two designs

### D1: one wait per sync period (simpler)

- **Queue instead of wait.** An exporting draw that is read back records its before and after
  copies into its own slot of a ring of readback buffers, and queues the merge instead of
  waiting.
- **Wait once, at the next delivery point.** At that point the GPU thread waits once, for the
  newest queued copy, then merges all queued copies in draw order.
- **The delivery points:**
  - fix 133's sync points;
  - **WAIT_REG_MEM**, before the GPU thread starts waiting for the guest. Battlefield 3's GPU
    waits for a command buffer that a CPU worker records only after it sees the export (fix 30).
    Without this point the GPU thread would wait forever.
  - **a resolve readback, a fix 30 refresh or a fix 48 upload over a queued range**, before it
    touches that range (section 4);
  - **the end of a submission**, so the queue never outlives one;
  - **a swap.**
- **The saving.** Waits fall from w to at most p. Each remaining wait drains more work, but the
  GPU stays busy until it. That is exactly what fix 133 (b) was built to estimate.
- **What D1 does not cover.** A CPU thread that polls memory without any sync point, such as
  Battlefield 3's workers polling a job slot, sees the export only at the next delivery point.
  On the hardware that just looks like a slower GPU, which games must tolerate. But it can delay
  work the GPU waits for, which is the WAIT_REG_MEM point above.

### D2: the reading thread waits (fix 157's mechanism)

- **The mechanism.** The merge happens when the GPU has made the copy, and a CPU access to a
  queued range waits for it in the fault handler, as fix 157 does for resolve copies (read watch,
  board, timeout).
- **The gain.** It removes the remaining waits, not just most of them.
- **The cost:**
  - every export range has to be read-watched;
  - Battlefield 3's 384 KiB job arena, which the workers touch all the time, would fault at nearly
    every access while a copy is queued;
  - writes to a watched page go through the read callbacks too (fix 91i), so the CPU's own writes
    would wait as well.

  Only worth it if D1 leaves too much.

## 4. Correctness

- **CPU writes in between.** Today's merge stores the GPU's value even where the CPU changed the
  dword after the draw. The window today is one drain; D1 makes it a sync period.
  - On the hardware the export lands at the GPU's execution, and a later CPU write wins.
  - So the delayed merge must **keep the CPU's value where guest memory no longer equals
    "before"**: a compare and swap against "before", as the second half of fix 30's README row
    describes.
  - Fix 30's 31 candidates show this queue is delicate: a link restored by a stale export ran a
    job twice. M1 counts how often that case occurs.
- **The next draw's refresh.** Fix 30's `RefreshRanges` makes the GPU's copy of a range be
  uploaded again from guest memory. If an earlier export into that range is still queued, guest
  memory lacks it and the upload would lose it. So a refresh, or any upload fix 48 would count as
  a loss, delivers the overlapping queued merges first.
- **Fix 48's learned exports** declare 0.7-1.4 MB ranges (Black Ops). Queued copies of that size
  need a ring of buffers sized by bytes, not by count. A full ring is a delivery point.
- **Fix 157 and 142i** handle resolve copies; their board is separate. A resolve readback that
  waits anyway is a natural delivery point for D1.

## 5. Plan

### M0: no build, from existing logs and one live A/B (go/no-go)

From the fix 160 session's logs (Battlefield 3, Modern Warfare 2) and one new MW2 session, read
per 30 s gameplay window:
1. **Fix 133's line:** memexport readbacks waited w times in p periods. **w/p** is the factor D1
   would cut the waits by.
2. **Fix 49's GPU waits:** memexport ms/s, and ms per wait (ms/s ÷ w/s).
3. **Fix 48's `Memexport:` line:** read back small, learned and by the option, KiB/s, and the
   largest shader. This shows which rule causes the waits.
4. **MW2 only:** "Readback of data made by video" switched off and on in the guide (it applies
   live), 90 s each, two rounds, at the reference spot:
   - if MW2's waits are mostly the option's and nothing on screen changes, MW2 needs a setting,
     not code;
   - the 1.0.6_candidate_4 A/B already found no gain from the option at another spot.

**Go to M1** if in Battlefield 3, where the option is required:
- w/p ≥ 2 (D1 would cut at least half the waits);
- the memexport waits are ≥ 20% of the GPU thread.

**Stop** if w ≈ p: one wait per period is already today's case.

### M1: diagnostic build (number from the coordinator)

It changes nothing the game sees. Per exporting draw that is read back, it logs at the next
delivery point what D1 would have found:
- how many dwords of its ranges the CPU changed since the draw (would be kept by the compare and
  swap);
- how many overlapping refreshes or uploads came first;
- the queued bytes at the deepest point.

It also counts the CPU accesses to export ranges between the draw and the next delivery point.
That uses a short read watch, sampled 1 draw in N. It is the measurement D2 would need.

**Go to M2** if the dwords the compare and swap would keep are rare (under about 1 in 10,000
stored), and the queue stays under a few MB.

### M2: D1 behind a switch (off by default)

A verify mode merges both ways:
- the immediate wait as today;
- the queued merge, compared against it.

It logs mismatches without using the queued result. Then the real mode. Console A/B in
Battlefield 3 and MW2, live switch, the same spots.

### M3: D2

Only if M2's remaining waits still dominate and M1's access counts show the CPU rarely touches
queued ranges.

## 6. Effort and stop rules

These are estimates.

| Step | Effort |
|---|---|
| M0 | the console session and an hour of log reading |
| M1 | 2-3 days |
| M2 | about 1 week |
| M3 | 1-2 weeks more |

Stop after:
- **M0** if w ≈ p in Battlefield 3, or its memexport waits are under 20% of the GPU thread;
- **M1** if the CPU often rewrites export dwords within a sync period (the compare and swap would
  then change behaviour);
- **M2** if the console A/B is within the scene noise, or any difference shows on screen
  (Battlefield 3's jobs, Black Ops' skin and hair, Kinect Sports' boxing).

## 7. Open questions

- **GTA IV's growing binding cost.** In fix 160's GTA IV session, `vkUpdateDescriptorSets` grew
  from 28 to 38 ms/s at the same draw rate. One possibility is fix 104's texture set reuse rate
  falling as live image views accumulate, with none destroyed. It is not a readback question and
  needs no work now, but fix 104's reuse percentage is already in the 30 s summary. Reading it
  across that session would confirm or rule this out at no cost.
- **Kinect Sports' boxing** (about 870 memexport waits a second) was not measured on the 1.0.6
  line. M0 can include it if a session is easy.

## 8. Result: fix 177 (D1) parked for Battlefield 3

D1 was built as fix 177:
- branch `fix/memexport-deliver`, not merged;
- label `1.0.6-work-fix177`;
- build run 45720482616, host checks run 45719733624.

It was behind `memexport-deliver.txt` (`on`, `verify`, `off`; off by default). Its `verify` mode
kept today's immediate wait and merge, and counted at the next sync point the stored dwords that
guest memory no longer held.

### The console step: Battlefield 3, `verify`

- **Session:** a campaign level, about 3-4 minutes of play (2026-11-26).
- **The count:** 56-74% of the stored dwords had changed before the next sync point, against a
  pass mark of about 1 in 10,000. The steady windows read 67-74%, for example 40,444,489 of
  54,784,943 in 30 s.
- **The scene:** it ran at the game's 30 swaps/s cap (29.6-30.0, minimum 21).
  - Memexport readback waits: 97-123 ms/s, at 289-303 readbacks/s.
  - The fix 160 session had 532-566 ms/s, at another level and scene.
  - Readback source: 288.8 exporting draws/s, all read back by the option.

### Why that count says little: GPU-over-GPU overwrites

`verify` kept one record per dword stored by each draw, and compared them all with guest memory at
the next sync point. When a later draw in the same period exported into the same dword, the earlier
draw's record no longer matched, and it was counted as "changed by the CPU" although the GPU had
changed it.

Battlefield 3 has 6-8 read-back draws per period, all exporting into the same 384 KiB job area. If
each draw rewrites the same dwords, that alone gives up to (k-1)/k, 83-88%. So the count is an upper
bound on the CPU's share, not the share.

- **Not a cause:** the CPU clearing a slot before the GPU fills it. For a fix 30 readback, the
  "before" copy is taken after the ranges are reloaded from guest memory, so a CPU write made
  before the draw is already in it.
- **`on` itself is unaffected.** It merges the draws in order, each against its own "before".
- **A correct `verify`** keeps only the last stored value per address, and counts GPU-over-GPU
  overwrites separately.

### The deeper problem: forced deliveries

Even with a correct count, `on` would wait at nearly every Battlefield 3 draw, for two reasons.

- **The fix 30 reload.** Each draw read back by the option or a small export reloads its ranges
  from guest memory before the draw (`RefreshRanges`). A merge still queued for an overlapping
  range is not in guest memory yet, so the queue has to be emptied first, with a wait (fix 177's
  "refreshes" point). Every Battlefield 3 export goes into the same area, so almost every draw
  overlaps the one before it.
- **Uploads over a queued range.** The workers write the area all the time. A page they wrote is
  uploaded before the next draw that uses it, and an upload over a queued range has to empty the
  queue first (fix 177's "uploads over a queued range" point).

Fix 133's w/p counts the waits between sync points, not these forced deliveries. So M0's go
criterion (w/p of 2 or more) overstated what D1 could save in Battlefield 3. A future M0 for any
game has to count how often a read-back draw's ranges overlap a draw queued earlier in the same
period, and how often an upload lands on one.

The same applies to Modern Warfare 2: its small exports go through the fix 30 reload too.

### What is left

| Variant | Verdict |
|---|---|
| Batch only the draws whose ranges the CPU does not touch | Nothing qualifies in Battlefield 3: the CPU touches the whole area. |
| D2, only for the area | Does not help. The CPU's own writes to the area go through the read watch too (fix 91i), so the workers would wait for their own writes, and the reload and upload waits stay. |
| **GPU-side merge** | The only variant left. |

The GPU-side merge works like this:
- after a reload or an upload over a queued range, a small compute pass re-applies that draw's
  changed dwords (where before differs from after) to the GPU's copy;
- the queue then only has to be emptied where the CPU could learn that the GPU went on;
- estimate: about 1-2 weeks, on top of a corrected `verify`, which has to show first that the
  CPU rarely writes the dwords the GPU exports;
- only worth it in heavy scenes like fix 160's, not in scenes already at the 30 fps cap.

### Decisions (2026-11-26)

- D1 is parked for Battlefield 3, with no revision of fix 177 for now.
- `fix/memexport-deliver` stays unmerged.
- Modern Warfare 2's `verify` run is skipped. It will run only if D1 is taken up again: the count
  can only overstate, so a low result there would be a valid pass even with today's error.
- Battlefield 3's `Memexport:` line in that session showed no losses ("0 units of exported data
  lost to uploads (0 uploads), 0 units newly learned"). The build has no fix 30 "differed" count to
  read.
- Modern Warfare 2's preset keeps memexport=1. In its A/B (1.0.6-work-fix173c), fix 30's small
  exports and fix 48's learned ones read back every exporting draw with the option off as well.
  The waits were the same (146-153 ms/s), and there was no coloured noise or wrong textures in
  the menu, a mission start, gameplay with smoke, fire and explosions, or a cutscene.
