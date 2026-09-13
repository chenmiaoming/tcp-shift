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
- `tcp-shift-p1` is a temporary privileged bring-up executable that owns TUN, lwIP netif state, timers and packet I/O while host address/route setup remains external.

Still required before P1a exit:

- transactional host-side address/MTU/route setup and cleanup;
- minimal lwIP TCP listener for SYN/SYN-ACK qualification;
- privileged ICMP/TCP packet-path CI and retained packet captures;
- explicit idle-wakeup measurement;
- failure-path tests for queue pressure and host resource cleanup.

The adapter currently assumes a 1500-byte TUN MTU and rejects larger received packets. Host configuration must set the same MTU before behavioral qualification.

## Manual IPv4 bring-up shape

The temporary runtime accepts:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <gateway>
```

A test supervisor can start `tcp-shift-p1 ts0 10.0.0.2 255.255.255.252 10.0.0.1`, wait for `ts0`, configure the host side as `10.0.0.1/30` with MTU 1500, and then ping `10.0.0.2`. This is a development harness, not the final product CLI.

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
- TCP SYN/SYN-ACK reaches a minimal lwIP listener;
- checksums and MTU behavior;
- IPv6 Packet Too Big/PMTU behavior;
- no permanent writable polling;
- bounded idle wakeups;
- no leaked TUN, route, or firewall resources after success and forced failure paths.

Packet captures, interface/routing state, runtime counters, and logs should be retained on failure.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. Raw TCP callbacks for payload forwarding, host loopback sockets, partial stream I/O, half-close/reset semantics, and connection lifecycle belong to P2.
