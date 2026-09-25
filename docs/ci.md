# CI architecture

`tcp-shift` CI is evidence-driven: upstream provenance is independent from product behavior, diagnostics survive failure, and observed resource behavior becomes a hard gate only after a workload is defined and measured.

## Principles

1. Pin upstream exactly and retain provenance.
2. Keep project-owned lwIP modifications explicit and mechanically bounded.
3. Validate source/configuration contracts before network behavior.
4. Preserve all earlier milestone regressions while adding later transport features.
5. Retain failure artifacts rather than rerunning away useful evidence.
6. Separate process-PSS planning from full-host memory claims.
7. Keep pure controller logic independently buildable from lwIP/Linux runtime code.
8. Qualify real packet-path events for ACK/loss/RTO/rate/pacing claims; synthetic controller calls alone are insufficient.
9. Harness failures must fail closed; parser/check failures must propagate nonzero status even when summaries are retained.
10. Timing CI must reject busy spin, fixed periodic polling, and per-flow timer designs that inflate wakeups.

## Upstream provenance

The production baseline is `.lwip-baseline`, pinning lwIP commit:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

`scripts/fetch-lwip.sh` records pristine critical-source hashes, applies `patches/lwip-p4-cc-hooks.patch` and then `patches/lwip-sack-recovery.patch`, and provenance CI proves exact pin, both patch identities, reverse application of the chain, bounded modified-file scope, no untracked dependency files, and byte-identical independent reapplication.

The controlled patch chain remains confined to `src/core/tcp.c`, `tcp_in.c`, and `tcp_out.c`; no new upstream file is added. The sender-SACK increment is dormant in default builds and is enabled only by `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY=ON` in dedicated qualification jobs. Because that option changes upstream `struct tcp_pcb`, CMake propagates the definition publicly to every project target that consumes lwIP headers, preventing cross-target PCB ABI skew.

## P0/P1/P2 regression layers

P0 builds the constrained dual-stack `NO_SYS=1` source allowlist with warnings-as-errors, source/configuration contracts, binary/RSS gates, and clean-runner smoke.

P1 qualifies privileged L3 TUN and ingress lifecycle behavior, IPv4/IPv6 packet paths, bounded TX retry, nonfatal RX drops, exact ingress DNAT, cleanup/collision invariants, extension-header-safe IPv6 TCP matching, and routed ICMPv6 PTB learning.

P2 qualifies the public-stream -> `127.0.0.1` bridge for IPv4/IPv6, bidirectional backpressure, half-close, refusal/reset recovery, concurrent flows, active shutdown, and repeated reuse. Integrated controller ownership requires:

```text
cc_bindings == bridge_accepts
cc_bind_failures == 0
cc_ack_events > 0
cc_controller_errors == 0
```

## P3 resource qualification

P3 measures tcp-shift process residency and CPU separately from backend Linux kernel/application memory. Workloads include 0/8/32/64/128 idle flows, both active-window directions, repeated 128-flow connect/drain rounds, idle/small-operation CPU, and constrained-host process-PSS modeling.

Latest P5c-head run `34870859880`, job `104066205524`, artifact `10358454897`:

```text
warm fixed process PSS:           367 KiB
idle 128-flow PSS:                359 KiB
conservative active slope:        37.773438 KiB/flow
128 active projected PSS:        5202 KiB
8-MiB process budget remaining:  2990 KiB = 23.359 KiB/flow
3x128 first->last drain growth:   5 KiB
idle CPU:                        0 ticks/s
small-operation CPU:             34.179688 us/op
```

The model remains tcp-shift process PSS only. Backend kernel/application and provider memory are excluded.

## P4 controller qualification

`src/cc/` is built independently with a restricted pure-C surface. Integrated recovery uses external TUN packet loss rather than calling controller loss/RTO handlers directly. Fast-loss must produce controller loss without falling through to RTO; the RTO workload must produce a real timeout and recover on the same connection.

## P5a/P5b observation qualification

P5a qualifies retransmission-safe unique delivery accounting through natural, fast-loss, and RTO traffic. P5b adds transport-neutral ACK delivery-rate samples and the event-driven app-limited workload.

Required delivery/rate invariants include exact payload bytes, 56-byte sidecar metadata, zero allocation/miss/abandon/clock errors, zero live slots at teardown, positive valid rate samples, and zero invalid samples in qualified natural/recovery fixtures.

The application-pause workload samples CPU entirely inside an intentional backend pause; app-limited enter/sample/exit must occur without a product polling loop.

## P5c event-driven pacing qualification — complete

`.github/workflows/p5c-pacer.yml` qualifies the process-wide scheduler independently and through the real bridge datapath.

Final behavior head:

```text
046152dbaba56a0be3b1d2a1902fee6f2bbf9044
```

All workflows on that same head passed:

```text
upstream provenance  34870859838  success
P0                   34870860022  success
P1                   34870859876  success
P2                   34870859828  success
P3                   34870859880  success
P4                   34870859949  success
P5 rate sampler      34870859809  success
P5c pacer            34870859911  success
```

P5c run `34870859911`, job `104066140125`, artifact `10358951402` retains all scheduler diagnostics.

### Scheduler contract

Standalone tests require:

- one nonblocking/cloexec monotonic timerfd;
- min-heap deadline ordering;
- no rearm for a later insert;
- rearm when the earliest entry changes;
- generation-specific cancellation;
- empty-heap disarm;
- a real blocking wait awakened by one-shot expiration;
- timer/heap/release/cancel/lateness telemetry.

### Adapter lifecycle contract

A deterministic test schedules one future deadline, unbinds before expiry, requires exactly one cancellation, then submits the old flow/generation as a stale release:

```text
pacer_adapter_lifecycle=ok schedules=1 cancels=1 stale_release_safe=1
```

This test caught a real ext-arg API misuse in manual unbind. The fix keeps pinned lwIP's required non-NULL callback table and clears only ext-arg data.

### Epoll contract

The timerfd is registered with the existing single-owner loop. The gate requires a real pacing wake/release and then zero idle pacing wakeups after the heap drains. Normal lwIP timeout wakeups remain intact.

### Real datapath contract

The qualification-only fixed-paced Reno policy publishes `65536 B/s` through the generic CC policy. Production Reno remains zero-rate/unpaced. No runtime test override forces the pacing rate.

The real bridge gate requires positive deferrals/releases, one timerfd, zero final heap, zero scheduler/stale/controller errors, and exact payload integrity.

### Multi-flow gate

Four concurrent 64-KiB flows must produce overlapping deadlines. Retained result:

```text
payload total=262144 B integrity=ok
deferrals=384
scheduled=released=resume=176
heap_peak=4 heap_current=0
max_lateness_ns=43136
errors=0
```

### Loss/RTO paced recovery gates

Fast-loss retained:

```text
payload=131072 B
loss_events=1 timeout_events=0 retransmit_events=1
deferrals=186 resume=90
max_lateness_ns=79624
heap_current=0 errors=0
```

RTO retained:

```text
payload=131072 B
timeout_events=2 retransmit_events=4
deferrals=187 resume=91
max_lateness_ns=57626
heap_current=0 errors=0
```

These are real native lwIP recovery paths; retransmissions are paced without moving recovery execution into project code.

### Sustained high-BDP/window-pressure gate

The test reuses the P2 bridge harness and applies test-only `netem` delay on the TUN:

```text
pacing policy:          65536 B/s
ACK-path delay:         750 ms
nominal BDP:            49152 B
current TCP window:     32768 B
payload:                262144 B
runtime wall:           15.067229 s
runtime CPU:            0.01 s = 0.066%
max pacing lateness:    65199 ns
loss/timeout:           0/0
scheduler/stale errors: 0/0
heap final:             0
```

The observed rate sample is about `43682 B/s`, consistent with a 32 KiB window at roughly 750 ms. This distinguishes genuine BDP/window pressure from a scheduler-only sleep test.

The gate does not enable window scaling and does not claim full utilization of arbitrary high-BDP links; it qualifies pacer correctness and wakeup efficiency under the current transport limits.

## Gate policy

A failure must identify the violated contract. Threshold changes require evidence explaining whether the implementation legitimately grew or the previous threshold was wrong. Do not weaken a gate solely to make CI green.

When a workflow exposes a harness assumption rather than a product defect, fix the harness while retaining or strengthening the intended assertion and preserving the diagnostics that established the distinction.

For merge decisions, a GitHub `success` conclusion is necessary but not sufficient when logs contradict the intended qualification assertion. Final milestone qualification requires fail-closed assertions plus all required prior regressions on the same behavior head, or an explicitly compared docs-only tail.
