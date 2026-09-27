# Development and agent handoff contract

This repository is the durable project memory. A human or coding agent should be able to continue from repository state without relying on a private chat transcript.

## Read order

1. `ARCHITECTURE.md` — architecture and ownership boundaries.
2. `docs/standards-conformance.md` — normative RFC, upstream, Linux-reference, and project-policy hierarchy.
3. `docs/lwip-roadmap.md` — milestone order, exit evidence, stop criteria.
4. `docs/ci.md` — qualification model and retained runs.
5. active milestone document under `docs/milestones/`.
6. relevant source and validation scripts.

Historical milestone documents explain design evolution; they do not override `ARCHITECTURE.md`.

## Development rules

- Use GitHub Actions as the primary validation/debug environment.
- Preserve diagnostics and failure evidence; do not rerun away a useful failure without understanding it.
- Do not weaken a gate solely to make CI pass.
- Do not trust a workflow's green conclusion if logs contradict the intended assertion; fix the harness and rerun fail-closed.
- If CI exposes a harness assumption, fix the harness while retaining or strengthening the product assertion.
- Behavior/architecture/CI milestone changes update repository docs in the same PR.
- When an applicable RFC exists, treat it as the normative source and scope claims to the implemented/qualified subset; use Linux as the primary behavioral source only for non-RFC implementation mechanisms or protocols without an RFC target, as defined in `docs/standards-conformance.md`.
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

The SACK/RACK patch remains an experimental transport increment, but SACK evidence and retransmission policy are separate build concerns. `TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE` enables the SACK negotiation/scoreboard/delivery substrate needed by RACK; `TCP_SHIFT_EXPERIMENTAL_LEGACY_SACK_SELECTOR` enables only the old fixed-count selective-retransmission compatibility path. `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY` is a deprecated alias for non-RACK compatibility during migration. RACK builds must enable SACK evidence and must not enable the legacy selector. None of these options may silently become part of production/default builds. Both patches stay within the existing three-file upstream boundary; any new upstream file still requires an architectural reason, bounded surface, provenance coverage, and regression evidence.

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

## Active next milestone: replace legacy recovery heuristics with RFC-defined transport recovery

The current development target is no longer "tune BBR until the deterministic-loss benchmark improves." Transport semantics come first.

Merged 2026-09-27 recovery work establishes the new baseline:

- PR #67 fixed RFC 8985 mixed cumulative-ACK/SACK ordering and restored the deterministic 28-drop path from fragmented recovery to the same recovery-episode count as the sender-SACK reference;
- PR #68 made RFC 8985 time evidence the sole fast-loss oracle whenever RACK-TLP is enabled. An unavailable RACK status now fails closed instead of falling back to the legacy fixed three-later-SACK detector;
- PR #69 proved the same RACK transport path across Reno, CUBIC, and internal BBR and fixed a selector integration bug that had dropped `on_sack` / `on_recovery_exit` transport hooks for loss-based controllers.

The intended transport ownership is:

```text
SACK parsing / scoreboard / delivery evidence
        -> RFC 8985 RACK loss detection
        -> RACK retransmission selection
        -> RFC 8985 TLP/PTO tail probing
        -> ordinary RTO only as conservative terminal fallback

Reno / CUBIC / internal BBR
        -> consume transport loss/recovery events
        -> do not own loss detection or retransmission selection
```

Development therefore proceeds by replacement, not accumulation:

1. **SACK evidence and selector ownership are now separated.** SACK parsing, scoreboard state, per-segment delivery evidence, D-SACK interpretation, and retransmission identity are an independently buildable transport substrate. RACK and the legacy fixed-count selector cannot be enabled together.
2. **RACK and legacy selection are now separate code paths.** RFC 8985 RACK timing evidence and the fixed three-later-SACK heuristic no longer share a policy helper; they share only neutral segment requeue/accounting mechanics.
3. **Delete the legacy fixed-count selector once rollback evidence is no longer needed.** The compatibility selector remains available only for explicit non-RACK differential qualification. Do not route new functionality through it.
4. **RACK recovery-timer lifecycle is qualified.** Teardown cancels the exact flow generation, stale releases are harmless across registry-slot reuse, and the real epoll-owned recovery scheduler reuses one process-wide timerfd without per-flow timers/threads/polling.
5. **Measure the incremental RACK cost next.** Rerun P3 memory/capacity and timer/wakeup/CPU gates with RACK-TLP enabled. Do not widen budgets to make the feature fit.
6. **Only then consider exposure or final legacy-selector deletion.** RACK-TLP remains experimental/default-OFF until resource qualification is complete. Public BBR selection remains a separate exposure decision and still requires provider/OpenVZ evidence.
7. **Keep BBR reference discipline.** BBR has no published RFC target; use Linux BBR behavior as the primary differential/reference implementation and the current IETF BBR draft only as a secondary semantic reference. Do not tune BBR gains to compensate for a transport/recovery defect.

Current main checkpoint after PR #69:

```text
main: 7bd0e310b50ed8595e6e8abb77488d131f35fc77

RACK-TLP:
- RFC 8985 is the sole fast-loss oracle in RACK builds
- mixed cumulative ACK + SACK ordering qualified
- ordinary tail loss and application-limited tail loss qualified
- lost retransmission qualified without RTO
- below/above-reordering-window behavior + D-SACK adaptation qualified
- 28-drop exact recovery qualified
- Reno / CUBIC / internal BBR controller matrix qualified
- remaining production blocker before the next exposure/delete review: incremental RACK memory/wakeup/timer cost
```

The compact internal BBR controller remains an internal/experimental controller. The remaining deterministic performance delta is a measurement topic, not permission to reintroduce non-RFC transport shortcuts.

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
