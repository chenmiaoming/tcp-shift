# Development and agent handoff contract

This repository is the durable project memory. A human or coding agent should be able to continue the work from repository state without relying on a private chat transcript.

## Read order for a new contributor

1. `ARCHITECTURE.md` — current architecture and ownership boundaries.
2. `docs/lwip-roadmap.md` — milestone order, exit evidence, stop criteria.
3. `docs/ci.md` — qualification model and current retained runs.
4. active milestone document under `docs/milestones/`.
5. relevant source and validation scripts.

Historical milestone documents explain how the design evolved; they do not override `ARCHITECTURE.md`.

## Development rules

- Use GitHub Actions as the primary validation/debug environment.
- Preserve diagnostics and failure evidence; do not rerun away a useful failure without understanding it.
- Do not weaken a gate solely to make CI pass.
- If CI exposes a harness assumption, fix the harness while retaining the intended product assertion.
- Behavior/architecture/CI milestone changes update repository docs in the same PR.
- Keep source modules separate even while the product remains one process.
- Keep public TCP and backend TCP semantics distinct.
- Do not infer provider/OpenVZ qualification from GitHub runner qualification.
- If a behavioral head is green and later commits are docs-only, merge may rely on the last green behavioral head after a compare proves no behavioral files changed.

## Pinned lwIP and controlled patch

The production lwIP pin is defined by `.lwip-baseline` and currently resolves to:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

The dependency remains upstream-pinned rather than vendored. P4 introduced one explicit project-owned integration patch:

```text
patches/lwip-p4-cc-hooks.patch
```

`scripts/fetch-lwip.sh` records pristine critical-source hashes first, then applies that patch. The provenance workflow requires exact pinning and independently proves that the final modified `tcp.c`, `tcp_in.c`, and `tcp_out.c` are exactly the result of applying the repository patch to the pin.

Do not add another lwIP patch casually. Any new upstream modification requires an architectural reason, a bounded file/hook surface, provenance coverage, and regression evidence.

## Current qualified architecture

The public path is:

```text
remote -> Linux routing/netfilter -> L3 TUN -> lwIP TCP
       -> raw callbacks -> userspace bridge -> 127.0.0.1 backend
```

Current constraints:

- L3 TUN only; no TAP/Ethernet product path.
- lwIP `NO_SYS=1`, one mutable event-loop owner.
- no lwIP socket/netconn/tcpip thread.
- public IPv4 and IPv6 are supported through the same runtime/bridge.
- backend forwarding remains ordinary IPv4 loopback `127.0.0.1`.
- IPv6 uses static internal L3 addressing; physical NDP remains host-kernel responsibility.
- RS/SLAAC/DHCPv6/MLD/ND6 packet queueing/RA MTU updates and IPv6 endpoint fragmentation/reassembly remain disabled in the low-memory profile.
- public ingress exact-match DNAT is product-owned; broad forwarding policy is operator-owned.

## P0-P3 status

P0 through P3 are runner-qualified and remain mandatory regressions.

P3 is the memory/CPU planning baseline. It measures tcp-shift process PSS only; backend kernel/application memory and provider overhead are excluded. The original P4 admission model used approximately 315 KiB warm fixed process PSS and 37.523438 KiB/fully-window-resident flow, leaving about 24 KiB/active-flow process headroom in the conservative 32-MiB-host/25%-budget scenario.

## P4 status — complete

P4 is fully runner-qualified on behavior head:

```text
0a3054159db03b017526b3faabfbe7f6a6c826ac
```

Final behavior-head runs:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

### Pure-C controller boundary

`src/cc/` is independently buildable pure C. It does not include lwIP/Linux/runtime/bridge APIs. Controller state is caller-owned. The conventional Reno baseline uses 16 bytes and emits `cwnd`, `ssthresh`, and zero pacing rate.

The generic transport observation currently carries MSS, inflight bytes, peer send window, and a transport cwnd limit. Generic events are init, ACK, loss, and RTO.

### lwIP adapter

`src/lwip/cc_adapter.c` is the platform adapter. It binds the generic controller to accepted public PCBs via one lwIP ext-arg slot.

The controlled lwIP patch delegates exactly three base congestion-policy decisions: ACK growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh. lwIP still owns retransmission execution, duplicate-ACK handling, fast-recovery mechanics, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and output.

The passive-open adapter must preserve the pinned lwIP ordering detail: lwIP invokes accept before assigning `LWIP_TCP_CALC_INITIAL_CWND()`. The adapter therefore initializes its controller with that same formula; do not weaken the generic initial-cwnd invariant to accommodate callback ordering.

### P4 integration gates

P2 now requires controller ownership for every workload:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

P4 uses real host-side packet loss injection, not synthetic event calls. Final evidence includes:

```text
fast-loss: cc_loss_events=1, cc_timeout_events=0, 262144-byte recovery=ok
rto:       cc_loss_events=0, cc_timeout_events=2, 262144-byte recovery=ok
```

Adapter-enabled P3 rerun `34815149825` retained:

```text
warm fixed process PSS: 335 KiB
fully-window-resident slope: 37.148438 KiB/flow
128-active projected process PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
three-round drain growth: 5 KiB
```

P4 therefore exits without consuming the constrained-host admission headroom.

## Active next milestone: P5

P5 is not BBR yet. It establishes the delivery-rate and pacing prerequisites BBR needs.

Required implementation sequence:

1. add high-resolution monotonic send/ACK timestamps;
2. add cumulative delivered-byte accounting;
3. add minimal per-segment metadata needed to reconstruct delivery samples;
4. derive ACK delivery-rate samples;
5. implement app-limited detection/marking;
6. expose loss/inflight/sample observations through the generic CC boundary;
7. add one process-wide pacing scheduler, preferably a heap plus one timerfd;
8. qualify fixed/per-flow/per-segment memory and CPU deltas before adding BBR modes.

Keep sampler semantics transport-neutral. `src/cc/` may consume a delivery sample but must not call timerfd/epoll or own the Linux scheduler.

## P5 memory discipline

P3/P4 show that active TCP window residency is already roughly 37 KiB/flow and dominates idle control objects. Per-segment metadata can therefore become material quickly.

Before retaining any P5 metadata layout, report:

- fixed process increment;
- per-flow increment;
- bytes per outstanding segment;
- worst-case metadata at the configured send/window limits;
- any timer/scheduler node cost;
- CPU/timer wakeup effect.

Do not assume future BBR metadata is free just because the controller state itself is small.

## Merge discipline

A milestone/sub-increment may be squash-merged when:

- its behavioral exit criteria are mechanically satisfied;
- relevant prior regressions are green;
- diagnostics are retained;
- docs match the behavior head;
- PR review/comments/threads are clean;
- a docs-only tail, if any, is verified by commit comparison.

Once mergeable, merge it rather than accumulating unrelated later-phase work in the same PR.

## Stop criteria

Stop and reassess the lwIP route before BBR if:

- conventional/model-based control requires rebuilding lwIP recovery/SACK machinery;
- the project accumulates a large hard-to-rebase lwIP fork;
- sampler/pacer metadata approaches hosted-Linux memory cost;
- unavoidable BDP/window buffering dominates small-host RAM;
- pacing requires per-flow timers or busy spinning instead of one bounded scheduler.

A small conventional-CC endpoint is still a valid outcome if BBR is rejected by evidence.
