# Fix 142c console results (GTA IV, build run 45148979687, label 1.0.5-work-fix142c)

Same city spot as the 142/142b runs, video clock 60 Hz, async-submit.txt = mode 1, game restarted after each change.
Logs: `research/fix142c/run-M.log` ... `run-Q.log`. Swaps/s are fix 116's 30 s lines after the first two (loading).

| Run | parallel-record.txt | swaps/s | decode | GPU thread executing | starved | coherency/frame | drains/frame | decoder woken | packet records with their draws | rest of batches |
|---|---|---|---|---|---|---|---|---|---|---|
| M | none | 43-48 | - | - | - | - | - | - | - | - |
| N | mode 2 | 40-41 | 156 | 833 | 0.0 | 83.0 | 7.1 | 0/s | 725 ms/s | 39 ms/s |
| O | mode 1 | 46-48 | 209 | 950 | 4.2 | 83.1 | 7.9 | 233/s | 832 ms/s | 44 ms/s |
| P | mode 1, sibling 1 | 44 (short run) | 264 | 964 | 3.9 | 83.2 | 7.6 | 208/s | 855 ms/s | 43 ms/s |
| Q | mode 1, waits 0 | 43-49 | 229 | 943 | 27.9 | 0 | 90.9 | 1204/s | 829 ms/s | 45 ms/s |

Per frame (N): batches 455, ops 467 (was ~78,800 in 142b), records 78,303. Register waits by register: none (all 83 were coherency waits). No stalls in any run. The switch files are removed from the console again.

## Findings

- **142c does what it set out to do.**
  - Ops per frame fell from ~80,000 to ~460.
  - The 83 register waits are all COHER_STATUS_HOST coherency waits, and they now pass without a drain (drains 91 -> 7-8 per frame).
  - Mode 2 improved from 36-38 (142b, run H) to 40-41.
- **Mode 1 now matches or slightly beats no split.** O gives 46-48 against M's 43-48. The gain is within scene noise, so it is not a clear win yet.
- **Drains no longer matter.** Q forces them back (91/frame, 1,204 wakes/s) and runs about the same as O (43-49).
- **Placement still doesn't matter** (P; a short run).
- **The GPU thread is still saturated** at ~950 ms/s. Its time is now almost all "packet records with their draws": 830-855 ms/s in mode 1, against 725 in mode 2. The "rest of the batches" is small (~44 ms/s). So the remaining limit is draw recording on the GPU thread, which the decoder does not offload. This is your stage 2 (option C: the order-independent part of IssueDrawInner on workers).
- Why is "packet records with their draws" ~110 ms/s higher in mode 1 than in mode 2? Possibly cache traffic, since the batch data is written on another core. That would explain why the split's gain is eaten.
