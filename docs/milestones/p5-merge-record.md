# P5c merge record

P5 is complete and merged.

PR #13, `Add and qualify P5c event-driven pacing`, was squash-merged into `main` on 2026-09-14.

```text
final qualified behavior head: 046152dbaba56a0be3b1d2a1902fee6f2bbf9044
final PR head:                 8c3ca8cfab5664c6362259e03b3a233ff230bd10
squash merge commit:           90c6601fb4cbcfc50cbeff41e47310b510237cc2
```

The final PR head was mergeable, had no review submissions, no inline review threads, and no conversation comments. All eight relevant GitHub Actions workflows also passed on the final PR head: upstream provenance, P0, P1, P2, P3, P4, P5 rate sampler, and P5c pacer.

The behavior qualification remains anchored at `046152dbaba56a0be3b1d2a1902fee6f2bbf9044`, where the same complete workflow set passed with retained P5c/P3 artifacts. The six commits between that behavior head and the final PR head modified documentation only, and the final PR head was independently green as well.

P5c therefore closes with these properties:

- one process-wide one-shot `CLOCK_MONOTONIC` timerfd;
- bounded process-wide pacing deadline heap;
- no per-flow pacing timer/thread, fixed pacing tick, periodic flow scan, or busy spin;
- generation-safe queued identities and teardown cancellation;
- narrow data-send eligibility hook before native lwIP transmission;
- paced resume through native `tcp_output()`;
- production Reno remains zero-rate/unpaced;
- qualification-only fixed-paced Reno exercises the generic policy path;
- multi-flow, fast-loss, RTO, teardown, high-BDP, memory, CPU, and prior milestone regressions are qualified.

Representative retained high-BDP result:

```text
pacing policy:          65536 B/s
ACK-path delay:         750 ms
nominal BDP:            49152 B
current TCP window:     32768 B
payload:                262144 B exact
runtime CPU:            0.066%
max pacing lateness:    65199 ns
loss / timeout:         0 / 0
heap final:             0
```

The next development milestone is P6 tcp-shift BBR. P6 must use the existing generic observation/policy/pacing surfaces and must not move lwIP retransmission/recovery ownership into `src/cc/`.
