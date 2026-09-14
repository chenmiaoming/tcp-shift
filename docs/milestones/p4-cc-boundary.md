# P4: generic congestion-control boundary

Status: **runner-qualified**.

## Goal

Establish a platform-independent congestion-control policy boundary and prove that real public-side lwIP ACK, fast-loss, and RTO decisions can delegate through it without taking retransmission/recovery mechanics away from lwIP or consuming the constrained-host memory budget.

## Pure-C policy boundary

`src/cc/` is independently buildable pure C. It may use only its own headers plus ISO C integer/size/limits headers. It must not depend on lwIP, Linux/POSIX APIs, TUN, epoll, timerfd, nftables, bridge/runtime objects, or controller-owned heap allocation.

The generic transport observation contains MSS, inflight bytes, peer send window, and a transport-representable cwnd limit. Events are init, ACK, loss, and RTO. Policy output contains cwnd, ssthresh, and optional pacing rate.

The conventional byte-counting Reno baseline uses 16 bytes of caller-owned state and always requests zero pacing rate. `cwnd_limit_bytes` prevents its 32-bit state from silently diverging from the current unscaled 16-bit lwIP cwnd representation.

The standalone contract covers slow start, congestion avoidance, loss, timeout, MSS changes, cwnd limits, saturation, invalid arguments, failed-init handle safety, explicit cwnd/ssthresh publication, and no pacing request.

Final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` passed the pure-C job in run `34815149645`; artifact `10335288981` retains the build/include/symbol evidence.

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
external_symbols=0
```

## lwIP adapter boundary

Pinned lwIP remains at:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

`scripts/fetch-lwip.sh` records pristine critical-source hashes and then applies `patches/lwip-p4-cc-hooks.patch`.

The patch changes exactly three congestion-policy sites:

1. ACK cwnd growth in `tcp_in.c`;
2. fast-retransmit loss cwnd/ssthresh policy in `tcp_out.c`;
3. RTO cwnd/ssthresh policy in `tcp.c`.

Unbound PCBs retain native lwIP policy. lwIP continues to own duplicate-ACK processing, retransmission execution, fast-recovery inflation/deflation and flags, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and `tcp_output()`.

One PCB ext-arg slot stores a project hook. `src/lwip/cc_adapter.c` translates lwIP state/events into generic observations and applies returned cwnd/ssthresh policy. The adapter may depend on lwIP; `src/cc/` may not.

Pinned lwIP calls the passive accept callback before setting `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter therefore initializes the controller with that same formula instead of weakening the generic `initial_cwnd >= MSS` invariant.

A public child that cannot bind a controller is rejected rather than silently falling back to native policy. PCB destroy callbacks release adapter-owned per-flow state.

## Controlled upstream provenance

Final provenance run `34815149550`, job `103884173736`, artifact `10335752874` proves:

- dependency `HEAD` equals the exact pin;
- pristine critical-source hashes are retained before patching;
- patch path/SHA256 match the repository patch;
- only `tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- reverse-apply succeeds;
- an independent worktree created from the same pin, after applying the repository patch, is byte-identical for all three modified files.

## Integrated ACK ownership

P2 run `34815149444`, job `103884173371`, artifact `10335932665` passed the full bridge regression suite with the adapter enabled: IPv4, IPv6-to-IPv4-loopback backend, bidirectional backpressure, half-close, refusal/reset recovery, eight concurrent flows, active shutdown, and 64-flow reuse.

Every P2 workload now has a hard controller-ownership gate:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

This prevents a green data-path test from accidentally using native lwIP congestion policy.

## Integrated fast-loss and RTO evidence

P4 run `34815149645`, integrated-recovery job `103884174233`, artifact `10336213268` injects packet loss outside lwIP on the real TUN path.

Fast-loss mode drops one response data packet while later packets continue. Native duplicate-ACK/fast-retransmit mechanics recover the exact 262144-byte stream while the generic controller receives the loss policy event:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=108
cc_loss_events=1
cc_timeout_events=0
cc_controller_errors=0
mode=fast-loss payload_bytes=262144 recovery=ok
```

RTO mode suppresses response data past the initial retransmission timeout and then restores delivery:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=144
cc_loss_events=0
cc_timeout_events=2
cc_controller_errors=0
mode=rto payload_bytes=262144 recovery=ok
```

Neither test calls generic loss/RTO functions directly.

## Memory/CPU requalification

P3 rerun `34815149825`, job `103884174726`, artifact `10335968830` qualifies the integrated adapter against the P3 planning model.

```text
warm fixed process PSS: 335 KiB
conservative idle slope: 0.523438 KiB/flow
controlled active payload delta: 36.625 KiB/flow
fully-window-resident slope: 37.148438 KiB/flow
128-active projected process PSS: 5090 KiB
8-MiB process budget remaining: 3102 KiB = 24.234 KiB/flow
three-round 128-flow drain growth: 5 KiB
maximum warm drain floor above ready: 69 KiB
```

The original P3 admission baseline was 315 KiB warm fixed PSS and 37.523438 KiB/fully-window-resident flow. P4 therefore adds a small fixed cost but does not consume the active-flow planning headroom. This remains tcp-shift process PSS only; backend kernel/application and provider memory are excluded.

## Final behavior-head regression matrix

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

## Exit criteria

P4 is complete because CI now retains all of the following:

- pure-C standalone controller library with no lwIP/Linux dependency;
- conventional state-machine contract and explicit cwnd/ssthresh policy;
- narrow three-site lwIP policy hook instead of a TCP recovery fork;
- exact controlled-patch provenance;
- real ACK ownership across P2 workloads;
- real fast-loss and RTO controller events on the integrated packet path;
- unchanged P0/P1/P2 behavior and passing P3 memory/capacity gates;
- explicit documentation of recovery mechanics that remain native lwIP responsibilities.

## P5 next

P5 adds BBR prerequisites, not BBR itself:

- high-resolution monotonic send/ACK timestamps;
- cumulative delivered-byte accounting;
- minimal per-segment delivery metadata;
- ACK-derived delivery-rate sampling;
- app-limited detection;
- loss/inflight sample observations;
- one process-wide pacing scheduler, preferably a heap plus one timerfd;
- fixed/per-flow/per-segment memory and CPU qualification.

The sampler publishes transport-neutral observations through the generic CC boundary. Linux timerfd/epoll scheduling remains outside `src/cc/`.

## Stop signal

Stop and reassess before BBR if P5 requires rebuilding lwIP retransmission/SACK/recovery, materially enlarging the lwIP fork, consuming the constrained-host memory budget with metadata, or using per-flow timers/busy spinning to obtain pacing accuracy.
