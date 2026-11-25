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
