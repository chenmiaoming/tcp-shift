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

## RFC 9937 PRR recovery — integrated

RFC 9937 Proportional Rate Reduction (PRR) is now the recovery-rate mechanism for the experimental RACK+SACK path when Reno or CUBIC owns congestion policy.

Ownership is intentionally split:

```text
SACK delivery ledger
    -> DeliveredData / scoreboard evidence

RACK
    -> sole project-owned loss oracle
    -> sole project-owned selective repair selector

Reno / CUBIC
    -> choose ssthresh target

RFC 9937 PRR
    -> RecoverFS / prr_delivered / prr_out
    -> SafeACK selects CRB vs SSRB
    -> SndCnt controls recovery send credit

pacer
    -> schedules only PRR-eligible bytes

RTO
    -> terminal fallback
```

The live adapter reuses the existing delivery/RACK sidecar; it does not create a second SACK scoreboard or loss detector. RecoverFS snapshots outstanding sequence space less SACKed delivery evidence, RACK-aware inflight excludes bytes already delivered or currently declared lost, and retransmitted bytes naturally re-enter inflight with their new transmission timestamp.

The controlled lwIP recovery path keeps RFC 6582 partial-ACK retransmission selection only as a non-RACK fallback. On RACK+SACK flows, partial ACKs do not directly call the NewReno `tcp_rexmit()` selector; RACK selects repairs and PRR only controls how many bytes may be sent.

Internal BBR is explicitly outside this integration. It retains its controller-owned recovery window and all PRR runtime counters must stay zero.

Current hosted-runner evidence includes:

```text
deterministic first-send loss, RTT 260 ms, 10 Mbit, 4 MiB:
- Reno:  28 drops, 28 retransmissions, 28 PRR enters/exits, 0 RTO
- CUBIC: 28 drops, 28 retransmissions, 28 PRR enters/exits, 0 RTO
- internal BBR: PRR counters remain zero

single recovery flight, two deterministic losses, Reno:
- fault_drops=2
- retransmit_events=2
- prr_recovery_enters=1
- prr_recovery_exits=1
- prr_safe_ack_events=1
- timeout_events=0
- payload_integrity=ok
```

PRR is a standards/recovery-correctness change, not a performance shortcut. On the deterministic 28-drop CUBIC differential path, PRR does not close the remaining Linux CUBIC goodput gap and may be slightly slower than the former NewReno-window recovery. The next performance work should therefore compare PRR/RACK recovery timing, inflight evolution, and pacing-rate publication against Linux rather than tuning RFC 9937 away from its specified behavior.

## 2026-09-30 post-PRR recovery checkpoint

PRs #83-#88 close the first RFC 9937 integration/debug cycle:

- #83 added the independently tested RFC 9937 PRR core without changing live recovery.
- #84 wired PRR into RACK+SACK Reno/CUBIC recovery, kept RFC 6582 NewReno as non-RACK fallback, kept internal BBR controller-owned recovery, and qualified 28-drop plus live SafeACK/SSRB behavior.
- #85 traced post-PRR CUBIC recovery against Linux and showed the remaining gap was not explained by CUBIC beta, RFC 5681 FlightSize, or a simple pacing-rate formula error.
- #86 proved that PRR was producing useful SndCnt and that the intended sequence-space window mapping admitted queued data; the apparent missing sends were not caused by the userspace pacer itself.
- #87 found the integration defect: production and qualification pacing wrappers replaced `hook.ops` without forwarding the base adapter's PRR-aware `effective_cwnd`. Native `tcp_output()` therefore fell back to plain `pcb->cwnd`, suppressing most PRR new-data credit. Both wrappers now forward `effective_cwnd`, and CI fails closed if that forwarding disappears.
- #88 quantified the remaining first-loss CUBIC cwnd delta without changing behavior. tcp-shift first-loss pre-cwnd is `116800 B`; Linux's last pre-retransmission sample is `128480 B`; tcp-shift has `4380 B` SACK-delivered ahead of SND.UNA. Hypothetically adding that Linux-style SACK credit still leaves about `7300 B` unexplained.

Latest same-path #88 hosted-runner evidence:

```text
RTT=260 ms, rate=10 Mbit/s, payload=4 MiB, 28 deterministic first-send drops

tcp-shift BBR / Linux BBR:   5.285094 / 5.492110 Mbit/s = 0.962307
tcp-shift CUBIC / Linux:     0.548827 / 0.741245 Mbit/s = 0.740412

CUBIC:
- loss_events=28
- recovery_exits=28
- timeout_events=0
- first pre-cwnd=116800 B
- first FlightSize=116800 B
- first post-cwnd=81760 B
- first SACK-ahead bytes=4380 B
- hypothetical RFC-cwnd + SACK-ahead=121180 B
- Linux pre-first-retrans cwnd=128480 B
- residual first-cwnd gap after SACK-ahead=7300 B

PRR:
- episodes=28
- ACK decisions=469
- PRR TX events=335
- median inflight=18980 B
- median SndCnt=1605 B
- pacer deferrals/resumes=143/143
```

Interpretation rules for the next work:

1. Keep RFC 9937 PRR and RFC 8985 RACK semantics fixed unless a standards error is proven.
2. Keep RFC 9438 cumulative-ACK semantics for CUBIC growth. Linux `tcp_newly_delivered()` counting newly ACKed or SACKed packets is an implementation differential, not authority to feed pure SACK credit into tcp-shift CUBIC.
3. The remaining first-loss gap predates recovery, so PRR cannot explain all of it. Investigate pre-loss ACK/cwnd evolution, HyStart++/slow-start exit timing, ACK aggregation and Linux implementation details before changing recovery.
4. Do not tune BBR gains, CUBIC beta, FlightSize, or CI thresholds to hide the remaining differential.
5. RACK-TLP, PRR integration, and ECN remain experimental/default-OFF pending provider/OpenVZ qualification and an explicit exposure decision.

## Pinned lwIP and controlled patch

Production lwIP pin:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

The repository-owned integration is a controlled two-patch chain: `patches/lwip-p4-cc-hooks.patch` followed by `patches/lwip-sack-recovery.patch`. `scripts/fetch-lwip.sh` records pristine critical-source hashes before applying either patch. Provenance CI independently proves the modified `tcp.c`, `tcp_in.c`, and `tcp_out.c` are exactly the chained patch result for the pin.

The SACK/RACK patch remains an experimental transport increment, with evidence and recovery policy kept separate. `TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE` enables the SACK negotiation/scoreboard/delivery substrate needed by RACK; `TCP_SHIFT_EXPERIMENTAL_RACK_TLP` is the only project-owned selective-retransmission policy. The old fixed-count selector and `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY` compatibility alias have been removed and their old CMake flags fail closed. RACK builds must enable SACK evidence. The experimental RACK path must not silently become part of production/default builds. Both patches stay within the existing three-file upstream boundary; any new upstream file still requires an architectural reason, bounded surface, provenance coverage, and regression evidence.

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

## Completed transport milestones: RACK, ECN, and PRR

The RFC 8985 recovery replacement is now closed on main for the current experimental scope:

- PR #75 deleted the legacy fixed-count SACK recovery selector;
- PR #77 made RACK the fast-loss/recovery-entry owner on RACK+SACK flows and connected the RFC 8985 section 7.4.2 no-SACK DupAck TLP ambiguity signal;
- PR #76 restarted ordinary RTO timing after every valid PTO probe opportunity while flight remains, including no-probe and failed-probe paths.

Current main checkpoint after merged PR #76:

```text
main: 3dd4fefe2b9bfdbfc02616e6c65b417a63c17830

RACK-TLP:
- RFC 8985 time evidence is the sole fast-loss oracle in RACK builds
- RACK owns selective retransmission and fast-loss entry on SACK-negotiated RACK flows
- mixed cumulative ACK + SACK ordering qualified
- ordinary and application-limited tail loss qualified
- lost retransmission qualified without RTO
- below/above-reordering-window behavior + D-SACK adaptation qualified
- 28-drop exact recovery qualified
- Reno / CUBIC / internal BBR controller matrix qualified
- generation-safe recovery timer teardown qualified
- incremental resource cost qualified
- TLP probe opportunity -> ordinary RTO restart semantics qualified
```

RACK-TLP stays experimental/default-OFF pending provider/OpenVZ evidence and a separate exposure decision. Recovery work should not be reopened to compensate for congestion-controller performance.

The next standards milestone is ECN, split by ownership rather than folded into CUBIC:

```text
IP/TCP transport
    RFC 3168 negotiation + ECT/CE/ECE/CWR
        -> explicit ECN congestion event
            -> Reno / CUBIC congestion response

loss path
    RACK loss evidence
        -> retransmission + congestion event

ECN path
    no packet-loss fact
    no retransmission trigger
```

PR #78 is the first ECN transport increment and remains experimental/default-OFF. Its contract is:

1. **Keep ECN distinct from loss.** The generic CC boundary has an explicit `on_ecn` event; ECN must never synthesize a RACK loss or retransmission.
2. **Use the RFC 3168 baseline first.** Active SYN uses ECE+CWR, a supporting SYN-ACK uses ECE, CE on valid negotiated data latches ECE, and CWR closes the receiver echo episode.
3. **Mark only eligible first-transmission data ECT(0).** Pure ACK/control traffic and retransmitted data remain Not-ECT. Preserve DSCP bits when changing the two ECN bits. RFC 8311 relaxations are not part of this profile.
4. **Keep state bounded.** The ECN build reuses free PCB flag bits, one conditional CWR response boundary, and one byte of 1-SMSS timer-gate state. It reuses lwIP's ordinary retransmit timer; no per-packet heap allocation, polling thread, or additional timer object is introduced.
5. **Give CUBIC the RFC 9438 ECN reduction semantics.** ECN uses beta=0.7 and repeated congestion events may reduce cwnd to 1 SMSS. The packet-loss path retains its existing recovery floor.
6. **Use the RFC-defined 1-SMSS rate response.** When a fresh ECE arrives while cwnd is already one SMSS, reset lwIP's retransmit timer and gate new data until that timer expires; do not invent a project-specific pacing multiplier. The timer expiry releases new data without synthesizing packet loss or an RTO congestion event.
7. **Do not silently extend internal BBR.** The compact internal BBR path has no ECN response in this increment and must reject an ECN-negotiated PCB in the experimental ECN build.
8. **Preserve provenance.** ECN lives in a third controlled lwIP patch after P4 hooks and SACK/RACK integration; default builds compile the patch with ECN disabled.

Current #78 qualification evidence:

```text
controlled lwIP patch replay:       success
ECN=ON IPv4/IPv6 production build: success
CC ECN model/contracts:             success
live pcap linktype:                 RAW / 101
captured TCP packets:               14
first data packet:                  #4
first ECE ACK:                      #5
CWR new-data packet:                #6
first post-CWR ACK without ECE:     #7
forced CE marks:                    1
controller ECN/loss/RTO events:     1 / 0 / 0
```

The live gate proves the baseline wire sequence `SYN(ECE+CWR) -> SYN-ACK(ECE) -> ECT(0)/CE data -> ECE ACK -> CWR on new data -> ECE cessation` and proves that the CE signal reaches CUBIC without creating loss or timeout events.

PR #79 extends that evidence to persistent congestion on the real TUN path. The qualification sends 18 independent CE-marked data episodes at a 1200-byte peer MSS. CUBIC reaches one SMSS, the RFC 3168 section 6.1.2 retransmit-timer send gate is entered and released repeatedly, the exact 21600-byte payload completes, and both synthetic packet-loss events and ordinary CC timeout events remain zero. The missing qualification signal was traced to the production pacing selector failing to forward the gate-lifecycle hook, not to the transport gate itself. The selector and qualification wrappers now forward that hook explicitly and ECN CI requires the forwarding boundary.

After #79, hosted-runner RFC 3168/CUBIC ECN qualification is complete for the current experimental/default-OFF profile. Provider/OpenVZ evidence and any default/public exposure decision remain separate.

After ECN transport + CUBIC ECN are qualified, return to BBR performance/reference work. BBR still has no published RFC target; Linux BBR remains the primary differential reference and the IETF BBR draft only a secondary semantic reference.

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
