# Development and agent handoff contract

This repository is the durable project memory. A human or coding agent should be able to continue the work from repository state without relying on a private chat transcript.

## Read order for a new contributor

1. `ARCHITECTURE.md` — current architecture and ownership boundaries.
2. `docs/lwip-roadmap.md` — milestone order, exit evidence, stop criteria.
3. `docs/ci.md` — qualification model and retained runs.
4. active milestone document under `docs/milestones/`.
5. relevant source and validation scripts.

Historical milestone documents explain how the design evolved; they do not override `ARCHITECTURE.md`.

## Development rules

- Use GitHub Actions as the primary validation/debug environment.
- Preserve diagnostics and failure evidence; do not rerun away a useful failure without understanding it.
- Do not weaken a gate solely to make CI pass.
- Do not trust a workflow's green conclusion if the log contradicts the intended qualification assertion; fix the harness and rerun fail-closed.
- If CI exposes a harness assumption, fix the harness while retaining or strengthening the intended product assertion.
- Behavior/architecture/CI milestone changes update repository docs in the same PR.
- Keep source modules separate even while the product remains one process.
- Keep public TCP and backend TCP semantics distinct.
- Prefer event/readiness/deadline-driven runtime work over fixed polling.
- Do not infer provider/OpenVZ qualification from GitHub runner qualification.
- If a behavioral head is green and later commits are docs-only, merge may rely on the last green behavioral head after a compare proves no behavioral files changed.

## Pinned lwIP and controlled patch

The production lwIP pin is defined by `.lwip-baseline` and currently resolves to:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

The dependency remains upstream-pinned rather than broadly vendored. The repository-owned integration patch is:

```text
patches/lwip-p4-cc-hooks.patch
```

`scripts/fetch-lwip.sh` records pristine critical-source hashes first, then applies that patch. Provenance CI requires exact pinning and independently proves that modified `tcp.c`, `tcp_in.c`, and `tcp_out.c` are exactly the result of applying the repository patch to the pin.

P5a extends the hook semantics inside already-controlled `tcp_in.c` / `tcp_out.c`; it does not add another patched upstream file.

Do not expand the lwIP patch surface casually. Any new upstream file requires an architectural reason, bounded hook surface, provenance coverage, and regression evidence.

## Current architecture

```text
remote -> Linux routing/netfilter -> L3 TUN -> lwIP TCP
       -> raw callbacks -> userspace bridge -> 127.0.0.1 backend
```

Constraints:

- L3 TUN only; no TAP/Ethernet product path.
- lwIP `NO_SYS=1`, one mutable event-loop owner.
- no lwIP socket/netconn/tcpip thread.
- public IPv4 and IPv6 share the same runtime/bridge.
- backend remains ordinary IPv4 loopback `127.0.0.1`.
- IPv6 internal addressing is static; physical NDP remains host-kernel responsibility.
- RS/SLAAC/DHCPv6/MLD/ND6 packet queueing/RA MTU updates and IPv6 endpoint fragmentation/reassembly remain disabled.
- public ingress exact-match DNAT is product-owned; broad forwarding policy is operator-owned.

## Event-driven runtime contract

The current runtime is already deadline/readiness driven:

- epoll handles TUN/backend readiness;
- epoll timeout is derived from `sys_timeouts_sleeptime()`, not a fixed tick;
- `sys_check_timeouts()` runs after an actual readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only for real queued packets;
- backend interests are removed/suppressed when no useful progress is possible.

P5 pacing must preserve or improve this behavior. The default design is:

```text
paced flows -> process-wide min-heap -> one one-shot CLOCK_MONOTONIC timerfd -> epoll
```

Rules:

- one timerfd for the process, never one per flow;
- timerfd is armed to the earliest pending pacing deadline only;
- disarm it when no paced send is pending;
- no periodic pacing tick;
- no busy spin;
- record timer arms/expirations, pacing wakeups, lateness, idle wakeups, and CPU in CI.

Do not replace deadline-driven waits with a convenience polling loop.

## P0-P3 status

P0 through P3 are runner-qualified and mandatory regressions.

P3 measures tcp-shift process PSS only; backend kernel/application memory and provider overhead are excluded. Original P4 admission was about 24 KiB/process-headroom per active flow in the conservative 32-MiB-host/25%-budget model.

## P4 status — complete

P4 behavior head:

```text
0a3054159db03b017526b3faabfbe7f6a6c826ac
```

Final P4 behavior-head runs:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

`src/cc/` is independently buildable pure C. Controller state is caller-owned. The conventional Reno baseline uses 16 bytes and emits cwnd, ssthresh, and zero pacing rate.

`src/lwip/cc_adapter.c` binds the generic controller to accepted public PCBs through one ext-arg slot. The patch delegates base ACK growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh. lwIP retains retransmission execution, duplicate-ACK handling, fast recovery, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and output.

P2 requires:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

P4 external fault injection retained:

```text
fast-loss: cc_loss_events=1, cc_timeout_events=0, 262144-byte recovery=ok
rto:       cc_loss_events=0, cc_timeout_events=2, 262144-byte recovery=ok
```

P4 adapter P3 baseline:

```text
warm fixed process PSS: 335 KiB
fully-window-resident slope: 37.148438 KiB/flow
128-active projected PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
```

## P5a status — complete and ready to merge

P5a establishes high-resolution send/ACK timestamps, cumulative unique delivered-byte accounting, and retransmission-safe segment metadata.

Design:

- do not enlarge upstream `struct tcp_seg`;
- sidecar metadata is project-owned in the lwIP adapter;
- vector allocation is lazy, 8 slots initially, bounded by current `TCP_SND_QUEUELEN=90`;
- every slot is 32 bytes;
- key by stable `tcp_seg *`;
- retransmission reuses the same slot;
- fully ACKed segments consume metadata before upstream free;
- teardown must leave zero live slots.

Final qualified P5a behavior head:

```text
ce6c89f399bed3535be52138e3c96ea2ea061b38
```

Runs:

```text
upstream provenance  34821205386  success
P0                   34821205401  success
P1                   34821205357  success
P2                   34821205366  success
P3                   34821205396  success
P4                   34821205367  success
P5                   34821205375  success
```

P5 run `34821205375`, job `103903019956`, artifact `10338108552` retained:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
rto:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

All paths require zero metadata allocation failures, misses, abandoned slots, clock errors, and timestamp regressions.

### P5 harness bug that must not be reintroduced

The first P5 workflow was falsely green even though its log printed a qualification failure:

- `sed` searching for `live_slots=` also matched `peak_live_slots=`;
- `check_delivery | tee` returned `tee`'s success rather than the checker's failure under POSIX `sh`.

The final checker parses exact `key=value` tokens, writes its summary directly, then `cat`s it. Do not reintroduce a pipeline that masks the checker's status.

`actions/upload-artifact` also needs `include-hidden-files: true` because diagnostics live under `.build`.

### P5a memory cost

```text
                         P4             P5a
warm fixed PSS          335 KiB         343 KiB
fully-window slope      37.148438       37.679688 KiB/flow
128-active projection   5090 KiB        5166 KiB
8-MiB budget remaining  3102 KiB        3026 KiB
headroom / active flow  24.234 KiB      23.641 KiB
```

The delivery ledger consumes only a small fraction of retained process headroom.

## Active next milestone: P5b rate sampler + app-limited

P5b converts P5a snapshots into transport-neutral ACK delivery-rate observations.

Required semantics:

1. newly delivered bytes per ACK;
2. delivery interval and send interval;
3. valid/invalid rate sample rules;
4. delayed ACK and ACK aggregation;
5. ACKs covering multiple segments;
6. retransmissions without duplicate delivery;
7. partial ACKs;
8. sequence-number wrap safety;
9. latest valid RTT observation;
10. prior inflight/loss fields;
11. app-limited enter/exit semantics.

Extend the generic controller input only with transport-neutral observations. Keep lwIP queue objects and Linux scheduling out of `src/cc/`.

CI should use real traffic and retain structured sample traces rather than only checking that a rate is nonzero.

## P5c after P5b: event-driven pacer

After rate/app-limited semantics are qualified, add the process-wide one-shot timerfd scheduler. Do not combine rate-sampler correctness and pacing timing bugs into one first increment.

Pacer qualification must include idle behavior, small packet workloads, high-BDP pacing, deadline lateness, timer wakeups, and CPU. Busy spinning or fixed periodic polling is a failure of the design, not an optimization to accept temporarily.

## Distance to tcp-shift BBR

Remaining work before the BBR controller becomes active:

```text
P5a delivery ledger        complete
P5b rate/app-limited       next
P5c event-driven pacing    then
P6 tcp-shift BBR           then active
```

The hard TCP ownership/recovery boundary is already solved. Main remaining risk is rate/pacing correctness under delayed ACK, aggregation, high BDP, and loss.

## Merge discipline

A milestone/sub-increment may be squash-merged when:

- behavioral exit criteria are mechanically satisfied;
- relevant prior regressions are green;
- diagnostics are retained;
- docs match the behavior head;
- PR review/comments/threads are clean;
- a docs-only tail, if any, is verified by commit comparison.

Once mergeable, merge it rather than accumulating unrelated later-phase work in the same PR.

## Stop criteria

Stop and reassess before BBR if:

- model-based control requires rebuilding lwIP recovery/SACK machinery;
- the project accumulates a large hard-to-rebase lwIP fork;
- sampler/pacer metadata approaches hosted-Linux memory cost;
- unavoidable BDP/window buffering dominates small-host RAM;
- pacing accuracy requires per-flow timers, periodic polling, or busy spinning.

A small conventional-CC endpoint remains a valid outcome if BBR is rejected by evidence.
