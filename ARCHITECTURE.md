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

Congestion control for the public connection belongs to lwIP/tcp-shift. Congestion control on the loopback backend is outside the public-side contract. Public IPv6 does not imply an IPv6 backend: the current bridge target is deliberately `127.0.0.1` for both public families.

## Packet path

The production direction is routed L3 TUN plus narrow DNAT/conntrack rules. TUN is chosen because lwIP must receive the original IP/TCP packets; terminating the public TCP connection in an ordinary host socket would return congestion-control ownership to the provider kernel.

TAP/Ethernet is not part of the current design. Physical ARP/NDP stays with the outer Linux network stack. The TUN boundary carries complete IPv4 or IPv6 packets only.

P1 runner qualification covers IPv4 and IPv6 through the same L3 adapter/runtime. P2 qualifies the same public families through one address-family-independent bridge state machine. P3 adds no alternate datapath; it measures that same bridge/runtime under staged idle, active-window, repeated-drain, and CPU workloads.

## Process model versus module model

The current implementation is one Linux process with one mutable lwIP owner. This keeps the runtime small and avoids IPC before there is a concrete privilege-separation requirement.

Source boundaries are strict:

```text
host/                 Linux TUN/interface/netfilter/route/lifecycle integration
runtime/              event loop and process lifecycle
lwip/                 lwIP L3/TCP integration and transport adapter
bridge/               public-stream <-> host-backend forwarding
cc/                   generic congestion-control core
```

The temporary public-ingress P1 executable runs with root privileges because `src/host/nft_ingress.*` briefly execs the system `nft` command during setup and cleanup. The established packet datapath itself remains the single lwIP owner and does not invoke nftables after setup.

The P2/P3 qualification executables intentionally exercise bridge behavior on directly addressed TUN endpoints, so they need TUN administration but do not own public nftables ingress. Product integration can later combine the already-qualified host ingress lifecycle and bridge without merging their source responsibilities.

A future root helper may own TUN/netfilter setup and pass a TUN fd to an unprivileged runtime. That would be a process-boundary hardening change only; it is not required for congestion-control portability.

## Congestion-control portability boundary

P4 introduces the `cc/` boundary as an independently buildable pure-C static library. It may be linked into the same `tcp-shift` process; an independent library does not imply an independent process.

The generic CC core must not depend on TUN, nftables, epoll, timerfd, host socket descriptors, backend bridge objects, or Linux syscalls. It consumes transport observations such as sent/acked/lost bytes, delivery-rate samples, RTT samples, inflight state, app-limited state, and monotonic timestamps, and produces policy outputs such as cwnd and pacing rate.

Linux-specific pacing belongs to the runtime scheduler introduced later. An embedded port may use an RTOS or hardware timer. The controller must not own that scheduler directly. This boundary is what permits a later extraction into a reusable `lwip-cc` project if the API proves stable.

P4 must first establish generic transport event/policy interfaces and validate them with a conventional controller. High-resolution delivery sampling, per-segment rate metadata, app-limited detection, and process-wide pacing are P5 prerequisites; BBR-specific state does not belong in the initial generic boundary.

## lwIP ownership and threading

The runtime uses `NO_SYS=1` and the callback/raw TCP API. There is no lwIP socket API, netconn layer, tcpip worker thread, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The event loop uses epoll readiness and derives its blocking timeout from `sys_timeouts_sleeptime()`, followed by `sys_check_timeouts()`. It does not use a fixed polling tick. TUN EPOLLOUT is armed only while the bounded whole-packet TX queue is non-empty.

P2 generalizes the same loop with caller-owned fd watcher objects for backend sockets. The loop intentionally consumes one ready fd per `epoll_wait`; a watcher callback may therefore unregister and free its own enclosing flow without leaving another event from the current batch pointing at freed memory. Backend readiness, TUN readiness, lwIP timers, and future pacing deadlines all remain under one mutable owner.

## P1 packet/lifecycle implementation boundary

P1 contains independently compiled host, L3, runtime, and probe modules:

- `src/host/tun.*`: acquisition/closing of a nonpersistent exclusive `IFF_TUN | IFF_NO_PI` fd;
- `src/host/ifconfig.*` and `ifconfig_ipv6.*`: Linux host-side static L3 configuration;
- `src/host/nft_ingress.*`: family-aware product-owned exact-match nftables ingress acquisition, prerequisite checks, collision rejection, and cleanup;
- `src/lwip/l3_tun.*`: one lwIP L3 netif for IPv4/IPv6 attachment, receive dispatch, complete-packet transmit, bounded TUN backpressure, and IPv6 PMTU destination-cache integration;
- `src/runtime/lwip_loop.*`: epoll ownership, TUN/backend watcher readiness, RX work budget, and lwIP timeout integration;
- `src/lwip/probe_listener.*`: temporary raw-API listener retained only for P1 qualification;
- `tcp-shift-p1` / `tcp-shift-p1-ipv6`: temporary packet/lifecycle bring-up executables.

The TUN TX queue holds at most 64 packets and 96 KiB. On `EAGAIN`, the adapter takes a pbuf reference and transfers responsibility to this queue; queue exhaustion returns `ERR_MEM`. Because TUN is packet-oriented, partial/stream-split packet transmission is prohibited.

## P2 bridge implementation boundary

`src/bridge/bridge.*` is the current public-stream/backend bridge. A successful public lwIP accept allocates one bridge flow control object and opens one nonblocking `AF_INET` socket to `127.0.0.1:<backend-port>`.

Public IPv4 and IPv6 listeners call the same bridge implementation. The backend stays IPv4 loopback even for an IPv6 public connection, preserving the architectural separation between the public transport and application-facing local transport.

The bridge adds no fixed application-data direction buffer.

Public-to-backend bytes remain in lwIP-delivered pbufs until the backend `writev()` commits them. `tcp_recved()` advances only by committed bytes. On host-socket `EAGAIN`, the flow retains the pbuf and arms backend `EPOLLOUT`, so the lwIP receive window provides bounded pressure rather than allowing an unbounded userspace queue.

Backend-to-public uses a 4-KiB stack scratch buffer and `MSG_PEEK`. Bytes are removed from the host socket only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes into lwIP. If the lwIP send path returns `ERR_MEM` or has no send buffer, backend readable interest is suppressed and later resumed by `tcp_sent`/`tcp_poll`.

Backend sockets request 16-KiB send and receive buffers. Linux reports 32 KiB for each on the current qualification runner. These kernel buffers are deliberately small and explicitly observed so a passing backpressure test cannot be explained by a large loopback socket absorbing the workload.

EOF is directional. Public EOF becomes backend `SHUT_WR` after queued public bytes drain. Backend EOF becomes lwIP transmit shutdown while the public receive direction may remain open. Because `EPOLLRDHUP` is level-triggered, the backend watcher is removed when no useful event remains and is re-added only when later write/read progress requires it; this prevents half-close readiness spin.

Flow failure is isolated. Backend refusal/reset or a public reset releases only that flow. Listener/runtime state remains reusable for later accepts. `tcp_shift_bridge_stop()` explicitly aborts active flows and clears pending residency before process/TUN teardown.

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

## Qualification state

IPv4 packet-path run `34744304038` proves direct ICMP/TCP, DNAT/conntrack, bounded idle wakeups, bounded TX backpressure, nonfatal oversize RX, MTU 1500/1501 behavior, and invalid/valid ICMP checksum handling. Product-owned IPv4 ingress lifecycle run `34763055040` proves forwarding preflight, exclusive ownership, exact DNAT, cleanup, rollback, and unrelated-firewall preservation.

P1b behavior head `6a9d82feb1f3faead580f560ae1d076999a64b0f` and run `34767386662` additionally prove direct IPv6 ICMP/TCP, IPv6 MTU behavior, product-owned exact IPv6 ingress/conntrack, extension-header-safe matching, deterministic cleanup, and routed PTB/PMTU adaptation.

P2 behavior head `601a49648610513d98173e3e3add722326591ffc` passed P0 run `34769960299`, the full P1 regression run `34769960302`, and P2 run `34769960275`.

P2 runner evidence covers IPv4 and IPv6 public-stream integrity to an IPv4 loopback backend, 1-MiB bounded bidirectional backpressure, both half-close directions, backend refusal recovery, public/backend resets, eight concurrent flows, active-flow process shutdown, and 64 sequential reuse flows without VmRSS ratcheting. The retained reuse samples were 1800 KiB after warm-up, 32 flows, and 64 flows.

P3 behavior head `775ea5832f7e308e2c908c2f5abedfa4175c69be` passed run `34805306193`, job `103855926986`. It runner-qualifies staged process PSS/private-dirty/fd observations, both directional active-window residency workloads, three repeated 128-flow load/drain rounds, a small-operation CPU baseline, and the constrained-host process-PSS planning model.

This is GitHub-runner qualification, not evidence that every target OpenVZ/VPS provider exposes the required TUN, forwarding, nftables, conntrack, capability, memory-accounting, or scheduling surface. Provider qualification remains separate.

## Memory and CPU model

Demand-backed libc allocation remains the Linux baseline so RSS/PSS follows real use. Static/custom pools are introduced only when measurements justify them.

P1 has mechanical bounds for its event loop and TUN retry queue: at most 64 queued packets / 96 KiB, real `EAGAIN` FIFO qualification, and a two-second idle gate with zero writable wakeups and a deliberately loose ceiling of 32 total waits. IPv6 PMTU integration reuses lwIP's already-allocated fixed destination cache rather than introducing another fixed table.

P2 adds bridge-level accounting for active/peak flow objects, current/peak public pbuf residency, blocked-read/write events, and actual backend socket buffer sizes. Its 64-flow reuse gate is specifically a lifecycle/no-ratcheting check; it is not the product's per-connection memory number.

P3 establishes the pre-CC process baseline. Final run `34805306193` measured 262 KiB ready PSS, 307 KiB at 128 idle flows, and a maximum direct idle slope of 0.3515625 KiB/flow. Repeated rounds make the conservative idle slope about 0.398 KiB/flow and set a 315-KiB warm fixed process floor for planning.

Controlled active residency is the dominant userspace cost. Public-to-backend pressure added 36.5 KiB/flow; backend-to-public pressure added 37.125 KiB/flow. The conservative fully-window-resident process slope used for planning is therefore about 37.52 KiB/flow, including the idle slope. The corresponding qualified public-side data residency is one 32-KiB lwIP window per flow.

The repeated-drain regression gate permits at most 32 KiB first-to-last drained PSS growth across three 128-flow rounds and at most 128 KiB warm drain floor above ready. The final run observed 5 KiB and 57 KiB respectively, with fd count returning to 5 every round.

The CPU baseline observed zero process CPU ticks during a one-second idle interval and about 34.18 microseconds of process CPU per operation for 2048 synchronous 64-byte request/echo operations across four flows. It is a runner comparison point, not a provider SLA.

For P4 admission, the model assigns only 25% of a 32-MiB host to tcp-shift process PSS. Under the 315-KiB fixed floor and 37.52-KiB active slope, 128 fully-window-resident flows project to about 5118 KiB, leaving about 3074 KiB or 24.0 KiB/flow inside the 8-MiB process budget for later CC/sampler/pacer structures.

That projection is not total host residency. Backend Linux TCP kernel memory, backend application memory, public-client kernel memory, and provider-specific overhead are intentionally outside process PSS and must fit in the remaining host budget. P4/P5 changes must report their incremental fixed/per-flow/per-segment process cost against this P3 baseline.

## Documentation as project memory

The repository is the durable project memory. Any change that invalidates product behavior or an architectural statement must update the relevant documentation in the same change.

Start with `README.md`, this file, `docs/lwip-roadmap.md`, `docs/ci.md`, and `docs/development.md`. Milestone documents record decisions, rejected alternatives, exit criteria, evidence, and unresolved questions so another agent can continue without relying on chat history.
