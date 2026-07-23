# NOTES

The sender wraps each 20 ms frame in one UDP packet that also carries an XOR
parity of frames (i−1, i−3), skipping the parity on every 16th packet because
full 2×160 B duplication cannot fit the 2.0× byte cap (steady overhead ≈ 1.97×).
The receiver forwards every frame to the player the instant it is first
received or decoded — the player judges first arrival, so no playout clock is
needed — and dedupes duplicates. A lost frame decodes from packet i+1 (+20 ms)
or i+3, and loss bursts up to ~3 packets decode by back-substitution through
later parities. A budget-guarded NACK path (47003/47004) additionally resends
frames after burst gaps when their deadline is still reachable. **Grade at
`--delay_ms 110`**: profile A is valid down to 55 ms and B down to 100 ms, so
110 keeps ≥ 10 ms margin on the harsher profile. What breaks it: loss bursts
longer than ~3 packets (parity chain and resends both need surviving packets),
delay spikes above ~90 ms on the first copy plus a lost neighbour, and any
blackout longer than the deadline minus one RTT, which no ≤ 2× scheme can
recover in time.
