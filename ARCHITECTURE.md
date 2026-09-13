# tcp-shift architecture

This file is the current architectural source of truth. Historical milestone documents explain how the design evolved, but they do not override this document. A new developer or coding agent should be able to reconstruct the product from this repository without access to an earlier chat session.

## Product problem

`tcp-shift` targets constrained VPS/container environments where the tenant can operate TUN/netfilter but cannot choose the outer kernel's TCP congestion-control implementation. The product moves ownership of the public TCP endpoint into lwIP while leaving the application on an ordinary host loopback socket.

The initial deployment target includes very small 32/64/128-MiB systems and IPv6-only low-cost VPSes. Fixed memory, idle CPU, cleanup behavior, and explicit host prerequisites are product properties rather than benchmark afterthoughts.

## Two TCP connections

The public and backend connections are intentionally different transports:

```text
remote client
    |
    | public TCP: IPv4 or IPv6, owned by lwIP and tcp-shift CC
    v
host netfilter/routing -> L3 TUN -> lwIP TCP listener
                                      |
                                      | accepted byte stream
                                      v
                               userspace bridge
                                      |
                                      | local AF_INET TCP: owned by host Linux
                                      v
                               127.0.0.1 backend
                                      |
                                      v
                                  nginx/app
```

Congestion control for the public connection belongs to lwIP/tcp-shift. Congestion control on the loopback backend is outside the public-side contract. Public IPv6 does not imply an IPv6 backend: the initial bridge target is deliberately `127.0.0.1` for both public families.

## Packet path

The production direction is routed L3 TUN plus narrow DNAT/conntrack rules. TUN is chosen because lwIP must receive the original IP/TCP packets; terminating the public TCP connection in an ordinary host socket would return congestion-control ownership to the provider kernel.

TAP/Ethernet is not part of the current design. Physical ARP/NDP stays with the outer Linux network stack. The TUN boundary carries complete IPv4 or IPv6 packets only.

P1 runner qualification covers IPv4 and IPv6 through the same L3 adapter/runtime. Bridge and congestion-control layers must remain address-family agnostic.

## Process model versus module model

The current implementation is one Linux process with one mutable lwIP owner. This keeps the first working runtime small and avoids IPC before there is a concrete privilege-separation requirement.

Source boundaries are strict:

```text
host/                 Linux TUN/interface/netfilter/route/lifecycle integration
runtime/              event loop and process lifecycle
lwip/                 lwIP L3/TCP integration and transport adapter
bridge/               public-stream <-> host-backend forwarding
cc/                   generic congestion-control core
```

The temporary public-ingress P1 executable currently runs with root privileges because `src/host/nft_ingress.*` briefly execs the system `nft` command during setup and cleanup. The established packet datapath itself remains the single lwIP owner and does not invoke nftables after setup.

A future root helper may own TUN/netfilter setup and pass a TUN fd to an unprivileged runtime. That would be a process-boundary hardening change only; it is not required for congestion-control portability.

## Congestion-control portability boundary

The future `cc/` code is intended to become an independently buildable pure-C static library. It may be linked into the same `tcp-shift` process; an independent library does not imply an independent process.

The generic CC core must not depend on TUN, nftables, epoll, timerfd, host socket descriptors, backend bridge objects, or Linux syscalls. It consumes transport observations such as sent/acked/lost bytes, delivery-rate samples, RTT samples, inflight state, app-limited state, and monotonic timestamps, and produces policy outputs such as cwnd and pacing rate.

Linux-specific pacing uses a runtime scheduler. An embedded port may use an RTOS or hardware timer. The controller must not own that scheduler directly. This boundary is what permits a later extraction into a reusable `lwip-cc` project if the API proves stable.

## lwIP ownership and threading

The runtime uses `NO_SYS=1` and the callback/raw TCP API. There is no lwIP socket API, netconn layer, tcpip worker thread, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The P1 event loop uses epoll readiness and derives its blocking timeout from `sys_timeouts_sleeptime()`, followed by `sys_check_timeouts()`. It does not use a fixed polling tick. TUN EPOLLOUT is armed only while the bounded whole-packet TX queue is non-empty. Backend socket readiness and later pacing deadlines will join the same mutable owner.

## P1 implementation boundary

P1 contains independently compiled host, L3, runtime, and probe modules:

- `src/host/tun.*`: acquisition/closing of a nonpersistent exclusive `IFF_TUN | IFF_NO_PI` fd;
- `src/host/ifconfig.*` and `ifconfig_ipv6.*`: Linux host-side static L3 configuration;
- `src/host/nft_ingress.*`: family-aware product-owned exact-match nftables ingress acquisition, prerequisite checks, collision rejection, and cleanup;
- `src/lwip/l3_tun.*`: one lwIP L3 netif for IPv4/IPv6 attachment, receive dispatch, complete-packet transmit, bounded TUN backpressure, and IPv6 PMTU destination-cache integration;
- `src/runtime/lwip_loop.*`: epoll ownership, TUN readiness, RX work budget, and lwIP timeout integration;
- `src/lwip/probe_listener.*`: temporary raw-API listener used only to qualify P1 TCP ownership;
- `tcp-shift-p1` / `tcp-shift-p1-ipv6`: temporary bring-up/lifecycle executables used before the final CLI exists.

The TUN TX queue holds at most 64 packets and 96 KiB. On `EAGAIN`, the adapter takes a pbuf reference and transfers responsibility to this queue; queue exhaustion returns `ERR_MEM`. Because TUN is packet-oriented, partial/stream-split packet transmission is prohibited.

## Host-resource ownership

The temporary P1 executables own nonpersistent TUN acquisition, static host-side address/MTU/up configuration, and optionally one exact public DNAT resource. Closing the nonpersistent TUN fd removes the interface and its directly associated state.

Public ingress uses one dedicated nftables table named `tcp_shift_p1` in family `ip` or `ip6`. Before mutation, the host module validates the complete ruleset with `nft -c -f -`. Installation sends the same batch as one transaction and begins with `create table`, so an existing/stale resource or a race fails rather than being adopted.

IPv4 matches exact `ip daddr <public-ip> tcp dport <port>`. IPv6 uses exact `ip6 daddr <public-ip> meta l4proto tcp tcp dport <port>` semantics and DNATs to an internal static IPv6 TUN address. CI uses an actual Hop-by-Hop extension-header packet to qualify the IPv6 L4 match instead of relying on nft's canonical printed syntax.

The module locates `nft` only at fixed system paths and execs it directly; it does not invoke a shell or permanently link libnftables into the low-memory runtime. The nft child exists only during setup/cleanup.

`net.ipv4.ip_forward`, `net.ipv6.conf.all.forwarding`, and surrounding broad host `FORWARD` policy are operator-managed prerequisites. tcp-shift reads and diagnoses forwarding state but never enables it or rewrites unrelated forwarding policy. Cleanup deletes only the table recorded as owned by this process before tearing down lwIP/TUN state. A table that existed before startup is never considered owned.

## IPv6 L3 and PMTU boundary

IPv6 uses static L3 addressing. tcp-shift intentionally keeps DHCPv6, SLAAC, Router Solicitation, MLD, ND6 packet queueing, RA MTU updates, endpoint fragmentation, and reassembly disabled in the current low-memory profile.

A pure L3 TUN output callback bypasses lwIP's Ethernet next-hop path. That path normally creates the ND6 destination-cache entry later used by ICMPv6 Packet Too Big handling. Without an entry, pinned upstream lwIP receives a valid PTB but drops its PMTU update, leaving TCP MSS at the interface-MTU value.

`src/lwip/l3_tun.c` closes this adapter gap by seeding or refreshing lwIP's existing fixed `destination_cache[]` for unicast IPv6 output. It follows lwIP's empty-first/oldest-entry replacement shape, starts an entry at the netif MTU, does not start neighbor discovery, and allocates no separate PMTU table. Upstream `nd6_input()` remains responsible for validating/applying PTB updates, and upstream `tcp_eff_send_mss_netif()` remains responsible for consuming the learned PMTU.

Run `34767386662` proves a routed MTU reduction from 1500 to 1280 changes a subsequent IPv6 SYN-ACK MSS from 1440 to 1220 while all IPv4 and other IPv6 regression gates stay green.

## P1 evidence and completion state

IPv4 packet-path run `34744304038` proves direct ICMP/TCP, DNAT/conntrack, bounded idle wakeups, bounded TX backpressure, nonfatal oversize RX, MTU 1500/1501 behavior, and invalid/valid ICMP checksum handling. Product-owned IPv4 ingress lifecycle run `34763055040` proves forwarding preflight, exclusive ownership, exact DNAT, cleanup, rollback, and unrelated-firewall preservation.

P1b behavior head `6a9d82feb1f3faead580f560ae1d076999a64b0f` passed both P0 and P1 workflows. P1 run `34767386662` additionally proves direct IPv6 ICMP/TCP, IPv6 MTU behavior, product-owned exact IPv6 ingress/conntrack, extension-header-safe matching, deterministic cleanup, and routed PTB/PMTU adaptation:

```text
ipv6_forwarding_disabled_preflight=ok
ipv6_exclusive_collision_rejection=ok
ipv6_extension_header_dnat=ok
product IPv6 DNAT connected [2001:db8:101::1]:18083
ipv6_signal_cleanup=ok unrelated_ruleset_unchanged=ok
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
```

P1 dual-stack packet/lifecycle qualification is complete on GitHub-hosted runners. This is not evidence that every target OpenVZ/VPS provider exposes the required TUN, forwarding, nftables, conntrack, or capability surface. Provider qualification remains separate.

## P2 bridge boundary

The next milestone replaces the temporary probe listener with the real public-stream/backend bridge. Public IPv4 and IPv6 must share one bridge state machine; the backend leg initially uses only an ordinary nonblocking `AF_INET` socket to `127.0.0.1`.

P2 must make partial I/O, bounded bidirectional buffering, backpressure, EOF/half-close, reset/backend failure, and deterministic cleanup explicit before congestion-control work begins.

## Memory and CPU model

Demand-backed libc allocation is the initial Linux baseline so RSS follows real use. Static/custom pools are introduced only when measurements justify them. CI records idle and loaded memory, repeated load/drain floors, and CPU under explicit workloads. A one-time RSS decrease is not sufficient evidence against long-lived allocator or lifecycle growth.

P1 has mechanical bounds and evidence for its event loop and TUN retry queue: at most 64 queued packets / 96 KiB, real `EAGAIN` FIFO qualification, and a two-second idle gate with zero writable wakeups and a deliberately loose ceiling of 32 total waits. IPv6 PMTU integration reuses lwIP's already-allocated fixed destination cache rather than introducing another fixed table. Future milestones add flow and BDP residency accounting rather than weakening these fixed bounds.

## Documentation as project memory

The repository is the durable project memory. Any change that invalidates product behavior or an architectural statement must update the relevant documentation in the same change.

Start with `README.md`, this file, `docs/lwip-roadmap.md`, `docs/ci.md`, and `docs/development.md`. Milestone documents record decisions, rejected alternatives, exit criteria, evidence, and unresolved questions so another agent can continue without relying on chat history.
