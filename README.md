# The Flaky Network — Submission

Loss-tolerant, low-latency frame transport over a hostile UDP relay.
C++17, standard library only. **Grade at `--delay_ms 110`.**

## Build & run

```bash
make                                                  # -> ./sender ./receiver
python3 run.py --profile profiles/A.json --delay_ms 110
python3 run.py --profile profiles/B.json --delay_ms 110
```

Requires Linux/WSL/macOS, g++ (C++17), Python 3.10+.

## Results (30 s runs, seeds 1–3, defaults)

| Profile | delay_ms | miss % | overhead | result |
|---------|----------|--------------------|----------|--------|
| A (2% loss, 10–40 ms jitter) | 110 | 0.00 / 0.07 / 0.00 | 1.97× | VALID |
| B (5% loss, 20–80 ms jitter) | 110 | 0.20 / 0.33 / 0.73 | 1.97× | VALID |

Lowest valid floors found: A = 55 ms, B = 100 ms (B's floor is the physics:
earliest recovery for a lost frame = next packet at +20 ms + 80 ms worst-case
jitter). 110 ms keeps ≥ 10 ms margin on the harsher public profile.

## Design in one paragraph

Each media packet carries frame *i* plus a 160 B **XOR parity of frames
(i−1, i−3)**, omitted on every 16th packet because full duplication cannot fit
the 2.0× byte cap (2×160 B = 320 B is the entire budget before headers) —
overhead is a flat 1.97× by construction. A single loss decodes from the very
next packet; bursts up to ~3 packets decode by back-substitution through later
parities (cascading worklist decoder). The receiver has **no jitter buffer**:
the harness player judges first arrival per seq, so every frame is forwarded
the instant it is received or decoded, with duplicates suppressed. A
deadline-gated, budget-guarded **NACK path** (47003→47004) resends frames
after burst gaps when their deadline is still reachable.

## Files

| File | What |
|------|------|
| `sender.cpp`, `receiver.cpp`, `Makefile` | Submission source (`make` → `./sender`, `./receiver`) |
| `RUNLOG.md` | All 22 experiments: profile, delay, miss %, overhead, change + why |
| `NOTES.md` | Design + grading delay + failure modes, ≤ 10 sentences |
| `SUMMARY.html` | Architecture and design-choice deep-dive |
| `run.py`, `relay.py`, `endpoints.py`, `common.py`, `score.py` | Harness (unmodified) |
| `sender.c`, `receiver.c` | Original naive baselines (unused) |
| `profiles/A.json`, `profiles/B.json` | Public practice profiles |
| `profiles/C.json`, `profiles/C2.json` | My burst/spike stress profiles (see RUNLOG #14–19) |
