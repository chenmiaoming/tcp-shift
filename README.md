# tcp-shift

A userspace TCP congestion-control experiment and proxy for constrained VPS/container environments.

The first implementation targets gVisor netstack so that congestion control can be evaluated without changing the host kernel. The project will compare gVisor CUBIC with an experimental BBR implementation under reproducible high-BDP network conditions in CI, while recording throughput, CPU time, and peak RSS.

> Status: early development. Do not use on production traffic yet.
