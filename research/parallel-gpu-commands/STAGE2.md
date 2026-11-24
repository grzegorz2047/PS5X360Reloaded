# Parallel GPU command processing, stage 2: the draws themselves

Status: **design, nothing built yet.** It follows stage 1 (fixes 142, 142b and 142c, `DESIGN.md`
sections 5 and 12-15).
- Stage 1 split the decoding of the command stream from its execution. It is correct and safe,
  but in Grand Theft Auto IV it is worth only about 6% of the GPU command thread.
- What is left is the translation of the draws, about 2.0 µs of the GPU thread's 2.78 µs a draw.
- This document asks which part of that work can leave the GPU thread, and at what cost.
- It proposes measuring before building (2.0), then two independent steps (2.1 and 2.2).

## 1. Where a draw's time goes

GTA IV, the same city spot, run M of `research/fix142c/` (no split, fix 134's worker on, 330,000
draws/s, 43-48 swaps/s). The figures are fix 73's own time by part, per draw. The sections nested
inside a draw (texture loads, uploads, readbacks) come on top.

| Part of `IssueDrawInner` | ms/s | µs a draw | What it is |
|---|---|---|---|
| primitives | 96.3 | 0.29 | VS/PS analysis, `PrimitiveProcessor::Process` (index conversion and its caches), shader modifications |
| render targets | 103.9 | 0.31 | `RenderTargetCache::Update`: the render pass and EDRAM ownership |
| pipeline | 104.6 | 0.32 | `ConfigurePipeline` (key, lookup, creation), `RequestTextures`, samplers (`UseSampler`), bind, layout, viewport info |
| dynamic state and constants | 34.7 | 0.11 | `UpdateDynamicState` (only what changed is recorded), `UpdateSystemConstantValues` |
| bindings | 179.6 | 0.54 | `UpdateBindings`: constant uploads, texture/sampler descriptor sets, binds |
| vertex and memexport buffers | 28.3 | 0.09 | `SharedMemory::RequestRange` per vertex fetch, memexport |
| recording | 15.8 | 0.05 | the draw command(s) into the deferred buffer |
| after | 4.1 | 0.01 | |
| **draws, own time** | **567** | **1.72** | |

Two rates from the same log say what "bindings" is made of:
- **Texture descriptor sets** (fix 104): 175,000 written a second, 51,000 reused (22%). So about
  half the draws write a set for a stage, with one `vkUpdateDescriptorSets` call per draw that
  needs any.
- **Constant descriptor sets** (fix 120): written for 0.3% of draws. The others are bound again
  with new dynamic offsets. Constant buffers are still uploaded at nearly every draw: the float
  constants change, and each upload is a `uniform_buffer_pool_->Request` plus a gather or
  `memcpy` from the register file.

## 2. What each part depends on

The question for every part is whether it can run after the GPU thread has moved on to the next
draw.

| Work | Reads | Writes / decides | Order-independent? |
|---|---|---|---|
| Primitive processing | registers, guest index memory, converted index caches | cache entries, the index buffer binding | **no**: caches and guest memory at this point in the stream |
| Render target update | registers, EDRAM ownership | ownership transfers (recorded), render pass begin/end | **no**: the EDRAM model is the most order-dependent state there is |
| Pipeline configure and bind | registers, shaders, pipeline map | pipeline creation, bound pipeline and layout | lookup and bind: **no** (bound state); creation is already asynchronous (fix 34) |
| Textures and samplers | fetch constants, texture cache, watches | loads (dispatches recorded), samplers created | **no**: watches and loads are ordered against the guest's writes and resolves |
| Dynamic state | viewport info, previous dynamic state | `CmdSet*` only when changed | the comparison: **no**; recording is cheap |
| System constants | registers, viewport info, primitive result, RT scale | `system_constants_`, dirty bits | computing: yes in principle, but its inputs are the sequential parts' results |
| Constant uploads | the register file (float, bool/loop, fetch), system constants | uniform pool allocation and data | allocation: **no** (bump allocator, per frame); the copy: only from a snapshot (below) |
| Texture descriptor writes | image views and samplers chosen above (handles) | a set from the per-frame free list, `vkUpdateDescriptorSets` | **yes**: plain handles, a set no command buffer uses yet |
| Descriptor binds | the sets, the layout, what is bound | `CmdVkBindDescriptorSets` when changed | the comparison: **no**; recording is cheap |
| Draw recording | everything above | the deferred stream | **no** for the order of commands |

What follows:
1. **Nearly everything a draw *decides* depends on what the previous draw left.** Bound state,
   caches, EDRAM, watches, pools. That is why splitting the frame into chunks translated in
   parallel (option A) needs speculation and two-phase caches (`DESIGN.md` 4A).
2. **What a draw only *produces* can wait,** as long as it is done before the submission that uses
   it is replayed and submitted. There are two candidates:
   - **The texture descriptor writes.** Their inputs are handles fixed at the draw. The set is
     fresh from `texture_transient_descriptor_sets_free_` or the pool, and nothing uses it yet.
     Vulkan allows `vkUpdateDescriptorSets` on different sets from different threads; only the
     pool needs external synchronization, and allocation stays on the GPU thread. The handles stay
     valid: a texture or sampler used by the current submission is not destroyed until a later
     completed submission (fix 104's epoch rules), and a job is done before its own submission is
     replayed.
   - **The constant data.** Its inputs are register values that the next packets overwrite. A
     worker could only copy from a snapshot. Taking the snapshot costs about what the copy does,
     unless the snapshot is free (section 4).
3. **Rule: a descriptor set must be fully written before any command buffer that binds it is
   recorded.** `vkCmdBindDescriptorSets` is recorded at replay, on fix 134's worker or the GPU
   thread. Updating a set bound in a command buffer in the recording state invalidates that
   command buffer unless update-after-bind is used. So every job of a submission must finish
   before that submission's replay. That is one join per submission (about 250 a second), not
   per draw.

## 3. Measurement first (2.0)

The table above does not say how "bindings" (0.54 µs a draw) splits between the constant uploads,
filling the image infos, fix 104's reuse check, allocation, `vkUpdateDescriptorSets` and the
binds. Without that split, no gain estimate is better than a guess.

Step 2.0 is a diagnostic build. It changes no behaviour; its timers sample like fix 73's.
- `UpdateBindings` by sub-part:
  - uniform requests and copies, by buffer (system, float VS, float PS, bool/loop, fetch, clip,
    tessellation), with bytes;
  - the image infos;
  - the reuse check;
  - set allocation;
  - `vkUpdateDescriptorSets`, with calls and descriptors;
  - binds.
- The same split for `UpdateDynamicState` and `UpdateSystemConstantValues`.
- How often a draw's inputs equal the previous draw's:
  - render target key and render pass;
  - pipeline (same handle, same layout);
  - primitive processing (same type, same index source, nothing converted);
  - textures (bindings unchanged since the last draw).

  These counters decide whether the sequential parts can be skipped on a single thread
  (section 6), which may be worth more than any worker.

Console: GTA IV, RDR and MW2, no split, one run each. Deciding numbers:
- **`vkUpdateDescriptorSets` time per draw.** It bounds step 2.1.
- **Bytes of float constants per draw.** They size step 2.2.
- **The "same as previous" rates.** They size section 6.

## 4. Design

### 4.1 Step 2.1: descriptor writes on a worker

On the GPU thread, `UpdateBindings` keeps every decision:
- fix 104's reuse check;
- allocating the set (the free list or the pool);
- recording `CmdVkBindDescriptorSets` with the set's handle.

Instead of calling `vkUpdateDescriptorSets`, it appends a job to a per-submission list. The job
holds the `VkWriteDescriptorSet`s, with their image infos copied into the job's own storage, since
`descriptor_write_image_info_` is reused at the next draw.
- **One or two workers** (the "GPU Bindings" threads, placed on the shared CPUs) take jobs from an
  SPSC queue in order. The order doesn't matter between sets, but it keeps the job storage simple.
- **The join.** `EndSubmission` waits for the jobs of the submission it ends before the replay:
  - before fix 134's hand-off;
  - before the GPU thread's own replay;
  - in front of fix 134c's query resolves, which are recorded into the same stream.

  If the workers keep up, the wait is about the last few jobs.
- **Waits for the GPU** (`AwaitSubmissionDrained`, readbacks) wait for submitted work, which is
  already joined. Nothing changes there.
- **Pools and recycling.** Sets go back to the free lists only when their frame has completed, as
  now. A job's set belongs to the current frame and is written before that frame is even
  submitted.
- **Fallback.** With the worker off, or the queue full, the GPU thread calls
  `vkUpdateDescriptorSets` itself, as now.
- **Gain.** At most the measured `vkUpdateDescriptorSets` time, minus about 50-100 ns a draw to
  build and queue the job. If 2.0 measures, say, 0.25 µs a draw, that is about 9% of the thread.

### 4.2 Step 2.2: constant uploads from a free snapshot

**The cost to remove.** A draw's float constants (up to 256 vec4 per stage, gathered by the
shader's constant map) are copied into the uniform pool. If 2.0 shows this is large (GTA IV writes
about 24 million float constant registers a second), a worker can do the gather. But only from data
that stays as it was at the draw.

**Where a free snapshot exists.** Stage 1's FIFO already holds every register value in order, and
nothing is overwritten until the GPU thread retires it.

**The design.** A *constant journal*:
- **On the GPU thread,** a copy-on-write page table over the float constant registers (512 vec4 in
  pages of 16 vec4 = 32 pages):
  - A register run that writes into a page shared with an older snapshot first copies the page.
    That costs 256 bytes, and only at the first write after a snapshot.
  - A draw's snapshot is the 32 page pointers plus a reference count: 256 bytes, no data copied.
- **On a worker:** a job is the destination (the uniform pool allocation, made on the GPU thread),
  the shader's constant map and the snapshot. The worker gathers into the destination, then
  releases the snapshot.
- **The join** is the same as in 2.1.
- **Bool/loop, fetch, system and clip constants** stay on the GPU thread: they are small, and
  their copies are plain `memcpy`s.

**Gain.** At most the float gathers' measured time, minus the page copies. Worth building only if
2.0 puts the gathers at 0.1 µs a draw or more.

### 4.3 What stays on the GPU thread

Everything that decides: primitives, render targets, pipeline, textures, samplers, dynamic state,
system constants, binds, recording, submissions. The deferred command stream stays exactly as
today, and only *when* two kinds of data are produced changes. So the host test can be exact: the
recorded stream must be byte-for-byte the same with and without workers, and the jobs' outputs must
equal what the GPU thread would have written.

## 5. Ordering, lifetimes, failure

- **Before a submission is replayed,** every job of that submission is done (the join in
  `EndSubmission`). That is the only ordering point.
- **The swap's submission** joins like any other.
- **Readbacks and queries** wait on submitted work, after the join.
- **A device loss or a failed allocation** behaves as today: allocation stays on the GPU thread.
  A failed job, which cannot happen since `vkUpdateDescriptorSets` returns nothing, would be
  undetectable, as it is today.
- **Shutdown and cache clears.** `ClearCaches` and texture destruction first join every pending
  job, as they already wait for the GPU.
- **Fix 134's worker** replays and submits as before; it receives submissions whose jobs are done.
- **Stage 1 independence.** Stage 1 is not required. 2.1 works with `gpu_parallel_record` off.
  2.2's journal sits behind the register writes and can be fed by the type 0 path as well as by
  stage 1's records.

## 6. The single-thread alternative the measurement may favour

Primitives, render targets and pipeline cost 0.92 µs a draw together. That is more than all the
order-independent work (at most about 0.65 µs: bindings, constants, recording). These parts can't
leave the GPU thread, but they can be skipped when their inputs are the same as for the previous
draw. Engines often issue runs of draws that differ only in constants and textures.

Stage 1 helps here. Its decoder sees every register write, so it can attach to each draw a mask of
the state groups that changed since the previous draw. The GPU thread could then skip
`RenderTargetCache::Update` and `ConfigurePipeline` and reuse the previous result. The groups are:
render target registers, pipeline-relevant registers, primitive and VGT registers, viewport.

The memoization has to be keyed by every input, not only registers: shaders, resolution scale, the
render target cache's own state (EDRAM transfers), device-lost and frame boundaries. It is the
riskiest kind of change in this file, and the most direct one. Step 2.0's "same as previous"
counters say whether it is worth it.

## 7. Expected gain, honestly

| Step | Bound (GTA IV, per draw of 2.78 µs in stage 1's mode 1) | Depends on |
|---|---|---|
| 2.1 descriptor writes | ≤ measured `vkUpdateDescriptorSets` time, probably 0.1-0.3 µs (4-11%) | 2.0 |
| 2.2 float constant gathers | ≤ measured gather time, probably 0.05-0.15 µs (2-5%) | 2.0; large change |
| 6 skipping unchanged RT/pipeline/primitive work | up to 0.92 µs × the share of draws with unchanged inputs | 2.0's counters; risky |
| Stage 1's remaining own cost | about 0.7 µs a draw (records, batches) | separate |

Stage 2 as parallel work is bounded by about 0.65 µs a draw. Realistically 2.1 and 2.2 together
give 6-15% of the GPU thread. Section 6 may give more, with more risk. Both need step 2.0's numbers
first.

## 8. Risks

- **The ordering rule.** A descriptor set updated after a command buffer binding it was recorded
  invalidates that command buffer (section 2, rule 3). The join must cover every path to a replay:
  `EndSubmission`, fix 134's hand-off, query resolves, the swap. A host test models every path.
- **Lifetimes.** Image views, samplers and sets referenced by a pending job must outlive it.
  Today's per-frame and completed-submission recycling gives that, if every destruction path joins
  first: cache clears, the resolution scale's trims (fix 117f), shutdown.
- **CPU placement.** One or two more busy threads on the shared CPUs. The guest's threads are
  there too, and fix 142c's `sibling 1` showed that sharing a core with the GPU thread costs about
  12% a draw.
- **Small jobs.** About 150,000 to 300,000 a second. Per-job overhead (copying the write
  structures, the queue) must stay well under the work it removes, which is why 2.0 measures
  first.
- **RADV on the PS5.** `vkUpdateDescriptorSets` from another thread is legal. Its cost, and any
  internal locking, is what 2.0 measures.

## 9. Tests

Host checks, as for stage 1 (`tools/check-*.cpp`, `run-fix-checks.sh`):
- **The job queue and the join.** Every job done before the submission's replay, in a model of
  `EndSubmission` with fix 134's hand-off, query resolves and the swap. Two threads, also under
  ThreadSanitizer.
- **Byte equality.** With a model of `UpdateBindings`, the recorded deferred stream is identical
  with and without the worker, and the descriptor contents written by jobs equal the
  single-threaded writes, for random draw sequences.
- **2.2's journal.** Snapshots see exactly the values at their draw under random register runs, and
  page copies happen only at the first write after a snapshot.
- **The off path.** The setting is off by default, and the patch only adds lines where it is off;
  fingerprints keep what the jobs mirror in step with the tree, as fix 142's check does.

## 10. Plan

1. **2.0 (a diagnostic fix, 142d):** the timers and counters of section 3. One console round:
   GTA IV, RDR, MW2.
2. **Decision with those numbers:** 2.1, 2.2 or section 6 first, or none.
3. **The chosen step,** behind its own setting (off by default), with the tests of section 9, then
   console runs against the same baseline.

## 11. Step 2.0 built: fix 142d

- **Base:** `fix/1.0.6-perf` at 77868b0 (`1.0.6_candidate_2`).
- **Branch:** `fix/parallel-gpu-record-106`.
- **Label:** `1.0.6-work-fix142d`.
- **Build run:** 45185681740, green, at `bf7c4b2` (artifact
  `eboot-1.0.2-from-source-NOT-FOR-RELEASE`). Host checks: run 45185681739, green.
- **Independent of stage 1:** fixes 142 to 142c are not in this branch; their material is on
  `fix/parallel-gpu-record`.
- **Diagnostic only:** the patch only adds lines.

It adds two lines to the 30 s summary, right after fix 73's `Frame time draws`:

```
Frame time bindings (GPU thread, fix 142d, 1 draw in 8 timed): N draws/s, C UpdateBindings/s, T us each;
  constants: system .. ms/s (u/s, b B each), clip .., tessellation .., float VS .., float PS .., bool/loop .., fetch ..;
  constant sets .. ms/s, texture image infos .. ms/s, texture set reuse check .. ms/s,
  set allocation and writes .. ms/s, vkUpdateDescriptorSets X ms/s in K calls/s (D descriptors each),
  binds .. ms/s, the rest .. ms/s; dynamic state .. ms/s, system constants .. ms/s
Frame time draws like the previous one (GPU thread, fix 142d):
  primitive processing P% (x ms/s when the same, y when not), render pass and framebuffer R% (..),
  pipeline Q% (..), shaders S%
```

What decides the next step:

| Reading | Decides |
|---|---|
| `vkUpdateDescriptorSets X ms/s` / draws/s | 2.1's bound (descriptor writes on a worker) |
| `float VS` and `float PS` ms/s and bytes each | 2.2's bound (float constants from a snapshot) |
| `when the same` ms/s of render pass, pipeline, primitive processing | section 6's bound (skipping unchanged work) |
| `the rest` | what the parts don't cover |

Also read, from the same 30 s block:
- fix 73's `Frame time draws` (the parts these split);
- fix 104's `texture sets`;
- fix 120's `constant sets`;
- fix 49's `GPU thread executing`, for the share.

## 12. Results of step 2.0 (fix 142d), and the next step

Console runs (build 45185681740 on `1.0.6_candidate_2`; GTA IV city, RDR town, MW2 mission; logs
and the coordinator's notes in `research/fix142d/`):

| | GTA IV | RDR | MW2 |
|---|---|---|---|
| draws/s | 299,640 | 179,741 | 324,022 |
| GPU thread executing | 929 ms/s | 992 ms/s | 910 ms/s (+ 84 ms/s memexport readback waits) |
| `vkUpdateDescriptorSets` | 25.3 ms/s, 144,162 calls/s | 20.4 ms/s | 20.6 ms/s |
| float constants (VS + PS) | 22.5 ms/s | 10.9 ms/s | 9.5 ms/s |
| render pass and framebuffer as the previous draw's | **99.4%**, 86-98 ms/s spent when the same | **99.1%**, 43.6 ms/s | **99.9%**, 74.5 ms/s |
| pipeline as the previous draw's | 63-80%, 11-12.5 ms/s when the same | 73.9%, 6.2 ms/s | 93.1%, 12.4 ms/s |
| primitive processing as the previous draw's | 22-33% | 35.6% | 39.9% |

**Reading.**
- **The worker candidates are small.** Descriptor writes (2.1) and float constants (2.2) together
  are about 3-5% of the thread, and need the per-submission join.
- **The largest single item is `RenderTargetCache::Update` running for draws whose render pass and
  framebuffer come out unchanged:** about 9-10% of the GPU thread in GTA IV and MW2, 4-5% in RDR.
  Section 6's skip comes first.
- **Unchanged pipelines** add only about 1% (`ConfigurePipeline` is cheap when it hits).

**What `Update` depends on.** This is why the skip is not "same render pass, skip `Update`":
- **Registers:**
  - the surface, color and depth infos, the mode;
  - the window offset and the scissors;
  - the viewport and `PA_CL_CLIP_CNTL`/`VTE_CNTL`/`SU_*` used by `DrawExtentEstimator`;
- **Parameters:** `is_rasterization_done`, the normalized depth control and color mask,
  `window_offset_tiles`;
- **The draw's estimated height** (`EstimateMaxY`). For clipped draws it comes from the viewport
  and scissor registers. For unclipped draws with no scissor it comes from running the vertex
  shader on the CPU, so from the draw's own vertices.
- **The render target cache's own state:**
  - the EDRAM ownership map, which `ChangeOwnership` changes over the rows the draw covers;
  - the render targets accumulated since the pass began;
  - the render targets' layouts (`TransitionRenderTarget`);
  - the resolution scale (fixes 117 to 117k).

A draw with the same render pass that reaches further down needs new ownership transfers. A
resolve between two draws changes ownership and layouts.

**Design of the skip (fix 142e).** A memo of the last successful `Update`. The next `Update`
returns the memo's result without running when all of these hold:
1. **A fingerprint matches:** the exact registers above, the parameters, the path, the resolution
   scale. Drawn from a list checked against the function's text by fingerprint, as fix 142's
   check does.
2. **The draw is clipped,** so its height comes from registers only. Unclipped draws always run
   `Update`.
3. **The render target cache's state version is unchanged.** It is a counter bumped by every path
   other than `Update` that changes ownership, render targets or their layouts: resolves, clears,
   `ClearCache`, trims (fix 117f), EDRAM snapshots, transfers, the interlock path. It is also
   bumped whenever `Update` itself changed anything.
4. **The previous `Update` changed nothing:** no transfers and no new accumulated render targets.

**The risk is a missed bump.** A single missed version bump renders wrong, so it comes with a
verify mode, the "speculate, then validate" idea:
- `render_target_skip 1`: `Update` still runs every time. When the memo would have skipped, the
  result is compared with it (render targets, transfers, render pass key, framebuffer), and
  mismatches are counted and the first few logged with the fingerprint.
- `2`: the skip itself.
- `0` (the default): today's path.

Console order: verify across GTA IV, RDR, MW2 and the titles with odd render target use (Tekken
TT2 at 2x, Kinect Sports' readbacks, BF3). Then skip, only where verify showed zero mismatches.
Expected gain: up to about 0.25 µs on most draws, about 8% of the GPU thread in GTA IV and MW2.

## 13. Fix 142e built: the render target update skip

- **Base:** `fix/parallel-gpu-record-106` on top of fix 142d (`1.0.6_candidate_2`).
- **Label:** `1.0.6-work-fix142e`.
- **Build run:** 45219977045, green, at `7586738` (artifact
  `eboot-1.0.2-from-source-NOT-FOR-RELEASE`). Host checks: run 45219967326, green.
- **Setting:** `render_target_skip` ([GPU] in `assets/config/<title>.config.toml`), or live in
  `render-target-skip.txt` in the title folder: one number, read again at most once a second, so
  the mode can change while the game runs. `0` (the default) is today's path, `1` verify, `2`
  skip.
- **The patch only adds lines.** With the mode at 0 `Update` runs as before. The state version
  is still counted; that is one increment where the state changes.
- **Host check:** `tools/check-render-target-skip.cpp`.

### What it does

`xenia/gpu/render_target_skip.h` holds the key, the outputs, the memo, the counters and the
line. `VulkanRenderTargetCache::Update` wraps them around the common `Update`. On the host render
target path, for a clipped draw, with the mode not 0:
- It builds the key: the 18 registers in `render_target_skip::kRegisters`, the four parameters,
  the resolution scale, the three settings `Update` reads, and fix 117g's frame.
- The memo **predicts** a skip when the last `Update` had the same key, changed nothing, and the
  state version has not moved since it returned.
- In **mode 2** a predicted draw returns at once.
- Otherwise `Update` runs. At its end (an `AtExit`, as `Update` has several returns) the
  outputs are compared with what they were before it:
  - the used and accumulated render targets and their validity;
  - the transfers;
  - the render pass key, the render pass, the framebuffer, its pitch and attachments.

  "Changed nothing" means the version did not move and the outputs are the same.
- In **mode 1**, a predicted draw whose `Update` then changed something is a **mismatch**,
  counted by kind:
  - `state`: the version moved;
  - `outputs`: what it leaves differs;
  - `failed`: it returned false.

  The first 16 are logged with what last bumped the version and the first output word that
  differs.

An unclipped draw, any other path, and a mode change all clear the memo.

### Who bumps the state version (`Fix142eTouch`)

**Where they change something:**
- `ChangeOwnership`, before it splits or claims a range;
- `GetOrCreateRenderTarget`, when it creates one;
- `TransitionRenderTarget`, when the layout, stage or access differ;
- `PerformTransfersAndResolveClears`, when it has a transfer or a clear;
- `ResetAccumulatedRenderTargets` and `KeepScaleNativeForPitch` (fix 117b).

**On entry:**
- `InitializeCommon`, `Initialize` and `Shutdown`;
- `DestroyAllRenderTargets`;
- both `ClearCache`;
- `BeginFrame`;
- the fix 117g trims;
- `Resolve` and `PrepareHostRenderTargetsResolveClear`;
- `PrepareFullEdram1280xRenderTargetForSnapshotRestoration`;
- `PixelShaderInterlockFullEdramBarrierPlaced`;
- `DumpRenderTargets`.

**The check keeps the list honest.** Every function of the two cache files that mentions the
state `Update` reads must either bump the version or be listed as only reading it, with the
reason. That state is the ownership map, the render targets, the accumulated and last-update
members, the layouts, the render pass and framebuffer caches, fix 117b's and 117g's members.

A new function from a Xenia update fails the check until someone classifies it. The check found
two problems while it was being written:
- the snapshot restoration function, which had been missed;
- an overflow in the key's word count.

### Reading the log

Once a second, while the mode is not 0:

```
Render target skip (fix 142e): mode 1 (verify), per second U updates, C clipped, N unclipped,
  P could skip (X%), S skipped, K changed; mismatches M (state a, outputs b, failed c), T in all
```

Also logged:
- each mode change: `Render target skip (fix 142e): mode .. (..), from ..`;
- in mode 1, up to 16 lines of `Render target skip (fix 142e): mismatch ..`.

In mode 2, fix 142d's `Frame time draws like the previous one` line shows what is left. Its
render pass `when the same` ms/s is the time `Update` still takes for unchanged passes; it should
drop by about the skipped share.

### Console plan

Five games, no Kinect Sports:
- Grand Theft Auto IV;
- Red Dead Redemption;
- Modern Warfare 2;
- Tekken Tag Tournament 2 at 2x;
- Battlefield 3.

1. **Verify.** Write `1` into `render-target-skip.txt` and play the usual scene for 2-3 minutes,
   including a menu, a loading screen and a cutscene where the game has them.
   - Read `T in all`. It must stay 0.
   - Read `X%`, the share that could be skipped. Compare it with fix 142d's 99%.
   - Look for any rendering difference. There should be none: mode 1 changes nothing.
2. **Skip.** Only in a game whose verify showed `T in all` 0, write `2` and play the same scene.
   - Compare the fps and fix 49's `GPU thread executing` with a mode 0 run of the same scene.
   - Compare fix 142d's `when the same` render pass time.
   - Look for render target corruption: missing or stale surfaces, wrong shadows or reflections,
     flicker after resolves.
3. **If a game shows mismatches,** send the `mismatch` lines. They name what bumped the version
   or which output word changed, and point to the missed case.

Expected gain in mode 2, from fix 142d: up to about 86-98 ms/s of GPU thread time in GTA IV,
43.6 in RDR and 74.5 in MW2. Draws the memo cannot predict lower that:
- the first two draws of each pass;
- unclipped draws;
- draws after a resolve.

### Verify results (mode 1)

Build 45219977045 (`1.0.6-work-fix142e`), default settings, `render-target-skip.txt` = 1:

| Game | Lines (s) | Updates/s avg (max) | Could skip avg (lowest second) | Mismatches |
|---|---|---|---|---|
| Grand Theft Auto IV | 825 | 255,379 (321,450) | 84% (30%) | 0 (state 0, outputs 0, failed 0) |
| Red Dead Redemption | 294 | 123,371 (262,235) | 79% (16%) | 0 |
| Modern Warfare 2 | 399 | 219,223 (332,357) | 90% | 0 |

No `mismatch` line in any log, and nothing wrong on screen. Tekken Tag Tournament 2 at 2x and
Battlefield 3 have not been verified yet.

The skip share is below fix 142d's 99.1-99.9% "same render pass" for three reasons:
- the memo needs one `Update` that changed nothing before it predicts;
- unclipped draws never skip;
- a resolve or transition between draws moves the version.

From fix 142d's `Update` time, the expected saving in mode 2 is about:
- 72-82 ms/s in GTA IV (84% of 86-98);
- 34 in RDR (79% of 43.6);
- 67 in MW2 (90% of 74.5).

### Skip results (mode 2)

Same build, `render-target-skip.txt` = 2:
- Grand Theft Auto IV: 276 s.
- Red Dead Redemption: 217 s.
- Modern Warfare 2: not run.

Correctness: 0 mismatches, no `mismatch` line, and nothing wrong on screen. Skipped draws, per
second (median of the busy seconds): 235,065 in GTA IV and 117,964 in RDR.

Each figure below is the median of the loaded 30 s windows. Each mode is a separate run (the
142d build, then 142e in modes 1 and 2), in different scenes.

| | 142d, no 142e | 142e mode 1 | 142e mode 2 |
|---|---|---|---|
| GTA IV render targets part (fix 73), ms/s | 100.7 at 299,640 draws/s | 144.2 at 270,104 | 54.7 at 285,527 |
| GTA IV per draw, us | 0.336 | 0.534 | 0.192 |
| GTA IV 142d render pass "when the same", ms/s | 90.1 | 133.0 | 43.5 |
| GTA IV GPU thread executing, ms/s | 961 | 958 | 894.5 |
| GTA IV swaps/s | 51.2 | 47.0 | 51.8 |
| RDR render targets part, ms/s | 49.0 at 168,055 | 62.9 at 127,220 | 32.4 at 165,431 |
| RDR per draw, us | 0.292 | 0.494 | 0.196 |
| RDR 142d "when the same", ms/s | 40.1 | 50.0 | 24.1 |
| RDR GPU thread executing, ms/s | ~993 | ~993 | ~993 |

Reading:
- **Saving.** Mode 2 cuts the render target part per draw by 43% in GTA IV (0.336 to 0.192 us,
  about 46 ms/s) and by 33% in RDR (0.292 to 0.196 us, about 17 ms/s).
- **Below the 72-82 ms/s expected.** What is left is mostly the draws that do not skip: 16-21%
  of them, whose `Update` does real work. The skip path itself (the key and the memo) is small
  next to that.
- **Mode 1 costs about 0.2 us a draw** (two output snapshots and a comparison) and about 8% of
  the frame rate in GTA IV. It is a check, not a mode to play in.
- **The frame rate gain is not shown yet.** The runs are different scenes. RDR is at its 30 fps
  cap with the GPU thread near 993 ms/s in all three modes. A same-spot comparison is next:
  switch the file between 0 and 2 every 30 s window while standing still. The file is read live.

### What the skip misses (mode 1 logs), and whether a fix 142f pays

These are medians over the busy seconds (more than 1000 updates/s). "Not skippable" means the
updates minus the ones that could skip.

| Game | Updates/s | Not skippable/s | Unclipped/s | Changed/s | Rest/s |
|---|---|---|---|---|---|
| GTA IV | 275,640 | 38,232 | 22,460 | 4,879 | ~10,900 |
| RDR | 129,793 | 22,076 | 13,950 | 2,490 | ~5,600 |
| MW2 | 230,942 | 9,752 | 2,752 | 807 | ~6,200 |

The columns:
- **Changed** is real work: transfers, transitions and new render targets. It cannot be skipped.
- **Unclipped** draws never skip in 142e, because `EstimateMaxY` may run their vertices on the
  CPU. It only does that when the scissor is at least 8192 in both directions, or when
  `execute_unclipped_draw_vs_on_cpu_with_scissor` is set. Whether it does is decided by
  registers and settings that can go in the key. So a memo entry can record whether its
  estimate used the vertices, and an unclipped draw whose key matched an entry that did not
  can skip. That needs one added flag in `DrawExtentEstimator`.
- **Rest** are updates that changed nothing but were not predicted, which happens after a change
  or with a different key. Most are likely draws alternating between a few keys in one pass. The
  state version makes a memo of several keys just as safe. Every entry recorded at the current
  version changed nothing, so all of them leave the same outputs. Four entries would catch
  alternation.

**Bound for 142f.** It covers the unclipped and rest columns, with the real work as the limit:
- about 33,000 more skips/s in GTA IV, about 19,600 in RDR, about 9,000 in MW2;
- at roughly 0.3 us each, that is about 10, 6 and 3 ms/s: about 1% of the GPU thread.

That is a fifth of what 142e already saves. Whether it is worth building depends on the
same-spot GTA IV test: if 142e's 46 ms/s does not show in the frame rate, 10 more will not
either.

### Same-spot A/B in GTA IV (mode 0 and 2, switched live)

The player stood still and the file was switched over FTP every 35 s. Swaps/s come from the hang
watcher's swap counter, with the first 3 s after each switch left out.

| Block | Mode | Swaps/s |
|---|---|---|
| 19:03:30-19:04:02 | 0 | 45.70 |
| 19:04:05-19:04:38 | 2 | 43.97 |
| 19:04:41-19:05:13 | 0 | 46.25 |
| 19:05:16-19:05:36 | 2 | 50.42 (19 s) |

- **Frame rate.** Mode 0 averages 46.0 and mode 2 averages 46.6, within the noise.
- **GPU thread executing.** 992-994 ms/s in every block.
- **Render targets per draw.** 0.269 us in a mode 0 block, 0.215 and 0.177 in blocks that were
  mostly mode 2.

Mode 2 is correct and cheaper, but at this spot it gives no measurable frame rate.

The GPU thread's "executing" stays at 992-994 ms/s while its work per draw drops. So the time
saved most likely goes into waiting for the guest. On this branch, fix 73's "executing" still
counts the WAIT_REG_MEM sleeps as busy time: fix 138, which separates them, is on
`fix/frame-cost-guest-wait` and not in 1.0.6. A GPU thread that "executes" 993 ms/s while
sleeping on the guest is not the bottleneck at this spot. Before more work comes off it, fix
138's split (or a port of it) has to show how much of "executing" is real work.

### 142e on all five games, and MW2's same-spot A/B

Verify mode (switch 1) showed 0 mismatches in all five games:

| Game | Seconds | Updates/s | Could skip |
|---|---|---|---|
| MW2 | 92 | 315,350 | 97% |
| Tekken TT2 at 2x ("2x, scaled resolve sparse") | 554 | 28,489 | 88% |
| BF3 | 374 | 70,541 | 53% |

GTA IV and RDR were verified before. MW2 in mode 2 skipped 343,828 draws a second with 0 mismatches.

MW2 same-spot A/B, standing still, switch flipped every 35 s, swaps/s from the hang watcher:

| Block | Mode | Swaps/s |
|---|---|---|
| 1 | 0 | 34.02 |
| 2 | 2 | 29.77 |
| 3 | 0 | 31.17 |
| 4 | 2 | 30.71 |

- **Frame rate.** Mode 0 averages 32.6 and mode 2 averages 30.2: no gain.
- **Render targets per draw.** 0.348 and 0.267 us in the mode 0 blocks, against 0.220 and
  0.145 us in the mode 2 blocks.
- **GPU thread executing.** 869-921 ms/s in every block.

At this spot MW2 is not bound by the GPU thread either. 142e is correct but gives no frame rate in
GTA IV or MW2, so the default stays 0.

## 14. Fix 142f-diag: one diagnostic image for what limits the frame rate

The coordinator merged the 142f idea of the review with the useful parts of another agent's 12
optimisation cards. Each card was checked against the logs; none is taken as written, and the
cards' helper headers are not ported.

The name is 142f-diag because the earlier 142f plan (widening 142e) is set aside.

- **Base:** `fix/1.0.6-perf` at 20f9b7d (`1.0.6_candidate_3`).
- **Fixes on top:** 142d, 142e (still 0 by default) and fix 138.
- **Patch:** `142f-diag-gpu-thread.patch`.
- **Branch:** `fix/gpu-thread-probe-106`.
- **Label:** `1.0.6-work-fix142f-diag`.
- **Build run:** 45264673575, green, at `bdce6de` (artifact
  `eboot-1.0.2-from-source-NOT-FOR-RELEASE`). Host checks: run 45264673127, green.
- **Nothing the emulator does changes.**

**One switch.** `gpu-diag-detail.txt` in the title folder turns on fix 142d's probes and the new
ones, so the probes' own cost can be measured by switching it at a fixed spot:
- `1` or `on` turns it on; `0`, `off` or no file turns it off;
- it is read at most once a second at the end of a submission;
- each change is logged as `GPU diagnostics detail (fix 142f-diag): on|off`.

Fixes 49 to 103's always-on counters stay on. They are estimated at 10-25 ms/s in GTA IV.

What it gives, item by item from the review:

| Item | Where in the log |
|---|---|
| (a) Fix 138: WAIT_REG_MEM sleeps out of "executing" | `Frame time:` line, `waiting for the guest (WAIT_REG_MEM) x ms/s in n sleeps/s` |
| (a) The first 30 s block | Every reporter's first line takes the ticks' length from the program's start instead of 1 ns. RDR's first WAIT_REG_MEM block had shown 1,321.6 ms/s of sleeping. |
| (b) The rest of executing without waits | `Frame time GPU thread without waits (fix 142f-diag)`. Executing with every wait in it, the sections, fix 49's GPU waits and the WAIT_REG_MEM sleeps are each shown apart; the rest has all of them taken out. The detail line's old rest still holds the waits. |
| (b) RDR's WAIT_REG_MEM | `Frame time packets counted` gives WAIT_REG_MEM's own time and, separately, the sleeping inside it. Fix 77's line gives its polling. |
| (c) The timer's cost; INDIRECT_BUFFER | `Frame time packet timing`: the timer's cost inside and outside a timed packet, and INDIRECT_BUFFER's own time per nested packet. GTA IV's IB time is a phantom if that is about the outside cost. |
| (c) Count scaling | `Frame time packets counted`: each kind counted one by one, next to fix 75's scaled count (EVENT_WRITE: 263-336k/s scaled against fix 96's 93-111k/s), with its own time from the timed packets |
| (d) Sub-marks | `Frame time packet timing`, draws by sub-part. Primitives: Process, shader modifications, translations, samplers, window offset. Pipeline: ConfigurePipeline, pipeline readiness (publication and placeholder checks), RequestTextures, pipeline bind (with its layout), viewport (its arguments and GetHostViewportInfo), BindVIZPredicate and UpdateQuerySegment. |
| (d) Inside Process | `Frame time draw detail`: Process by index buffer type, with the time before the shared memory request (the normalisation and CacheTransaction) and RequestRange |
| (e) Textures | `Frame time draw detail`, texture sets per stage: the previous draw's, or among the last 32 of the frame. Keys only (layout, binding epoch, views, samplers), compared in full, nothing reused. |
| (f) Type 0 | `Frame time packet timing` by register group; `Frame time draw detail` at the hot registers 2102, 48BA, 4800 and 5000 |
| (f) System constants | Fix 142d's line splits them into the compute (`system constants`) and the upload (`system` in the constants). `Frame time draw detail` adds the updates and the changed ones per second. |
| (f) Constant writes | `Frame time draw detail`, float constant writes per stage: identical, unused by the current shaders, used |
| (g) Descriptor indexing | The Vulkan device dump at start: `Vulkan descriptor indexing (fix 142f-diag, supported): ...`, the four features and the update-after-bind limits |
| (h) Resolve waits | `Frame time readback waits`. Resolve waits by reason: full mode, first copy, own copy (fixes 91k, 133), downscale growth. The fast copies that did not wait: handed over, a steady family's late copy (fix 133), carried over (fix 59). The resolves' own time; memexport by reason: option, small (fix 30), lost before (fix 48). |
| (h) Faults | `Frame time faults`: MMIO, a write to a watched page, a read of a read-watched page, a watch already lifted; and the guest code faulting most, 1 in 8 resolved through the JIT's code map |

A correction for the record: textures (fix 49's `textures` section) and the samplers and
translations (before `Mark(kDrawPrimitive)`) are not inside fix 73's "pipeline" part. The sub-parts
above follow that.

**Acceptance (card OPT-03).** Each run notes:
- the build and the scene;
- the exact 30 s window (the summary's time stamp);
- the waits (GPU, pipelines, WAIT_REG_MEM sleeps), the sections, and the rest without waits;
- no double counting: executing with every wait in it equals the sections plus the WAIT_REG_MEM
  sleeps plus the rest, within the rounding;
- RDR's sleeping shown apart from WAIT_REG_MEM's own time.

The coordinator runs GTA IV, MW2, RDR and Sims 3 live mode at fixed spots, with the switch off and
on.

**Decisions waiting on these numbers:**

| Option | Taken only if |
|---|---|
| A texture set cache prototype | More than 30% of the writes repeat within a frame |
| A system constants memo | The compute stays at 20 ms/s or more in MW2 |
| A pipeline memo | ConfigurePipeline on a repeat is a real share |
| Bindless | The descriptor work stays large |

**Rejected:**
- an OPT-04C RAM cache;
- the OPT-05B JIT audit (it exists, issue #5);
- the OPT-05C driver pin bump (the GPU is only 17-38% busy).

## 15. Results of fix 142f-diag, and fix 142g

### Fix 142f-diag on the console

Build `1.0.6-work-fix142f-diag`, fixed spots, the switch flipped between 0 and 1 every 35 s.

**What limits the frame rate:**

| Game | FPS | What limits it |
|---|---|---|
| GTA IV | 42.9 | The GPU thread's own work: 994 ms/s with no waits. Draws 571 ms/s (bindings 173), the rest 232. |
| MW2 | 30.8 | The GPU thread is saturated at 1,001 ms/s: about 916 of own work plus 38-88 ms/s of memexport readback waits, which "the option" causes. The waits track the frame rate. |
| RDR | 30 (the game's cap) | The GPU thread takes 28.1 ms a frame, about 5 ms of headroom. |
| Sims 3 | 19.6 (3-vblank lock) | About 31 ms of work plus an 8.5 ms resolve wait ("first copy of a key") per frame: 166 ms/s in 236 waits/s. |

**The probes' own cost with the switch on:** GTA IV -16%, MW2 -13%, RDR -20%, Sims 3 0 fps.

**Decisions:**

| Option | Decision | Why |
|---|---|---|
| Texture set cache | No | Writes repeating among the last 32: MW2 8%, GTA IV 23.8%, Sims 3 29.5%, RDR 49.8% (capped) |
| System constants memo | Go, small | MW2 compute 22-23 ms/s, 95.8% of the updates unchanged |
| Pipeline memo | No | ConfigurePipeline is 14-15 ms/s, about 1.5% |
| Bindless | Measure more | Descriptor work 7-11%. The indexing line was missing (a bug, fixed in 142g). |
| Sampler and viewport memo | Go | MW2 45.9 + 25.9 ms/s (about 2.3 ms a frame), GTA IV 21.5 + 24.0 |
| MW2's memexport readback option | A/B it off | No build needed |
| Sims 3 | A/B fix 125 off (3.4 ms a frame) | Also: why fix 91k stops giving the 3.6 MiB keys their own copies in steady live mode |

**Diagnostic bugs found:**
- The per-block on/off line showed the state at the report's time, not over the block.
- The descriptor indexing line was info-level, which the game log leaves out.
- The watch faults were counted only in the non-Linux branch, which the PS5 build does not take.
- The texture set history ran inside fix 142d's reuse check span.
- The switching side's timestamps ran about 23.5 s ahead of the console's clock.

### Fix 142g

- **Base:** `1.0.6_candidate_3` with 142d, 142e, 138 and 142f-diag.
- **Patch:** `142g-draw-memo.patch`.
- **Label:** `1.0.6-work-fix142g`.
- **Build run:** 45289009881, green, at `5549609` (artifact
  `eboot-1.0.2-from-source-NOT-FOR-RELEASE`). Host checks: run 45285846713, green.
- **The first build:** run 45282574470, at `5c1e7a1`, stopped in its host check step.
  `check-submit-parts` read only the first 14,000 characters of `EndSubmission`; it now reads the
  whole function.

**The diagnostic fixes:**
- The switch's line now says `on for X% of this block (now on|off; ...)`.
- The descriptor indexing line is a warning.
- Watch faults are counted in the branch the PS5 build takes. A watch already lifted is not told
  apart there.
- The texture set history runs before fix 142d's reuse check span, with its own pass over the
  stages.
- Each change of the switch is logged with the console's clock and the switch file's write time
  by that clock, in unix seconds.

**Three repeat memos** (`xenia/gpu/vulkan/draw_memo.h`). Each builds a key of everything its
computation reads; a repeat of the last key gives the last result.

| Memo | The key holds | Notes |
|---|---|---|
| System constants (`UpdateSystemConstantValues`) | The fields of the parameters it uses; its registers (the four render targets' color info and blend control, the six clip planes); the render target cache's answers; the texture cache's answers for each used texture; the query state | A forced zpd counter update never repeats. check-draw-memo compares the key with the function's text: a register, a field, a cache call or a member read and not keyed fails the check. |
| Samplers, per stage | The shader; for each sampler binding, its fetch constant's six words and its filters; `anisotropic_override`; the submission; the texture cache's binding epoch | A sampler used in a submission is not destroyed in it, and using it again there changes nothing. A stage computed again is no memo until it completes; an overflow commits nothing. |
| Viewport (`GetHostViewportInfo`) | `GetViewportInfoArgs`' 64-byte union (what its `operator==` compares) and `half_pixel_offset` | — |

**The modes.** Each memo has its own cvar, 0 by default: `draw_memo_system`,
`draw_memo_samplers` and `draw_memo_viewport`. Or `draw-memo.txt` in the title folder, read at
most once a second:
- one number sets all three;
- `system N`, `samplers N` and `viewport N` lines set one each.

The values:
- **0 off**;
- **1 verify:** computed every time; a repeat that comes out different is a mismatch, counted, and
  the first 8 are logged;
- **2 skip.**

The log gives:
- each change: `Draw memo (fix 142g): system m, samplers m, viewport m (0 off, 1 verify, 2 skip), from ...`;
- every 30 s while one is on: `Draw memo (fix 142g): system mode m: n/s, repeats p%, skipped s/s,
  mismatches x (t in all); samplers ...; viewport ...`.

**Console protocol, MW2 and GTA IV, at the same fixed spots:**
1. `draw-memo.txt` = `1` (verify all) for a few minutes, including a menu and a loading screen.
   `t in all` must stay 0 for all three memos.
2. Then switch `draw-memo.txt` between `0` and `2` every 35 s, with `gpu-diag-detail.txt` at 0
   (the probes cost 13-16%). Compare the swaps per second, `GPU thread executing` and fix 73's
   draws parts.

With the detail switch on in a separate pass, the 142f-diag lines show:
- the samplers sub-part, and the viewport;
- the draw detail's system constants;
- fix 142d's `system constants`.

### Fix 142g on the console, and why its skip mode is slower

MW2 (41560817), `gpu-diag-detail.txt` 0.

**Verify (`draw-memo.txt` 1), about 3 minutes including a menu, a load and the mission:**
- 0 mismatches for all three memos over 5 full windows;
- repeats: system 35-46%, samplers 85-88%, viewport 96-98%;
- what it costs: draw own time 2.7 us against 1.5 us on candidate 3, and 27-29 fps against 38-40.

**Same-spot A/B, 2 against 0 every 35 s, 8 windows:**
- mode 2 averages 26.0 fps and mode 0 averages 34.7 fps: skip is about 25% slower, in all 4 pairs;
- the skips do happen: system 104k of 230k/s, samplers 307k of 349k/s, viewport 226k of 230k/s;
- 0 mismatches, nothing wrong on screen.

**The cause: building and comparing the keys costs more than the work they replace.** The three
keys append about 200 words a draw, one word at a time, into vectors, then compare and swap them.

| | Per draw |
|---|---|
| The work the memos replace, if all of it were skipped (fix 142f-diag's MW2 figures at about 300k draws/s: samplers 45.9, viewport 25.9, system 22.5 ms/s) | 0.15 + 0.09 + 0.08 = 0.31 us |
| What they saved at the measured repeat rates | About 0.25 us |
| The three keys alone, measured on the host (`draw_memo.h`'s own Key and Slot, a 2.1 GHz Xeon) | 0.29 us at -O3, 0.93 us at -O2 |
| Net cost in mode 2 on the console (frame time 28.8 to 38.5 ms over about 11.7k draws a frame) | +0.83 us; gross, about 1.1 us |
| Verify's extra cost (the keys plus the comparison) | About 1.2 us |

The two console figures agree once the savings are added back. Nothing points to a lost fast path
or a descriptor or dirty-state side effect: the skips counted are real, there are 0 mismatches, and
the size of the slowdown matches the keys' cost.

**Decision.** Key-based memos do not pay for computations of 0.1-0.15 us. The memos stay at 0.

A memo would pay only with a check of a few nanoseconds: a dirty flag set where the inputs are
written. That means:
- a per-register flag table on the register write path, in `WriteRegister` and `register_run.h`;
- `TextureFetchConstantsWritten` for the fetch constants;
- the few other inputs compared as scalars (the shader, the submission, the binding epoch).

The bound is about 0.25 us a draw, about 70 ms/s in MW2. The write path's cost comes off it: about
1.4 million register writes a second at 1-2 ns each.

The viewport (97% repeats) and the samplers (88%) read only registers and a few scalars. The system
constants (40% repeats, about 9 ms/s at best) also depend on the texture cache's per-texture state
and are not worth it.

**Per-memo A/B** (same build, spot and protocol; one memo at 2, the others at 0, against all at 0,
two rounds):

| Mode | fps | Against all off |
|---|---|---|
| System at 2 | 25.2, 27.2 (26.2) | -22% |
| Samplers at 2 | 32.2, 32.3 (32.3) | -4% |
| Viewport at 2 | 32.6, 32.7 (32.7) | -3% |
| All off | 32.7-33.9 (33.7) | — |

0 mismatches throughout.

**Reading:**
- **Samplers and viewport.** Their losses match their keys' cost, about 0.1 us a draw each. The
  host measurement puts all three keys at 0.29 us at -O3.
- **System constants.** Its loss, about 0.77 us a draw (frame time 29.7 to 38.2 ms over about 11k
  draws a frame), is about five times what its key can cost: about 0.1-0.15 us, the largest of the
  three but still under half of them. So the key is not the explanation. Nothing else changes
  either:
  - the skipped function writes nothing besides the system constants and
    `zpd_counter_index_force_update_`, which a repeat never has set;
  - verify found every predicted repeat unchanged.
- **The open question.** Whether the game itself does more per frame in that mode: more draws a
  frame, or other query results. The summary's 30 s blocks straddled the 35 s switches, so they
  cannot tell. A system-only run with 90 s windows (two whole blocks each) would.

**System memo alone, 90 s windows** (whole 30 s blocks of one mode):

| | System at 2 | Off |
|---|---|---|
| fps | 24.5, 26.5 | 30.3, 33.5 |
| Draws/s | 268-296k | 323-368k |
| Draws per frame | 11.0-11.2k | 11.0k |
| Own time per draw | 2.3 us | 1.6 us |

Draws per frame do not change, and the own time per draw rises by 0.7 us. So the game does the
same work; the memo's own code in `UpdateSystemConstantValues` costs about 0.7 us a draw on the
console. That is about five times what its key (about 110 words appended one at a time into a
`std::vector`, then compared) costs on the host at -O3. The core also builds at -O3, so this
investigation does not explain the gap.

**What any version of it could save.** About 45% repeats of a 0.075 us computation: at most 0.03 us
a draw, even with a free key.

**Decision: the 142g memos are closed.**
- They stay at 0, and are not for a candidate.
- The system memo cannot pay at any key cost.
- For the samplers and the viewport, a fixed-size key (one memcpy and one memcmp) would be cheaper
  than the vector key. But their ceiling is about 0.2 us a draw, and the console has just shown that
  host estimates of such code can be five times off. No work goes into them without a cost
  measured on the console first.

**The larger bounds left:**
- MW2's memexport readback waits ("the option", 38-88 ms/s, tracking the frame rate): an A/B with
  the option off.
- Sims 3's resolve "first copy of a key" waits (166 ms/s): fix 125 off A/B, and fix 91k's own copies
  in steady live mode.

### MW2's memexport readback option, A/B on 1.0.6_candidate_4 (no build)

"Readback of data made by video" was toggled in the guide On, Off, On, Off at the same spot. The 30
s blocks that hold a guide visit are left out.

| Block | Swaps/s | GPU thread executing | Waiting for the GPU |
|---|---|---|---|
| On (start) | 36.3 | 849 ms/s | 133 ms/s (memexport readback 84 in 158/s) |
| On (start) | 30.6 | 853 ms/s | 149 ms/s (83 in 93/s) |
| Off (1st) | 33.4 | 928 ms/s | 73 ms/s (73 in 41/s) |
| On (2nd) | 36.7 | 1,001 ms/s | 0 |
| Off (2nd) | 36.6 | 1,001 ms/s | 0 |

Draws ran at 340-395k/s, 1.4 us each, throughout. Nothing changed on screen.

Reading:
- **The waits faded in both modes.** Once they were gone, On and Off give the same 36.6-36.7
  swaps/s with the GPU thread saturated. The waits belong to a phase of the game after loading, not
  to the option. Fix 133's steady families took the late copy in the early blocks (788, 681 and 845
  resolves) and 0 later.
- **Why "Off" still waited.** The option decides only one of memexport_loss's reasons. A small
  export (fix 30, `readback_memexport_small`) and a shader whose exported data was lost before
  (fix 48) are read back with the option off too. Fix 142f-diag's readback line names these
  reasons; candidate 4 does not have it.
- **Decision.** No gain from the option at this spot; it stays as it is. In steady play MW2 is
  bound by the GPU thread's own work: about 1,000 ms/s with no waits, 1.4 us a draw at about 370k
  draws/s.
