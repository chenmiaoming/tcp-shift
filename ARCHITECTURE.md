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
    | public TCP: owned by lwIP and tcp-shift CC
    v
host netfilter/routing -> L3 TUN -> lwIP TCP listener
                                      |
                                      | accepted byte stream
                                      v
                               userspace bridge
                                      |
                                      | local TCP: owned by host Linux
                                      v
                               127.0.0.1 backend
                                      |
                                      v
                                  nginx/app
```

Congestion control for the public connection belongs to lwIP/tcp-shift. Congestion control on the loopback backend is outside the public-side contract.

## Packet path

The production direction is routed L3 TUN plus narrow DNAT/conntrack rules. TUN is chosen because lwIP must receive the original IP/TCP packets; terminating the public TCP connection in an ordinary host socket would return congestion-control ownership to the provider kernel.

TAP/Ethernet is not part of the current design. Physical ARP/NDP stays with the outer Linux network stack. The TUN boundary carries complete IPv4 or IPv6 packets only.

P1a qualifies IPv4. P1b extends the same L3 adapter/runtime to IPv6 and is a first-class product requirement, not a late optional feature. Bridge and congestion-control layers must remain address-family agnostic.

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

- `src/host/tun.*`: acquisition/closing of a nonpersistent `IFF_TUN | IFF_NO_PI` fd;
- `src/host/ifconfig.*`: Linux host-side IPv4 MTU/address/link-up configuration;
- `src/host/nft_ingress.*`: product-owned exact-match nftables ingress acquisition, prerequisite checks, collision rejection, and cleanup;
- `src/lwip/l3_tun.*`: lwIP L3 netif attachment, one-packet receive injection, complete-packet transmit, and bounded TUN backpressure;
- `src/runtime/lwip_loop.*`: epoll ownership, TUN readiness, RX work budget, and lwIP timeout integration;
- `src/lwip/probe_listener.*`: temporary raw-API listener used only to qualify P1 TCP ownership;
- `tcp-shift-p1`: temporary bring-up/lifecycle executable used before the final CLI exists.

The TUN TX queue holds at most 64 packets and 96 KiB. On `EAGAIN`, the adapter takes a pbuf reference and transfers responsibility to this queue; queue exhaustion returns `ERR_MEM`. Because TUN is packet-oriented, partial/stream-split packet transmission is prohibited.

### IPv4 host-resource ownership

The temporary P1 executable owns TUN creation, IPv4 MTU/address/up configuration, and optionally one public IPv4 DNAT resource. TUN is nonpersistent, so closing its fd removes its address and connected route.

Public ingress uses a dedicated nftables table named `ip tcp_shift_p1`. Before mutation, the host module validates the complete ruleset with `nft -c -f -`. Installation sends the same batch as one transaction and begins with `create table`, so an existing/stale resource or a race fails rather than being adopted. The table contains only a PREROUTING NAT chain and an exact `ip daddr <public-ip> tcp dport <port> dnat to <lwip-ip>:<port>` rule.

The module locates `nft` only at fixed system paths and execs it directly; it does not invoke a shell or permanently link libnftables into the low-memory runtime. The nft child exists only during setup/cleanup.

`net.ipv4.ip_forward` and surrounding broad host `FORWARD` policy are operator-managed prerequisites. tcp-shift reads and diagnoses the forwarding sysctl but never enables it or rewrites unrelated forwarding policy. Cleanup is reverse acquisition order: delete the table recorded as owned by this process, then tear down the event loop/lwIP state and finally close the TUN. A table that existed before startup is never considered owned.

## P1a IPv4 evidence

The packet-path qualification in run `34744304038` proves direct ICMP/TCP, DNAT/conntrack, bounded idle wakeups, bounded TX backpressure, nonfatal oversize RX, MTU 1500/1501 behavior, and invalid/valid ICMP checksum handling.

The product-owned ingress lifecycle was then qualified in run `34763055040`. The lifecycle gate proved:

```text
forwarding_disabled_preflight=ok
exclusive_collision_rejection=ok
product DNAT connected 198.51.101.1:18081
signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1 product-owned nft ingress lifecycle passed
```

The live product-owned flow terminated in lwIP and reported `tcp_accepts=1`, `tcp_errors=0`. A disabled `net.ipv4.ip_forward` caused startup failure without changing the sysctl or leaking the TUN/table. A pre-existing `tcp_shift_p1` table caused startup rejection and remained intact. SIGTERM removed the product table and TUN, while a separate unrelated nftables table was byte-for-byte unchanged.

GitHub-hosted runners have Docker's IPv4 `FORWARD` policy set to `DROP`. CI therefore owns two exact interface/IP/port-specific ACCEPT exceptions around the lifecycle test. Those exceptions are runner scaffolding and are deliberately outside product ownership.

P1a IPv4 is complete by its current exit criteria. The active transport milestone is P1b IPv6.

## IPv6 requirements

IPv6-only operation is a product gate. P1b must cover IPv6 TUN addressing, `output_ip6`, IPv6 packet input, ICMPv6, TCP SYN/SYN-ACK, exact product-owned IPv6 ingress, Packet Too Big/PMTU behavior, extension-header-safe L4 matching, and cleanup. Host capability checks must distinguish lwIP support from container restrictions such as unavailable TUN, forwarding, conntrack/NAT, or `CAP_NET_ADMIN`.

The backend remains IPv4 loopback initially. Public IPv6 does not require the application backend to be IPv6.

## Memory and CPU model

Demand-backed libc allocation is the initial Linux baseline so RSS follows real use. Static/custom pools are introduced only when measurements justify them. CI records idle and loaded memory, repeated load/drain floors, and CPU under explicit workloads. A one-time RSS decrease is not sufficient evidence against long-lived allocator or lifecycle growth.

P1a has mechanical bounds and evidence for its event loop and TUN retry queue: at most 64 queued packets / 96 KiB, real `EAGAIN` FIFO qualification, and a two-second idle gate with zero writable wakeups and a deliberately loose ceiling of 32 total waits. Future milestones add flow and BDP residency accounting rather than weakening these fixed bounds.

## Documentation as project memory

The repository is the durable project memory. Any change that invalidates product behavior or an architectural statement must update the relevant documentation in the same change.

Start with `README.md`, this file, `docs/lwip-roadmap.md`, `docs/ci.md`, and `docs/development.md`. Milestone documents record decisions, rejected alternatives, exit criteria, evidence, and unresolved questions so another agent can continue without relying on chat history.
