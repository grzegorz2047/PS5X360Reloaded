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
