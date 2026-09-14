# P5: delivery-rate sampling and pacing prerequisites

Status: **active; P5a delivery ledger and P5b rate/app-limited sampler runner-qualified; P5c event-driven pacer next**.

## Goal

Build the transport observations and runtime scheduling primitives required by model-based congestion control before adding BBR-specific modes or state.

P5 is not BBR. It must make unique delivery accounting, ACK delivery-rate samples, app-limited semantics, and pacing independently observable and memory-accounted through the already-qualified P4 controller boundary.

## Required sequence

1. high-resolution monotonic transmit/ACK timestamps — qualified;
2. cumulative unique delivered-byte accounting — qualified;
3. retransmission-safe per-segment delivery metadata — qualified;
4. ACK-derived delivery-rate samples — qualified;
5. event-driven app-limited detection/marking — qualified;
6. transport-neutral sample publication through the generic CC observation surface — qualified;
7. one process-wide event-driven pacing scheduler — next;
8. fixed/per-flow/per-segment memory, CPU, and wakeup qualification — ongoing per increment.

## P5a: delivery ledger — runner-qualified

P5a established trustworthy accounting without changing controller behavior or pacing packets.

The project keeps metadata outside upstream `struct tcp_seg`. Bound public PCBs allocate a lazy sidecar vector keyed by stable `tcp_seg *`, starting at 8 entries and bounded by current `TCP_SND_QUEUELEN=90`. P5a slots were 32 bytes and stored segment identity, first-transmit monotonic timestamp, and delivered-state snapshots. Retransmission reused the same slot; teardown required zero live entries.

Final behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed provenance/P0/P1/P2/P3/P4 and P5 run `34821205375`, job `103903019956`. Artifact `10338108552` retained:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
rto:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

All modes require zero allocation failures, metadata misses, abandoned slots, clock errors, and timestamp regressions.

The original P5 workflow exposed a false-green harness bug before qualification: field parsing matched `live_slots` inside `peak_live_slots`, and `check_delivery | tee` masked the failed checker status. The final checker parses exact tokens, fails closed, and explicitly retains hidden `.build` artifacts.

## P5b: ACK delivery-rate sampler + app-limited — runner-qualified

P5b derives a transport-neutral ACK rate observation while leaving conventional Reno policy unchanged.

### Observation contract

`struct tcp_shift_cc_ack` now carries a rate sample with:

- selected delivery rate in bytes/second;
- selected interval;
- send-phase interval;
- ACK-phase interval;
- RTT when valid;
- newly delivered payload bytes;
- prior inflight;
- VALID / APP_LIMITED / RETRANSMITTED / RTT_VALID flags.

The generic CC layer sees none of lwIP's PCB/segment objects. Reno ignores these fields and continues to use `acked_bytes` only.

### Sidecar and ACK accounting

P5b expands each lazy sidecar entry to 56 bytes:

- stable segment identity;
- host-order payload sequence start and payload length/progress;
- first-transmit timestamp;
- send-phase timestamp snapshot;
- delivered/delivered-mstamp snapshots;
- prior inflight;
- app-limited/retransmission flags.

Pinned lwIP updates `pcb->lastack` before the project ACK hook and frees acknowledged segments afterwards. The adapter therefore sees the new cumulative ACK while relevant sidecars are still live. It computes exact newly delivered payload from sequence overlap/progress before upstream frees segments. FIN sequence space is excluded. This defines partial-ACK accounting without moving queue ownership into project code.

Retransmission reuses a sidecar and marks it retransmitted. RTO qualification proves delivered payload remains unique. Retransmitted rate candidates do not publish RTT, following Karn-style validity.

### Rate interval semantics

The adapter derives both a send-phase interval and an ACK-phase interval and selects the larger interval for the rate estimate. This follows the mature TCP rate-sampling principle used to resist ACK compression. Invalid/zero-interval samples are diagnosed rather than fed to a controller as valid bandwidth evidence.

### App-limited semantics are event driven

The bridge marks a possible application-limited gap only when its real backend `MSG_PEEK | MSG_DONTWAIT` read path reaches `EAGAIN`. The adapter then requires no unsent lwIP data and available public transport capacity before storing a Linux-style delivered+inflight exit marker. Delivery past that marker clears app-limited state.

There is no periodic flow scan and no app-limited timer.

### Final qualification

Final behavior head:

```text
327efe7e29adfc230e7d201b466f2bd4980e976c
```

All layers passed:

```text
upstream provenance  34843587049  success
P0                   34843587091  success
P1                   34843587026  success
P2                   34843587113  success
P3                   34843587033  success
P4                   34843587045  success
P5                   34843586990  success
```

P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained:

```text
normal:
first_tx=184 retransmit=0 acked_segments=184 delivered=262144
samples=138 valid=138 invalid=0 max_rate_bytes_per_sec=696998778

fast-loss:
first_tx=180 retransmit=1 delivered=262144
samples=121 valid=121 invalid=0 cc_loss_events=1 recovery=ok

rto:
first_tx=180 retransmit=4 delivered=262144
samples=135 valid=135 invalid=0 retransmitted_samples=3
cc_timeout_events=2 recovery=ok
```

Application-pause workload:

```text
first_burst=4096
pause_seconds=0.8
second_burst=131072
delivered=135168
rate_samples=71 valid_samples=71 invalid_samples=0
app_limited_samples=7
app_limited_enters=2
app_limited_exits=2
pause_cpu_ticks=0
event_driven=ok
```

The 300-ms CPU sample occurs entirely inside the backend application's pause. Zero ticks demonstrates that qualification did not require a product-side busy loop or fixed app-limited polling timer.

### P5b memory/CPU cost

P3 run `34843587033`, job `103974029078`, artifact `10346739278` retained:

```text
warm fixed process PSS: 347 KiB
idle 128-flow delta: 65 KiB
public->backend active: 36.5 KiB/flow
backend->public active: 37.125 KiB/flow
conservative fully-window slope: 37.710938 KiB/flow
128-active projected PSS: 5174 KiB
8-MiB process budget remaining: 3018 KiB = 23.578 KiB/flow
3x128 drain PSS: 342 -> 346 -> 347 KiB
first-to-last drain growth: 5 KiB
idle CPU: 0 ticks/s
```

P5b therefore consumes little additional process headroom relative to P5a; active residency remains dominated by TCP-window/pbuf/send-segment state.

## P5c: event-driven pacer — active next

The runtime, not `src/cc/`, owns pacing mechanics. P5c must turn a nonzero controller pacing-rate policy into actual send eligibility while extending the existing single-owner epoll architecture rather than adding periodic polling.

Target design:

```text
paced flow eligibility
        |
        v
process-wide min-heap keyed by absolute monotonic deadlines
        |
        v
one process-wide one-shot timerfd
        |
        v
existing epoll owner
        |
        v
narrow lwIP send-resume hook / native tcp_output mechanics
```

Hard rules:

- one timerfd for the process, never one per flow;
- nonblocking/cloexec `timerfd` on `CLOCK_MONOTONIC`;
- arm/rearm only to the earliest pending deadline;
- disarm when the heap is empty;
- no fixed 1-ms/10-ms tick;
- no busy spin;
- stale/cancelled flow entries must be generation-safe after teardown;
- timerfd/epoll/heap mechanics stay out of `src/cc/`;
- Reno's zero pacing rate remains on the current unpaced path;
- the integration must not replace lwIP retransmission/recovery/segment queues.

### P5c implementation sequence

1. introduce a runtime-owned pacer heap/timerfd module with standalone deadline/cancel/order tests;
2. register the single timerfd in the existing epoll owner and qualify idle disarm/no-spurious-wakeup behavior;
3. add the narrowest lwIP pacing eligibility/resume integration required to defer a data send until a deadline without cloning `tcp_output()`;
4. use a deterministic test policy/controller that requests a fixed nonzero pacing rate so scheduler mechanics can be qualified before BBR exists;
5. qualify multi-flow deadline ordering, cancellation during teardown, loss/RTO recovery, timer lateness, released bytes per wakeup, and no starvation;
6. rerun P3 memory/CPU and high-BDP traffic before declaring pacing ready for BBR.

### P5c CI observations

Retain at least:

```text
timerfd_creates
timer_arms
timer_rearms
timer_disarms
timer_expirations
pacing_wakeups
released_packets / released_bytes
heap_current / heap_peak
cancelled_or_stale_entries
requested_deadline_ns
actual_release_ns
lateness_ns distribution
idle runtime CPU and wakeups
```

A later cleanup may unify lwIP timer deadlines and pacing deadlines behind one one-shot timerfd only if CI proves unchanged timeout semantics and fewer/equal idle wakeups. Do not make that refactor a prerequisite for initial P5c qualification.

## Exit criteria

P5 is complete only when:

- retransmission-safe delivery accounting — **qualified**;
- ACK delivery-rate samples — **qualified**;
- event-driven app-limited classification — **qualified**;
- transport-neutral rate/RTT/inflight sample publication — **qualified**;
- event-driven process-wide pacing — pending;
- P0-P5b regressions remain green;
- fixed/per-flow/per-segment memory, CPU, and timer wakeups remain compatible with the constrained-host model.

## Distance to BBR

Only one prerequisite increment remains before the BBR controller itself becomes active:

1. P5c event-driven pacing and integrated send scheduling.

P6 then implements tcp-shift's BBR bandwidth/min-RTT model, pacing/cwnd policy, mode transitions, probing, loss response, and app-limited treatment. The hard TCP ownership/recovery and delivery-rate sampling boundaries are already solved; the remaining architectural risk is pacing accuracy/wakeup efficiency under high BDP and loss.

## Stop signal

Stop and reassess before BBR if pacing requires rebuilding lwIP retransmission/SACK/recovery, materially enlarging the controlled lwIP patch, consuming the retained memory budget, or using per-flow timers / periodic polling / busy spinning for pacing accuracy.
