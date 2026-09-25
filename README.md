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
4. **P6 — tcp-shift BBR: active and live internally.** The compact BBRv1-style controller now publishes real cwnd/pacing policy through the generic adapter, runs on the shared process-wide pacer, and is qualified against Linux BBR on clean single-flow, multi-flow, app-limited, loss, and long-RTT burst scenarios. It remains intentionally unavailable through the public production selector.

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

The repository carries one controlled lwIP integration patch, `patches/lwip-p4-cc-hooks.patch`. Provenance CI hashes pristine critical TCP sources, verifies patch identity and modified-file scope, reverse-applies it, and independently reapplies it to a fresh worktree.

The patch remains confined to `tcp.c`, `tcp_in.c`, and `tcp_out.c`. P4 uses those sites for ACK/loss/RTO policy delegation; P5 adds delivery observations and a narrow data-send pacing eligibility hook without replacing `tcp_output()` or recovery mechanics. Unbound PCBs retain native lwIP behavior.

## Qualification summary

### P1 packet path and ingress

P1 qualifies a real nonpersistent L3 TUN, IPv4/IPv6 packet paths, exact ingress DNAT/conntrack lifecycle, bounded whole-packet TX backpressure, nonfatal RX drops, cleanup/collision handling, forwarding preflight, extension-header-safe IPv6 TCP matching, and routed ICMPv6 Packet Too Big learning.

### P2 bridge

P2 qualifies IPv4/IPv6 public streams bridged to a nonblocking `127.0.0.1` backend with bounded transport-driven backpressure, half-close handling, refusal/reset recovery, concurrent flows, active-flow shutdown cleanup, and repeated reuse without sustained RSS ratcheting.

### P3 memory/capacity

P3 measures tcp-shift process PSS separately from backend/kernel state. On the final P5c behavior head the retained model reports:

```text
warm fixed process PSS:           367 KiB
fully-window-resident slope:      37.773438 KiB/flow
128-active projected PSS:        5202 KiB
8-MiB process budget remaining:  2990 KiB = 23.359 KiB/flow
3x128 drain growth:              5 KiB
idle CPU:                        0 ticks/s
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

### P6 BBR runtime — internal / experimental

The compact `bbr` controller is implemented as a BBRv1-style core with selected BBRv3-informed fixes. It is bound to live lwIP PCBs only through the internal `tcp-shift-p6-bbr` qualification target; production `tcp-shift-p2` still exposes only `reno` and `cubic`.

Current qualification includes:

- clean Linux BBR + `sch_fq` reference cases across low-, edge-, and high-BDP paths;
- four concurrent internal BBR flows sharing one bottleneck;
- live app-limited enter/sample/exit behavior;
- transport-owned RFC 6582/NewReno partial-ACK recovery for multiple losses;
- deterministic 260 ms three- and six-packet WAN bursts with zero RTO fallback;
- repeated burst recovery episodes;
- retransmission-safe delivery/send snapshot refresh, RTO send-phase reset, and filtered-`max_bw` Startup detection.

PR #34 merged the live internal BBR runtime, PR #35 added NewReno partial-ACK recovery, PRs #36-#37 expanded deterministic burst qualification, and PR #38 fixed BBR delivery sampling / Startup telemetry. The next product boundary is controlled experimental `bbr` exposure plus real provider/VPS qualification, not a claim of Linux-BBR equivalence.

## Project state

Start here:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — architecture and ownership boundaries;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and handoff contract;
- [`docs/milestones/p5-rate-sampler-pacer.md`](docs/milestones/p5-rate-sampler-pacer.md) — completed P5 evidence;
- [`docs/milestones/p5-merge-record.md`](docs/milestones/p5-merge-record.md) — PR #13 review/merge provenance and final P5 handoff;
- [`docs/milestones/p6-bbr.md`](docs/milestones/p6-bbr.md) — active P6 model/controller work and qualification plan.

> Status: P0-P5 are GitHub-runner-qualified; P6 internal BBR is live and broadly runner-qualified through merged PR #38 (`1c8b7b4477f71edde0f5961674f37de0a0dd9832`). Public `bbr` selection, provider/OpenVZ qualification, and production packaging/operations remain separate.
