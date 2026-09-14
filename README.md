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

The public TCP connection and backend TCP connection are distinct. Congestion control for the public connection belongs to lwIP/tcp-shift; the loopback backend remains an ordinary host Linux socket. Public IPv6 therefore does not require an IPv6-capable application backend: the same bridge accepts IPv4 or IPv6 public streams and connects them to `127.0.0.1`.

The runtime uses lwIP `NO_SYS=1`: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. The same L3 adapter and event-loop owner qualify both IPv4 and IPv6.

## Module direction

Source boundaries are deliberate even while the initial product remains a single process:

```text
host/       Linux TUN/interface/netfilter/lifecycle integration
runtime/    epoll owner, fd readiness, timers, pacer
lwip/       L3/TCP adapter, PMTU, CC/delivery integration
bridge/     public stream <-> 127.0.0.1 backend
cc/         pure-C congestion-control policy
```

`src/cc/` is independently buildable pure C. It has no lwIP, Linux, TUN, epoll, timerfd, nftables, host-socket, bridge, or lifecycle dependency. `src/lwip/cc_adapter.c` is the deliberately thin layer that may depend on both lwIP and the generic controller.

## Congestion-control status

P0-P4 are runner-qualified. P4 proves that public-side ACK, fast-loss, and retransmission-timeout policy decisions reach the generic controller while lwIP retains retransmission execution, duplicate-ACK handling, fast recovery, SACK/recovery, RTT/RTO calculation, packet queues, sequence space, packet construction, and output.

P5a and P5b are also runner-qualified. P5a established high-resolution send/ACK timestamps, cumulative unique delivered-byte accounting, and retransmission-safe segment metadata. P5b turns that ledger into transport-neutral ACK delivery-rate samples, including app-limited and retransmission metadata, while keeping conventional Reno behavior unchanged.

The current progression is:

1. **P5a — delivery ledger: complete.** Lazy sidecar metadata keyed by `tcp_seg *`; retransmission reuses the same slot and cannot double-count delivery.
2. **P5b — rate sampler + app-limited: complete.** ACK-derived delivery samples use exact payload delivery, send/ACK intervals, retransmission-safe snapshots, RTT validity, prior inflight, and event-driven app-limited marking.
3. **P5c — event-driven pacer: next.** One process-wide deadline heap + one one-shot `timerfd` integrated with epoll; no fixed pacing tick, no per-flow timerfd/thread, no busy spin.
4. **P6 — tcp-shift BBR.** Implement bandwidth/min-RTT model and mode logic, then compare against native Linux BBR reference runs.

The architectural ownership problem is therefore already solved. The main remaining risk before BBR is pacing correctness under high BDP/loss, not rebuilding TCP.

An lwIP controller will not be described as Linux BBR unless the relevant transport semantics are actually equivalent.

## Event-driven runtime

The runtime is deadline/readiness driven rather than fixed-polling:

- `epoll_wait()` blocks until fd readiness or the next lwIP timeout from `sys_timeouts_sleeptime()`;
- `sys_check_timeouts()` runs after a real readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only while the bounded TX queue is non-empty;
- backend read/write interests are suppressed when they cannot make progress;
- app-limited detection is triggered by the actual backend read path reaching `EAGAIN`, not by a periodic flow scan.

The P5b application-pause gate observed zero runtime CPU ticks during a 300-ms sample window while still proving app-limited enter/sample/exit transitions.

P5c must preserve this shape. The intended pacer uses a single process-wide min-heap and a one-shot monotonic `timerfd` armed only to the earliest pacing deadline. When no paced transmission is pending, that timer is disarmed.

## Build and upstream provenance

lwIP is fetched rather than vendored. `.lwip-baseline` pins exact upstream commit:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

```bash
make build
```

The repository carries one controlled lwIP integration patch, `patches/lwip-p4-cc-hooks.patch`. Provenance CI records pristine critical-source hashes before patching, verifies the patch SHA and modified-file set, checks reverse application, and independently reapplies the patch to a fresh worktree to prove byte-identical results.

The patch stays confined to `tcp.c`, `tcp_in.c`, and `tcp_out.c`. P4 uses those sites for ACK/loss/RTO policy delegation; P5 adds send/ACK observation data in the already-controlled `tcp_in.c`/`tcp_out.c` surface. Unbound PCBs retain native lwIP behavior.

## Qualification summary

### P1 packet path and ingress

P1a qualifies a real nonpersistent L3 IPv4 TUN, ICMP/TCP, exact DNAT/conntrack, bounded whole-packet TX backpressure, nonfatal RX drops, MTU/checksum behavior, cleanup, collision handling, and forwarding preflight.

P1b extends the same runtime to IPv6 without Ethernet, SLAAC, router solicitation, DHCPv6, MLD, ND6 queueing, or endpoint fragmentation/reassembly. It qualifies exact IPv6 DNAT, Hop-by-Hop extension-header-safe TCP matching, and routed ICMPv6 Packet Too Big learning:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
```

### P2 bridge

P2 qualifies IPv4/IPv6 public streams bridged to the same nonblocking `127.0.0.1` backend with bounded transport-driven backpressure, half-close handling, refusal/reset recovery, concurrent flows, active-flow shutdown cleanup, and repeated reuse without sustained RSS ratcheting.

### P3 memory/capacity

P3 measures process PSS, directional active-window residency, repeated drain floors, CPU, and constrained-host process budgets. These are tcp-shift process measurements only; backend kernel/application memory and provider overhead are deliberately excluded.

### P4 controller integration

Final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` passed provenance/P0/P1/P2/P3/P4. Real external TUN fault injection retained:

```text
fast-loss: cc_loss_events=1 cc_timeout_events=0 payload_bytes=262144 recovery=ok
RTO:       cc_loss_events=0 cc_timeout_events=2 payload_bytes=262144 recovery=ok
```

### P5a delivery ledger

Behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed provenance/P0/P1/P2/P3/P4 and P5 run `34821205375`, job `103903019956`. Artifact `10338108552` retained corrected fail-closed diagnostics:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
RTO:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

### P5b delivery-rate sampler + app-limited

Final behavior head `327efe7e29adfc230e7d201b466f2bd4980e976c` passed provenance/P0/P1/P2/P3/P4/P5. P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained:

```text
normal:    samples=138 valid=138 invalid=0 max_rate=696998778 B/s
fast-loss: samples=121 valid=121 invalid=0 cc_loss_events=1
RTO:       samples=135 valid=135 invalid=0 retransmitted_samples=3 cc_timeout_events=2

app-pause:
first_burst=4096 second_burst=131072 delivered=135168
app_limited_samples=7 app_limited_enters=2 app_limited_exits=2
pause_cpu_ticks=0 event_driven=ok
```

P5b widened the lazy sidecar to 56 bytes so it can retain sequence/progress, send-phase, prior-inflight, app-limited, and retransmission state. Partial-ACK accounting is based on cumulative ACK sequence progress before upstream frees the segment; FIN sequence space is not counted as delivered payload.

The P5b P3 rerun, run `34843587033`, job `103974029078`, artifact `10346739278`, retained:

```text
warm fixed process PSS: 347 KiB
fully-window-resident process slope: 37.710938 KiB/flow
128 active projected process PSS: 5174 KiB
8-MiB process-budget remaining: 3018 KiB
headroom: 23.578 KiB/active flow
```

Active residency remains dominated by TCP-window/pbuf/send-segment state; sampler metadata remains well inside the constrained-process planning budget.

This remains GitHub-runner qualification, not provider/OpenVZ qualification.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — completed P1 state;
- [`docs/milestones/p2-bridge.md`](docs/milestones/p2-bridge.md) — completed P2 state;
- [`docs/milestones/p3-memory-capacity.md`](docs/milestones/p3-memory-capacity.md) — P3 baseline;
- [`docs/milestones/p4-cc-boundary.md`](docs/milestones/p4-cc-boundary.md) — completed P4 controller integration;
- [`docs/milestones/p5-rate-sampler-pacer.md`](docs/milestones/p5-rate-sampler-pacer.md) — active P5 sampler/pacer work.

> Status: P0-P4, P5a delivery ledger, and P5b delivery-rate/app-limited sampling are runner-qualified; P5c event-driven pacing is next. Do not use on production traffic.
