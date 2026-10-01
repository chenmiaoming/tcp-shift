# tcp-shift

`tcp-shift` is a low-memory userspace TCP endpoint for constrained VPS/container environments where the tenant cannot select the outer kernel's congestion-control implementation.

## Current direction: lwIP

The active design terminates the WAN-facing TCP connection in lwIP, moves raw L3 packets through TUN, and bridges the accepted byte stream to an ordinary host-loopback backend.

```text
remote client
    |
    | public IPv4 or IPv6 TCP
    v
host netfilter / routing
    |
    v
TUN (L3)
    |
    v
lwIP TCP endpoint
    |
    | accepted byte stream
    v
single-owner userspace bridge
    |
    v
127.0.0.1 backend
```

The public and backend TCP connections are distinct. Congestion control for the public connection belongs to lwIP/tcp-shift; the loopback backend remains an ordinary host Linux socket. Public IPv6 can therefore bridge to the same IPv4 loopback backend.

The runtime uses lwIP `NO_SYS=1`: no lwIP socket/netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. The same L3 adapter and event-loop owner serve IPv4 and IPv6.


## Operator configuration

The service entrypoint is configuration-driven. IPv4/IPv6 selection comes from
the public listener address, and the same `cc` setting selects Reno, CUBIC, or
compact BBR on either family:

```toml
version = 1
cc = "bbr"

[[forward]]
listen = "203.0.113.10:443"
backend = "127.0.0.1:443"
```

```bash
sudo tcp-shift --config /etc/tcp-shift/tcp-shift.toml
sudo tcp-shift --config /etc/tcp-shift/tcp-shift.toml --check
```

The version-1 service schema is strict and startup-only. It uses one
`[[forward]]` mapping today because the current bridge owns one listener;
additional mappings fail closed until the multi-listener runtime exists. See
[`docs/configuration.md`](docs/configuration.md).

## Module boundaries

```text
host/       Linux TUN/interface/netfilter/lifecycle integration
runtime/    epoll owner, fd readiness, timers, process-wide pacer
lwip/       L3/TCP adapter, PMTU, CC/delivery/pacing integration
bridge/     public stream <-> 127.0.0.1 backend
cc/         pure-C congestion-control policy
```

`src/cc/` is independently buildable pure C. It has no lwIP, Linux, TUN, epoll, timerfd, nftables, host-socket, bridge, or lifecycle dependency. `src/lwip/cc_adapter.c` is the deliberately thin transport adapter.

## Congestion-control status

P0 through P5 are runner-qualified. P4 established the generic controller boundary while leaving retransmission execution, duplicate-ACK handling, fast recovery, SACK/recovery, RTT/RTO calculation, segment queues, sequence space, packet construction, and output in lwIP.

P5 completed the prerequisites for model-based congestion control:

1. **P5a — delivery ledger: complete.** High-resolution send/ACK timestamps, unique delivered-byte accounting, and retransmission-safe lazy sidecar metadata.
2. **P5b — rate sampler + app-limited: complete.** Transport-neutral ACK delivery-rate samples with send/ACK intervals, RTT validity, prior inflight, retransmission metadata, and event-driven app-limited marking.
3. **P5c — event-driven pacer: complete.** Controller pacing policy now gates real data sends through one process-wide deadline heap and one one-shot `CLOCK_MONOTONIC` timerfd registered in the existing epoll owner. There is no fixed pacing tick, per-flow timerfd/thread, or busy spin.
4. **P6 — tcp-shift BBR: active and default-build selectable.** The compact BBRv1-style controller publishes real cwnd/pacing policy through the generic adapter, runs on the shared process-wide pacer, and is qualified against Linux BBR on clean single-flow, multi-flow, app-limited, loss, and long-RTT burst scenarios. The config-driven service and both IPv4/IPv6 p2 runtimes select `reno|cubic|bbr`; provider/OpenVZ field qualification remains follow-up work.

An experimental controller will not be described as Linux BBR unless the relevant transport semantics are actually equivalent.

## Event-driven runtime

The runtime is readiness/deadline driven:

- `epoll_wait()` blocks for fd readiness or the next lwIP timeout;
- `sys_check_timeouts()` runs after a real readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only while the bounded TX queue is non-empty;
- backend read/write interests are suppressed when they cannot make progress;
- app-limited detection is triggered by the actual backend read path reaching `EAGAIN`;
- pacing uses one process-wide one-shot timerfd armed only to the earliest pending deadline and disarmed when the pacing heap is empty.

P5b observed zero CPU ticks during an application pause. P5c's sustained high-BDP gate used only one 100-Hz CPU tick over about 15 seconds while paced traffic remained active.

## Build and upstream provenance

lwIP is fetched rather than vendored. `.lwip-baseline` pins:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

```bash
make build
```

The repository carries three controlled lwIP patches. `patches/lwip-p4-cc-hooks.patch` provides the generic ACK/loss/RTO, delivery, pacing, and congestion-event hook boundary. `patches/lwip-sack-recovery.patch` contains SACK/RACK transport integration; sender SACK evidence and RFC 8985 RACK-TLP are enabled by the default production profile, while the historical `TCP_SHIFT_EXPERIMENTAL_*` option names remain available as explicit build controls. `patches/lwip-ecn.patch` adds the RFC 3168 TCP ECN state/packet substrate behind `TCP_SHIFT_EXPERIMENTAL_ECN=ON`, which remains default-OFF. The old fixed-count selective-retransmission selector and its compatibility alias remain removed. Provenance CI hashes pristine critical TCP sources, verifies all patch identities and modified-file scope, reverse-checks the chain, and independently reapplies it to a fresh worktree.

The controlled surface is now `tcp.c`, `tcp_in.c`, `tcp_out.c`, `include/lwip/tcp.h`, and `include/lwip/prot/tcp.h`. ECN header/layout changes are conditional on the experimental build flag; default PCBs retain the previously qualified layout/behavior. P4/P5 keep policy, observations, and pacing behind the project hook boundary; SACK/RACK retains transport recovery ownership; ECN reports congestion without creating packet-loss or retransmission state.

## Qualification summary

### P1 packet path and ingress

P1 qualifies a real nonpersistent L3 TUN, IPv4/IPv6 packet paths, exact ingress DNAT/conntrack lifecycle, bounded whole-packet TX backpressure, nonfatal RX drops, cleanup/collision handling, forwarding preflight, extension-header-safe IPv6 TCP matching, and routed ICMPv6 Packet Too Big learning.

### P2 bridge

P2 qualifies IPv4/IPv6 public streams bridged to a nonblocking `127.0.0.1` backend with bounded transport-driven backpressure, half-close handling, refusal/reset recovery, concurrent flows, active-flow shutdown cleanup, and repeated reuse without sustained RSS ratcheting.

### P3 memory/capacity

P3 measures tcp-shift process PSS separately from backend/kernel state. The current default-RACK/default-BBR-available profile reports:

```text
warm fixed process PSS:             507 KiB
fully-window-resident slope:       44.773438 KiB/flow
128-active projected PSS:        6238 KiB
8-MiB process budget remaining:   1954 KiB = 15.266 KiB/flow
3x128 drain growth:                 5 KiB
maximum warm drain floor:         149 KiB (160-KiB gate)
idle CPU:                           0 ticks/s
```

This is process-PSS planning evidence, not a full-host capacity guarantee.

### P4 controller integration

Real external TUN fault injection proves public-side loss/RTO policy reaches the generic controller while lwIP owns recovery execution.

### P5a/P5b delivery observations

P5a and P5b qualify exact unique payload delivery across natural, fast-loss, and RTO workloads, retransmission-safe 56-byte lazy sidecars, valid ACK delivery-rate sampling, RTT validity, and event-driven app-limited enter/sample/exit behavior.

### P5c event-driven pacing

Final behavior head:

```text
046152dbaba56a0be3b1d2a1902fee6f2bbf9044
```

PR #13 was squash-merged into `main` as `90c6601fb4cbcfc50cbeff41e47310b510237cc2`. The final PR head `8c3ca8cfab5664c6362259e03b3a233ff230bd10` also passed all eight relevant workflows; the behavior qualification remains anchored at the retained evidence head above.

All provenance/P0/P1/P2/P3/P4/P5/P5c workflows passed on that same behavior head. P5c run `34870859911`, job `104066140125`, artifact `10358951402` retained the scheduler, lifecycle, epoll, real bridge, multi-flow, fast-loss, RTO, and high-BDP gates.

Representative results:

```text
fixed policy rate:       65536 B/s
single flow:             96 deferrals, 44 releases, heap final 0
4 concurrent flows:      176 releases, heap peak 4, heap final 0
fast loss:               loss=1 timeout=0, paced recovery=ok
RTO:                     timeout=2, paced recovery=ok
scheduler/stale errors:  0
one process timerfd:     yes
```

The deterministic teardown contract schedules a future deadline, unbinds before expiry, requires exactly one cancellation, then proves an old `(flow_id,generation)` release is safely stale.

The high-BDP gate keeps the same 64 KiB/s policy and applies test-only 750-ms TUN delay:

```text
nominal BDP:             49152 B
current TCP window:      32768 B
payload:                 262144 B exact
runtime wall:            15.067 s
runtime CPU:             0.066%
max pacing lateness:     65199 ns
loss / timeout:          0 / 0
heap final:              0
```

The observed delivery rate becomes window-limited at about 43.7 KiB/s, consistent with a 32 KiB window at roughly 750 ms. P5c therefore proves scheduler correctness/efficiency under BDP pressure; it does not claim the current unscaled window can fully utilize arbitrary high-BDP links. Window scaling remains a later transport concern.

### P6 BBR runtime + RFC 8985 recovery — default-build selectable

The compact `bbr` controller remains internal/experimental, but the transport-recovery direction has changed substantially from the earlier bounded sender-SACK experiment.

Merged PRs #67-#69 establish the current RACK-TLP baseline:

- RFC 8985 mixed cumulative-ACK/SACK ordering is enforced, including the 28-drop regression that exposed the old ordering bug;
- RFC 8985 time evidence is the sole fast-loss oracle whenever RACK-TLP is enabled; the legacy fixed three-later-SACK detector is no longer a fallback in the RACK path;
- ordinary tail loss, application-limited tail loss, lost retransmission, below/above-window reordering, D-SACK adaptation, and exact 28-drop recovery are live-qualified;
- Reno, CUBIC, and internal BBR now receive the same RACK transport evidence. SACK delivery/timing observation is transport-owned and independent from whether a controller consumes selective delivery as ACK credit.

The intended replacement architecture is:

```text
SACK scoreboard / delivery evidence
    -> RFC 8985 RACK loss detector
    -> RACK retransmission selection
    -> TLP/PTO tail probe
    -> ordinary RTO fallback

Reno / CUBIC / internal BBR
    -> congestion response only
```

The old fixed-count sender-SACK selector has been deleted after RACK teardown and incremental resource-cost qualification. SACK scoreboard/evidence remains independently buildable because it is required transport input for RFC 8985 RACK-TLP; it no longer implies a second recovery policy.

On the qualified 260 ms / 10 Mbit/s / 4 MiB / 28-drop RACK path, the transport invariants are exact: 28 injected first-send drops, 28 retransmissions, zero false reordering, zero RTO fallback, zero unrelated qdisc drops, and exact payload delivery. The same recovery path is qualified across Reno, CUBIC, and internal BBR.

RACK-TLP and sender SACK evidence are enabled in the default production build. Compact `bbr` is also selectable through the ordinary IPv4 `tcp-shift-p2` path; Reno remains the implicit controller when no name is supplied. Provider/OpenVZ qualification and IPv6 selector parity remain outstanding; BBR gains must not be tuned to mask transport/recovery defects.

RFC 9937 PRR now owns recovery send credit for RACK+SACK Reno/CUBIC. PR #87 fixed a transparent-wrapper omission: the production pacing wrapper and pacing-qualification wrapper did not forward the adapter's PRR-aware `effective_cwnd`, so `tcp_output()` saw plain `pcb->cwnd` and usually released only the RACK repair even when PRR had accumulated new-data credit. After the fix, the 28-drop CUBIC qualification observes hundreds of PRR transmissions and real pacer defer/resume activity while preserving exact recovery invariants.

PR #88 kept the standards boundary explicit: pure SACK delivery remains RACK/PRR evidence rather than Reno/CUBIC cwnd-growth credit. PR #91 then traced every cumulative ACK before the first deterministic CUBIC loss and found an exact 10-SMSS deficit: the first ten full-sized data ACKs were valid but marked app-limited, and the CUBIC model froze cwnd at IW10 while HyStart++ remained in ordinary initial slow start. PR #92 fixed that RFC 9406 violation by limiting app-limited pausing to CUBIC congestion avoidance; initial slow-start ACK growth now continues normally while the CA epoch-pause behavior remains unchanged.

On the same 260 ms / 10 Mbit/s / 4 MiB / 28-drop path after #92, tcp-shift reaches `131400 B` before the first loss and preserves the expected IW10 offset from cumulative delivered data. Linux's coarse pre-retransmission sample reports `128480 B`; tcp-shift reaches exactly `128480 B` on the adjacent ACK.

PRs #94-#101 close the follow-on CUBIC/RACK/PRR differential investigation without weakening RFC behavior. Independent qualification re-computes RFC 9438 epoch/K/Wmax/W_est/target/cwnd updates with zero mismatches, verifies FlightSize-based multiplicative decrease, and shows RFC 9937 PRR drives steady recovery flight to within one MSS of tcp-shift's own ssthresh target. RFC 9438 section 4.7's single-flow no-fast-convergence profile materially improves the deterministic single-flow benchmark, while a four-flow shared-bottleneck qualification supports keeping fast convergence enabled by default because tcp-shift cannot prove the absence of competing path traffic. Recovery pacing refresh cadence and Linux-like outstanding-byte basis are real implementation differences but did not materially close the residual gap. The remaining Linux CUBIC throughput difference is therefore retained as implementation-differential evidence, not a reason to replace tcp-shift's RFC-first FlightSize/new-ACK/PRR semantics.

The recovery invariants remain exact: 28 injected drops / 28 retransmissions / 0 RTO / 0 unrelated qdisc drops / exact payload.

## Project state

Start here:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — architecture and ownership boundaries;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and handoff contract;
- [`docs/standards-conformance.md`](docs/standards-conformance.md) — RFC/Linux/upstream reference hierarchy and scoped conformance claims;
- [`docs/milestones/p5-rate-sampler-pacer.md`](docs/milestones/p5-rate-sampler-pacer.md) — completed P5 evidence;
- [`docs/milestones/p5-merge-record.md`](docs/milestones/p5-merge-record.md) — PR #13 review/merge provenance and final P5 handoff;
- [`docs/milestones/p6-bbr.md`](docs/milestones/p6-bbr.md) — active P6 model/controller work and qualification plan.
- [`docs/milestones/rack-tlp.md`](docs/milestones/rack-tlp.md) — RFC 8985 RACK-TLP implementation boundary, live gates, and remaining production blockers.

> Status: P0-P5 remain GitHub-runner-qualified. The #91-#101 CUBIC/RACK/PRR qualification sequence closed the standards-defined recovery investigation. The default production profile now enables sender SACK evidence and RFC 8985 RACK-TLP, uses RFC 9937 PRR for RACK+SACK Reno/CUBIC, and makes compact BBR selectable on the ordinary IPv4 `tcp-shift-p2` path while keeping BBR outside PRR. RFC 3168 ECN remains default-OFF and rejects BBR selection because compact BBR has no ECN response. Provider/OpenVZ evidence and IPv6 controller-selection parity remain separate.
