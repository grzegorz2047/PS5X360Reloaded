# Bindless textures for the Vulkan backend: design

Status: **design only**. No code, no build. The user decides on implementation after reading it.
Fix numbers below are placeholders (B0 to B3); the coordinator assigns the real ones.

Sources:
- `research/parallel-gpu-commands/STAGE2.md` (sections 12 to 16);
- `research/fix142d/console-results.md` (on `fix/gpu-thread-probe-106`);
- the code of the assembled 1.0.2 tree (1.0.6 line). File references are to
  `.deps/xenia-canary/src/xenia/gpu/`.

Every number is marked **measured** (a console log, with its build) or **estimate** (worked out
here, not measured).

## 1. Summary

- **What it is.** Today every draw allocates, writes (`vkUpdateDescriptorSets`) and binds a
  descriptor set of textures and samplers for each shader stage. Bindless replaces that with:
  - **one large descriptor set** that holds every live image view and sampler, written once when
    the view or sampler is made;
  - **a small per-draw table of indices** into it.
- **Why it is cheaper than it first looks.** The Xbox 360's texture fetch names its fetch constant
  as a 5-bit field of the instruction, so the index is a compile-time constant in every shader
  (`spirv_shader_translator_fetch.cc:684`, `shader_translator.cc:1141-1145`).
  - The per-draw index is therefore *dynamically uniform*: the same for every invocation of a draw.
  - It needs **no `NonUniform` indexing**. That removes the main GPU-side risk: the PS5 driver
    reports `shaderSampledImageArrayNonUniformIndexingNative false`, which means it would turn a
    non-uniform index into a loop.
- **What it removes from the GPU thread** (measured parts, estimated net):
  - about 5-6% in GTA IV;
  - about 4% in MW2;
  - about 3-4% in RDR, which is already at its 30 fps cap.
  - **Expected gain: about +2.5 swaps/s in GTA IV, about +1.5 in MW2** (estimates). BF3 and Tekken
    TT2 are not measured; the probe measures them first.
- **This is a modest gain for a large change.** It touches the shader translator, the pipeline
  layouts and the texture cache. Section 4.7 describes a much smaller alternative,
  `VK_KHR_push_descriptor`, which may capture most of the same saving. The first step is a cheap
  probe that decides between bindless, push descriptors and stopping.

## 2. Goal and numbers

### 2.1 Which games are bound by the GPU thread

| Game | Spot | Swaps/s | GPU thread | Source |
|---|---|---|---|---|
| GTA IV | city | 42.9 | 994 ms/s own work, no waits | measured, 1.0.6-work-fix142f-diag |
| GTA IV | city | 50-52 | 929 ms/s executing, no waits | measured, 1.0.6-work-fix142d |
| MW2 | mission, steady | 36.6-36.7 | 1,001 ms/s, no waits, 1.4 µs a draw at about 370k draws/s | measured, 1.0.6_candidate_4 |
| RDR | town | 30 (cap) | 992 ms/s; 28.1 ms a frame, about 5 ms of headroom | measured, 142d and 142f-diag |
| BF3 | - | - | the GPU thread was the limit on 1.0.5_candidate_57 | measured then; not re-measured on 1.0.6 |
| Tekken TT2 | - | - | not measured on 1.0.6 | - |

The console's GPU is 17-38% busy in these games (measured), so the GPU itself has room.

### 2.2 What the per-draw binding work costs today

The parts below are fix 142d's binding parts (measured, 1.0.6-work-fix142d, one 30 s block per
game, 1 draw in 8 timed), in ms/s:

| Part | GTA IV | RDR | MW2 | Removed by bindless? |
|---|---|---|---|---|
| draws/s | 299,640 | 179,741 | 324,022 | |
| texture image infos | 14.0 | 13.2 | 24.2 | no: becomes "look up the indices", similar cost |
| texture set reuse check (fix 104) | 9.1 | 6.9 | 12.1 | **yes** |
| set allocation and writes | 18.4 | 9.4 | 10.3 | **yes** (texture sets; constant sets are rewritten for 0.1-0.3% of draws) |
| `vkUpdateDescriptorSets` | 25.3 | 20.4 | 20.6 | **yes**, nearly all of it (the same reason) |
| binds | 21.9 | 11.3 | 12.5 | **partly**: texture sets go; the constant set's dynamic-offset rebind (fix 120) stays |
| Fix 73 "bindings" part (all of `UpdateBindings`) | 162.0 | 101.0 | 127.3 | |
| GPU thread executing | 929 | 992 | 910 | |

**What bindless removes:**

| | GTA IV | RDR | MW2 | |
|---|---|---|---|---|
| Upper bound (reuse check + allocation and writes + `vkUpdateDescriptorSets` + binds) | 74.7 ms/s, 8.0% | 48.0 ms/s, 4.8% | 55.5 ms/s, 6.1% | measured parts |
| New cost: one push of the index table per draw (section 4.2), at 0.03-0.05 µs | 9-15 ms/s | 5-9 ms/s | 10-16 ms/s | estimate |
| The constant set's rebind that stays (about half of "binds") | about 11 ms/s | about 6 ms/s | about 6 ms/s | estimate |
| **Net** | **about 50-55 ms/s, 5.4-5.9%** | **about 33-37 ms/s, 3.3-3.7%** | **about 34-40 ms/s, 3.7-4.4%** | estimate |

The "7-11% descriptor work" quoted in fix 142f-diag's decisions table includes the image infos
and the constant uploads. Bindless keeps those, so its net is lower.

### 2.3 Expected swaps/s

In a game bound by the GPU thread, swaps/s scales about as 1 / (1 − saving). All of these are
estimates:

| Game | Today | With bindless | Note |
|---|---|---|---|
| GTA IV | 42.9 (142f-diag spot) / 50-52 (142d spot) | 45.3-45.6 / 53-55 | +2.4 to +3 |
| MW2 | 36.6 (steady) | 38.0-38.3 | +1.4 to +1.7 |
| RDR | 30 (cap) | 30 | Headroom only: about 1 ms more a frame |
| BF3 | ? | ? | Unknown. Fix 73 measured BF3's draws at 292 ms/s on 1.0.5 (c31), but its packet layer dominated then (468 ms/s, before fix 103). The probe measures it. |
| Tekken TT2 | ? | ? | Unknown. It runs at 2x, where the GPU side may be the limit instead. The probe measures it. |

For comparison, fix 142e's skip of an 8% item gave no measurable FPS. Gains of 4-6% are close to
the scene noise (section 7).

## 3. PS5 driver support

### 3.1 What RADV on the PS5 already reported

Measured from the 1.0.6-work-fix142g launcher log, the 142f-diag probe in `vulkan_device.cc`:

| Feature or limit | Value |
|---|---|
| `shaderSampledImageArrayNonUniformIndexing` | true |
| `descriptorBindingSampledImageUpdateAfterBind` | true |
| `descriptorBindingPartiallyBound` | true |
| `runtimeDescriptorArray` | true |
| `maxUpdateAfterBindDescriptorsInAllPools` | 67,108,863 |
| `maxPerStageDescriptorUpdateAfterBindSampledImages` | 8,388,606 |
| `maxPerStageDescriptorUpdateAfterBindSamplers` | 8,388,606 |
| `maxPerStageUpdateAfterBindResources` | 8,388,606 |
| `maxDescriptorSetUpdateAfterBindSampledImages` | 8,388,606 |
| `maxDescriptorSetUpdateAfterBindSamplers` | 8,388,606 |
| `shaderSampledImageArrayNonUniformIndexingNative` | **false** (not needed, section 1) |

### 3.2 What the design also needs, not yet seen on the console

| Item | Why | Expected on RADV (to verify) |
|---|---|---|
| `shaderSampledImageArrayDynamicIndexing` (core feature) | a dynamically uniform index into the arrays | true |
| `descriptorBindingSamplerUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending` | samplers in the same update-after-bind set; writing free slots while work is pending | true |
| `maxPushConstantsSize` | the index table in push constants (section 4.2) | 256 bytes |
| `maxBoundDescriptorSets`, `maxDescriptorSetUniformBuffersDynamic` | the set count stays at 4; a table UBO would be an 8th dynamic UBO | 32 / at least 8 |
| `maxSamplerAllocationCount` | the sampler array's size (`sampler_max_count_`, `vulkan_texture_cache.cc:3236-3243`) | (log it) |
| Extensions `VK_KHR_push_descriptor` (`maxPushDescriptors`), `VK_EXT_descriptor_buffer`, `VK_EXT_mutable_descriptor_type` | the alternative of section 4.7, and options for later | depends on the PS5 Mesa version |

### 3.3 How each is verified before anything is built on it

Step B0 (section 6) adds three things to the existing one-switch diagnostics build (the 142f-diag
line on `fix/gpu-thread-probe-106`). All of it runs once, at start, behind `gpu-diag-detail.txt`:
1. **A log line** with every feature, limit and extension in 3.1 and 3.2.
2. **A functional test.** Not a feature bit, but the real path:
   - create an update-after-bind set layout with `PARTIALLY_BOUND | UPDATE_AFTER_BIND |
     UPDATE_UNUSED_WHILE_PENDING`, about 65,536 sampled images in three bindings (2D array, 3D,
     cube) and 4,096 samplers;
   - allocate the set, write a few slots far apart (0, 1, 40,000), leave the rest unwritten;
   - run a tiny compute shader, written in the shape the translator would produce, that samples
     `heap[index]` with the index from a push constant and stores the texel;
   - read it back. The log line says `ok` or what differed.
   - Then write slot 40,000 again with another view while the first submission is still pending,
     submit again, and check that both results are right (`UPDATE_UNUSED_WHILE_PENDING`).
3. **GPU timing.** The same shader with a dynamically uniform index, with a fixed binding as today,
   and with a deliberately non-uniform index. 1,000 dispatches each, timed with timestamp queries.
   This gives the real cost per sample on this GPU instead of assuming it.

## 4. Design

### 4.1 Today, in short (from the code)

- **Sets** (`spirv_shader_translator.h:368-405`):
  - 0 = shared memory and EDRAM;
  - 1 = the seven constant buffers, dynamic since fix 120;
  - 2 = vertex textures;
  - 3 = pixel textures.
  - `kDescriptorSetCount <= 4` is asserted for Mali-class devices.
- **One SPIR-V variable per binding.**
  - Images are one variable per (fetch constant, dimension, sign): `xe_texture{fc}_{2d|3d|cube}_{u|s}`.
  - Samplers are one variable per (fetch constant, filter overrides).
  - Binding numbers are in order of first use; samplers come after the images
    (`spirv_shader_translator_fetch.cc:2949-3063`, `spirv_shader_translator.cc:864-875`).
- **No descriptor indexing anywhere in the tree.**
- **Pipeline layouts** are keyed by the four counts (textures and samplers per stage) and made on
  demand (`vulkan_command_processor.cc:3940-4083`). No push constants are used for guest pipelines.
- **Per draw** (`UpdateBindings`, `vulkan_command_processor.cc:8841-9508`):
  - fill image infos from `GetActiveBindingOrNullImageView`, and samplers from `UseSampler`;
  - fix 104's reuse check;
  - allocate a transient set;
  - one `vkUpdateDescriptorSets`;
  - `CmdVkBindDescriptorSets` on the deferred command buffer, which fix 134's worker replays.
- **Views** live in each texture's `views_` (keyed by sign, host swizzle and array).
  - They are destroyed with the texture.
  - A texture is destroyed only once its last use's submission has completed
    (`texture_cache.cc:251-253`).
  - Samplers are an LRU cache, destroyed only after their last use completed
    (`vulkan_texture_cache.cc:791-993`).

### 4.2 Shader translator

Behind a translation mode chosen at game start (not per shader):
- **Declarations.**
  - Set 2 becomes the heap, with four bindings:
    - 0 = `image2DArray xe_heap_2d[N2]`;
    - 1 = `image3D xe_heap_3d[N3]`;
    - 2 = `imageCube xe_heap_cube[NC]`;
    - 3 = `sampler xe_heap_samplers[NS]`.
  - Fixed-size arrays: they need `shaderSampledImageArrayDynamicIndexing`, not
    `runtimeDescriptorArray`.
  - 1D stays folded into 2D, as today. The three image types are separate bindings, so no
    descriptor aliasing of different image types is needed.
- **The index table.** Per stage, one 16-bit index per `TextureBinding` and per `SamplerBinding`
  of the shader, in the same order as today's binding numbers.
  - **Push constants:** vertex stage first, pixel second, at offsets fixed by the pipeline layout.
    It fits when the vertex and pixel bindings together are at most 128 (256 bytes); the probe
    logs the real maximum.
  - **A shader with more bindings** reads its table from an 8th dynamic uniform buffer in set 1
    (`binding 7`, uploaded like the float constants), so there is no hard limit.
- **Fetch.** `FindOrAddTextureBinding` and `FindOrAddSamplerBinding` keep their bookkeeping (the
  same binding lists, so the command processor keeps the same order). Instead of a variable, each
  binding's fetch emits:
  - `idx = table[binding]` (a uniform load);
  - `OpAccessChain heap_X[idx]`, then `OpLoad`.
  - Everything after that is unchanged: `OpSampledImage`, the run-time sign selection
    (`SampleTexture`, `fetch.cc:3065-3106`), the run-time 3D/stacked selection from fetch constant
    word 5 (`fetch.cc:943-965`), and `QueryTextureLod`.
- **No `NonUniform` decoration.** `idx` comes from push constants or a uniform buffer and is
  the same for the whole draw. A host test checks that no translated shader carries
  `NonUniform` or `ShaderNonUniform`.
- **Capabilities.** None beyond core Vulkan 1.0 for dynamically uniform indexing of fixed-size
  arrays. `SPV_EXT_descriptor_indexing` and `RuntimeDescriptorArray` are not needed.

### 4.3 Pipeline layout

- **One pipeline layout for every guest pipeline:**
  - set 0 as today;
  - set 1 as today, plus binding 7 (the table UBO) when that path is used;
  - set 2 = the heap (an update-after-bind layout);
  - set 3 = empty.
  - The 4-set assert holds.
- **Push constant ranges:** vertex stages `[0, V)`, fragment `[V, 256)`. One fixed split, chosen
  from the probe's counts.
- **What goes:**
  - the per-count texture set layouts and layout keys (`descriptor_set_layouts_textures_`,
    `pipeline_layouts_`);
  - the layout-switch handling on pipeline change (`vulkan_command_processor.cc:4702-4734`).
  - These stay for the bindful path.
- **The heap set** is bound once per command buffer, at submission begin, after fix 134's
  hand-off and after any `EndSubmission`, including fix 157's.
- **The pipeline description** (`PipelineDescription`) does not include the layout, so stored
  pipelines (`.xpso`) need no new version: their layout is derived from the shaders.
- **The driver's pipeline cache** (`{title}.vk.bin`) holds compiled SPIR-V, which changes in
  bindless mode. That mode uses its own blob name (`{title}.bl.vk.bin`), so switching modes does
  not throw away the other mode's cache.

### 4.4 Texture cache: heap slots and their lifetime

- **A slot allocator per heap binding** (`heap_slots.h`, header-only, host-tested), with three
  operations:
  - `Allocate()` returns a slot;
  - `Free(slot, submission)` puts it in quarantine;
  - `Reclaim(completed_submission)` makes slots usable again.
- **Views.** `VulkanTexture::GetView` gets a slot when it creates a view, writes it to the heap and
  keeps the slot in the view entry. This also covers 3D-as-2D views (`GetOrCreate3DAs2DImageView`).
  The null views (2D array, cube, 3D; `vulkan_texture_cache.cc:3189-3227`) take fixed slots 0-2.
- **Samplers.** `UseSampler` gets a slot when it creates a sampler. The sampler array's size is
  `sampler_max_count_`.
- **Freeing.** `~VulkanTexture` and sampler eviction free their slots with the texture's or
  sampler's last usage submission. They already run only after that submission completed, so the
  quarantine is a second guard, not the only one.
- **Sizes and memory.**
  - The probe logs the largest number of live views per binding type in each game.
  - Proposed starting sizes (estimates): 2D array 32,768; 3D 4,096; cube 4,096; samplers =
    `sampler_max_count_`.
  - At about 32 bytes per image descriptor and 16 per sampler (RDNA, estimate), that is about
    1.3 MB of descriptor memory.
- **A full heap.** `Allocate` fails. The texture cache then evicts least-recently-used textures
  whose last use completed (as its memory limits already do), and retries.
  - If nothing can be evicted, the draw uses the null view's slot, which shows as a missing
    texture rather than a wrong one, and the failure is counted.
  - This is logged every 30 s. It must stay at 0 in every test.
- **Heap writes and fix 134's worker.**
  - `vkUpdateDescriptorSets` on the heap needs the set externally synchronized, and the worker
    replays `vkCmdBindDescriptorSets` of the same set on another thread.
  - The heap writes are therefore recorded into the deferred command stream as their own command,
    and the replay (on whichever thread runs it) performs them before the commands recorded after
    them. Writes and binds then always run on one thread, in order.

### 4.5 How stale descriptors are avoided

- **Order of a slot's life.** A slot is rewritten only:
  1. after the view that held it was destroyed, which happens only after its last use completed;
  2. and after its quarantine submission completed.

  No pending command buffer can then read it. `UPDATE_UNUSED_WHILE_PENDING` makes the write legal.
  Probe B0 checks the hardware behaviour of exactly this sequence (section 3.3, item 2).
- **The table is rebuilt from the current views every draw.**
  - The per-draw table is built the same way the bindful path picks views
    (`GetActiveBindingOrNullImageView` per binding).
  - A texture that changes between draws (a new load, a resolve into it, a new scaled copy) gives
    either the same view with new content, as today, or a new view and slot.
  - The table is pushed only when it differs from the last one pushed in this command buffer.
- **A verify mode, as fix 142e had** (`gpu_bindless`: 0 off, the default; 1 verify; 2 bindless).
  In mode 1 the bindful path still renders. For every draw it also:
  - builds the bindless table;
  - checks that each slot's recorded view or sampler equals the handle the bindful path writes;
  - checks that no slot in the table is free or in quarantine.
  
  Mismatches are counted and the first 16 logged with the fetch constant, slot and frame. This
  needs no shader change, so B1 can run it on every game before any GPU-visible change.

### 4.6 Interaction with other features

| Feature | Effect |
|---|---|
| Resolution scale 2x (fixes 117 to 117k) | Scaled textures are separate images (`TextureKey::scaled_resolve`), so they get their own views and slots. No special case. The shader still learns "scaled" from `textures_resolved` in the system constants. Fix 117d's lower memory limits under pressure evict textures and free slots. More live textures at 2x means a larger peak slot count, which the probe measures at 2x in Tekken TT2. |
| Memexport (fixes 30, 48) | Uses set 0's shared-memory SSBO, which is unchanged. |
| Resolves and readback (fixes 35, 59, 91, 133, 142i, 157) | Resolves write shared memory or the scaled-resolve buffers through compute pipelines, which have their own layouts. Textures loaded from them come through the texture cache as before. |
| Fix 142i / 157 | They act on readback copies to guest memory, not on descriptors. The only contact: fix 157 may end a submission at a draw, and the heap is rebound at every submission begin (4.3). |
| Fix 104 (texture set reuse) | Bypassed in bindless mode; kept for the bindful path. Its `binding_epoch` is not needed by bindless. |
| Fix 120 (dynamic constants) | Unchanged. Set 1 keeps its seven dynamic uniform buffers, plus the optional table UBO. |
| Fix 134 (async submit) | Heap writes are recorded into the deferred stream (4.4), so the worker's replay keeps writes and binds in order. |
| Fix 137 / 141 (swap effects, FXAA) | Separate pipelines with their own layouts. Unchanged. |
| Host swizzle fallback (`texture_swizzles` when `imageViewFormatSwizzle` is missing) | Unchanged; it is shader arithmetic, not a binding. |

### 4.7 A smaller alternative: push descriptors

`VK_KHR_push_descriptor` lets a draw record its descriptors into the command buffer
(`vkCmdPushDescriptorSetKHR`) instead of allocating and writing a set:
- **No translator change and no layout change**, apart from the texture set layouts being created
  with the push flag.
- **What leaves the GPU thread:** set allocation, fix 104's check, and `vkUpdateDescriptorSets`.
  The GPU thread still fills the image infos and copies them into the deferred stream. Fix 134's
  worker then makes the actual push call during replay.
- **The saving:** about the same three items as bindless (52.8 / 36.7 / 43.0 ms/s, measured parts),
  minus the cost of copying the infos into the stream (estimate). That is perhaps 70-90% of
  bindless's net saving for a fraction of the work.
- **Limits:**
  - `maxPushDescriptors` (32 on RADV desktop, to verify) per set. A pixel stage with more images
    and samplers than that falls back to the transient set for that draw. The probe logs how
    often that would happen.
  - It needs the extension in the PS5's Mesa. Probe B0 logs it.

If the probe shows push descriptors available, with almost every draw under the limit, **that
should be built first**. Bindless is then worth it only if a later measurement shows the remaining
per-draw infos and pushes still matter.

## 5. Risks and GPU-side cost

| Risk | Assessment | Mitigation |
|---|---|---|
| Non-uniform indexing cost on AMD | Not needed: the index is dynamically uniform (section 1). On RDNA a dynamically uniform descriptor index becomes a scalar load of the descriptor from the heap; that is the same kind of load as today, plus one scalar load of the index. | A host test that no `NonUniform` appears; the probe's GPU timing (3.3, item 3). |
| Register pressure | Descriptors still sit in scalar registers as today. The table adds one push constant or UBO read per binding, so vector register pressure is unchanged. Shaders with many textures may keep more descriptors live if the compiler hoists the loads. | Measured in B2: the GPU busy % before and after, at the same spots. |
| Shader compile time and the pipeline cache | Every shader's SPIR-V changes, so the first run in bindless mode recompiles every pipeline. Asynchronous compilation shows stand-ins (pop-in) during that session. | The mode is chosen at game start; a separate `.bl.vk.bin` blob (4.3); stored `.xpso` descriptions are reused. B2's A/B runs a warm-up session first. |
| RADV on the PS5 behaving differently with update-after-bind or partially bound sets | Unknown until tested. | Probe B0's functional test (3.3, item 2), then the verify mode. |
| A stale or reused slot | Wrong textures for a frame. | Lifetime order, quarantine, verify mode (4.5). |
| A full heap | Missing textures. | Eviction, the null slot, a counter that must stay 0 (4.4). |
| Heap write ordering with fix 134's worker | A torn or early write. | Writes recorded into the deferred stream (4.4); a host test for the order. |
| Larger per-draw tables than push constants allow | A slower UBO path for those shaders. | The probe logs the maximum; the UBO path exists. |
| The gain being in the noise | Fix 142e precedent: an 8% item gave no measurable FPS. | Go/no-go after B0 and stop rules (section 7). |

**Fallback.** `gpu_bindless` is 0 by default. The bindful path stays complete and is the one
used whenever:
- the setting is 0;
- a feature or limit from 3.1 or 3.2 is missing;
- the functional test at start fails (in bindless mode that test runs at every start, about 1 ms).

The mode is fixed for the session, because translated shaders and pipelines depend on it.

## 6. Plan in measurable steps

Each step:
- gets its fix number from the coordinator;
- is behind a switch that is off by default;
- has host checks wired into `run-fix-checks.sh` and CI;
- gets a console A/B, and a decision before the next step starts.

| Step | What | Host tests | Console | Decides |
|---|---|---|---|---|
| **B0: probe** (diagnostics only, on the 142f-diag line) | 3.3's log line, functional test and GPU timing. Per game, every 30 s: the largest vertex and pixel binding counts; the share of draws over the push-constant and push-descriptor limits; live views per binding type; live samplers; views and samplers made and destroyed per second; fix 142d's binding parts. | The probe's text against the tree; the log line's format. | One run each: GTA IV, MW2, BF3, Tekken TT2 at 2x, RDR. About 2 minutes each, plus the start log. | **Go/no-go**, and bindless vs push descriptors (section 7). |
| **B1: heap and verify** (no shader change) | Slot allocator with quarantine; heap writes through the deferred stream; per-draw table building. `gpu_bindless 1` = verify. | Allocator (allocate, free, quarantine, reclaim, full); table builder against the bindful order; write ordering in the deferred stream. | Verify on all five games plus Kinect Sports and The Sims 3 (texture-heavy). Mismatches 0, heap-full 0; mode 1's own cost per draw. | Whether the bookkeeping is right and cheap (under 0.1 µs a draw). |
| **B2: translator and layout** (`gpu_bindless 2`) | 4.2 and 4.3: heap declarations, table loads, one layout, push constants or table UBO, the `.bl.vk.bin` blob. | Translated shaders validated with `spirv-val` for a corpus of stored ucode; no `NonUniform`; binding order equal to the bindful path's; layout equality across pipelines. | A/B with a restart between runs, alternating, same spots: GTA IV, MW2, BF3, Tekken TT2; warm cache first. Read swaps/s, GPU thread executing, fix 73/142d parts and the GPU busy %. Screenshots at fixed spots compared by eye. | The gain, and visual parity. |
| **B3: per-draw trim** | Skip the push when the table is unchanged; drop the transient pools and fix 104 in mode 2. | Unchanged-table skip. | Same A/B. | The final number. |

If B0 favours push descriptors, B1-B3 are replaced by one step: **P1**, push descriptors behind a
switch, with a host test of the fallback over the limit and the same console A/B.

## 7. Effort and stop rules

### 7.1 Effort

These are estimates of session time; each step also needs one or two console sessions.

| Step | Effort |
|---|---|
| B0 | 2-3 days |
| B1 | 4-6 days |
| B2 | 1.5-3 weeks: the translator, the layouts, the pipeline cache and validation |
| B3 | 2-3 days |
| **Bindless in total** | **about 4-6 weeks** |
| P1 (push descriptors instead) | about 1 week |

### 7.2 Stop rules

| After | Stop if |
|---|---|
| **B0** | Any of these: <ul><li>a needed feature or limit is missing, or the functional test fails;</li><li>the GPU timing shows the dynamically uniform heap index costing more than about 3% of a sample;</li><li>the removable work (reuse check + allocation and writes + `vkUpdateDescriptorSets` + texture binds) is under 4% of the GPU thread in both GTA IV and MW2, and under 4% in BF3 too, so the expected gain is under about +3% swaps/s;</li><li>push descriptors are available and cover at least 95% of draws: build P1 instead and stop here.</li></ul> |
| **B1** | Any of these: <ul><li>verify mismatches that the design cannot explain;</li><li>a heap-full count above 0 that eviction cannot fix;</li><li>bookkeeping above 0.1 µs a draw.</li></ul> |
| **B2** | Any of these: <ul><li>under +3% swaps/s in GTA IV and MW2 at the same spots, after a warm cache;</li><li>any visual difference not explained by run-to-run variance;</li><li>a hang or device loss;</li><li>a first-session compile stutter that the async stand-ins do not hide.</li></ul> |
| **Overall** | If the measured gain is within the scene noise, the work is closed and kept off, as with fixes 142e and 142g. |

## 8. Related findings that change priorities

- **GTA V** is no longer a target: its preset (readback off, memory boost) reaches its 30 fps cap.
- **The Sims 3** (fix 142h A/B, measured):
  - content-checked units (fix 125) on take live mode from 9.8 to 19.6 swaps/s, its 3-vblank
    cap;
  - its remaining resolve readback waits (140-169 ms/s) come from a steady 3.6 MiB key whose
    resolves all find "no earlier copy";
  - that is the case fixes 142i and 157 handle, not a binding cost. With about 31 ms of work a
    frame, removing the 8.5 ms of waits could let it reach the 2-vblank step (30 fps). That is
    a cheaper test than anything in this document.
