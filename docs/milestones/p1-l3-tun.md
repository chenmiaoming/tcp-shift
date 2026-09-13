# P1: dual-stack L3 TUN packet path

Status: **in progress**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O and bounded memory.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches were considered, but they introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps that boundary explicit and testable.

## P1a implementation state

Implemented and compiling under `-Werror`:

- `src/host/tun.*` opens/closes a nonpersistent nonblocking `IFF_TUN | IFF_NO_PI` device;
- `src/host/ifconfig.*` configures the host-side IPv4 address, MTU, and link-up state from the same process that owns the TUN fd;
- `src/lwip/l3_tun.*` attaches an IPv4 lwIP netif and injects complete received packets into `ip4_input`;
- transmit preserves one complete IP packet using `writev` over the pbuf chain;
- TUN `EAGAIN` retains a pbuf reference in a bounded FIFO instead of dropping or stream-splitting the packet;
- the FIFO is capped at 64 packets and 96 KiB and records peak queued bytes and queue drops;
- once a packet is queued, subsequent packets queue behind it so TUN packet order is preserved;
- `src/runtime/lwip_loop.*` uses epoll and `sys_timeouts_sleeptime()` / `sys_check_timeouts()`; there is no fixed polling tick;
- EPOLLOUT is armed only while the bounded TUN TX queue is non-empty;
- `src/lwip/probe_listener.*` installs a minimal raw-API IPv4 TCP listener used only to qualify SYN/SYN-ACK/accept behavior before P2;
- `tcp-shift-p1` is a temporary privileged bring-up executable that owns TUN, host IPv4 interface configuration, lwIP netif state, timers, and packet I/O.

The nonpersistent TUN fd is the rollback boundary for host-side address/MTU/connected-route state. If initialization fails or the process exits, closing the fd removes the interface and those dependent resources together.

Still required before P1a exit:

- explicit idle-wakeup measurement;
- failure-path tests for TUN TX queue pressure and cleanup;
- production lifecycle ownership for narrow forwarding/NAT rules instead of CI-only harness rules;
- checksum/MTU boundary qualification beyond the normal 1500-byte smoke path.

## Retained IPv4 evidence

### Run 34740740077: ICMP and fd-lifetime cleanup

The first `lwIP P1 IPv4 TUN` workflow passed on Ubuntu 24.04.5 using the pinned lwIP baseline. The test granted only `cap_net_admin=ep` to `tcp-shift-p1`, created nonpersistent TUN `tsp1ci0`, configured host `10.231.0.1/30`, and sent ICMP directly to lwIP `10.231.0.2`.

All 3 echo requests received replies with 0% loss. The runtime reported:

```text
rx_packets=4 tx_packets=3 tx_queue_peak_bytes=0 tx_queue_drops=0
```

The TUN device disappeared after the runtime exited. This proved direct IPv4 L3 TUN ingress/egress, ICMP handling, and nonpersistent-fd cleanup.

### Run 34740867645: TCP SYN/SYN-ACK/accept

The next workflow extended the same path with a real host `connect()` to lwIP port 18080. The connection succeeded and the lwIP raw-API accept callback ran exactly once. ICMP remained 3/3 with 0% loss. The runtime reported:

```text
rx_packets=8 tx_packets=5 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=1 tcp_rx_bytes=0 tcp_errors=0
```

This proved that the direct IPv4 TUN path reaches lwIP TCP and completes SYN/SYN-ACK/ACK through `tcp_accept`.

### Run 34743049605: product-owned TUN configuration and DNAT/conntrack

The temporary runtime now configures its own host-side TUN IPv4 address, netmask, MTU, and link-up state. The CI script no longer uses `ip addr` or `ip link set` to configure the TUN for the product.

The workflow created a separate network namespace and veth pair to model an external client:

```text
198.51.100.2 client namespace
        |
        v
198.51.100.1 host-side veth:18080
        |
        | nftables PREROUTING DNAT
        v
10.231.0.2:18080 over TUN
        |
        v
lwIP raw TCP listener
```

Both the direct connection and the DNAT connection completed:

```text
direct connected 10.231.0.2:18080
DNAT connected 198.51.100.1:18080
```

The runtime reported:

```text
rx_packets=12 tx_packets=7 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=2 tcp_rx_bytes=0 tcp_errors=0
```

The harness also required conntrack evidence for both the original external tuple and the reply tuple involving lwIP. It then verified that the nonpersistent TUN, namespace, veth pair, nftables table, and temporary forwarding rules were removed.

The first DNAT attempt intentionally remained a failed CI record: run `34742949259` showed that GitHub runners have Docker's IPv4 `FORWARD` policy set to `DROP`. Routes and DNAT were present, but forwarded TCP timed out. The retained nftables/conntrack/runtime diagnostics isolated that environment prerequisite. The harness now inserts two exact interface/IP/port ACCEPT rules during the test and removes them during cleanup. This is CI scaffolding, not product firewall behavior.

For commit `4d3141499cfd1624f6a15552d509edf73a30e11a`, P0 qualification, upstream provenance, and the P1 IPv4 behavior workflow all passed.

## Temporary IPv4 bring-up shape

The current bring-up executable accepts:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <host-ipv4> [listen-port]
```

Example:

```text
tcp-shift-p1 ts0 10.0.0.2 255.255.255.252 10.0.0.1 18080
```

The process creates `ts0`, sets its host-side address to `10.0.0.1`, sets MTU 1500, brings it up, attaches lwIP as `10.0.0.2`, and starts the temporary TCP probe listener. This is a development harness, not the final product CLI.

## P1b: IPv6

Immediately after the remaining IPv4 event-loop/backpressure/lifecycle invariants are qualified, extend the same adapter rather than creating a parallel runtime. Required work includes IPv6 address configuration, `netif->output_ip6`, `ip6_input`, ICMPv6, TCP, Packet Too Big/PMTU validation, and extension-header-safe netfilter rules.

IPv6-only deployment is an exit requirement because it is common in the low-cost VPS environments the product targets.

## Event-loop invariant

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

The loop derives its sleep deadline from lwIP timers. It does not use a fixed periodic polling tick. TUN writable interest is disabled by default and enabled only while at least one whole packet is queued after backpressure.

P1a still needs a measured idle-wakeup bound so this design property is evidence rather than inspection alone.

## Packet ownership

TUN is packet-oriented. A transmit packet is never stream-split across multiple writes. When a nonblocking write returns `EAGAIN`, tcp-shift takes an additional pbuf reference, keeps that whole packet in the bounded FIFO, and returns success to the synchronous lwIP output path because ownership has transferred to the queue. The reference is released only after successful transmit or adapter teardown.

Queue exhaustion is observable through `tx_queue_drops` and returns `ERR_MEM` to lwIP. Unbounded buffering is prohibited.

RX allocates only the pbuf required by the received packet after copying from the bounded host read buffer. Later optimization may reduce copies, but correctness and bounded residency come first.

## Exit evidence

P1 is complete only when retained evidence proves both address families where supported by the runner:

- interface acquisition/configuration and cleanup;
- ICMP/ICMPv6 echo through lwIP;
- TCP SYN/SYN-ACK and accept at a minimal lwIP listener;
- DNAT/conntrack path where NAT is part of the deployment model;
- checksums and MTU behavior;
- IPv6 Packet Too Big/PMTU behavior;
- no permanent writable polling;
- bounded idle wakeups;
- bounded TX backpressure behavior;
- no leaked TUN, route, firewall, namespace, or test resources after success and forced failure paths.

Packet captures, interface/routing/firewall/conntrack state, runtime counters, and logs should be retained on failure.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. The probe listener intentionally discards received payload and closes normally when the peer closes. Production raw TCP callback ownership, host loopback sockets, partial stream I/O, half-close/reset semantics, and connection lifecycle belong to P2.
