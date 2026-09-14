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

The production lwIP pin is:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

The repository-owned integration patch is `patches/lwip-p4-cc-hooks.patch`. `scripts/fetch-lwip.sh` records pristine critical-source hashes first, then applies that patch. Provenance CI independently proves modified `tcp.c`, `tcp_in.c`, and `tcp_out.c` are exactly the patch result for the pin.

P5a/P5b extend hook semantics only inside already-controlled `tcp_in.c` / `tcp_out.c`; no new upstream file was added. Do not expand the lwIP patch surface casually. Any new upstream file requires an architectural reason, bounded hook surface, provenance coverage, and regression evidence.

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

The runtime is deadline/readiness driven:

- epoll handles TUN/backend readiness;
- epoll timeout is derived from `sys_timeouts_sleeptime()`, not a fixed tick;
- `sys_check_timeouts()` runs after an actual readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only for real queued packets;
- backend interests are removed/suppressed when no useful progress is possible;
- app-limited marking occurs when the real backend read path reaches `EAGAIN`, not from a periodic flow scan.

P5b qualification observed zero runtime CPU ticks during an application-pause measurement window while still recording app-limited enter/sample/exit.

P5c must preserve or improve this behavior. Default design:

```text
paced flows -> process-wide min-heap -> one one-shot CLOCK_MONOTONIC timerfd -> epoll
```

Rules:

- one timerfd for the process, never one per flow;
- arm to the earliest pending pacing deadline only;
- disarm when no paced send is pending;
- no periodic pacing tick;
- no busy spin;
- timerfd/epoll mechanics stay outside `src/cc/`;
- record timer arms/disarms/expirations, pacing wakeups, released bytes, lateness, heap occupancy, idle wakeups, CPU, and memory in CI.

## P0-P4 status

P0 through P4 are runner-qualified and mandatory regressions. P4 owns public-side base cwnd/ssthresh policy for ACK, fast loss, and RTO through the generic controller, while lwIP retains retransmission/recovery mechanics.

`src/cc/` is independently buildable pure C. Conventional Reno uses 16 bytes caller-owned state and publishes zero pacing rate.

## P5a status — complete

P5a established high-resolution send/ACK timestamps, unique delivered-byte accounting, and retransmission-safe lazy segment metadata without enlarging upstream `struct tcp_seg`.

Final qualified behavior head:

```text
ce6c89f399bed3535be52138e3c96ea2ea061b38
```

P5 run `34821205375`, job `103903019956`, artifact `10338108552` retained exact 262144-byte delivery through normal, fast-loss, and RTO with zero metadata leaks/errors. P5a slots were 32 bytes.

The original false-green harness bug must not be reintroduced: exact `key=value` token parsing and fail-closed checker exit propagation are required; `.build` artifact uploads need `include-hidden-files: true`.

## P5b status — complete, merge-ready after docs-only verification

P5b converts the ledger into transport-neutral ACK delivery-rate observations and event-driven app-limited state.

Generic ACK input now carries:

- selected delivery rate;
- selected interval plus send and ACK intervals;
- latest RTT when valid;
- newly delivered payload bytes;
- prior inflight;
- VALID / APP_LIMITED / RETRANSMITTED / RTT_VALID flags.

Reno ignores the new sample and remains keyed to `acked_bytes`.

The lwIP adapter owns all queue/sequence/time mechanics. Sidecar entries are now 56 bytes and include sequence/progress, first-transmit/send-phase snapshots, delivered snapshots, prior inflight, and app-limited/retransmission state. Cumulative ACK progress is accounted before upstream frees sidecars; FIN sequence space is excluded from payload delivery. Retransmission reuses sidecar state and RTO candidates withhold RTT.

App-limited marking is invoked only from the bridge's real backend-read `EAGAIN` path. The adapter verifies no unsent data and available transport capacity before setting a delivered+inflight marker.

Final behavior head:

```text
327efe7e29adfc230e7d201b466f2bd4980e976c
```

Runs:

```text
upstream provenance  34843587049  success
P0                   34843587091  success
P1                   34843587026  success
P2                   34843587113  success
P3                   34843587033  success
P4                   34843587045  success
P5                   34843586990  success
```

P5 run `34843586990`, job `103974027867`, artifact `10347003115`:

```text
normal:    138/138 valid samples, invalid=0, delivered=262144
fast-loss: 121/121 valid samples, loss_events=1
rto:       135/135 valid samples, retransmitted_samples=3, timeout_events=2
app-pause: delivered=135168, app_limited_samples=7,
           enters=2 exits=2 pause_cpu_ticks=0 event_driven=ok
```

P3 run `34843587033`, job `103974029078`, artifact `10346739278`:

```text
warm fixed PSS: 347 KiB
conservative active slope: 37.710938 KiB/flow
128-active projection: 5174 KiB
8-MiB budget remaining: 3018 KiB = 23.578 KiB/flow
3x128 drain growth: 5 KiB
idle CPU: 0 ticks/s
```

This remains tcp-shift process-PSS qualification only; backend kernel/application/provider memory is excluded.

## Active next milestone: P5c event-driven pacer

Do not combine P5b sampler changes with BBR state logic. P5c must first qualify scheduler mechanics independently.

Initial implementation direction:

1. add a runtime-owned pacer module, not controller-owned Linux code;
2. maintain one process-wide min-heap of pending flow/send deadlines;
3. create one nonblocking/cloexec `timerfd` using `CLOCK_MONOTONIC`;
4. register it with the existing epoll owner;
5. arm one-shot to the current earliest deadline and disarm when heap empty;
6. make cancellation/stale flow entries generation-safe so teardown cannot dereference freed state;
7. leave Reno's zero pacing rate on the existing unpaced path;
8. use a deterministic test controller/policy with a fixed nonzero pacing rate to qualify the scheduler before BBR exists;
9. integrate pacing at the narrowest lwIP send-decision boundary without replacing `tcp_output()`/recovery mechanics;
10. preserve P0-P5 regressions and rerun P3 memory/CPU/wakeup measurements.

P5c CI must include idle behavior, small packets, sustained/high-BDP pacing, requested-vs-actual deadline lateness, timer arm/disarm/expiration counts, heap peak, stale/cancel cleanup, and CPU. Busy spinning or fixed periodic polling is a design failure, not a temporary shortcut.

## Distance to tcp-shift BBR

```text
P5a delivery ledger        complete
P5b rate/app-limited       complete
P5c event-driven pacing    next
P6 tcp-shift BBR           then active
```

Only the pacing substrate remains before BBR becomes the active controller milestone. The difficult TCP ownership/recovery and delivery-rate sampling boundaries are already qualified.

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

Stop and reassess before BBR if model-based control requires rebuilding lwIP recovery/SACK machinery, the project accumulates a large hard-to-rebase lwIP fork, sampler/pacer metadata approaches hosted-Linux memory cost, unavoidable BDP/window buffering dominates small-host RAM, or pacing accuracy requires per-flow timers, periodic polling, or busy spinning.
