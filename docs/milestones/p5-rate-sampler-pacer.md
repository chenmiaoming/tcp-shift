# P5: delivery-rate sampling and pacing prerequisites

Status: **active; delivery ledger runner-qualified, rate sampler next**.

## Goal

Build the transport observations and runtime scheduling primitives required by model-based congestion control before adding BBR-specific modes or state.

P5 is not BBR. It must make unique delivery accounting, ACK delivery-rate samples, app-limited semantics, and pacing independently observable and memory-accounted through the already-qualified P4 controller boundary.

## Required sequence

1. high-resolution monotonic transmit/ACK timestamps;
2. cumulative unique delivered-byte accounting;
3. minimal retransmission-safe per-segment delivery metadata;
4. ACK-derived delivery-rate samples;
5. app-limited detection/marking;
6. loss/inflight sample publication through the generic CC observation surface;
7. one process-wide event-driven pacing scheduler;
8. fixed/per-flow/per-segment memory, CPU, and wakeup qualification.

## P5a: delivery ledger — runner-qualified

The first increment does not change controller behavior or pace packets. It establishes trustworthy accounting underneath later rate samples.

Design:

- upstream `struct tcp_seg` is not enlarged;
- metadata belongs to `src/lwip/cc_adapter.*`, not `src/cc/`;
- only bound public PCBs that actually transmit data allocate metadata;
- metadata is keyed by the stable `tcp_seg *`; fast retransmit and RTO reuse the same slot rather than creating a second delivery record;
- the sidecar vector starts at 8 slots and grows lazily, bounded by current `TCP_SND_QUEUELEN=90`;
- each slot is 32 bytes: segment identity, first-transmit monotonic timestamp, delivered snapshot, and delivered-mstamp snapshot;
- successful segment transmission records or reuses the slot only after the output path accepts the send;
- a fully acknowledged segment consumes its slot before upstream frees the `tcp_seg`;
- normal teardown, reset, or abort releases all remaining metadata.

The existing repository-owned lwIP patch remains confined to the same three upstream files introduced by P4. P5a adds transport observation calls in `tcp_in.c` and `tcp_out.c`; it does not copy or replace lwIP retransmission/recovery logic.

### CI qualification

The original P5 workflow accidentally allowed a failed parser check to be masked by a `check_delivery | tee` pipeline and its field parser matched `live_slots` inside `peak_live_slots`. This was fixed before qualification: exact token parsing is now used, the checker writes the summary before `cat`, and hidden `.build` diagnostics are explicitly retained.

Final behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed:

- upstream provenance run `34821205386`;
- P0 run `34821205401`;
- P1 run `34821205357`;
- P2 run `34821205366`;
- P3 run `34821205396`;
- P4 run `34821205367`;
- P5 run `34821205375`, job `103903019956`.

P5 artifact `10338108552` retains the corrected fail-closed summaries and full normal / fast-loss / RTO diagnostics.

Retained delivery-ledger evidence:

```text
normal:
first_tx_events=184 retransmit_events=0 acked_segment_events=184
delivered_payload_bytes=262144
peak_slots_per_flow=23 peak_capacity_slots_per_flow=32 live_slots=0

fast-loss:
first_tx_events=180 retransmit_events=1 acked_segment_events=180
delivered_payload_bytes=262144
peak_slots_per_flow=18 peak_capacity_slots_per_flow=32 live_slots=0

rto:
first_tx_events=180 retransmit_events=4 acked_segment_events=180
delivered_payload_bytes=262144
peak_slots_per_flow=18 peak_capacity_slots_per_flow=32 live_slots=0
```

All three paths also require:

```text
metadata_bytes_per_slot=32
metadata_alloc_failures=0
metadata_misses=0
metadata_abandoned_slots=0
clock_errors=0
timestamp_regressions=0
```

The retransmission cases therefore prove that retransmitting a segment does not double-count delivered payload.

### Memory cost

P3 was rerun with P5a enabled. Compared with the final P4 adapter baseline:

```text
                         P4             P5a
warm fixed PSS          335 KiB         343 KiB
fully-window slope      37.148438       37.679688 KiB/flow
128-active projection   5090 KiB        5166 KiB
8-MiB budget remaining  3102 KiB        3026 KiB
headroom / active flow  24.234 KiB      23.641 KiB
```

The delivery ledger therefore consumes about 8 KiB fixed PSS and about 76 KiB of the conservative 128-flow process budget in this runner sample. The constrained-host admission model still passes comfortably. Active memory remains dominated by the qualified 32-KiB TCP-window residency, not by the delivery sidecar.

## P5b: ACK delivery-rate sampler — next

The next increment derives a transport-neutral ACK sample from the P5a snapshots. It must define and qualify:

- delivered bytes over a measured delivery interval;
- send interval and ACK interval semantics;
- delayed ACKs and one ACK covering multiple segments;
- retransmitted data without duplicate delivered accounting;
- partial ACK behavior;
- sequence-number wrap safety;
- latest RTT observation when valid;
- prior inflight/loss observations;
- app-limited marking and exit semantics.

No bytes/second estimate is considered qualified until these rules are exercised through real public-side TCP traffic and deterministic loss/RTO tests.

## Pacing boundary: event driven, not polled

The runtime, not `src/cc/`, owns pacing mechanics. Pacing must extend the existing single-owner epoll architecture rather than introduce periodic polling.

Target design:

- one process-wide min-heap keyed by each paced flow's next eligible send deadline;
- one process-wide `timerfd` using `CLOCK_MONOTONIC`;
- one-shot/absolute arming to the earliest pending pacing deadline;
- the timerfd is registered in the existing epoll loop;
- no timerfd or thread per flow;
- no fixed 1-ms/10-ms pacing tick;
- no busy spin;
- when no pacing deadline exists, the timerfd is disarmed and the runtime sleeps on real fd/timer events.

The current runtime is already deadline-driven for lwIP timers: `epoll_wait()` derives its timeout from `sys_timeouts_sleeptime()` rather than polling at a fixed tick, and TUN `EPOLLOUT` is armed only while a real TX backlog exists. P5c should preserve or improve that property. A later cleanup may unify lwIP and pacing deadlines behind one one-shot timerfd if CI proves that doing so reduces wakeups without changing timeout behavior.

## Exit criteria

P5 is complete only when:

- retransmission-safe delivery accounting — **qualified**;
- ACK delivery-rate samples — pending;
- app-limited classification — pending;
- loss/inflight sample publication — pending;
- event-driven process-wide pacing — pending;
- P0-P4 regressions remain green;
- fixed/per-flow/per-segment memory, CPU, and timer wakeups remain compatible with the constrained-host model.

## Distance to BBR

After P5a, two prerequisite increments remain before the BBR controller itself should become the active work item:

1. P5b rate sampling + app-limited semantics;
2. P5c event-driven pacing + integrated sample publication.

P6 then implements the tcp-shift BBR model/state machine and compares bandwidth estimate, min RTT, cwnd, pacing rate, mode transitions, loss response, throughput, CPU, and memory against native Linux reference runs. The remaining architectural risk is primarily sampler/pacer correctness under high BDP and loss, not ownership of the TCP endpoint or retransmission machinery.

## Stop signal

Stop and reassess before BBR if sampling/pacing requires rebuilding lwIP retransmission/SACK/recovery, materially enlarging the controlled lwIP patch, consuming the retained memory budget, or using per-flow timers / periodic polling / busy spinning for pacing accuracy.
