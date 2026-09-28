# lwIP route: architecture and milestones

This roadmap defines implementation order and exit evidence. `ARCHITECTURE.md` remains the current product source of truth.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. Fixed memory cost is the first optimization target.

```text
public IPv4/IPv6 packet -> host routing/netfilter -> L3 TUN -> lwIP TCP
                                                        -> raw TCP callbacks
                                                        -> single-owner bridge
                                                        -> 127.0.0.1 backend
```

The public and backend TCP legs are distinct. Congestion control belongs to the lwIP public leg. Public operation is dual-stack/IPv6-capable; the backend remains IPv4 loopback.

## Hard boundaries

- L3 TUN, not TAP/Ethernet.
- One mutable owner for lwIP; no per-flow forwarding threads.
- `NO_SYS=1`; no lwIP socket/netconn/tcpip-thread path.
- IPv6-only public operation is required.
- Linux host integration, lwIP transport integration, bridge logic, and congestion-control policy remain separate modules.
- `src/cc/` stays pure C and independently buildable.
- Linux timerfd/epoll pacing stays outside `src/cc/`.
- Pacing is deadline/event driven: no periodic pacing poll loop, per-flow timerfd/thread, or busy spin.
- Do not call an experimental controller Linux BBR unless transport semantics actually match.

## P0: reproducible lwIP userspace build — complete

Exact upstream pinning, explicit dual-stack TCP `NO_SYS=1` source allowlist, configuration/source/binary/RSS gates, and clean-runner artifact smoke are qualified.

## P1: dual-stack L3 TUN and ingress lifecycle — complete

P1 qualifies one nonpersistent `IFF_TUN | IFF_NO_PI` adapter, lwIP timer-driven epoll ownership, bounded whole-packet TX retry, nonfatal RX drops, IPv4/IPv6 packet paths, exact product-owned ingress DNAT/conntrack lifecycle, forwarding prerequisite checks, collision/rollback/cleanup, extension-header-safe IPv6 TCP traversal, and routed ICMPv6 PTB learning.

Provider/OpenVZ capability qualification remains separate.

## P2: dual-stack listener and backend bridge — complete

P2 bridges accepted IPv4/IPv6 public streams to an ordinary nonblocking `127.0.0.1:<backend-port>` socket. Public->backend bytes remain in lwIP pbufs until backend `writev()` commits them; backend->public bytes are removed only after `tcp_write(..., COPY)` accepts them. The bridge therefore adds no fixed bidirectional application buffer.

P2 qualifies stream integrity, real bidirectional blocking, half-close, refusal/reset recovery, concurrent flows, active-flow shutdown, and long-lived reuse without sustained RSS ratcheting.

## P3: memory/capacity baseline — complete and rerun each transport increment

P3 separates tcp-shift process residency from backend Linux TCP/kernel state and measures idle flows, both active-window directions, repeated load/drain floors, and CPU.

Latest P5c-head evidence:

```text
warm fixed process PSS:           367 KiB
fully-window-resident slope:      37.773438 KiB/flow
128-active projected PSS:        5202 KiB
8-MiB process budget remaining:  2990 KiB = 23.359 KiB/flow
3x128 first-to-last drain growth: 5 KiB
idle CPU:                        0 ticks/s
```

This remains a process-PSS planning model, not a full-host capacity guarantee.

## P4: generic CC boundary + lwIP adapter — complete

P4 established an independently buildable pure-C controller boundary. The generic controller consumes transport-neutral MSS, inflight, peer send window, and cwnd limit; events are init/ACK/loss/RTO; policy publishes cwnd, ssthresh, and optional pacing rate. Conventional Reno uses 16 bytes caller-owned state and requests zero pacing.

Pinned lwIP remains at `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. The controlled patch chain remains confined to `tcp.c`, `tcp_in.c`, and `tcp_out.c`. The second sender-SACK patch is default OFF and exists only for evidence-driven recovery qualification. lwIP still retains retransmission execution, duplicate-ACK processing, recovery/RTO machinery, queues, sequence space, packet construction, and output; tcp-shift only adds bounded scoreboard/selective-requeue logic inside that transport-owned path when the experiment is enabled.

## P5a: retransmission-safe delivery ledger — complete

P5a supplied high-resolution send/ACK timestamps, unique delivered-byte accounting, and lazy per-flow sidecar metadata outside upstream `struct tcp_seg`. Retransmission reuses the same slot and cannot double-count payload delivery.

The first P5 workflow exposed a false-green harness bug; qualification was withheld until exact-token parsing and fail-closed checker exit propagation were fixed.

## P5b: ACK delivery-rate sampler + app-limited — complete

P5b converts the ledger into a transport-neutral ACK rate observation without changing Reno policy. ACK observations carry delivery rate, selected/send/ACK intervals, RTT when valid, newly delivered payload, prior inflight, and validity/app-limited/retransmission flags.

The 56-byte lazy sidecar retains sequence/progress and delivery snapshots. ACK delivery is computed before pinned lwIP frees acknowledged segments; FIN sequence space is excluded. Rate selection uses the larger of send and ACK intervals to resist ACK compression. Retransmitted candidates do not publish RTT.

App-limited detection is event-driven from the real backend `EAGAIN` path; there is no product-side periodic flow scan.

## P5c: event-driven pacer — complete

P5c converts nonzero controller pacing policy into real data-send timing through a narrow lwIP eligibility hook while preserving native `tcp_output()` and recovery mechanics.

Runtime shape:

```text
controller rate
    -> CC adapter / lwIP send eligibility
    -> one process-wide min-heap of absolute deadlines
    -> one one-shot CLOCK_MONOTONIC timerfd
    -> existing epoll owner
    -> native tcp_output() resume
```

Qualified invariants:

- one timerfd for the process, not per flow;
- one-shot earliest-deadline arm/rearm and empty-heap disarm;
- no fixed pacing tick, flow scan, per-flow thread, or busy spin;
- generation-safe cancellation/stale release after teardown;
- timerfd/epoll mechanics remain outside `src/cc/`;
- production Reno remains unpaced (`pacing_rate=0`);
- fast retransmit/RTO execution remains native lwIP;
- multi-flow overlapping deadlines drain correctly;
- high-BDP/window-pressure traffic remains event-driven and low-CPU.

Final behavior head `046152dbaba56a0be3b1d2a1902fee6f2bbf9044` passed provenance/P0/P1/P2/P3/P4/P5/P5c on the same head. P5c run `34870859911`, job `104066140125`, artifact `10358951402` retained:

```text
fixed qualification pacing rate: 65536 B/s
4-flow heap peak:                 4
fast-loss:                       loss=1 timeout=0 paced recovery=ok
RTO:                             timeout=2 paced recovery=ok
teardown:                        cancels=1 stale-generation-safe=1
scheduler/stale/controller errors: 0
```

High-BDP fixture:

```text
ACK-path delay:       750 ms
nominal BDP:          49152 B
current TCP window:   32768 B
payload:              262144 B exact
wall time:            15.067 s
runtime CPU:          0.066%
max pacing lateness:  65199 ns
loss/timeout:         0/0
heap final:           0
```

The observed delivery rate is window-limited near 43.7 KiB/s, consistent with a 32 KiB window at roughly 750 ms. This qualifies pacer correctness under BDP pressure; it does not remove the later need to evaluate window scaling for high-throughput/high-RTT scenarios.

## P6: tcp-shift BBR + RFC-first transport recovery — active

The compact internal `bbr` controller runs on real lwIP PCBs through the generic CC adapter and shared event-driven pacer, while remaining intentionally absent from the public production registry.

The transport work has moved beyond the earlier bounded sender-SACK reference. Merged PRs #67-#69 establish RFC 8985 RACK-TLP as the active recovery direction:

- PR #67 fixes same-ACK cumulative/SACK ordering so RACK processes one ACK's newly delivered segments as one ordered RFC 8985 set; the 260 ms / 10 Mbit/s / 4 MiB / 28-drop reference returns to exact 28 retransmissions, zero RTO, zero false reordering, and the same recovery-episode count as the non-RACK sender-SACK reference;
- PR #68 removes the fixed three-later-SACK fallback from RACK-enabled loss detection. RFC 8985 time evidence is now the sole fast-loss oracle in a RACK build;
- PR #69 separates transport SACK observation from controller ACK credit and preserves `on_sack`, `rack_loss_status`, and `on_recovery_exit` through the production pacing selector. The deterministic 28-drop RACK path is qualified across Reno, CUBIC, and internal BBR.

The target transport stack is now:

```text
SACK scoreboard / delivery evidence
    -> RFC 8985 RACK loss detection
    -> RACK selective retransmission
    -> TLP/PTO for tail probing
    -> ordinary RTO fallback

CC controller
    -> Reno / CUBIC / internal BBR congestion response only
```

The old fixed-count sender-SACK recovery selector and compatibility alias have been removed. SACK evidence remains independent, and RFC 8985 RACK-TLP is now the sole project-owned selective-recovery path. Teardown/stale-generation safety and the incremental memory/timer/CPU resource gate are also qualified.

The next development steps are:

1. complete an RFC 8985 exposure-readiness audit, prioritizing normative MUST/SHOULD timer and recovery semantics over performance tuning;
2. close the section 7.3 PTO/RTO lifecycle requirement so every valid PTO probe opportunity re-arms the ordinary RTO after the attempt, including the no-probe path when the RTT/probe guard blocks transmission;
3. keep RACK-TLP experimental/default-OFF while rerunning the full RACK, resource, controller, and Linux-reference regression set;
4. after transport semantics are closed, perform provider/OpenVZ qualification and make a separate explicit decision about RACK default exposure;
5. keep public `bbr` exposure separate from transport recovery, and do not tune BBR gains to compensate for recovery defects.

BBR remains a compact BBRv1-style controller rather than a Linux-BBR implementation. Because BBR has no published RFC target, Linux BBR remains the primary behavioral/differential reference; the current IETF BBR draft is secondary semantic guidance. Do not compensate for transport defects by tuning BBR gains.

## Milestone state

```text
P0 reproducible lwIP       complete
P1 dual-stack L3 TUN       complete
P2 bridge                  complete
P3 resource baseline       complete / continuously rerun
P4 generic CC boundary     complete
P5a delivery ledger        complete
P5b rate/app-limited       complete
P5c event-driven pacing    complete
P6 BBR + RACK recovery     active / RFC 8985 path qualified, lifecycle/resource closeout pending
```

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, unavoidable BDP buffering dominates the fixed-memory advantage, or pacing/controller accuracy requires per-flow timers, periodic polling, or busy spinning.

The project is still successful if evidence supports only a very small conventional-CC userspace TCP endpoint and rejects BBR.
