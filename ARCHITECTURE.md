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

P1 starts with IPv4 because it is the simplest packet-path qualification. IPv6 follows immediately in P1b and is a first-class product requirement, not a late optional feature. The same L3 adapter will gain an IPv6 output callback and IPv6 input path; the bridge and congestion-control layers must remain address-family agnostic.

## Process model versus module model

The current implementation is one Linux process with one mutable lwIP owner. This keeps the first working runtime small and avoids IPC before there is a concrete privilege-separation requirement.

Source boundaries are nevertheless strict:

```text
host/                 Linux TUN/netfilter/route/lifecycle integration
runtime/              event loop and process lifecycle
lwip/                 lwIP L3/TCP integration and transport adapter
bridge/               public-stream <-> host-backend forwarding
cc/                   generic congestion-control core
```

A future root helper may own TUN/netfilter setup and pass a TUN fd to an unprivileged runtime. That would be a process-boundary change only; it must not be required to make the congestion-control code portable.

## Congestion-control portability boundary

The future `cc/` code is intended to become an independently buildable pure-C static library. It may be linked into the same `tcp-shift` process; an independent library does not imply an independent process.

The generic CC core must not depend on TUN, nftables, epoll, timerfd, host socket descriptors, backend bridge objects, or Linux syscalls. It consumes transport observations such as sent/acked/lost bytes, delivery-rate samples, RTT samples, inflight state, app-limited state, and monotonic timestamps, and produces policy outputs such as cwnd and pacing rate.

Linux-specific pacing uses a runtime scheduler. An embedded port may use an RTOS or hardware timer. The controller must not own that scheduler directly. This boundary is what permits a later extraction into a reusable `lwip-cc` project if the API proves stable.

## lwIP ownership and threading

The runtime uses `NO_SYS=1` and the callback/raw TCP API. There is no lwIP socket API, netconn layer, tcpip worker thread, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The current P1 event loop uses epoll readiness and derives its blocking timeout from `sys_timeouts_sleeptime()`, followed by `sys_check_timeouts()`. It does not use a fixed polling tick. TUN EPOLLOUT is armed only while the bounded whole-packet TX queue is non-empty. Backend socket readiness and later pacing deadlines will join the same mutable owner.

## P1 implementation boundary

P1 currently contains independently compiled host, L3, and runtime modules:

- `src/host/tun.*`: Linux-only acquisition/closing of a nonpersistent `IFF_TUN | IFF_NO_PI` fd;
- `src/lwip/l3_tun.*`: IPv4 lwIP netif attachment, one-packet receive injection, complete-packet transmit, and a bounded TUN backpressure queue;
- `src/runtime/lwip_loop.*`: epoll ownership, TUN read/write readiness, RX work budget, and lwIP timeout integration;
- `tcp-shift-p1`: temporary bring-up executable used before the final CLI/lifecycle layer exists.

The TUN TX queue holds at most 64 packets and 96 KiB. On `EAGAIN`, the adapter takes a pbuf reference and transfers responsibility to this queue; queue exhaustion returns `ERR_MEM`. Because TUN is packet-oriented, partial/stream-split packet transmission is prohibited.

This is still P1 bring-up, not the product runtime. Host address/route/firewall mutation is not yet owned transactionally, and privileged packet-path CI has not yet proven ICMP/TCP behavior.

## IPv6 requirements

IPv6-only operation is a product gate. P1b must cover IPv6 TUN addressing, `output_ip6`, ICMPv6, TCP SYN/SYN-ACK, Packet Too Big/PMTU behavior, extension-header-safe netfilter matching, and cleanup. Host capability checks must distinguish lwIP support from container restrictions such as unavailable TUN, forwarding, conntrack/NAT, or `CAP_NET_ADMIN`.

The backend remains IPv4 loopback initially. Public IPv6 does not require the application backend to be IPv6.

## Memory and CPU model

Demand-backed libc allocation is the initial Linux baseline so RSS follows real use. Static/custom pools are introduced only when measurements justify them. CI records idle and loaded memory, repeated load/drain floors, and CPU under explicit workloads. A one-time RSS decrease is not sufficient evidence against long-lived allocator or lifecycle growth.

The P1 TUN retry queue has explicit packet and byte ceilings so temporary host write backpressure cannot become an unbounded memory path.

## Documentation as project memory

The repository is the durable project memory. Any change that invalidates product behavior or an architectural statement must update the relevant documentation in the same change.

Start with `README.md`, this file, `docs/lwip-roadmap.md`, `docs/ci.md`, and `docs/development.md`. Milestone documents record decisions, rejected alternatives, exit criteria, evidence, and unresolved questions so another agent can continue without relying on chat history.
