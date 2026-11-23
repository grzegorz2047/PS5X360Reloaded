# Fix 142d console results (build run 45185681740, label 1.0.6-work-fix142d, on 1.0.6_candidate_2)

One run per game, no switch files and no settings changed, about 2-3 minutes each. The lines below are one 30 s block per game, taken from the middle of each run, after loading.
Logs: `research/fix142d/` (GTA IV run 153650, RDR 154021, MW2 154410, plus their hang-watch files).

## Whole thread

| Game | swaps/s | draws/s | µs/draw | GPU thread executing | GPU waits |
|---|---|---|---|---|---|
| GTA IV (city) | 50-52 | 299,640 | 1.8 | 929 ms/s | 0 |
| RDR (town) | 29.7-30.0 | 179,741 | 1.9 | 992 ms/s | 0 |
| MW2 (mission) | (no fix 116 line) | 324,022 | 1.6 | 910 ms/s | 91 ms/s (memexport readback 84) |

## Fix 73 parts (ms/s)

| Game | primitives | render targets | pipeline | dyn. state+consts | bindings | vtx/memexport buffers | recording |
|---|---|---|---|---|---|---|---|
| GTA IV | 96.1 | 97.1 | 96.9 | 35.9 | 162.0 | 29.8 | 13.8 |
| RDR | 71.4 | 51.9 | 57.5 | 24.0 | 101.0 | 23.9 | 8.3 |
| MW2 | 118.1 | 78.9 | 75.2 | 41.2 | 127.3 | 56.4 | 15.1 |

## 142d: bindings (1 draw in 8 timed), ms/s

| Game | UpdateBindings µs | vkUpdateDescriptorSets | calls/s (descr. each) | float VS (B each) | float PS | fetch | constant sets | tex image infos | tex reuse check | set alloc+writes | binds | rest | dyn. state | system consts |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| GTA IV | 0.62 | 25.3 | 144,162 (4.4) | 18.8 (1404) | 3.7 | 12.2 | 18.4 | 14.0 | 9.1 | 18.4 | 21.9 | 41.8 | 11.2 | 22.2 |
| RDR | 0.66 | 20.4 | 75,156 (10.8) | 7.8 (339) | 3.1 | 6.1 | 11.3 | 13.2 | 6.9 | 9.4 | 11.3 | 24.8 | 7.0 | 15.2 |
| MW2 | 0.46 | 20.6 | 53,226 (18.1) | 7.3 (163) | 2.2 | 6.9 | 16.1 | 24.2 | 12.1 | 10.3 | 12.5 | 35.5 | 12.2 | 26.0 |

Texture sets reused (fix 104): GTA IV 37%, RDR 43%, MW2 67%. Constant sets written (fix 120): 0.1-0.3%.

## 142d: draws like the previous one

| Game | primitive processing same | render pass + framebuffer same | pipeline same | shaders same |
|---|---|---|---|---|
| GTA IV | 25.7% (2.5 ms/s same, 13.3 not) | **99.4% (86.2 ms/s same, 8.9 not)** | 80.4% (12.5 same, 10.9 not) | 81.8% |
| RDR | 35.6% (2.7, 9.7) | **99.1% (43.6, 6.9)** | 73.9% (6.2, 8.8) | 74.5% |
| MW2 | 39.9% (6.3, 17.6) | **99.9% (74.5, 2.4)** | 93.1% (12.4, 6.3) | 93.6% |

## Reading

- **The single-thread skip wins clearly.** Draws repeat the previous draw's render pass and framebuffer 99.1-99.9% of the time, yet that unchanged work still costs 86 ms/s in GTA IV, 44 ms/s in RDR and 75 ms/s in MW2. Adding unchanged pipeline work (12.5 / 6.2 / 12.4 ms/s) gives a ceiling of about **99 ms/s (~11% of the GPU thread) in GTA IV, ~50 ms/s (~5%) in RDR, ~87 ms/s (~10%) in MW2**, with no threads and no Vulkan ordering constraints.
- **The worker candidates are smaller.**
  - vkUpdateDescriptorSets is 20-25 ms/s (2-3%).
  - Float constants are 7-19 ms/s, VS mostly: GTA IV uploads 1.4 KB per VS upload, 112k/s.
  - Together that is about 3-5% of the thread, and it still needs one wait per submission.
- Primitive processing repeats only 26-40% of the time, so little to skip there.
- MW2 additionally waits 84 ms/s on memexport readbacks. That is a separate item.

Suggested order: the render pass/framebuffer and pipeline "same as previous" skip first, measured with the same lines. Then descriptor writes on a worker, only if the thread is still the limit.
