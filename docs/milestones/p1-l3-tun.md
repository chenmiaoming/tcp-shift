# P1: dual-stack L3 TUN packet path

Status: **complete on the GitHub runner for IPv4 and IPv6**.

## Goal

Create the smallest correct packet boundary between Linux TUN and lwIP. P1 does not implement the application stream bridge or congestion control. Its job is to prove that lwIP can own public IPv4 and IPv6 TCP packets with event-driven host I/O, bounded memory, explicit host-resource ownership, PMTU behavior, and deterministic cleanup.

## Why TUN

The public TCP connection must terminate inside lwIP. A normal host TCP relay such as stock rinetd would terminate TCP in the outer kernel and therefore would not solve the congestion-control ownership problem. Raw physical-interface approaches introduce L2/NDP/ARP ownership and host-kernel packet competition before the transport path is proven. L3 TUN keeps that boundary explicit and testable.

## Qualified implementation

P1 contains:

- `src/host/tun.*`: nonpersistent nonblocking exclusive `IFF_TUN | IFF_NO_PI` acquisition/cleanup;
- `src/host/ifconfig.*` and `ifconfig_ipv6.*`: static host L3 address, MTU, and link-up ownership;
- `src/host/nft_ingress.*`: family-aware exact-match product-owned nftables DNAT lifecycle;
- `src/lwip/l3_tun.*`: IPv4/IPv6 lwIP netif, RX dispatch, complete-packet TX, bounded TUN retry queue, and IPv6 PMTU destination-cache integration;
- `src/runtime/lwip_loop.*`: epoll plus lwIP timeout integration with no fixed polling tick;
- `src/lwip/probe_listener.*`: temporary raw-API TCP listener for handshake qualification;
- unprivileged TX/RX contract binaries that exercise production packet paths without runtime test hooks.

TX backpressure is capped at 64 packets / 96 KiB. The 65th full-MTU packet is rejected with `ERR_MEM`; FIFO ordering and reference cleanup have deterministic tests. Oversize RX is a nonfatal drop.

The nonpersistent TUN fd is the rollback boundary for interface/address/MTU/connected-route state. Public ingress uses one exclusive nftables table in family `ip` or `ip6`. Installation validates the complete batch with `nft -c -f -`, then atomically creates the table, NAT chain, and exact address/TCP-port DNAT rule. A pre-existing table is rejected rather than adopted. Cleanup deletes only the table recorded as owned by this process before TUN teardown.

`net.ipv4.ip_forward`, `net.ipv6.conf.all.forwarding`, and broad host FORWARD policy are operator-managed prerequisites. The product reads/diagnoses forwarding but never enables it. GitHub runner-specific forwarding exceptions remain test-harness resources.

The temporary public-ingress P1 process currently runs as root so its short-lived `nft` child has the required privilege. `nft` is exec'd only during setup/cleanup; libnftables is not linked into the long-lived runtime. A future privileged helper can absorb this host module without changing lwIP or CC boundaries.

## IPv4 evidence

### Packet path: run `34744304038`

```text
tcp-shift-p1-rx-contract: mtu=1500 oversize=1501 rx_drops=1 rx_errors=0 runtime_survives_drop=ok
send seq=4097 valid=0 checksum_field=0xebbc software_verify=0x2886
send seq=4098 valid=1 checksum_field=0x1442 software_verify=0x0000
icmp_checksum_bad_reply=none icmp_checksum_good_reply=received
direct connected 10.231.0.2:18080
DNAT connected 198.51.100.1:18080
```

The same packet-path qualification preserves the idle/backpressure contracts: a separate two-second idle process makes only a handful of `epoll_wait` calls with zero TUN writable wakeups, and the unprivileged TX test retains at most 64 × 1500-byte packets for a 96,000-byte peak, rejects the 65th, observes real `EAGAIN`, preserves FIFO ordering, and cleans queued references on detach.

### Product-owned IPv4 ingress: run `34763055040`

```text
forwarding_disabled_preflight=ok
exclusive_collision_rejection=ok
product DNAT connected 198.51.101.1:18081
signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1 product-owned nft ingress lifecycle passed
```

The live public flow reached lwIP with `tcp_accepts=1`, `tcp_errors=0`. The gate separately proves forwarding-prerequisite failure without mutation/leak, stale-resource collision rejection without adoption/deletion, normal public ingress, SIGTERM cleanup, and unrelated nftables preservation.

## P1b IPv6 implementation

The pinned lwIP core is explicitly dual-stack but remains an L3 server profile. The following stay disabled:

```text
DHCPv6
SLAAC / Router Solicitation
MLD
ND6 packet queueing
RA MTU updates
IPv6 endpoint fragmentation
IPv6 reassembly
```

The two unused diagnostics produced by pinned `nd6.c` under this intentionally narrow configuration remain visible in CI but are not promoted to errors for that source file only. Project sources and all other warnings remain `-Werror`.

The same L3 netif provides `output_ip6`; RX selects `ip4_input` or `ip6_input` from the packet version nibble. IPv6 host/lwIP addresses are static. Physical NDP remains the host kernel's responsibility; the TUN boundary carries complete L3 packets and does not perform Ethernet neighbor resolution.

Public IPv6 ingress uses an exact product-owned `ip6` DNAT rule into an internal static IPv6 TUN address. The gate sends an actual Hop-by-Hop extension-header TCP SYN through the rule to prove L4 matching is extension-header-safe rather than inferring that property from nft's printed rule form.

### Pure-L3 PMTU integration

Pinned lwIP's ICMPv6 PTB handler updates only an already-existing ND6 destination-cache entry. Normal Ethernet IPv6 output creates this entry during next-hop resolution, but tcp-shift's pure L3 `output_ip6` intentionally bypasses that path. CI reproduced the resulting defect: a valid `mtu 1280` PTB reached lwIP, but a subsequent SYN-ACK still advertised MSS 1440.

`src/lwip/l3_tun.c` now seeds or refreshes the existing fixed `destination_cache[]` before unicast IPv6 output. It uses an empty slot first and otherwise the oldest entry, initializes PMTU to the netif MTU, starts no neighbor discovery, and allocates no second PMTU cache. Upstream `nd6_input()` still validates/applies the PTB, and upstream `tcp_eff_send_mss_netif()` consumes the learned value.

## Retained IPv6 evidence: run `34767386662`

Behavior head: `6a9d82feb1f3faead580f560ae1d076999a64b0f`. Both P0 and full P1 workflows passed on this head.

Direct IPv6 qualification proves 3/3 ICMPv6 echo, the 1500/1501 MTU boundary, a real AF_INET6 TCP connect/accept, zero TCP errors, and TUN cleanup.

Product-owned IPv6 lifecycle retained:

```text
ipv6_forwarding_disabled_preflight=ok
ipv6_exclusive_collision_rejection=ok
ipv6_extension_header_dnat=ok
product IPv6 DNAT connected [2001:db8:101::1]:18083
tcp-shift-p1-ipv6: ready tun=tsp1v6nft0 host-ipv6=fd00:198:18::1/126 lwip-ipv6=fd00:198:18::2 mtu=1500 tcp-port=18083 public-ipv6=2001:db8:101::1 nft-table=tcp_shift_p1
ipv6_signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1b product-owned IPv6 nft ingress lifecycle passed
```

Routed PTB/PMTU qualification retained:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
P1b routed IPv6 Packet Too Big/PMTU qualification passed
```

The PTB topology keeps the client link at MTU 1500 so the 1500-byte request reaches lwIP, then applies an MTU 1280 only to the router's /128 egress route back to that client. tcpdump proves the original request and the resulting `ICMP6, packet too big, mtu 1280` both cross the TUN. The next TCP handshake advertises MSS 1220 (`1280 - 40 - 20`).

## P1 exit criteria — satisfied on GitHub runner

Retained evidence now proves both address families for:

- exclusive nonpersistent TUN acquisition/configuration and cleanup;
- ICMP/ICMPv6 through lwIP;
- TCP SYN/SYN-ACK/accept at a minimal lwIP listener;
- exact product-owned IPv4/IPv6 ingress DNAT/conntrack;
- checksum and MTU behavior;
- IPv6 extension-header-safe TCP ingress matching;
- IPv6 Packet Too Big/PMTU learning and TCP MSS adaptation;
- no permanent writable polling and bounded idle wakeups;
- bounded whole-packet TX backpressure;
- forwarding-prerequisite failure without host-wide mutation;
- stale-resource collision rejection;
- signal/startup cleanup and preservation of unrelated firewall state.

This is not evidence that every target OpenVZ/VPS provider permits the required TUN, forwarding, nftables, conntrack, and capability operations. Provider qualification is a separate deployment task and must not be inferred from the runner result.

## Event-loop and packet-ownership invariants

There is one mutable lwIP owner. TUN RX, TUN TX retry, lwIP timers, and later backend sockets all execute on that owner. No per-flow worker threads are introduced.

TUN is packet-oriented. A transmit packet is never stream-split across writes. On `EAGAIN`, tcp-shift takes a pbuf reference and retains the entire packet in the bounded FIFO. The reference is released after successful transmit or adapter teardown. Queue exhaustion is observable and bounded.

RX allocates only the pbuf required by the received packet after copying from a bounded host read buffer. Correctness and bounded residency take priority over copy reduction in P1.

## Deferred to P2

P1 does not connect accepted TCP streams to an application backend. P2 replaces `probe_listener` with the real flow bridge. Public IPv4 and IPv6 share one bridge state machine, while the backend initially remains a normal nonblocking `AF_INET` socket to `127.0.0.1`.

P2 owns partial stream I/O, bounded bidirectional buffering/backpressure, EOF/half-close/reset semantics, backend-connect failure, deterministic flow teardown, and repeated-flow memory behavior. Congestion-control work remains deferred until this bridge and its memory profile are observable.
