# Parallel GPU command processing (fix 142)

Status: **prototype, off by default** (`gpu_parallel_record = 0`). It is not tied to a release.
Branch `fix/parallel-gpu-record`, based on `fix/1.0.5-perf` (`1.0.5_candidate_90`: fixes 130, 133,
134/134b/134c and 139 included). The fix number is 142: 136 to 141 are taken on other branches
(`fix/memexport-batching*`, `fix/color-overscan`, `fix/frame-cost-guest-wait`, `fix/gfx-900p`).
Patch: `tools/source-1.0.2/fixes/142-parallel-record.patch`. Check: `tools/check-parallel-record.cpp`.

## 1. The problem

The emulated Xenos consumes one ordered, stateful PM4 command ring. Xenia translates it on one
host thread, "GPU Commands" (`CommandProcessor::WorkerThreadMain`, compiled per backend through
`pm4_command_processor_implement.h`). On the PS5 (Zen 2, about 3.5 GHz), that thread is the limit
in the heavy titles: Grand Theft Auto IV, Red Dead Redemption and Modern Warfare 2 keep it 90-99%
busy while the PS5's GPU is 25-33% busy. Fix 134 moved the replay of the deferred command buffer
into Vulkan, and the queue submit, to a worker (`gpu_async_submit` 1: GTA IV 43 to 45.5 FPS). The
parsing, the register writes and the draws' translation are still on one thread.

## 2. Where the time goes

Everything below was measured on the console by earlier diagnostic fixes. The figures are from
the README rows of those fixes; none were re-measured for this document.

| Part of the GPU thread | Measured | Source |
|---|---|---|
| Draws, own time | MW2 448 ms/s at 194k draws/s (about 2.3 µs a draw), Black Ops 329, BF3 292, TTT2 227 (c31) | fix 73 |
| Bindings, largest part of a draw | GTA IV 177 ms/s at 196k draws/s before fix 104; 191-203 ms/s (GTA IV), 150-163 (RDR) before fix 120 | fixes 73, 104, 120 |
| Textures (`RequestTextures`) | GTA IV 79 ms/s at 194k draws/s, before fix 105 | fixes 49, 105 |
| "Rest of executing": the packets themselves, outside draws and sections | BF3 468 ms/s, TTT2 316, MW2 150 (c31, before fix 103) | fix 75 |
| Type 0 register packets | GTA IV 1.36 million/s (about 7 a draw), 132 ms/s at 97 ns each (MW2 55 ns), before fix 103 | fix 103 |
| Submissions | RDR about 100 ms/s, MW2 170, The Sims 3 140 | fix 130 |
| Replay of the deferred stream | GTA IV 145-147 ms/s, RDR 92-140 (moved to a worker by fix 134) | fixes 130, 134 |

So a heavy frame on the GPU thread falls into three groups:

1. **The packet layer**: reading the ring and the indirect buffers, decoding packet headers,
   predication, and writing registers into the register file, with their dirty-tracking side
   effects. This was 150-470 ms/s before fix 103; it is less now, but it is still large in GTA IV,
   whose engine rewrites most fetch and float constants for every draw, mostly with unchanged
   values (fix 105).
2. **Draw translation**: `IssueDrawInner`, about 2 µs a draw. It covers the primitive processor,
   the render target cache, the pipeline, dynamic state, system constants, bindings (descriptor
   sets), vertex and memexport buffers, and recording into the deferred command buffer.
3. **Resolves, readbacks, queries and submissions**: these depend on the GPU, the EDRAM model and
   the guest's memory.

Groups 2 and 3 share almost all of the backend's state. Group 1 needs almost none of it: only the
register file it writes, and the predication bits.

## 3. The state a split must carry

For any split, this is what translating draw *N+1* depends on that draw *N*, or the packets
between them, can change.

**Register file** (`RegisterFile`, 0x5003 dwords, about 80 KiB). Shader constants are
0x4000-0x4927 (float 0x4000-0x47FF, fetch 0x4800-0x48BF, bool/loop 0x4900-0x4927), 2344 dwords.
The rest are render, primitive, viewport and resolve state. The guest CPU also reads and writes
the register file through MMIO (`GraphicsSystem::ReadRegister/WriteRegister`). The ring's write
pointer arrives that way, and display registers are written that way.

**`CommandProcessor` shadow state:**
- `active_vertex_shader_` / `active_pixel_shader_` (IM_LOAD*)
- `bin_mask_` / `bin_select_` (predication)
- `me_bin_`
- the ZPD/VIZ query state (`active_segment_`, `viz_queries_`, `zpd_awaited_report_count_`)
- the gamma ramps, `swap_count_`
- `read_ptr_index_` and its write-back to guest memory
- fix 87's `command_chunk_epoch`

**`VulkanCommandProcessor` shadow state:**
- `current_constant_buffers_up_to_date_` and the per-draw float constant maps
  (`current_float_constant_map_vertex_/pixel_`), which decide whether a constant write marks a
  buffer dirty
- the dynamic state last set (viewport, scissor, depth bias, blend constants, stencil)
- the bound pipeline and descriptor sets, `current_guest_graphics_pipeline_layout_`
- the open render pass and framebuffer; `submission_open_` and `frame_open_`
- the deferred command buffer
- the upload pools (uniform, fix 120's tail), the transient descriptor pools
- fix 104's texture-set reuse table, fix 120's dynamic constant sets
- `memexport_ranges_`, `draws_since_resolve_`, fix 134's async-submit state

**Pipeline cache:**
- the shader map (keyed by microcode hash), the translations by modification
- the pipeline map and asynchronous creation (fix 34's claims)

**Texture cache:**
- the binding derived for each of the 32 fetch constants (out of sync after a fetch write)
- the textures, their loads (compute dispatches recorded into the stream)
- the write watches on guest memory, `binding_epoch()` (fix 104)
- the samplers

**Render target cache:**
- the EDRAM ownership ranges (which host render target owns which tiles)
- the current render target bindings and render pass
- ownership transfers (recorded draws)
- resolves (EDRAM to guest memory, with or without readback, fixes 59/91/133)

**Shared memory:**
- valid and watched pages, uploads recorded into the stream
- fix 87's unwatched chunks, fix 125's content-checked units
- memexport invalidation

**Primitive processor:** converted index buffer caches (per frame).

**Query pool / VIZ:** open queries, resolve batches (fix 134c).

Most of this is *sequential by construction*. Each draw's render target update depends on the
ownership the previous draws left. Each texture request depends on watches that previous draws
armed and that the CPU may have triggered. Each "bind only if changed" decision depends on the
previous draw's binding.

## 4. Options

### A. Data-parallel chunks (snapshot, translate chunk N+1 beside chunk N, secondary command buffers)

This was the task's first idea. At a chunk boundary (every K draws), snapshot the register file
and the shadow state. Translate chunk N+1 on another thread with its own deferred command buffer,
or a Vulkan secondary command buffer, and execute the chunks in order.

What it needs:
- **Thread-safe or partitioned caches.** The pipeline cache could be shared under a lock: lookups
  dominate. The texture cache, shared memory and render target cache cannot simply be locked.
  Their results depend on order. A texture loaded by chunk N+1 before chunk N's resolve writes it
  would be stale. A render target that chunk N+1 binds depends on the EDRAM ownership chunk N
  leaves.
- **Speculation.** Chunk N+1 is translated against a predicted state (chunk N's state at its end
  is unknown until chunk N is done). Every cache decision has to be validated afterwards:
  - render target keys and ownership
  - texture bindings and their epochs
  - shared memory ranges uploaded, watches armed
  - pipelines created
  - the redundancy decisions of dynamic state and bindings

  Any miss is replayed. In heavy frames, resolves happen many times a frame (fix 133: MW2
  resolves at almost every few draws; fix 117 classes in RDR). The chunk boundaries that are
  safe without speculation are therefore the resolves and render target switches, and those are
  exactly where the state changes.
- **Secondary command buffers.** Each chunk must begin with full rebinding (pipeline, sets,
  dynamic state, vertex and index buffers). Render passes would have to be split at chunk
  boundaries, or use `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` with inheritance. On RADV
  (the PS5 path) secondaries are fine, but every chunk boundary costs rebinds.

**Verdict:** the largest possible gain, and by far the most invasive change. It means rewriting
the texture, render target and shared memory caches into a two-phase "plan, then commit" form.
It is not a prototype that can be made safe without console iterations. It stays the long-term
direction (section 8, stage 3).

### B. Functional pipeline: decode | translate (fix 142)

Split the thread by *kind of work* instead of by draws. A decode thread does group 1, the packet
layer, which needs no backend state. The GPU command thread does groups 2 and 3 on ordered ops it
receives. The two overlap. While the GPU thread translates draw N, the decoder is already reading
the packets of draws N+1, N+2, and so on.

This keeps *every* backend structure on one thread, unchanged, so the sequential caches stay
correct by construction. What has to be proven is only that the ops do exactly what the packets
did. That is testable on the host (section 6).

The gain is bounded by the packet layer's share of the thread (section 7), and it removes only
the part of that share the decoder can absorb.

### C. Parallel "phase 2" of a draw

Keep everything that touches the sequential caches on the GPU thread: render targets, textures,
shared memory, pipeline lookup. Hand the order-independent rest of each draw to workers, each
with its own upload and descriptor pools and its own deferred stream, merged in order before the
replay: system constant computation, constant copies, descriptor writes, recording. This is
possible after fixes 104 and 120. It still needs the redundancy decisions (fix 104's set reuse,
fix 120's set reuse, dynamic state diffing) made per worker chunk. Medium invasiveness. Stage 2.

**Choice for the prototype: B.** It is the only split that preserves guest-visible ordering
without speculation. It touches no cache, it can be checked against a reference interpreter on
the host, and it lays the infrastructure later stages need: the op FIFO, serialization points by
reason, and per-thread diagnostics.

## 5. The prototype (fix 142)

### 5.1 Threads and data flow

```
 guest CPU --WPTR--> [ring + indirect buffers in guest memory]
                              |
                   "GPU Decode" thread (mode 1)
                   - WorkerThreadMain's ring loop
                   - walks ring + IBs, predication
                   - drops unchanged constant writes
                              |  op FIFO (SPSC, 4 MiB default)
                              v
                   "GPU Commands" thread (unchanged state)
                   - register runs -> fix 103's WriteRegistersSwapped
                   - packets -> ExecutePacket (same handlers)
                   - draws, resolves, events, swaps, queries
                   - read pointer write-back
                              |
                   fix 134's submit worker (unchanged)
```

Mode 2 runs both halves on the GPU command thread: decode a part of the ring into the FIFO, then
execute it. It measures the decoder and the ops without a second thread, and checks them on the
console. It plays the role fix 134's mode 2 plays for its worker.

### 5.2 Ops

`xenia/gpu/vulkan/parallel_record.h`, header-only.

An op is a 16-byte header plus payload, 8-byte aligned and contiguous. A wrap op skips to the
start of the buffer. Positions are monotonic byte counts. `published` (decoder to GPU thread) and
`retired` (GPU thread to decoder) are the only shared variables. They use seq_cst stores and a
mutex and condition variable for sleeping, with timed waits as a safety net. `retired` is always
the end of an executed op; it never sits between a wrap and the op after it.

| Op | Payload | What the GPU command thread does |
|---|---|---|
| `kOpPackets` | whole PM4 packets, guest byte order | `ExecutePacket()` with `reader_` over the payload, the same handlers as before; a failing packet ends the op (the decoder put it last) |
| `kOpRegisters` | a run of register values from register `arg` | fix 103's `WriteRegistersSwapped` (exactly the type 0 path's effects) |
| `kOpPrimaryBegin` | none | `++command_chunk_epoch` (fix 87), `CheckContentUnits()` (fix 125): `ExecutePrimaryBuffer`'s prologue |
| `kOpPrimaryEnd` | none | `OnPrimaryBufferEnd()` |
| `kOpReadPointer` | `arg` = read index | writes the ring's read pointer back to guest memory |

### 5.3 What the decoder does with each packet (the split points)

| Packet | Decoder | Serializes? |
|---|---|---|
| type 0 (runs) | runs of `kOpRegisters`, dropping unchanged shader constants | no |
| type 0, one register | forwarded; forgets that constant | no |
| type 1 | forwarded (registers below 0x800, never constants) | no |
| type 2, 0, 0x0BADF00D | nothing (as before) | no |
| SET_CONSTANT (ALU/FETCH/BOOL/LOOP/REGISTERS), SET_CONSTANT2, SET_SHADER_CONSTANTS | runs of `kOpRegisters` | no |
| LOAD_ALU_CONSTANT | forwarded (the GPU thread reads the memory in order); forgets the target range | no |
| REG_RMW, COND_WRITE | forwarded; forgets every shadow constant | no |
| MEM_WRITE, EVENT_WRITE_SHD/EXT, REG_TO_MEM, COND_WRITE | forwarded; the address noted as a pending write | no |
| INDIRECT_BUFFER(_PFD) | decoded in place (recursively) | yes if it overlaps a pending write (`ib-write`) |
| WAIT_REG_MEM | forwarded, then waits until it has executed | yes (`wait`) |
| SET_BIN_* | tracked for predication and forwarded | no |
| predicated packet, predicate false (or a predicated swap) | dropped (as before) | no |
| DRAW_INDX(_2), IM_LOAD(_IMMEDIATE), EVENT_WRITE(_ZPD), VIZ_QUERY, XE_SWAP, INTERRUPT, NOP, ME_INIT, ... | forwarded | no |
| unknown opcode, draw too small, bad shader type, count overflow | forwarded last in its op; the buffer ends (as before) | no |
| FIFO full | waits for the GPU thread to retire ops | yes (`room`) |

### 5.4 Ordering rules (why the guest cannot tell)

1. **Everything guest-visible stays on the GPU command thread, in stream order.** That covers:
   - memory writes by packets (MEM_WRITE, EVENT_WRITE_SHD/EXT/ZPD, REG_TO_MEM, COND_WRITE)
   - scratch register write-backs (`HandleSpecialRegisterWrite`)
   - interrupts (INTERRUPT, the swap's)
   - swaps and the presenter
   - resolves and readbacks into guest memory
   - and the **ring's read pointer**: it is written back only by `kOpReadPointer`, after
     everything before it was executed.

   So the guest observes the GPU's progress no earlier than before. Any buffer it reuses after a
   fence or after the read pointer passes was consumed by the GPU thread. (The decoder copied
   ring and IB contents earlier, which is harmless: the CPU writes them before moving the write
   pointer.)
2. **The register file has one owner, the GPU command thread**, as before. MMIO reads see what
   they saw. The decoder keeps only a private shadow of the shader constants for its filter.
   Dropping a write equal to the shadow is exactly `vulkan_skip_redundant_constants`'
   early-return (float, fix 105's fetch) or a no-op (bool/loop). The filter is used only when that
   cvar is on. The shadow is forgotten wherever a packet may write constants behind the decoder.
3. **WAIT_REG_MEM serializes.** Its condition may depend on the stream's own writes (a fence by
   EVENT_WRITE_SHD) or on the guest waiting for the GPU to get there. The GPU thread executes it
   with the original handler, `PrepareForWait` included. The decoder decodes nothing after it
   until then.
4. **Indirect buffers over pending writes serialize.** A MEM_WRITE (or the like) not yet executed
   that overlaps an IB's range is executed first. This covers GPU-patched command buffers, which
   no title is known to use. More than 128 pending writes counts as an overlap.
5. **Idle semantics.** The GPU thread runs `PrepareForWait` (ZPD strict reports, VIZ resolves) and
   counts fix 49's idle time only while the decoder waits for the guest. When the decoder is
   merely behind, the GPU thread spins and counts as "starved". So no extra submissions come in
   the middle of a frame.
6. **Shutdown and `CallInThread`.** Pending functions still run on the GPU thread between op
   batches. The decoder stops on `worker_running_`, and a wait for room that cannot be met while
   stopping aborts the decoder instead of overwriting the FIFO.

### 5.5 Assumptions (documented risks)

- The guest CPU never writes shader constants through MMIO. D3D on the 360 sets them in the
  command stream. If it did, the filter could drop a write that MMIO had made different.
- Malformed packets whose handler reads fewer dwords than their count (none of the well-formed
  ones) are parsed the same way by the GPU thread from the op. But the decoder's own tracking
  (predication, shadow) may then miss what is hidden in the extra dwords.
- GPU writes into command memory by resolves or memexport (not by explicit packets) are not
  tracked.
- `Save`/`Restore` of the command processor (debug traces) is not supported while split.
- Fix 103's register line (`CountRegisterWrites`) no longer sees the runs in modes 1 and 2.

### 5.6 Settings

The cvars below can be set per game in `assets/config/<title>.config.toml` (`[GPU]`), or in
`parallel-record.txt` in the title folder. The file is read once when the GPU thread starts.

| Setting | Values | Default |
|---|---|---|
| `gpu_parallel_record` / `mode` | 0 off, 1 decode thread, 2 decoded on the GPU thread | 0 |
| `gpu_parallel_record_fifo_kib` / `fifo` | 256-65536 KiB | 4096 |
| `gpu_parallel_record_filter` / `filter` | drop unchanged constant writes | on |
| `gpu_parallel_record_sibling` / `sibling` | the decode thread on CPU 3, the other half of the GPU thread's core (`PlaceCallingThreadBesideGpu`) | off (the shared CPUs) |

**Off path.** `RunParallelRecord` (called once in `WorkerThreadMain` after `SetupContext`) reads
the settings and returns false before it makes, logs or stores anything. The ring loop below it is
unchanged. The patch only adds lines, and the check verifies that (section 6). The one difference
off is an `fopen` of a missing `parallel-record.txt` at GPU thread start.

### 5.7 Diagnostics

The game log gets this line every 30 s, after fix 134's. In mode 0 it is `Frame time parallel
record (fix 142): off`.

```
Frame time parallel record (fix 142, mode m (...), FIFO k KiB, filter f):
  decode d ms/s, waiting for the ring r, room o, drains w;
  GPU thread executing e ms/s, starved s, idle i;
  per frame: batches b, ring parts p, draws n, indirect buffers x, ops y (z KiB);
  registers u/s, dropped unchanged v/s (q%);
  serialized per frame: wait a, room b, ib-write c;
  ahead avg A KiB, max M KiB;
  packets handed over h/s, predicated out t/s, shadow resets j/s, buffers ended early g
```

At start: `Parallel record (fix 142): mode ..., FIFO ... KiB, unchanged constants ..., decode
thread ..., from ...`. Also `the decode thread started` and `stopped`.

- **Per-thread record time:** `decode` is the decoder's own work, its waits excluded; `executing`
  is the GPU thread's. In mode 2 both are on the GPU thread, and their ratio is the split.
- **Chunks per frame:** `batches` are publishes, the hand-offs the GPU thread can start on; `ring
  parts` are primary buffers.
- **Serialization points by reason:** `wait`, `room`, `ib-write`, with the time the decoder spent
  in them (`drains`, `room`).
- **Overlap:** `starved` (the GPU thread had nothing while the decoder worked) and `ahead` (bytes
  decoded and not yet executed).

Fixes 49, 73, 75, 77, 130 and 134 keep their lines. The GPU thread is still "the GPU thread" for
them.

## 6. Host tests

`tools/check-parallel-record.cpp` runs in `run-fix-checks.sh` and is also built under
ThreadSanitizer there. It is listed in `source-fix-checks.yml`'s paths.

- the settings and the file
- the FIFO: order, wraps, room, retired at op ends, 200,000 ops on two threads
- **a reference PM4 interpreter.** It is an independent model with its own reader and every
  packet kind above. It is compared with the decoder plus ops executed by the same model. The
  comparison covers the same guest-visible events in the same order, the whole register file at
  every draw, and the final registers and memory. It runs 160 random streams over a wrapping ring
  × {filter on, filter off, two threads}. The streams cover waits, `ib-write` serializations,
  predication, nested IBs changed by a MEM_WRITE, failing packets, scratch registers and
  LOAD_ALU_CONSTANT.
- the ordering rules alone
- the filter alone
- the log line
- the tree: the hook's place before the unchanged ring loop, mode 0 returning before anything, the
  cvar defaults, the ops' execution, the opcodes' `static_assert`s against `xenos.h`
- the patch: no line removed; `command_processor.cc` gets only the 6-line hook

Writing the check found one real bug before any build. A failing packet inside an IB was followed,
in the same op, by the parent buffer's next packets, which the GPU thread's `break` then skipped.
Failing packets now end their op.

## 7. Expected gain

Upper bound (Amdahl): the GPU thread loses the decoder's share `d` of its work, so its throughput
rises by at most `1/(1-d)`. On top of that, it no longer does the dropped constant writes.

- d = 0.15 gives +18% at best; d = 0.30 gives +43%.

Fix 75's packet layer (150-468 ms/s on c31) puts `d` between 0.15 and 0.45 depending on the
game, before fix 103. Fix 103 made register writes cheaper, so today's `d` is lower. Mode 2
measures it directly.

Realistic: +0 to +15% FPS where the GPU thread is the limit (GTA IV, RDR, MW2, BF3, TTT2), lower
in games bound elsewhere. Three costs eat into the bound:
- **Cache-line traffic.** The FIFO crosses cores. The PS5's Zen 2 has two 4-core CCXs, and a
  decode thread on the other CCX pays L3-to-L3 latency on every line. `sibling 1` (CPU 3, same
  core, shared L1/L2) avoids that but shares the core's execution units. The test plan compares
  both.
- **The decode thread competes with guest threads** on the shared CPUs.
- **Every WAIT_REG_MEM empties the pipeline.** Fix 77's counts say how many per frame. Titles
  that wait mid-frame overlap less.

If the prototype shows `starved` near 0 and the GPU thread's `executing` time falls by the
decoder's share, stage 2 (option C) is worth building on the same FIFO. If the decoder is the
slower stage (the GPU thread is starved), the packet layer was larger than thought, and the
decoder can be split further (IB prefetch).

## 8. Roadmap

1. **Stage 1 (this fix):** decode | translate. Measure in modes 2 and 1, sibling 0 and 1. Confirm
   that rendering and pacing are unchanged.
2. **Stage 2 (option C):** move more of the packet-side work into the decoder. Candidates are a
   precomputed per-draw dirty mask (which constant buffers changed, so the GPU thread skips its
   per-write bookkeeping) and pre-resolved index/DMA info. Then move the order-independent phase
   of `IssueDrawInner` (system constants, constant copies, descriptor writes, recording) to
   workers, with per-worker pools and streams merged in order before fix 134's replay.
3. **Stage 3 (option A):** two-phase texture, render target and shared memory caches ("plan" in
   order, "commit" at the merge). Chunk boundaries at resolves and render target switches,
   secondary command buffers per chunk, speculation validated at the merge.

## 9. Prior art

- **Xenia (upstream and Canary):** one command processor thread. The D3D12 backend's
  `DeferredCommandList` and the Vulkan `DeferredCommandBuffer` are the record-then-replay design
  fix 134 builds on. To our knowledge there is no upstream multi-threaded split of the PM4
  processing.
- **Ryujinx, `Ryujinx.Graphics.GAL.Multithreading.ThreadedRenderer`:** a layer in front of any
  renderer backend that moves its work to a separate thread. It uses a command queue and a
  single-producer, single-consumer `CircularSpanPool` for data released in creation order. The
  threaded window limits how far ahead the producer runs (at most a frame). This is the same shape
  as fix 142: emulation state on one side, an ordered queue, the backend on the other. The authors
  reported it was about as fast as NVIDIA's OpenGL threading. ([ThreadedRenderer.cs mirror](https://git.dawidpotocki.com/mirror/ryujinx/plain/Ryujinx.Graphics.GAL/Multithreading/ThreadedRenderer.cs?h=1.1.380))
- **Mesa, `u_threaded_context` (Gallium, 2017):** wraps `pipe_context` and runs every call on a
  driver thread in batches. It syncs only where the caller needs a result (mappings, queries), and
  it promotes buffer maps to unsynchronized when safe. Its sync points correspond to fix 142's
  WAIT_REG_MEM and ib-write drains. ([mesa-dev patch](https://lists.freedesktop.org/archives/mesa-dev/2017-May/155443.html))
- **Dolphin, "Dual Core":** the CPU thread produces the GPU FIFO and a GPU thread consumes it.
  SyncGPU, a tighter-sync variant, turned out barely faster than single core and stuttery. The
  lesson for fix 142: frequent serialization loses the overlap, so the reasons are counted.
  ([Dolphin issue 12584](https://bugs.dolphin-emu.org/issues/12584))
- **PCSX2, MTGS:** the EE thread copies GS packets into a FIFO that the GS thread consumes. It
  needs tight synchronization to avoid overwrites, and has a debug mode that waits for each
  command. Its gain was small at first (2-4%), the same caution as section 7.
  ([PCSX2 forum, 2009](https://forums.pcsx2.net/archive/index.php/thread-6599.html))
- **D3D11 deferred contexts and Vulkan secondary command buffers:** the parallel-recording
  mechanism of option A. They require independent state per chunk, which is exactly what the
  Xenos stream does not give.

## 10. Console test plan

Build: run id and label in section 11. Compare the same scene with the same save, 60 s after it
loads, using fix 49/134's 30 s lines and the guide's Measure performance. Use
`parallel-record.txt` in the title folder, and restart the game between settings (the file is
read at start). Keep `async-submit.txt` as noted.

| Run | `parallel-record.txt` | `async-submit.txt` | What it shows |
|---|---|---|---|
| A | (none, or `mode 0`) | `mode 0` | baseline: must equal c90; the line says `off` |
| B | (none) | `mode 1` | fix 134's baseline |
| C | `mode 2` | `mode 1` | the decoder's share on one thread (`decode` vs `executing`); correctness of the ops; FPS about B or a little lower |
| D | `mode 1` | `mode 1` | the split on the shared CPUs |
| E | `mode 1`<br>`sibling 1` | `mode 1` | the split beside the GPU thread (CPU 3) |
| F | `mode 1`<br>`filter 0` | `mode 1` | the filter's part of the gain |
| G | `mode 1` | `mode 0` | the split without fix 134's worker |

**Games:** GTA IV (the city, driving), RDR (Armadillo, the town), MW2 (a campaign mission start),
Kinect Sports (boxing: Kinect, readbacks and queries).

**Record from the log:**
- fix 49's GPU thread busy, fix 142's line, fix 134's line
- fix 77's WAIT_REG_MEM counts
- FPS and swaps/s

**Look for:**
- **Correctness:** identical picture and HUD; no flicker in shadows or reflections (occlusion
  queries, RDR); Kinect Sports' crowd board (readback); no hangs at loading screens (WAIT_REG_MEM
  on fences). A hang watch report naming the "GPU Decode" thread or `starved` at 1000 ms/s means
  a stuck decoder.
- **Gain:** run D or E faster than B; `executing` lower than B's GPU thread busy; `starved` low;
  `wait` and `ib-write` per frame low; `room` near 0 (else raise `fifo`).
- **If C already renders wrong,** the decoder or the ops are at fault (not threading). If C is
  right and D is wrong, it is threading. Report which.

## 11. Builds

- Branch: `fix/parallel-gpu-record`
- `BUILD_VERSION`: `1.0.5-work-fix142`
- Build run (Rebuild v1.0.2 from source drops): see the branch's Actions, and the final message
  of the session that made this fix.
