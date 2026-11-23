# Fix 142 console results (GTA IV, build run 45126559706, commit e33de55)

Game: GTA IV, same city spot, ~1.5-5 min per run, game restarted after each settings change.
Logs: `research/fix142/run-A.log` ... `run-G.log`. Swaps/s are fix 116's 30 s lines (the first one or two are the loading screen).

| Run | parallel-record.txt | async-submit.txt | swaps/s in game | GPU thread executing | fix 142 line |
|---|---|---|---|---|---|
| A | none | mode 0 | 47.7 (short run) | 691 ms/s | off |
| B | none | mode 1 | 44-45, 42-44 later | 987-990 | off |
| C | mode 2 | mode 1 | 38-40 | 992 | decode 166-170 ms/s, drains 621, wait 89.2/frame |
| D | mode 1 | mode 1 | **12-13** | 988 | decode 80-82, drains 916, starved 6.9, wait 89.3/frame, ib-write 2.0 |
| E | mode 1 + sibling 1 | mode 1 | **12-13** | 989 | decode 90, drains 908, starved 5.6, wait 89.4/frame |
| F | mode 1 + filter 0 | mode 1 | **13-14** | 989 | decode 71, drains 928, starved 6.5, wait 89.4/frame, 0% dropped |
| G | mode 1 | mode 0 | **12-13** | 988 | decode ?, drains 918, starved 7.2, wait 89.2/frame |

Run D detail (last line): batches 764/frame, draws 7684, IBs 76.8, ops 87,681 (2,839 KiB) per frame; registers 9.05 M/s, 66% dropped unchanged; ahead avg 64 KiB, max 407 KiB; packets handed over 334,332/s, predicated out 69,769/s.

## Findings

- **No deadlock, no hang, no visual problem reported.** The fixed FIFO held in all four mode-1 runs. Run F's hang-watch "WAITING ... 13.1 frames a second, unchanged for two minutes" is the slowness note, not a stall.
- **Mode 2 is correct** and costs ~10% vs B (38-40 vs 42-45). Decode share on one thread ≈ 170 / 990 ≈ 17%, so the Amdahl bound is ~+20%.
- **Mode 1 is ~3.3x slower than mode 2** (12-13 vs 38-40 swaps/s), in every variant:
  - placement does not matter (E = D), so it is not cross-CCX latency;
  - the filter does not matter (F = D);
  - fix 134's worker does not matter (G = D).
- The decode thread spends ~92% of its time in `drains` (908-928 ms/s), and there are ~89 `wait` serializations per frame (WAIT_REG_MEM). The GPU thread shows `executing` ~988 ms/s while producing a third of the frames of mode 2. So the GPU thread is busy (probably spinning or polling inside "executing"), not starved.
- Likely cause: each WAIT_REG_MEM drains the whole pipeline, and the hand-off/wake-up latency of a drain is paid ~89 x 13 ≈ 1,160 times a second, plus per-op overhead on the consumer side. Things to check:
  - whether the GPU thread's wait for the FIFO busy-spins and is counted as executing;
  - the cost of the drain round-trip (condition variable or futex wake vs spin);
  - whether WAIT_REG_MEM on registers or memory that the decoder can evaluate itself needs a full drain at all.

Next console step suggested: a mode 1 build with timing around a drain (decoder wait → GPU thread reaches the op → decoder resumes), so the per-drain cost is measured directly.
