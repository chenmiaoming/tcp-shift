# P1: dual-stack L3 TUN packet path

Status: **in progress**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O and bounded memory.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches were considered, but they introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps that boundary explicit and testable.

## Phases

### P1a: IPv4 plumbing and event loop

Current first increment:

- `src/host/tun.*` opens a nonpersistent nonblocking `IFF_TUN | IFF_NO_PI` device;
- `src/lwip/l3_tun.*` attaches an IPv4 lwIP netif;
- RX copies one complete TUN packet into a correctly sized pbuf and calls lwIP input;
- TX preserves a complete IP packet with `writev` over the pbuf chain;
- basic packet/byte/error counters exist for later CI evidence;
- the new modules compile under `-Werror` while the P0 executable remains unprivileged.

Not complete yet:

- epoll event loop and lwIP timeout integration;
- bounded whole-packet TX queue for `EAGAIN`;
- temporary EPOLLOUT arming while the TX queue is non-empty;
- host address/MTU/route setup and transactional cleanup;
- privileged ICMP and TCP packet-path CI.

The adapter currently assumes a 1500-byte TUN MTU and rejects larger received packets. Host configuration must set the same MTU before behavioral qualification.

### P1b: IPv6

Immediately after the IPv4 path is correct, extend the same adapter rather than creating a parallel runtime. Required work includes IPv6 address configuration, `netif->output_ip6`, `ip6_input`, ICMPv6, TCP, Packet Too Big/PMTU validation, and extension-header-safe netfilter rules.

IPv6-only deployment is an exit requirement because it is common in the low-cost VPS environments the product targets.

## Event-loop invariant

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

The loop should derive its sleep deadline from lwIP timers. It must not use a fixed periodic polling tick. TUN writable interest is disabled by default and enabled only when a previous write returned `EAGAIN` and at least one whole packet is queued.

## Packet ownership

TUN is packet-oriented. A transmit packet must never be stream-split across multiple writes. If a nonblocking write cannot accept it, tcp-shift retains the whole packet subject to a bounded global queue and retries it later. Queue exhaustion must be observable and deterministic; unbounded buffering is prohibited.

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

P1 does not connect accepted TCP streams to an application backend. Raw TCP callbacks, host loopback sockets, partial stream I/O, half-close/reset semantics, and connection lifecycle belong to P2.
