# RUNLOG

Every run: ~35 s, harness `run.py`, 1500 frames (30 s), seed 1 unless noted.
Raw stream = 1500 × 160 B = 240 000 B, overhead cap = 2.0×, miss cap = 1.0%.

| # | profile | delay_ms | miss % | overhead | change / why |
|---|---------|----------|--------|----------|--------------|
| 1 | A | 40 | 16.40% | 1.02x | Naive baseline (forward once, no recovery). 34 drops ≈ 2.3% gone forever; rest of misses are frames whose 10–40 ms network delay lands past the 40 ms deadline. INVALID as expected. |
| 2 | B | 40 | 75.53% | 1.02x | Naive baseline on B. Delay is uniform 20–80 ms, so most frames arrive after their 40 ms deadline; 5% loss on top. Confirms two separate problems: loss (needs redundancy) and jitter (needs a bigger deadline). |
| 3 | A | 80 | 0.67% | 1.97x | **First C++ rewrite, VALID.** Piggyback FEC: each packet carries frame i + copy of frame i−1, skipping the copy on every 16th packet (full 2×160 B duplication cannot fit under the 2.0× cap). Receiver dedupes and forwards on first arrival (player judges first arrival — no playout clock needed). Missed-frame analysis: most misses are %16==15 frames (their copy rides in the skipped packet) + frame 1499 (last frame, no successor packet). |
| 4 | A | 70 | 0.40% | 1.97x | Fixed the last-frame hole: sender reads DURATION_S and sends the final frame's packet twice. Copy mode still leaves the skip class exposed. |
| 5 | A | 70 | 0.20% | 1.97x | **Switched aux block from plain copy to XOR parity of frames (i−1, i−2)** — identical bytes, but each lost frame now has two carrier packets (i+1, i+2) instead of one, and 2-packet bursts decode by chaining parities. Recovered the skip-class misses; remaining 3 misses are recoveries whose carrier landed past the deadline. |
| 6 | A | 65 | 0.20% | 1.97x | Same misses as #5 — deadline not the binding constraint yet. |
| 7 | A | 60 | 3.40%* | 1.91x | *Polluted run: harness source lagged ~1 s (relay saw only 1457/1501 pkts and closed early; misses 1456–1499 are consecutive). WSL/OS scheduling hiccup, not protocol. Rerun below. |
| 8 | A | 60 | 0.60% | 1.97x | Clean rerun. m+1 recovery worst case is 20 ms + 40 ms jitter = 60 ms — right at the deadline, a few recoveries land late. |
| 9 | A | 55 | 0.60% | 1.97x | Still VALID. Lost frames recovered via packet i+1 when its delay ≤ 35 ms. |
| 10 | A | 50 | 1.27% | 1.97x | INVALID — recovery window too tight (needs next-packet delay ≤ 10 ms). **Profile A floor: 55 ms.** |
| 11 | B | 110 | 0.27% | 1.97x | XOR parity (S=2) on B, comfortable. |
| 12 | B | 100 | 0.87% | 1.97x | Still VALID, thin margin. Binding constraint: a lost frame's earliest recovery is packet i+1 at +20 ms + up to 80 ms jitter = 100 ms. |
| 13 | B | 95 | 1.13% | 1.97x | INVALID. **Profile B floor: 100 ms** — matches theory. |
| 14 | C (custom stress: burst_loss p_enter .02/p_exit .3/p_loss .7, spike 3%/+60ms, loss 3%) | 110 | 4.07% | 1.97x | Wrote a hostile profile since the relay supports burst_loss/spike and hidden profiles will differ from A/B. Gilbert-Elliott bursts drop 4–6 consecutive packets; contiguous parity chain XOR(i−1,i−2) cannot decode them. |
| 15 | C | 110 | 4.00% | 1.97x | **Changed parity to spread cover XOR(i−1, i−S), S=3** — singles still decode from packet i+1 (+20 ms, B floor intact), and bursts ≤ S decode by back-substitution through later parities. Barely moved C: its bursts average 3+ packets at 70% in-burst loss, so the parity chain itself gets shredded. |
| 16 | C | 130 | 3.13% | 1.97x | More deadline room recovers some burst tails. C has ~400 ms blackout episodes — no ≤2× scheme can recover a frame before any post-blackout packet arrives; this profile is unpassable at low delay by design. Kept as a worst-case reference. |
| 17 | C2 (moderate: burst p_enter .015/p_exit .5/p_loss .9, spike 2%/+40ms, loss 2%) | 110 | 1.73% | 1.97x | More plausible hidden-profile stand-in. Misses are 3–8-frame near-blackout bursts; FEC recovers bursts ≤ 2 at this deadline. |
| 18 | B | 100 | 0.93% | 1.97x | **NACK path enabled** (receiver detects gaps after a 25 ms reorder guard, NACKs via 47003, sender resends, both budget-guarded). At 100 ms deadline NACKs almost never fire on B (RTT doesn't fit) — no regression, 34 feedback bytes total. |
| 19 | C2 | 130 | 1.33% | 1.97x | NACK on: recovers burst tails whose deadlines are still reachable post-blackout (5 resends). Burst heads are information-theoretically gone at this delay. Defaults locked: XOR spread S=3, NACK on. |
| 20 | A | 110 (seeds 1/2/3) | 0.00% / 0.07% / 0.00% | 1.97x | Validation sweep at the grading delay, pure defaults. |
| 21 | B | 110 (seeds 1/2/3) | 0.20% / 0.33% / 0.73% | 1.97x | All VALID with margin. Worst seed leaves 0.27 pp headroom. |
| 22 | B | 110 (60 s, 3000 frames) | 0.73% | 1.97x | Double-duration sanity run — no drift, no counter issues, overhead flat. |

## Conclusion

- **Grade at `--delay_ms 110`.** Floors found: A = 55 ms, B = 100 ms; 110 keeps ≥ 10 ms margin on the harsher public profile and absorbs moderate delay spikes on unseen ones.
- Overhead is a flat **1.97×** by construction (5 B header + 160 B frame + 15/16 × 160 B parity), independent of network behavior; NACK traffic is negligible and budget-guarded.
- Mechanism, not profile tuning: XOR parity spread S=3 recovers singles at +20 ms and bursts ≤ 3; deadline-gated NACKs rescue post-burst tails; the receiver has no playout buffer at all (the player judges first arrival).
