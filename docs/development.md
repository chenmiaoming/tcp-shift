# Development and agent handoff contract

This repository is the durable project memory. A human or coding agent should be able to continue from repository state without relying on a private chat transcript.

## Read order

1. `ARCHITECTURE.md` — architecture and ownership boundaries.
2. `docs/lwip-roadmap.md` — milestone order, exit evidence, stop criteria.
3. `docs/ci.md` — qualification model and retained runs.
4. active milestone document under `docs/milestones/`.
5. relevant source and validation scripts.

Historical milestone documents explain design evolution; they do not override `ARCHITECTURE.md`.

## Development rules

- Use GitHub Actions as the primary validation/debug environment.
- Preserve diagnostics and failure evidence; do not rerun away a useful failure without understanding it.
- Do not weaken a gate solely to make CI pass.
- Do not trust a workflow's green conclusion if logs contradict the intended assertion; fix the harness and rerun fail-closed.
- If CI exposes a harness assumption, fix the harness while retaining or strengthening the product assertion.
- Behavior/architecture/CI milestone changes update repository docs in the same PR.
- Keep source modules separate even while the product remains one process.
- Keep public TCP and backend TCP semantics distinct.
- Prefer readiness/deadline-driven runtime work over fixed polling.
- Do not infer provider/OpenVZ qualification from GitHub-runner qualification.
- If a behavioral head is green and later commits are docs-only, merge may rely on the last green behavioral head only after a commit comparison proves the tail is non-behavioral.

## Pinned lwIP and controlled patch

Production lwIP pin:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

The repository-owned integration is a controlled two-patch chain: `patches/lwip-p4-cc-hooks.patch` followed by `patches/lwip-sack-recovery.patch`. `scripts/fetch-lwip.sh` records pristine critical-source hashes before applying either patch. Provenance CI independently proves the modified `tcp.c`, `tcp_in.c`, and `tcp_out.c` are exactly the chained patch result for the pin.

The sender-SACK patch is an experimental transport increment, default OFF behind `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY`. It must not silently become part of production builds. Both patches stay within the existing three-file upstream boundary; any new upstream file still requires an architectural reason, bounded surface, provenance coverage, and regression evidence.

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

The runtime is readiness/deadline driven:

- epoll owns TUN/backend/pacer readiness;
- lwIP timeout sleep comes from `sys_timeouts_sleeptime()`, not a fixed tick;
- `sys_check_timeouts()` runs after an actual readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only for real queued packets;
- backend interests are removed/suppressed when no useful progress is possible;
- app-limited marking occurs when the real backend read path reaches `EAGAIN`;
- pacing uses one process-wide min-heap and one one-shot `CLOCK_MONOTONIC` timerfd;
- the pacing timer is armed only to the earliest deadline and disarmed when the heap is empty;
- there is no per-flow pacing timer/thread, periodic pacing tick, flow scan, or busy spin.

P5c qualification preserved this shape. A 15.067-second high-BDP/window-pressure workload consumed one 100-Hz runtime CPU tick while producing 87 real timer releases.

## P0-P4 status

P0 through P4 are runner-qualified and mandatory regressions. P4 owns public-side cwnd/ssthresh policy for ACK, fast loss, and RTO through the generic controller while lwIP retains retransmission/recovery mechanics.

`src/cc/` is independently buildable pure C. Conventional Reno uses 16 bytes caller-owned state and publishes zero pacing rate.

## P5 status — complete

### P5a — delivery ledger

P5a established high-resolution send/ACK timestamps, unique delivered-byte accounting, and retransmission-safe lazy segment metadata without enlarging upstream `struct tcp_seg`.

### P5b — rate sampler + app-limited

P5b converts that ledger into transport-neutral ACK delivery-rate observations. Sidecars are 56 bytes and retain sequence/progress, timing/delivery snapshots, prior inflight, and app-limited/retransmission state. Cumulative ACK progress is accounted before upstream frees sidecars; FIN sequence space is excluded. Retransmitted candidates withhold RTT.

App-limited marking is invoked only from the bridge's real backend-read `EAGAIN` path and requires available transport capacity with no unsent data.

### P5c — event-driven pacer

P5c converts generic nonzero pacing policy into actual data-send eligibility while preserving native lwIP `tcp_output()` and recovery mechanics.

The production scheduler is runtime-owned:

```text
controller pacing policy
 -> lwIP adapter/send eligibility
 -> process-wide min-heap
 -> one one-shot monotonic timerfd
 -> existing epoll owner
 -> native tcp_output() resume
```

Production Reno remains zero-rate/unpaced. The qualification-only fixed-paced Reno uses the same Reno cwnd/ssthresh state machine and publishes `65536 B/s` through the generic policy path.

The deterministic lifecycle gate schedules a future deadline, unbinds it, requires one cancellation, then proves the old generation cannot access stale flow state. This gate found and fixed a real manual-unbind ext-arg bug: pinned lwIP forbids `tcp_ext_arg_set_callbacks(..., NULL)`, so unbind now clears only ext-arg data and leaves the static callback table installed.

Final behavioral head:

```text
046152dbaba56a0be3b1d2a1902fee6f2bbf9044
```

Same-head workflow set:

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

P5c run `34870859911`, job `104066140125`, artifact `10358951402` retained:

```text
lifecycle: schedules=1 cancels=1 stale_release_safe=1
single-flow fixed pacing: positive deferral/release, heap final 0
4-flow pacing: heap peak 4, 176 releases, integrity=ok
fast-loss paced recovery: loss=1 timeout=0
RTO paced recovery: timeout=2
scheduler/stale/controller errors=0
```

High-BDP/window-pressure gate:

```text
pacing rate:            65536 B/s
ACK-path delay:         750 ms
nominal BDP:            49152 B
TCP window:             32768 B
payload:                262144 B exact
runtime wall:           15.067229 s
runtime CPU:            0.066%
max pacing lateness:    65199 ns
loss/timeout:           0/0
heap final:             0
```

Latest P3 rerun `34870859880`, job `104066205524`, artifact `10358454897`:

```text
warm fixed PSS:           367 KiB
conservative active slope: 37.773438 KiB/flow
128-active projection:    5202 KiB
8-MiB budget remaining:   2990 KiB = 23.359 KiB/flow
3x128 drain growth:       5 KiB
idle CPU:                 0 ticks/s
```

This is process-PSS qualification only; backend kernel/application/provider memory is excluded.

## Active next milestone: close the residual BBR gap and qualify provider/OpenVZ before exposure

The compact P6 BBR controller is implemented and runs on real lwIP PCBs through the existing generic observation/policy/pacing surfaces. The merged path now includes explicit-loss ProbeBW semantics (#40), bounded sender-SACK selective recovery (#41), deterministic first-send-loss qualification (#44), SACK-aware delivery/rate accounting (#45), and SACK-aware effective-cwnd send gating (#49). Sender SACK remains compile-time experimental/default-OFF; production `tcp-shift-p2` still exposes only `reno|cubic`.

PR #49 proved that the previous residual loss-path gap was partly transport send-window accounting rather than BBR gain tuning. On the stable 260 ms / 10 Mbit/s / 4 MiB / 28-drop reference, tcp-shift now measures 5.052860 Mbit/s versus Linux BBR at 5.491376 Mbit/s, a 0.920145 diagnostic ratio with exact 28/28 retransmission accounting and zero RTO fallback.

The next development increment remains evidence-first:

1. qualify `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY=ON` on the target provider/OpenVZ environment with real RTT/loss/memory/TUN behavior;
2. reproduce the deterministic first-send-loss case outside GitHub runners where practical and verify the SACK-aware send-window behavior on the target path;
3. keep the existing P3 memory gate and measure sender-SACK-enabled constrained-host cost rather than widening thresholds;
4. investigate the remaining roughly 8% deterministic goodput gap using retained recovery/TX-gap telemetry, with ACK aggregation, packet-conservation duration, rate-sample timing, and residual send-idle behavior as hypotheses rather than preselected fixes;
5. make the IPv4 deployment path reproducible for a real VPS, including TUN/forwarding/nftables prerequisites and cleanup;
6. only after provider evidence and another explicit review, decide whether to register `bbr` behind an experimental production selector; Reno remains the default;
7. keep `bbrv3` separate if full current-draft semantics are ever implemented.

Do not call the compact controller Linux BBR. The current stable first-send-loss reference is about 0.920x Linux BBR goodput after #49; that is substantially closer but still explicitly not parity.

## Merge discipline

A milestone/sub-increment may be squash-merged when:

- behavioral exit criteria are mechanically satisfied;
- relevant prior regressions are green;
- diagnostics are retained;
- docs match the behavior head;
- PR review/comments/threads are clean;
- any docs-only tail is verified by commit comparison.

Once mergeable, merge rather than accumulating unrelated later-phase work in the same PR.

## Stop criteria

Stop and reassess if model-based control requires rebuilding lwIP recovery/SACK machinery, the project accumulates a large hard-to-rebase lwIP fork, controller/sampler/pacer metadata approaches hosted-Linux memory cost, unavoidable BDP/window buffering dominates small-host RAM, or pacing/controller accuracy requires per-flow timers, periodic polling, or busy spinning.
