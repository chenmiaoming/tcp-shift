# P5: delivery-rate sampling and pacing prerequisites

Status: **active; delivery-ledger increment first**.

## Goal

Build and qualify the transport observations and runtime scheduling primitives required by model-based congestion control before adding BBR-specific modes or state.

P5 is not BBR. It must make delivery-rate samples, app-limited semantics, and pacing independently observable and memory-accounted through the already-qualified P4 controller boundary.

## Required sequence

1. high-resolution monotonic transmit/ACK timestamps;
2. cumulative delivered-byte accounting;
3. minimal retransmission-safe per-segment delivery metadata;
4. ACK-derived delivery-rate samples;
5. app-limited detection/marking;
6. loss/inflight sample publication through the generic CC observation surface;
7. one process-wide pacing scheduler, preferably min-heap + one timerfd;
8. fixed/per-flow/per-segment memory and CPU qualification.

## First increment: delivery ledger

The first increment deliberately does not change controller behavior or pace packets. It establishes trustworthy accounting underneath later rate samples.

Design constraints:

- do not enlarge upstream `struct tcp_seg` merely to attach project metadata;
- metadata belongs to the lwIP adapter, not `src/cc/`;
- allocate metadata lazily only for bound public PCBs that actually transmit data;
- key metadata by the existing `tcp_seg *`; RTO and fast retransmit move/reuse the same segment object, so retransmission must not create a second delivery record;
- create/update metadata only after `ip_output_if()` accepts a segment;
- consume metadata before upstream frees a fully acknowledged segment;
- a retransmitted segment may update transmission diagnostics but must be counted as delivered only once;
- normal close/reset/abort must leave zero live metadata.

A growable per-flow sidecar vector is preferred over one heap allocation per segment. The vector begins small and grows only as required, bounded by the configured send-queue limit. This avoids allocator overhead per segment and avoids charging idle flows for the maximum queue.

The minimum metadata needed for the later sampler is expected to include a segment identity plus first-transmit timestamp and delivery-state snapshots. Any additional field must justify its per-segment cost.

## Qualification for the first increment

CI must retain at least:

- metadata bytes per slot and peak slots per flow;
- first-transmit timestamps that are monotonic/nonzero;
- cumulative delivered bytes equal to unique acknowledged TCP payload bytes;
- retransmission events observed under the already-qualified fast-loss and RTO fault paths;
- no duplicate delivered-byte accounting after retransmission;
- zero live segment metadata after flow teardown;
- P0-P4 regressions green;
- P3-style memory measurement showing fixed, idle-flow, and active-flow deltas from the P4 baseline.

No rate in bytes/second is considered qualified until the next increment defines ACK aggregation/interval semantics.

## Later sampler semantics

The rate-sample increment will derive a transport-neutral sample containing at least delivered bytes, delivery interval, send interval, latest RTT observation where valid, prior inflight/loss state, and app-limited classification. It must explicitly define behavior for delayed ACKs, ACKs covering multiple segments, retransmitted data, partial ACKs, and sequence-number wrap.

## Pacing boundary

The Linux runtime, not `src/cc/`, owns pacing mechanics. The intended shape is one process-wide deadline heap plus one timerfd integrated into the existing single-owner epoll loop. There must not be one timerfd/thread per flow and pacing must not use busy spinning.

## Memory discipline

P4 leaves about 24 KiB/active-flow of tcp-shift process-PSS planning headroom in the conservative 32-MiB-host/25%-process-budget model. P5 must report actual incremental cost instead of assuming metadata is free.

Before retaining a metadata layout, report:

- fixed process increment;
- idle-flow increment;
- bytes per allocated metadata slot;
- typical and worst-case slots under the current `TCP_SND_BUF` / `TCP_SND_QUEUELEN`;
- active-flow PSS increment under controlled window pressure;
- post-drain allocator floor.

## Exit criteria

P5 is complete only when delivery-rate sampling, app-limited classification, and pacing are behaviorally qualified under real traffic; P0-P4 remain green; and fixed/per-flow/per-segment memory plus CPU/timer wakeups remain compatible with the constrained-host model.

## Stop signal

Stop and reassess before BBR if sampling/pacing requires rebuilding lwIP retransmission/SACK/recovery, materially enlarging the controlled lwIP patch, consuming the retained memory budget, or using per-flow timers/busy spinning for pacing accuracy.
