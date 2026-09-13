# lwIP route: architecture and milestones

This document defines the reset boundary for `tcp-shift`. The old gVisor/LKL experiments are historical only; new implementation work starts from lwIP and a Linux userspace event loop.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. The new path therefore optimizes fixed memory cost first and accepts that congestion-control behavior must be implemented and validated explicitly.

The target shape is:

```text
public packet -> host routing/netfilter -> TUN -> lwIP IPv4/TCP
                                              -> raw TCP callbacks
                                              -> single-owner bridge
                                              -> host 127.0.0.1 backend
```

The public and backend TCP legs remain distinct. Congestion control belongs to the lwIP public leg.

## Hard boundaries

- L3 TUN, not TAP/Ethernet. No ARP or software Ethernet bridge is required.
- Single process and single mutable owner for lwIP state.
- `NO_SYS=1`; no lwIP TCP/IP thread, socket API, or netconn API.
- IPv4 first. IPv6 is added only after the IPv4 memory and correctness baseline is stable.
- Do not patch congestion control before the packet path, TCP bridge, shutdown behavior, and memory accounting are observable.
- Do not call an experimental controller "Linux BBR" merely because its state names resemble Linux BBR.

## P0: reproducible lwIP userspace build

Exit criteria:

- exact lwIP commit is pinned and fetched reproducibly;
- release build initializes `lwip_init()` in a normal Linux process;
- binary and idle RSS are recorded;
- no gVisor/Go dependency exists.

The first baseline intentionally uses libc allocation (`MEM_LIBC_MALLOC` and `MEMP_MEM_MALLOC`) so host RSS reflects demand. Static/custom pools can be introduced later only if measurements justify them.

## P1: raw L3 TUN netif

Implement a custom lwIP `netif` backed by one nonblocking TUN fd.

Receive path:

```text
TUN read -> pbuf allocation/copy -> netif input -> IPv4 -> TCP
```

Transmit path:

```text
lwIP IPv4 output -> netif output callback -> gather/copy pbuf chain -> TUN write
```

The event loop must integrate `sys_timeouts_sleeptime()` / `sys_check_timeouts()` rather than polling on a fixed timer. Temporary writable interest is armed only after a TUN write returns `EAGAIN`.

Exit criteria:

- ICMP echo through the lwIP endpoint;
- TCP SYN/SYN-ACK reaches a test listener;
- packet checksums and MTU behavior are verified;
- idle loop has no periodic busy wakeup.

## P2: TCP listener and backend bridge

Use lwIP's raw TCP API (`tcp_accept`, `tcp_recv`, `tcp_sent`, `tcp_err`, `tcp_poll`) and ordinary nonblocking host sockets for the backend leg.

Each flow owns only control metadata while idle. Direction buffers are demand allocated and globally budgeted. Backpressure must stop reads instead of allowing unbounded buffering.

Exit criteria:

- bidirectional byte-stream integrity under partial reads/writes;
- half-close and reset semantics;
- deterministic teardown with zero active bridge objects;
- repeated connect/drain/reuse cycles without RSS ratcheting.

## P3: memory/capacity baseline

Before custom congestion control, measure:

- process idle RSS/PSS/private dirty;
- incremental memory per idle established connection;
- incremental memory per active connection at fixed in-flight data;
- peak and post-drain RSS;
- CPU under idle and small-packet loads.

Test stages should be chosen from the target VPS sizes rather than from an arbitrary high connection count. The primary question is whether a 32/64/128-MiB host can run the endpoint with useful headroom.

## P4: congestion-control abstraction

Introduce a project-local interface instead of scattering policy through lwIP TCP code. Conceptually:

```c
struct tcp_shift_cc_ops {
    void (*init)(void *state);
    void (*packet_sent)(void *state, const struct tx_sample *tx);
    void (*acked)(void *state, const struct rate_sample *rs);
    void (*lost)(void *state, const struct loss_sample *ls);
    void (*app_limited)(void *state);
    uint32_t (*cwnd_bytes)(const void *state);
    uint64_t (*pacing_rate_bps)(const void *state);
};
```

The adapter may need small additions to `tcp_pcb` and transmitted segment metadata. Keep that patch surface explicit and mechanically testable.

Validate the interface first with a conventional controller before BBR.

## P5: delivery-rate sampler and pacer

BBR depends more on transport instrumentation than on its visible state machine.

Required primitives:

- high-resolution monotonic timestamps;
- monotonically increasing delivered-byte accounting;
- per-transmitted-segment delivery snapshot and send timestamp;
- ACK-derived delivery interval/rate samples;
- RTT sample associated with the ACKed transmission, not only smoothed RTT;
- app-limited detection;
- loss/inflight accounting;
- a global pacing scheduler.

The preferred pacer is one process-wide min-heap/timerfd scheduler keyed by each flow's next eligible send time. Avoid one host timer per connection.

## P6: experimental BBR

Reference order:

1. current IETF BBR specification for algorithm semantics;
2. Google QUICHE for network-model and sampler decomposition;
3. ns-3 `TcpBbr` for mapping BBR concepts onto a non-Linux TCP control block;
4. Picoquic for a compact C implementation reference;
5. Linux `tcp_bbr.c` and `tcp_rate.c` as the final TCP behavior cross-check.

Validation must compare cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, loss response, throughput, retransmission behavior, CPU, and memory against native Linux baselines under reproducible RTT/loss/bandwidth scenarios.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if any of these become true:

- acceptable BBR behavior requires replacing most of lwIP recovery/SACK machinery;
- per-segment metadata and pacing raise memory close to the hosted-Linux design;
- correctness requires a long-lived fork of large parts of lwIP TCP;
- performance at the target VPS sizes is dominated by unavoidable BDP buffering rather than fixed stack overhead.

The project is successful even if the result is a very small CUBIC-capable userspace TCP endpoint and BBR is rejected by evidence.
