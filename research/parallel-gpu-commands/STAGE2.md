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
