# P4: generic congestion-control boundary

Status: **runner-qualified**.

## Goal

Introduce a mechanically enforceable congestion-control policy boundary before delivery-rate sampling, pacing, or BBR-specific state exists.

P4 is successful when congestion-control policy can be built and tested as pure C independently from lwIP and the Linux runtime, and when a thin lwIP adapter delegates conventional congestion-window decisions through that boundary without regressing the P0-P3 transport behavior or consuming the P3 memory headroom unexpectedly.

## Hard boundary

`src/cc/` is policy code, not a second network stack and not a runtime module. It may use ISO C integer/size types and its own headers only. It must not include or call:

- lwIP headers or `tcp_pcb`/`tcp_seg` objects;
- Linux, POSIX socket, TUN, epoll, timerfd, or nftables APIs;
- bridge/runtime/process lifecycle objects;
- heap allocation owned by the controller;
- a pacing scheduler.

Controller state is caller-owned. The controller consumes transport observations and publishes policy. P5 may extend ACK observations with qualified delivery-rate samples, but the runtime remains responsible for timestamps and pacing mechanics.

## Standalone policy library — qualified

The `src/cc/` API contains:

- `tcp_shift_cc_transport`: MSS, bytes in flight, peer send-window observation, and the transport's representable cwnd limit;
- init parameters: initial cwnd, initial ssthresh, and minimum cwnd;
- ACK, loss, and retransmission-timeout events;
- policy output: cwnd, ssthresh, and optional pacing rate in bytes/second;
- an ops table and caller-owned opaque controller state.

The explicit `cwnd_limit_bytes` transport capability prevents a 32-bit controller from silently diverging from the current unscaled 16-bit lwIP `tcpwnd_size_t`. `ssthresh` is explicit policy so native recovery can consume the controller's threshold without inspecting opaque state. A pacing rate of zero means the controller does not request pacing. P4's conventional controller leaves pacing zero; no timer or scheduler is introduced here.

The first controller is a small byte-counting Reno baseline. It exists to validate the generic boundary, not to claim Linux Reno equivalence. Its state is four `uint32_t` values (16 bytes): cwnd, ssthresh, congestion-avoidance ACK accumulator, and configured minimum cwnd.

The conventional behavior contract covers capped byte-counted slow start, additive congestion avoidance, loss reduction using `min(cwnd, peer window)`, timeout collapse, MSS changes, transport cwnd limits, saturating arithmetic, invalid initialization/state-size rejection, failed-init handle safety, explicit cwnd/ssthresh publication, and no pacing request.

`scripts/validate-p4-cc.sh` configures `src/cc/` as its own CMake project, compiles with warnings-as-errors plus `-ffreestanding -fno-builtin`, runs the state-machine contract, scans includes against an explicit allowlist, and requires `libtcp_shift_cc.a` to have no undefined external symbols.

The standalone boundary remains qualified on final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac`, P4 run `34815149645`, pure-C job `103884174400`, artifact `10335288981`.

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
controller=reno
state_bytes=16
external_symbols=0
```

## lwIP adapter and controlled patch — qualified

Pinned lwIP remains pinned at `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. The integration does not vendor or replace the upstream TCP files. `scripts/fetch-lwip.sh` records pristine critical-source hashes first, then applies the repository-owned `patches/lwip-p4-cc-hooks.patch`.

The patch changes only three congestion-policy sites:

1. ACK-driven cwnd growth in `tcp_in.c`;
2. fast-retransmit loss threshold/base cwnd policy in `tcp_out.c`;
3. RTO cwnd/ssthresh reduction in `tcp.c`.

Unbound PCBs fall through to the original native lwIP policy. Fast-recovery inflation/deflation mechanics, retransmission execution, SACK/recovery, packet queues, sequence space, RTT/RTO calculation, and `tcp_output()` remain lwIP responsibilities.

One lwIP PCB ext-arg slot carries the tcp-shift hook pointer. `src/lwip/cc_adapter.c` is allowed to depend on lwIP and `src/cc/`; `src/cc/` itself remains platform-independent. P2 listeners are registered through a narrow accept wrapper so the established passive-open PCB gets a project controller before the existing bridge accept callback runs.

Pinned lwIP invokes the passive accept callback before assigning `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter therefore explicitly initializes the controller with the same pinned-lwIP initial-cwnd calculation instead of weakening the generic invariant that initial cwnd must be at least one MSS. lwIP writes the same initial cwnd immediately after the callback.

Upstream provenance run `34815149550`, job `103884173736`, artifact `10335752874` proves all of the following on the final behavior head:

- checkout `HEAD` is still the exact pinned commit;
- pristine critical-source hashes are recorded before patching;
- repository patch SHA is recorded and verified;
- only `src/core/tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- the working tree can reverse-apply the patch;
- an independent worktree made from the same pin, after applying the repository patch, is byte-identical for all three modified files.

## Integrated controller ownership — qualified

P2 run `34815149444`, job `103884173371`, artifact `10335932665` passed the complete bridge regression suite with the adapter enabled: IPv4, IPv6 public side to IPv4 loopback backend, bidirectional backpressure, backend-first half-close, backend refusal/recovery, public/backend reset recovery, eight concurrent flows, active-flow shutdown, and 64-flow reuse.

The P2 workflow now has a cross-workload controller-ownership hard gate. Every runtime must satisfy:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

Representative prior green diagnostics on the same adapter design observed one-flow IPv4/IPv6 ACK ownership, eight simultaneous controller bindings, and 64 sequential bindings without controller errors. The final workflow converts those observations into a mechanical gate rather than relying on individual smoke-script output.

## Real loss and timeout recovery — qualified

P4 run `34815149645`, integrated-recovery job `103884174233`, artifact `10336213268` injects loss outside lwIP on the real TUN path. It does not call controller event functions directly.

Fast-loss mode drops one data packet while later packets continue, forcing duplicate ACKs and native fast retransmit/recovery. The same 262144-byte stream completes with exact echo integrity:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=108
cc_loss_events=1
cc_timeout_events=0
cc_controller_errors=0
mode=fast-loss payload_bytes=262144 recovery=ok
```

RTO mode suppresses response data past the pinned initial retransmission timeout, then restores delivery. The same stream recovers and completes:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=144
cc_loss_events=0
cc_timeout_events=2
cc_controller_errors=0
mode=rto payload_bytes=262144 recovery=ok
```

These gates prove that ACK, fast-loss, and retransmission-timeout policy decisions reach the generic controller while retransmission and recovery mechanics remain native lwIP code.

## Memory/CPU requalification against P3

P3 was rerun with the integrated adapter on final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac`: run `34815149825`, job `103884174528`, artifact `10335968830`.

The retained process-PSS model reports:

```text
warm fixed process PSS: 335 KiB
conservative idle PSS slope: 0.523438 KiB/flow
controlled active payload PSS delta: 36.625 KiB/flow
fully-window-resident process slope: 37.148438 KiB/flow
128-active-flow projected process PSS: 5090 KiB
32-MiB host / 25% tcp-shift process budget remaining: 3102 KiB
remaining planning headroom: 24.234 KiB/active flow
three-round 128-flow drain growth: 5 KiB
maximum warm drain floor above ready: 69 KiB
```

The pre-P4 P3 admission baseline was 315 KiB warm fixed PSS and 37.523438 KiB/fully-window-resident flow. The adapter adds a small fixed userspace cost while the per-active-flow residency remains within runner noise around the existing 32-KiB window/pbuf-dominated slope. The conservative 8-MiB tcp-shift process budget still admits 128 fully-window-resident flows with essentially unchanged per-flow headroom. Backend kernel/application memory remains outside this model.

## Regression qualification

Final behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` retained green:

- upstream provenance: run `34815149550`;
- P0: run `34815149474`;
- P1 IPv4/IPv6 lifecycle + PMTU: run `34815149646`;
- P2 bridge + controller ownership: run `34815149444`;
- P3 memory/capacity: run `34815149825`;
- P4 pure-C + integrated recovery: run `34815149645`.

## Exit criteria

P4 exit criteria are satisfied on the GitHub runner:

- standalone pure-C `tcp_shift_cc` library with no lwIP/Linux dependency and no unresolved external symbols;
- conventional controller state-machine contracts;
- a narrow, documented lwIP policy-hook surface rather than a TCP recovery fork;
- conventional controller ownership of public-side cwnd/ssthresh under real P2 traffic;
- real ACK, fast-loss, and RTO state-transition evidence through the integrated TUN/lwIP path;
- pinned-upstream provenance plus exact controlled-patch qualification;
- P0/P1/P2/P3 regressions green with the adapter enabled;
- fixed/per-flow process memory remains inside the P3 planning budget;
- native lwIP recovery responsibilities remain explicit.

## P5 next: delivery-rate sampling and pacing prerequisites

P4 deliberately does not implement high-resolution send timestamps, delivered-byte snapshots, per-segment delivery metadata, ACK delivery-rate estimation, app-limited detection, a pacing heap, timerfd scheduling, or BBR state/modes.

P5 must introduce those prerequisites without weakening the P4 boundary. The controller should consume transport-neutral delivery samples; Linux timerfd/epoll pacing mechanics must stay outside `src/cc/`. P5 must report fixed, per-flow, and per-segment memory increments against the P3/P4 baseline before BBR is attempted.

## Stop signal

If delivery-rate sampling or pacing requires rebuilding lwIP retransmission/recovery/SACK semantics, a large permanent upstream fork, or per-flow/per-segment memory growth that consumes the retained constrained-host budget, stop and reassess before adding BBR-specific complexity.
