# P1: dual-stack L3 TUN packet path

Status: **P1a IPv4 runner-qualified; P1b IPv6 is next**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O, bounded memory, explicit host-resource ownership, and deterministic cleanup.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps that boundary explicit and testable.

## P1a implementation state — complete on the GitHub runner

The qualified IPv4 implementation contains:

- `src/host/tun.*`: nonpersistent nonblocking `IFF_TUN | IFF_NO_PI` acquisition/cleanup;
- `src/host/ifconfig.*`: host IPv4 address, MTU, and link-up ownership;
- `src/host/nft_ingress.*`: exact-match product-owned nftables DNAT lifecycle;
- `src/lwip/l3_tun.*`: IPv4 lwIP netif, complete packet RX/TX, bounded TUN retry queue;
- `src/runtime/lwip_loop.*`: epoll plus lwIP timeout integration with no fixed polling tick;
- `src/lwip/probe_listener.*`: temporary raw-API TCP listener for handshake qualification;
- unprivileged TX/RX contract binaries that exercise production packet paths without runtime test hooks.

TX backpressure is capped at 64 packets / 96 KiB. The 65th full-MTU packet is rejected with `ERR_MEM`; FIFO ordering and reference cleanup have deterministic tests. Oversize RX is a nonfatal drop.

The nonpersistent TUN fd is the rollback boundary for interface/address/MTU/connected-route state. Public IPv4 ingress uses a single exclusive nftables table `ip tcp_shift_p1`. Installation validates the complete batch with `nft -c -f -`, then atomically creates the table, NAT chain, and exact `ip daddr <public> tcp dport <port> dnat to <lwip>:<port>` rule. A pre-existing table is rejected rather than adopted. Cleanup deletes only the table recorded as owned by this process before TUN teardown.

`net.ipv4.ip_forward` and broad host FORWARD policy are operator-managed prerequisites. The product reads/diagnoses forwarding but never enables it. GitHub runner-specific forwarding exceptions remain test-harness resources.

The temporary public-ingress P1 process currently runs as root so its short-lived `nft` child has the required privilege. `nft` is exec'd only during setup/cleanup; libnftables is not linked into the long-lived runtime. A future privileged helper can absorb this host module without changing lwIP or CC boundaries.

## Retained IPv4 evidence

### Packet path: run 34744304038

This run consolidates the earlier IPv4 qualification:

```text
tcp-shift-p1-rx-contract: mtu=1500 oversize=1501 rx_drops=1 rx_errors=0 runtime_survives_drop=ok
send seq=4097 valid=0 checksum_field=0xebbc software_verify=0x2886
send seq=4098 valid=1 checksum_field=0x1442 software_verify=0x0000
icmp_checksum_bad_reply=none icmp_checksum_good_reply=received
direct connected 10.231.0.2:18080
DNAT connected 198.51.100.1:18080
```

The same run preserved the established idle/backpressure contracts: a separate two-second idle process made only four `epoll_wait` calls with zero TUN writable wakeups, and the unprivileged TX test retained 64 × 1500-byte packets for a 96,000-byte peak, rejected the 65th, observed real `EAGAIN`, preserved FIFO ordering, and cleaned queued references on detach.

The active runtime finished with:

```text
rx_packets=16 rx_drops=0 rx_errors=0 tx_packets=9 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=2 tcp_rx_bytes=0 tcp_errors=0 loop_wait_calls=18 loop_ready_wakeups=12 loop_timeout_wakeups=5 loop_eintr_wakeups=1 loop_tun_readable_wakeups=12 loop_tun_writable_wakeups=0
```

### Product-owned IPv4 ingress: run 34763055040

This run kept all previous packet-path gates green and added the remaining lifecycle qualification. Retained lifecycle output:

```text
forwarding_disabled_preflight=ok
exclusive_collision_rejection=ok
product DNAT connected 198.51.101.1:18081
tcp-shift-p1: ready tun=tsp1nft0 host-ipv4=10.232.0.1 lwip-ipv4=10.232.0.2 mtu=1500 tcp-port=18081 public-ipv4=198.51.101.1 nft-table=tcp_shift_p1
signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1 product-owned nft ingress lifecycle passed
```

The live public flow reached lwIP with `tcp_accepts=1`, `tcp_errors=0`. The gate also proved three lifecycle invariants separately: with `ip_forward=0`, startup failed without changing the sysctl and left no TUN/table; when the product table name was already occupied, startup failed without adopting or deleting that resource; after normal public traffic, SIGTERM removed product TUN/table while the serialized unrelated nft table had the same SHA-256 before and after.

These results close all current P1a runner exit criteria: interface ownership/cleanup, ICMP, TCP accept, DNAT/conntrack, checksum/MTU, bounded idle wakeups, bounded TX backpressure, startup rollback, stale-resource collision rejection, signal cleanup, and unrelated-firewall preservation.

This is not yet evidence that every target OpenVZ/VPS provider permits the required TUN, forwarding, nftables, conntrack, and namespace-equivalent operations. Provider qualification is a separate deployment task and must not be inferred from the runner result.

## Temporary IPv4 bring-up shape

Without public ingress:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <host-ipv4> [listen-port]
```

With product-owned public ingress:

```text
tcp-shift-p1 <tun-name> <lwip-ipv4> <netmask> <host-ipv4> <listen-port> <public-ipv4>
```

The latter currently requires root because it execs `nft` during setup/cleanup. This remains a development/lifecycle harness, not the final product CLI.

## P1b: IPv6 — active next milestone

Extend the same adapter and host lifecycle rather than creating a parallel runtime. Required work includes lwIP IPv6 source/config enablement, IPv6 host/TUN addressing, RX version dispatch, `netif->output_ip6`, ICMPv6, TCP, product-owned exact IPv6 ingress, IPv6-only operation, Packet Too Big/PMTU, and extension-header-safe host L4 matching.

IPv6-only deployment is a product requirement because it is common in the low-cost VPS/OpenVZ environments targeted by tcp-shift. P1b must distinguish product defects from unavailable TUN, IPv6 forwarding, conntrack/NAT, or container privileges.

## Event-loop invariant

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

## Packet ownership

TUN is packet-oriented. A transmit packet is never stream-split across writes. On `EAGAIN`, tcp-shift takes a pbuf reference and retains the entire packet in the bounded FIFO. The reference is released after successful transmit or adapter teardown. Queue exhaustion is observable and bounded.

RX allocates only the pbuf required by the received packet after copying from a bounded host read buffer. Correctness and bounded residency take priority over copy reduction in P1.

## P1 exit evidence

P1 as a whole is complete only when retained evidence proves both address families:

- interface acquisition/configuration and cleanup;
- ICMP/ICMPv6 through lwIP;
- TCP SYN/SYN-ACK/accept at a minimal lwIP listener;
- exact product-owned ingress DNAT/conntrack where NAT is part of deployment;
- checksums and MTU behavior;
- IPv6 Packet Too Big/PMTU behavior;
- no permanent writable polling and bounded idle wakeups;
- bounded TX backpressure;
- no leaked TUN, route, firewall, namespace, or test resources after success and forced failure paths.

P1a satisfies the IPv4 half of this contract on the GitHub runner. P1b must satisfy the IPv6 half without regressing IPv4.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. Production raw TCP callback ownership, host loopback sockets, partial stream I/O, half-close/reset semantics, backend failure, and connection lifecycle belong to P2.
