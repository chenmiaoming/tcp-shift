# lwIP route: architecture and milestones

This roadmap defines implementation order and exit evidence. `ARCHITECTURE.md` is the current product source of truth; this file explains how the project gets there.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. Fixed memory cost is the first optimization target.

The target shape is:

```text
public IPv4/IPv6 packet -> host routing/netfilter -> L3 TUN -> lwIP TCP
                                                        -> raw TCP callbacks
                                                        -> single-owner bridge
                                                        -> 127.0.0.1 backend
```

The public and backend TCP legs remain distinct. Congestion control belongs to the lwIP public leg.

## Hard boundaries

- L3 TUN, not TAP/Ethernet, for the current product path.
- One mutable owner for lwIP state; no per-flow forwarding threads.
- `NO_SYS=1`; no lwIP TCP/IP thread, socket API, or netconn API.
- IPv4 is the bring-up path, but IPv6-only operation is a product requirement and follows immediately in P1b.
- Linux host integration, lwIP transport integration, bridge logic, and congestion-control policy remain separate source modules.
- The future CC core is pure C and independently buildable; library separation does not imply process separation.
- Do not patch congestion control before packet path, bridge, shutdown behavior, and memory accounting are observable.
- Do not call an experimental controller "Linux BBR" merely because its state names resemble Linux BBR.

## P0: reproducible lwIP userspace build — complete

Exit evidence includes exact upstream pinning, clean reproducible fetch, an IPv4/TCP `NO_SYS=1` source allowlist, configuration/source/binary/RSS gates, and clean-runner artifact smoke.

The initial Linux baseline intentionally uses libc allocation (`MEM_LIBC_MALLOC` and `MEMP_MEM_MALLOC`) so host RSS reflects demand.

## P1a: IPv4 L3 TUN

Implement a custom lwIP netif backed by one nonblocking `IFF_TUN | IFF_NO_PI` fd.

```text
RX: TUN read -> packet pbuf -> ip4_input -> TCP/ICMP
TX: lwIP ip4 output -> netif output -> whole-packet TUN write
```

The event loop integrates `sys_timeouts_sleeptime()` / `sys_check_timeouts()` rather than polling on a fixed timer. Temporary writable interest is armed only after a TUN write returns `EAGAIN`, with a bounded whole-packet retry queue.

Exit criteria: ICMP echo, TCP SYN/SYN-ACK to a minimal listener, checksum/MTU validation, transactional host cleanup, and no periodic busy wakeup.

## P1b: IPv6 L3 TUN

Extend the same L3 adapter with IPv6; do not create a second runtime. Add IPv6 address configuration, `output_ip6`, `ip6_input`, ICMPv6, TCP, and host-side IPv6 netfilter/routing rules.

Exit criteria include IPv6-only operation, Packet Too Big/PMTU behavior, extension-header-safe L4 matching, and explicit diagnosis of unavailable TUN/forwarding/conntrack/NAT/container capabilities.

## P2: dual-stack TCP listener and backend bridge

Use lwIP raw TCP callbacks (`tcp_accept`, `tcp_recv`, `tcp_sent`, `tcp_err`, `tcp_poll`) and ordinary nonblocking host sockets for the backend leg. Public IPv4 and IPv6 share the same flow/bridge state machine; the initial backend remains IPv4 loopback.

Each flow owns only control metadata while idle. Direction buffers are demand allocated and globally budgeted. Backpressure stops reads instead of allowing unbounded buffering.

Exit criteria include bidirectional integrity under partial I/O, half-close/reset semantics, deterministic teardown with zero active bridge objects, and repeated connect/drain/reuse without RSS ratcheting.

## P3: memory/capacity baseline

Measure idle RSS/PSS/private dirty, incremental memory per idle established connection, active-flow residency at fixed in-flight data, peak/post-drain floors, and CPU under idle/small-packet loads. Test stages are chosen for 32/64/128-MiB target hosts.

## P4: generic congestion-control library boundary

Introduce an independently buildable pure-C CC interface instead of scattering policy through lwIP TCP code. The core consumes transport events/samples and emits cwnd/pacing policy. It does not call Linux host APIs and it does not own the pacing scheduler.

The lwIP adapter may require small, explicit additions to `tcp_pcb` and transmitted-segment metadata. Keep that patch surface mechanically testable. Validate the interface with a conventional controller before BBR.

## P5: delivery-rate sampler and pacer

Add high-resolution monotonic timestamps, delivered-byte accounting, per-segment delivery snapshots/send timestamps, ACK-derived delivery-rate samples, latest RTT samples, app-limited detection, and loss/inflight accounting.

The Linux runtime pacer is one process-wide scheduler, preferably min-heap plus timerfd, keyed by each flow's next eligible send time. The generic controller sees pacing rate, not timerfd.

## P6: experimental BBR

Reference order: current IETF BBR specification, Google QUICHE, ns-3 `TcpBbr`, Picoquic, then Linux `tcp_bbr.c`/`tcp_rate.c` as TCP behavior cross-checks.

Validation compares cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, loss response, throughput, retransmission behavior, CPU, and memory against native Linux baselines under reproducible RTT/loss/bandwidth scenarios.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, or unavoidable BDP buffering dominates the fixed-memory advantage.

The project is still successful if the result is a very small conventional-CC userspace TCP endpoint and BBR is rejected by evidence.
