# P5: delivery-rate sampling and pacing prerequisites

Status: **complete; P5a delivery ledger, P5b rate/app-limited sampler, and P5c event-driven pacer are runner-qualified**.

## Goal

P5 builds the transport observations and runtime scheduling primitives required by model-based congestion control without adding BBR state. It makes unique delivery accounting, ACK delivery-rate samples, app-limited semantics, and pacing independently observable and memory-accounted through the already-qualified P4 controller boundary.

P5 is not BBR. The generic controller publishes transport-neutral cwnd/ssthresh/pacing policy; lwIP still owns sequence space, segment queues, retransmission, fast recovery, SACK/recovery, RTT/RTO calculation, packet construction, and output.

## Required sequence — complete

1. high-resolution monotonic transmit/ACK timestamps — qualified;
2. cumulative unique delivered-byte accounting — qualified;
3. retransmission-safe per-segment delivery metadata — qualified;
4. ACK-derived delivery-rate samples — qualified;
5. event-driven app-limited detection/marking — qualified;
6. transport-neutral sample publication through the generic CC observation surface — qualified;
7. one process-wide event-driven pacing scheduler — qualified;
8. fixed/per-flow/per-segment memory, CPU, wakeup, loss/RTO, teardown, multi-flow, and high-BDP qualification — qualified.

## P5a: delivery ledger — runner-qualified

P5a established trustworthy delivery accounting without changing controller behavior or pacing packets. Metadata remains outside upstream `struct tcp_seg` in a lazy sidecar keyed by stable `tcp_seg *`; retransmission reuses the same slot and cannot double-count delivered payload.

Final P5a behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38`, run `34821205375`, job `103903019956`, artifact `10338108552` retained exact 262144-byte delivery through natural, fast-loss, and RTO traffic with zero live metadata at teardown.

The original P5 workflow exposed a false-green harness bug: field parsing matched `live_slots` inside `peak_live_slots`, and a checker piped through `tee` lost the failing status. Qualification was withheld until exact-token parsing and fail-closed checker propagation were in place.

## P5b: ACK delivery-rate sampler + app-limited — runner-qualified

P5b converts the delivery ledger into a transport-neutral ACK observation while conventional Reno remains unchanged.

`struct tcp_shift_cc_ack` carries delivery rate, selected interval, send interval, ACK interval, RTT when valid, newly delivered payload bytes, prior inflight, and VALID / APP_LIMITED / RETRANSMITTED / RTT_VALID flags. The generic CC layer sees no lwIP PCB or segment objects.

The lazy sidecar is 56 bytes and retains sequence/progress, first-transmit and send-phase timestamps, delivered snapshots, prior inflight, and app-limited/retransmission state. ACK delivery is computed from cumulative sequence progress while relevant sidecars are still live, before pinned lwIP frees acknowledged segments. FIN sequence space is excluded. Retransmitted candidates do not publish RTT.

App-limited marking is event driven: the bridge invokes the marker only when the real backend `MSG_PEEK | MSG_DONTWAIT` path reaches `EAGAIN`; the adapter additionally requires no unsent data and available public transport capacity. There is no periodic flow scan or app-limited timer.

Final P5b behavior head `327efe7e29adfc230e7d201b466f2bd4980e976c`, P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained natural/loss/RTO rate sampling plus the application-pause workload. The pause gate recorded zero runtime CPU ticks while still observing app-limited enter/sample/exit transitions.

## P5c: event-driven pacer — runner-qualified

P5c turns a nonzero controller pacing policy into actual send eligibility without introducing periodic polling or replacing native lwIP recovery/output mechanics.

Production shape:

```text
controller pacing policy
        |
        v
lwIP CC adapter / data-send eligibility hook
        |
        v
process-wide min-heap keyed by absolute monotonic deadline
        |
        v
one process-wide one-shot CLOCK_MONOTONIC timerfd
        |
        v
existing epoll owner
        |
        v
resume through native tcp_output()
```

Hard rules are satisfied:

- exactly one timerfd for the process, never one per flow;
- nonblocking/cloexec `timerfd`, one-shot monotonic deadlines;
- rearm only when the earliest pending deadline changes;
- disarm when the heap is empty;
- no fixed pacing tick, flow scan, per-flow thread, or busy spin;
- queued entries carry `flow_id` + generation rather than flow pointers;
- teardown cancellation and stale-generation release are safe;
- timerfd/epoll/heap mechanics remain outside `src/cc/`;
- production Reno continues to publish pacing rate 0 and therefore stays on the unpaced path;
- retransmission/recovery/segment queues remain native lwIP mechanics.

### Qualification controller

P5c uses a qualification-only fixed-paced Reno policy. Its cwnd/ssthresh transitions are the same Reno state machine; ACK/loss/RTO policy additionally publishes a fixed pacing rate of `65536 B/s`. The rate enters the generic policy/adapter path. There is no runtime force-rate backdoor.

### Deterministic teardown contract

A dedicated adapter lifecycle contract first creates a future pacing deadline, then unbinds before expiry. It requires exactly one scheduler cancellation and then submits the old `(flow_id, generation)` as a stale release. Final result:

```text
pacer_adapter_lifecycle=ok schedules=1 cancels=1 stale_release_safe=1 flow_id=1 generation=1
```

This contract exposed a real manual-unbind bug: `tcp_shift_lwip_cc_adapter_unbind()` attempted `tcp_ext_arg_set_callbacks(..., NULL)`, but pinned lwIP requires a non-NULL callback table. The production fix keeps the static callbacks installed and clears only ext-arg data. A later PCB destroy therefore invokes the existing destroy callback with NULL data, which is a deliberate no-op; no double-free is possible.

### Integrated datapath evidence

Final P5c behavior head:

```text
046152dbaba56a0be3b1d2a1902fee6f2bbf9044
```

All required workflows on that same head passed:

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

P5c run `34870859911`, job `104066140125`, artifact `10358951402` (`sha256:65a7d2d5eb4351cb0211e1260d362bcbd3d87945e674766e5e2f03a55fc3bf88`) retained the scheduler, adapter lifecycle, epoll, bridge, multi-flow, recovery, and high-BDP evidence.

Representative paced results:

```text
single 64 KiB:
  rate=65536 B/s
  deferrals=96 resume=44 scheduled=44 released=44
  timerfd_creates=1 heap_peak=1 heap_current=0
  scheduler/stale/controller errors=0

4 x 64 KiB concurrent:
  total=262144 B integrity=ok
  deferrals=384 resume=176 scheduled=176 released=176
  heap_peak=4 heap_current=0 max_lateness_ns=43136
  loss=0 timeout=0 scheduler/stale/controller errors=0

fast loss, 128 KiB:
  loss_events=1 timeout_events=0 retransmit_events=1
  deferrals=186 resume=90 released=90
  heap_current=0 max_lateness_ns=79624
  scheduler/stale/controller errors=0

RTO, 128 KiB:
  loss_events=0 timeout_events=2 retransmit_events=4
  deferrals=187 resume=91 released=91
  heap_current=0 max_lateness_ns=57626
  scheduler/stale/controller errors=0
```

Retransmissions remain paced because the existing successful-segment-TX observation advances the next pacing deadline before distinguishing first transmission from retransmission. Delivery accounting still marks retransmission without double-counting bytes.

### Sustained high-BDP gate

The high-BDP qualification reuses the real P2 bridge harness and applies test-only `netem` delay to the TUN. Product runtime policy is unchanged.

Fixture:

```text
pacing rate:             65536 B/s
ACK-path netem delay:    750 ms
nominal BDP:             49152 B
qualified TCP window:    32768 B
payload:                 262144 B
```

Thus the nominal BDP exceeds the current unscaled 32 KiB transport window. The observed final rate sample was about `43682 B/s`, consistent with window-limited delivery at roughly 750 ms, so this is genuine high-BDP/window pressure rather than a synthetic sleep around the scheduler.

Retained result:

```text
payload integrity:       ok
loss_events:             0
timeout_events:          0
deferrals:               112
resume_events:           87
scheduled/released:      87 / 87
released_bytes:          123772
timerfd_creates:         1
heap_peak/current:       1 / 0
max_lateness_ns:         65199
runtime wall:            15.067229 s
runtime CPU:             0.01 s = 0.066%
controller_errors:       0
scheduler_errors:        0
stale_releases:          0
netem packet drops:      0
```

Window scaling remains a later transport milestone. P5c proves event-driven pacing remains correct and cheap when path BDP exceeds the current window; it does not claim that the current 32 KiB window can fully utilize arbitrary high-BDP links.

### P5c memory/CPU cost

P3 was rerun on the same final head. Run `34870859880`, job `104066205524`, artifact `10358454897` retained:

```text
warm fixed process PSS:                367 KiB
idle 128-flow PSS:                     359 KiB
conservative fully-window slope:       37.773438 KiB/flow
128-active projected process PSS:      5202 KiB
8-MiB process budget remaining:        2990 KiB = 23.359 KiB/flow
3x128 first-to-last drain growth:      5 KiB
idle CPU:                              0 ticks/s
small-operation CPU:                   34.179688 us/op
```

The scheduler/registry increment remains well inside the constrained-process planning model. High-BDP pacing separately consumed only one 100-Hz CPU tick over about 15 seconds.

## Exit criteria — satisfied

- retransmission-safe delivery accounting — **qualified**;
- ACK delivery-rate samples — **qualified**;
- event-driven app-limited classification — **qualified**;
- transport-neutral rate/RTT/inflight sample publication — **qualified**;
- event-driven process-wide pacing — **qualified**;
- generation-safe cancellation/teardown — **qualified**;
- real multi-flow overlapping deadlines — **qualified**;
- fast-loss and RTO recovery while paced — **qualified**;
- sustained high-BDP/window-pressure traffic — **qualified**;
- P0-P5b regressions on the final behavior head — **green**;
- fixed/per-flow/per-segment memory, CPU, and timer wakeups remain compatible with the constrained-host model — **qualified**.

## Distance to BBR

P5 is complete. The next controller milestone is P6 tcp-shift BBR: bandwidth/min-RTT model, pacing/cwnd policy, mode transitions, probing, loss response, and app-limited treatment over the already-qualified generic boundary and pacer.

P6 must still prove its own algorithmic behavior against reproducible native/reference scenarios. P5c qualification is only the pacing substrate; it is not evidence that a future controller is Linux BBR-equivalent.

## Stop signal

No P5 stop criterion fired. Pacing did not require rebuilding lwIP retransmission/SACK/recovery, materially widening the controlled lwIP patch, per-flow timers, periodic polling, busy spinning, or consuming the retained memory budget. Reassess during P6 if any of those conditions change.
