# P1: dual-stack L3 TUN packet path

Status: **in progress**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O and bounded memory.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches were considered, but they introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps that boundary explicit and testable.

## P1a implementation state

Implemented and compiling under `-Werror`:

- `src/host/tun.*` opens/closes a nonpersistent nonblocking `IFF_TUN | IFF_NO_PI` device;
- `src/lwip/l3_tun.*` attaches an IPv4 lwIP netif and injects complete received packets into `ip4_input`;
- transmit preserves one complete IP packet using `writev` over the pbuf chain;
- TUN `EAGAIN` retains a pbuf reference in a bounded FIFO instead of dropping or stream-splitting the packet;
- the FIFO is capped at 64 packets and 96 KiB and records peak queued bytes and queue drops;
- once a packet is queued, subsequent packets queue behind it so TUN packet order is preserved;
- `src/runtime/lwip_loop.*` uses epoll and `sys_timeouts_sleeptime()` / `sys_check_timeouts()`; there is no fixed polling tick;
- EPOLLOUT is armed only while the bounded TUN TX queue is non-empty;
- `src/lwip/probe_listener.*` installs a minimal raw-API IPv4 TCP listener used only to qualify SYN/SYN-ACK/accept behavior before P2;
- `tcp-shift-p1` is a temporary privileged bring-up executable that owns TUN, lwIP netif state, timers and packet I/O while host address/route setup remains external.

Still required before P1a exit:

- transactional host-side address/MTU/route setup and cleanup in the product lifecycle layer;
- explicit idle-wakeup measurement;
- failure-path tests for queue pressure and host resource cleanup;
- DNAT/conntrack packet-path qualification rather than only direct TUN subnet traffic.

The adapter currently assumes a 1500-byte TUN MTU and rejects larger received packets. Host configuration must set the same MTU before behavioral qualification.

## First retained IPv4 evidence

GitHub Actions workflow `lwIP P1 IPv4 TUN`, run `34740740077`, passed on Ubuntu 24.04.5 using the pinned lwIP baseline.

The test granted only `cap_net_admin=ep` to the temporary `tcp-shift-p1` executable, created nonpersistent TUN `tsp1ci0`, configured host `10.231.0.1/30`, and sent ICMP directly to lwIP `10.231.0.2`. All 3 echo requests received replies with 0% loss. The runtime reported:

```text
rx_packets=4 tx_packets=3 tx_queue_peak_bytes=0 tx_queue_drops=0
```

The test then verified that the TUN device disappeared after the runtime exited. Diagnostics retained interface, route and link state plus runtime output. This evidence proves the direct IPv4 L3 TUN + lwIP ICMP path and nonpersistent-fd cleanup. It does **not** yet prove DNAT, public-address routing, TCP, queue-pressure behavior, or idle wakeup bounds.

The current CI extends this same workflow with a real host TCP `connect()` to the lwIP probe listener and requires `tcp_accepts > 0`; that TCP evidence is recorded only after the updated workflow passes.

## Manual IPv4 bring-up shape

The temporary runtime accepts:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <gateway> [listen-port]
```

A test supervisor can start `tcp-shift-p1 ts0 10.0.0.2 255.255.255.252 10.0.0.1 18080`, wait for `ts0`, configure the host side as `10.0.0.1/30` with MTU 1500, then ping `10.0.0.2` and connect to `10.0.0.2:18080`. This is a development harness, not the final product CLI.

## P1b: IPv6

Immediately after the IPv4 path is behaviorally correct, extend the same adapter rather than creating a parallel runtime. Required work includes IPv6 address configuration, `netif->output_ip6`, `ip6_input`, ICMPv6, TCP, Packet Too Big/PMTU validation, and extension-header-safe netfilter rules.

IPv6-only deployment is an exit requirement because it is common in the low-cost VPS environments the product targets.

## Event-loop invariant

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

The loop derives its sleep deadline from lwIP timers. It does not use a fixed periodic polling tick. TUN writable interest is disabled by default and enabled only while at least one whole packet is queued after backpressure.

## Packet ownership

TUN is packet-oriented. A transmit packet is never stream-split across multiple writes. When a nonblocking write returns `EAGAIN`, tcp-shift takes an additional pbuf reference, keeps that whole packet in the bounded FIFO, and returns success to the synchronous lwIP output path because ownership has transferred to the queue. The reference is released only after successful transmit or adapter teardown.

Queue exhaustion is observable through `tx_queue_drops` and returns `ERR_MEM` to lwIP. Unbounded buffering is prohibited.

RX allocates only the pbuf required by the received packet after copying from the bounded host read buffer. Later optimization may reduce copies, but correctness and bounded residency come first.

## Exit evidence

P1 is complete only when CI or retained privileged test evidence proves both address families where supported by the runner:

- interface acquisition/configuration and cleanup;
- ICMP/ICMPv6 echo through lwIP;
- TCP SYN/SYN-ACK and accept at a minimal lwIP listener;
- checksums and MTU behavior;
- IPv6 Packet Too Big/PMTU behavior;
- no permanent writable polling;
- bounded idle wakeups;
- no leaked TUN, route, or firewall resources after success and forced failure paths.

Packet captures, interface/routing state, runtime counters, and logs should be retained on failure.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. The probe listener intentionally discards received payload and closes normally when the peer closes. Production raw TCP callback ownership, host loopback sockets, partial stream I/O, half-close/reset semantics, and connection lifecycle belong to P2.
