# Fix 142b console results (GTA IV, build run 45137416431, label 1.0.5-work-fix142b)

Same city spot as the 142 runs, game restarted after each change. async-submit.txt = mode 1 in all runs.
Logs: `research/fix142b/run-H.log` ... `run-L.log` (the 142 runs A-G are in `research/fix142/`).
Swaps/s are fix 116's 30 s lines after the first two (loading).

| Run | parallel-record.txt | swaps/s in game | decode | GPU thread executing | starved | drains/frame | woken after | decoder woken |
|---|---|---|---|---|---|---|---|---|
| B (142 build) | none | 42-45 | - | 987-990 | - | - | - | - |
| H | mode 2 | 36-38 | 150 | 721 | 0.0 | 90.1 | 0 us | 0/s |
| I | mode 1 | 40-42 | 298 | 952 | 30.3 | 91.1 | 9.2 us | 1079/s |
| J | mode 1, waits 0 | 41-45 | 290 | 951 | 31.0 | 91.1 | 8.9 us | 1127/s |
| K | mode 1, waits 2 | 42-45 | 290 | 948 | 33.3 | 90.9 | 9.0 us | 1188/s |
| L | mode 1, sibling 1 | 40-44 | 282 | 956 | 22.0 | 91.2 | 9.8 us | 1110/s |
| D (142, for reference) | mode 1 | 12-13 | 80 | 988 | 6.9 | (wait 89.3) | - | ~1.1 M/s |

Wait outcomes per frame (H, I, K, L; waits 1 or 2): passed early 0.00, register 83, unmet 3, write pending 3. ib-after-wait 0.00 in every run.

## Findings

- **The single wake fixes the mode 1 collapse.** 12-13 -> 40-45 swaps/s, decoder woken ~1,100/s instead of ~1.1 million. At ~43 fps and ~91 drains per frame there are ~3,900 drains/s, so most drains need no wake at all (the GPU thread is already there). Woken-after latency ~9 us.
- **No gain over fix 134 alone yet.** Mode 1 with 142b is 41-45, the same as run B (42-45, no fix 142). The GPU thread still reports executing ~950 ms/s, not the ~720 of mode 2 minus the decode share, so it is still the bottleneck.
- **GTA IV's waits are register waits, not memory waits.** 83 of 89 waits per frame are `register`, so 142b's "met memory wait passes early" never applies (passed early 0.00). Each of them still drains: ~91 drains per frame. `waits 0/1/2` make no difference here (J = I = K), and ib-after-wait costs nothing in GTA IV (0.00).
- Placement (L, sibling 1) does not matter: same FPS within noise.
- Mode 2 (H) costs ~15% vs B (36-38 vs 42-45), a bit more than run C on 142 (38-40).
- No hangs, no stalls, no visual problems reported in any run. The switch files are removed from the console again.

## Questions for the next step

- Which register do those 83 waits poll? If it is a register the decoder can evaluate (e.g. one written earlier in the same stream, or a GPU status register the emulator answers synchronously), the decoder could pass it early too, removing most drains.
- Why is the GPU thread still at ~950 ms/s executing in mode 1 when mode 2 shows decode at ~150-170 ms/s of its time? Either the ops path costs about as much as the decode it replaced, or the drains' idle time is counted as executing.
