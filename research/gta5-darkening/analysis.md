# GTA V (545408A7): brief global darkening while driving — analysis (fix 158)

Side session, 2026-11-24. Base: `1.0.6_candidate_5` (a17ebb3). The code references below are for the
tree that candidate 5 assembles: `.deps/xenia-canary/src/xenia/gpu/...`, checked in an assembled
copy whose GPU files match candidate 5, because no fix after 154 touches them. The log paths are under
`C:\Users\grzeg\Desktop\ps5x360\research\`.

## Symptom and what is already ruled out

- For a moment the whole picture gets darker, then it comes back. Every object changes at once, so it
  looks like exposure or eye adaptation, not like a missing draw.
- **Readback timing and delivery: ruled out.**
  - With "Image readback by the game" = Full (`gta5-full-readback/gta2.log:8`, `resolve readback full
    (choice 2)`), the waits are 236-247 ms/s (`gta2.log:909, 951, 1084, 1135`) and the flashes still
    appear, "maybe even more often", at 13.6-14.0 swaps/s (`gta2.log:939, 1052, 1124`).
  - With fix 142i3's listed keys, the early CPU reads of the waiting keys fell to zero and the flashes
    did not go away (`fix142i3/cloud-paste-12.md`).
- **memexport: ruled out.** It is not used ("memexport readback 0 ms/s").
- **Pipeline stand-ins and refused draws: ruled out.** Every "before the resolve" line in both logs
  reads `0 refused, 0 with the stand-in shader, 0 waited for their pipeline`, and the 30 s lines read
  `for pipelines 0 ms/s` (`gta2.log:909` and following). Draws are not being dropped or drawn with a
  placeholder shader while pipelines compile.
- **Present stalls: none.**
- **Occlusion-query guesses: ruled out.** Strict mode still flashes (hypothesis 1 below).
- **Async submit ordering (fix 134): ruled out.** It is off for GTA V (hypothesis 5 below).
- **No older baseline.** 1.0.5 (`1.0.5_candidate_91`) stops while loading GTA V
  (`research/gta5-105/gta.log`), so the darkening cannot be dated against the release code.

## What the game does (from the readback-key lines)

- `gta2.log:1112-1119` lists each resolve key's destination format (`fN` is `xenos::ColorFormat`):
  - `f32` = 16_16_16_16_FLOAT, `f36` = 32_FLOAT, `f6` = 8_8_8_8, `f7` = 2_10_10_10.
  - The sizes form a float downsample chain:
    - 0623A000/1966080 f32 (640x384);
    - 06082000/491520 f32 (320x192);
    - 06172000 and 06190000/122880 f32 (160x96);
    - 061AE000, 061BA000 and 1F0D8000/49152 f32 (80x48, padded to whole tiles).
  - Then come 32_FLOAT targets:
    - 061CE000/16384 (64x64);
    - **061D2000 and 061D3000/4096**, each resolved once per frame (x428 in 30 s at about 14/s). A
      4 KiB 32_FLOAT resolve is one 32x32 tile, so these are 1x1 values: the average and the adapted
      luminance.
  - A ring of three 8 KiB f32 keys, 1F0E8000/1F0EA000/1F0EC000 (x139 each, so one per frame in turn),
    plus 1F0E7000/4096 f36 and 1F0C0000/8192 f36, is what the CPU reads 1 frame later
    (cloud-paste-12). The upstream "gpu-readback" label on GTA V describes this as "HDR adaptation".
- **Occlusion queries.** While driving, the game issues 161-183 ZPASS_DONE events a second at about
  14 swaps/s (`gta2.log:1091, 1142`), about 12 a frame, against 13-14/s in the menus (`gta2.log:916,
  958`). The fast-readback run shows 171-274/s (`fix142i3/gta.log`). The upstream compatibility
  entry (xenia-canary/game-compatibility#1140) labels GTA V `gpu-viz-query`. Its sun glare and lens
  flare were fixed by the ZPD occlusion-query work (xenia-canary/xenia-canary PR #1016), which is the
  code this base has.

## Ranked hypotheses

### 1. The fast occlusion-query mode's guesses (ruled out by the console test)

- **Console result:** with "Zapytania o widoczność" = "Ścisłe (wolniejsze)", the dark flashes stayed.
  - `research/gta5-occlusion-strict/gta.log:12` shows GTA V's own `occlusion=3` (strict) and
    readback Fast.
  - The swap rate was 13.5-15 swaps/s, with 105-203 ZPASS_DONE a second.
- Strict waits for every real result, so the guesses below cannot cause the darkening. The fix 158
  query lines are left in as a cross-check.
- What was suspected:

- `occlusion_query` defaults to `"fast"` (`command_processor.cc:59-72`). The launcher's "Zapytania o
  widoczność" / Visibility queries option sets it (`src/launcher.cpp:210, 408-409`), and GTA V has no
  preset for it (`assets/presets.txt:1071-1073` sets only readback=1).
- In fast mode, `QueueZPDReport` (`command_processor.cc:1151-1186`) writes a guess into guest memory
  at once. The guess is the same report address's last real delta, or 1 sample when there is none,
  and a cached zero is never replayed. When the real result arrives, `PumpPendingRetire`
  (`command_processor.cc:1415-1470`) overwrites the report. It also *rebases* every report still
  pending, rewriting their values.
- So for a frame or more, the game can see "visible" for a query that was hidden. When the sun goes
  behind a bridge, a building or trees, the guess still says the sun is visible. Sun visibility drives
  glare, bloom and, in RAGE, plausibly the exposure.
- The fewer frames a second, the more reports are pending at once. That fits "more often at 14 than
  at 18 swaps/s".
- Full readback does not change this path. That fits the flashes staying with Full.
- **Test (no build):** set "Zapytania o widoczność" to "Ścisłe (wolniejsze)" (strict waits for the
  real result), then to "Fałszywe (lżejsze)".
  - If strict removes the flashes, this is the cause. The fix would then be a GTA V preset
    (`occlusion=3`, as Assassin's Creed already has: `assets/presets.txt:31-34`) or a better guess.
- **Fix 158 shows it:** a line every second while guesses said "visible" for hidden queries (or the
  reverse), plus 30 s counts and rebases.

### 2. A transient spike or NaN in the luminance chain from render-target emulation (now the most likely)

- On the default host render-target path (FBO; FSI is off unless chosen,
  `vulkan_render_target_cache.cc:223-227`, `src/launcher.cpp:150-160`), a 7e3 (k_2_10_10_10_FLOAT)
  target is an R16G16B16A16_SFLOAT image (`vulkan_render_target_cache.cc:1958-1960`).
- The pixel shader output goes there unclamped (`spirv_shader_translator_rb.cc:1277-1315`: only the
  gamma conversion; the FSI path clamps to [0, 31.875]: `render_target_cache.cc:254-258`). Blending
  happens in fp16. So negative values, values over 31.875, infinity and NaN can survive and accumulate
  across blended passes (deferred lights, particles, the sun) where the 360 clamps at every write.
- The value is clamped only when the target is dumped to EDRAM for a resolve
  (`UnclampedFloat32To7e3`, NClamp to [0, 31.875] with NaN to 0:
  `spirv_shader_translator_rb.cc:109-136`, used at `vulkan_render_target_cache.cc:3648, 3997`).
  So a resolved HDR picture cannot go over 31.875, but a NaN pixel turns black, and a value pushed
  negative by earlier blends ends lower than on the 360.
- Either way, a one-frame outlier in the average luminance makes the adapted value jump. The exposure
  then drops for every object at once and recovers over the adaptation time. That is the reported
  look.
- **Test (no build):** "FSI (dla gry)" = "Włączone" (accurate render targets: clamping and 7e3 as the
  360 has it). It is slower.
- **Fix 158 shows it:** a jump line for 061D2000/061D3000 (or the 64x64/80x48 keys) at the moment of
  a flash, with NaN, infinity, negative and over-31.875 counts and the source format of each key (it
  tells whether GTA V's HDR target is 7e3 at all).

### 3. Frame-time dependence (amplifier, not a cause by itself)

- GTA V's adaptation step depends on the frame time. At 14 swaps/s, with frames over 100 ms, one bad
  input moves the adapted value much further than at 30 fps.
- On its own a long frame converges to the right exposure. It cannot make the picture go dark and
  come back without a wrong input. It does explain "more often / more visible at the lower frame rate"
  under hypotheses 1 and 2.
- **Fix 158 shows it:** the last frame's length on every jump line, plus the longest frame and the
  number of frames over 100 ms per 30 s.

### 4. Shader translation of exp/log/rcp/saturate (low)

- The SPIR-V translator implements the Xenos rules:
  - 0 × anything = 0 for mul/mad/dp (`spirv_shader_translator_alu.cc:189-241, 463`);
  - SM3 max/min with NaN (`:375, 1149`);
  - logc, rcpc, rcpf, rsqc and rsqf clamping (`:1209-1299`).
- The PS5 precision reduction keeps infinity and NaN as they are (`:900-935`).
- Nothing frame-dependent here. A NaN or infinity this produced would show in hypothesis 2's counters.

### 5. Stale or partly written luminance texture in the texture cache (low)

- Resolves write shared memory on the GPU, in order, in one queue, and mark the range resolved for the
  texture cache. The next frame's sampling of 061D2000/061D3000 is ordered after the resolve.
- The only CPU write to these addresses is the readback copy:
  - In fast mode, it is the previous resolve's data.
  - With Full, it is the same frame's data, and the flashes persist with Full. So a stale copy
    reaching the GPU through guest memory is not the cause.
- Async submit (fix 134) is mode 0 (off) for GTA V (`gta2.log:399`; strict run `gta.log:398`), so there
  is no ordering change between a resolve and its use from fix 134. Readback batching (fix 133) only
  changes when readback copies are waited for, and Full waits for all of them.

## Upstream

- No public report of brief global darkening in GTA V was found:
  - Canary and master trackers and both game-compatibility repositories, including
    xenia-canary/game-compatibility#1140 and xenia-project/game-compatibility#445.
  - GTA IV, Red Dead Redemption and Max Payne 3.
- Related:
  - #1140: readback fixes the ocean flicker, PR #1016 fixed the sun glare and lens flare, and
    "some post-processing is too bright".
  - Max Payne 3 (#42): "rare rainbow-colored artifacts" in post-processing that clear up after a short
    time.
  - GTA IV (#14): dark interiors on the Vulkan FBO path that FSI does not have.
- None of these is the same symptom. Treat them as hints for hypotheses 1 and 2.

## Fix 158 (diagnostic, `1.0.6-work-fix158`)

- The patch is `tools/source-1.0.2/fixes/158-hdr-trace.patch`, with `include/xbox360ps5/hdr_trace.hpp`.
  The check is `tools/check-hdr-trace.cpp`.
- **Switch:** `/data/homebrew/PPSA83600/hdr-trace.txt` with the first line `on`. Without the file it
  is off and writes no lines. It is read again every 5 s, so it can be created or removed while the
  game runs.
- Optional lines:
  - `key 0623A000` traces a key of any size or format.
  - `max-bytes 2000000` raises the 64 KiB limit for float resolves, so the 640x384 HDR picture is
    traced too. That costs about 2 MB of CPU scanning per frame.
- Log lines, all starting `HDR trace (fix 158):`:
  - `on from ...`: the switch was read.
  - `key XXXXXXXX/N traced, from rtK <format>, Nx MSAA, exp bias B -> fF eE`: once per key. It says
    whether the HDR target is 7e3.
  - `HH:MM:SS.mmm UTC frame F (the last frame took T ms): key ... first texel|R,G,B mean A -> B, Kx
    its median ...; this copy: ..., NaN, inf, negative, over 31.875 (source)`: a jump.
  - `... back to V (within 1.5x of A) after N frames, at most Kx away`: the end of a jump.
  - `HH:MM:SS UTC occlusion queries in the last second: N answered, A guessed visible but hidden, B
    guessed hidden but visible`.
  - Every 30 s, a summary: the longest frame, frames over 100 ms, guess counts and rebases, and per
    key copies, first-texel min/median/max, largest value, NaN, infinity, negative, over-31.875 and
    jumps.
- **How to read a run with flashes:**
  - Jump lines at the flash times (console clock = the video's time):
    - For the 1x1 keys (061D2000/061D3000) only: the luminance input jumped. Look at which bigger key
      jumped too, and at NaN/negative, for hypothesis 2.
    - None, while the picture still darkens: the exposure comes from elsewhere (the CPU or the
      queries). Look at the query lines.
  - Query lines with "guessed visible but hidden" at the flash times would point back to hypothesis
    1, but strict mode already rules it out.

## Next console tests

1. **No build:** "FSI (dla gry)" = "Włączone" for GTA V, everything else as usual, the same route.
   - If the flashes are gone, hypothesis 2 (the FBO path's fp16 7e3 target) is confirmed.
   - FSI is slower, so also note the swaps/s.
2. **Build `1.0.6-work-fix158`:** `hdr-trace.txt` = `on`, default settings ("Zapytania o widoczność"
   back to "Szybkie" or the game's choice, readback Fast), the same route with a video. Note the
   video times of the flashes.
