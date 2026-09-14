# tcp-shift architecture

This file is the current architectural source of truth. Historical milestone documents explain how the design evolved but do not override this document.

## Product problem and transport split

`tcp-shift` targets constrained VPS/container environments where the tenant can operate TUN/netfilter but cannot choose the outer kernel's TCP congestion-control implementation. The product moves ownership of the public TCP endpoint into lwIP while leaving the application on an ordinary host loopback socket.

```text
remote client
    |
    | public IPv4 or IPv6 TCP
    v
Linux routing / narrow DNAT
    |
    v
L3 TUN
    |
    v
lwIP public TCP endpoint
    |
    | accepted byte stream
    v
single-owner userspace bridge
    |
    v
ordinary AF_INET socket
    |
    v
127.0.0.1 backend
```

The public and backend TCP connections are distinct. Public-side congestion control belongs to tcp-shift/lwIP. Backend loopback congestion control is outside the product contract. Public IPv6 therefore does not require an IPv6 application backend.

The deployment target includes small 32/64/128-MiB systems and IPv6-only low-cost VPSes. Memory, idle CPU, cleanup behavior, forwarding prerequisites, and firewall ownership are product properties.

## Packet path and host ownership

The production direction is routed L3 TUN, not TAP/Ethernet. The outer Linux stack continues to own physical ARP/NDP. The TUN carries complete IPv4/IPv6 packets only.

The product-owned public ingress resource is narrow DNAT/conntrack state. IPv4 uses an exact public destination/port match. IPv6 uses exact destination plus `meta l4proto tcp` so extension headers do not invalidate TCP classification. tcp-shift does not enable broad host forwarding, SNAT/masquerade, or unrelated firewall policy.

`net.ipv4.ip_forward` and `net.ipv6.conf.all.forwarding` are read-only prerequisites. Product setup fails before mutation when the selected family's forwarding prerequisite is disabled.

TUN interfaces are nonpersistent. nftables ownership is exclusive: a pre-existing product table is a collision, never an adopted resource. Cleanup deletes only state recorded as owned by the current process.

## Single-owner runtime

lwIP runs with `NO_SYS=1`. There is no lwIP socket API, netconn layer, `tcpip_thread`, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The event loop uses epoll readiness plus lwIP timer deadlines. It does not spin on a fixed polling tick. TUN `EPOLLOUT` is armed only while the bounded complete-packet TX queue is non-empty. Backend socket watchers use the same owner; callbacks may remove/free their own watcher because one ready fd is processed per `epoll_wait` iteration.

The same owner is the future home for a process-wide pacing scheduler. Linux timerfd/epoll mechanics must remain outside the generic congestion-control library.

## Source/module boundaries

The current product is one process, but source responsibilities are separate:

```text
host/       Linux TUN/interface/netfilter/lifecycle integration
runtime/    event loop and process lifecycle
lwip/       L3/TCP integration and lwIP-specific adapters
bridge/     public-stream <-> loopback-backend forwarding
cc/         platform-independent congestion-control policy core
```

A later privileged helper may own TUN/netfilter setup and pass a TUN fd to an unprivileged runtime. That is a security-hardening boundary, not a congestion-control requirement.

## L3 adapter and IPv6 model

The lwIP netif is a pure L3 adapter with MTU 1500. IPv4 and IPv6 share the same TUN/event-loop path. Static internal addressing is used; DHCPv6, SLAAC, router solicitation, MLD, ND6 packet queueing, RA MTU updates, endpoint IPv6 fragmentation, and reassembly are disabled in the low-memory profile.

The TUN TX queue is mechanically bounded to 64 packets / 96 KiB. On `EAGAIN` it holds a pbuf reference to the whole packet; queue exhaustion returns `ERR_MEM`. Packet splitting is not allowed.

A pure L3 output path bypasses lwIP's Ethernet next-hop code, so tcp-shift seeds/refreshes lwIP's existing fixed IPv6 destination cache for unicast output. This lets upstream ICMPv6 PTB processing update PMTU and lets upstream TCP MSS calculation consume that learned PMTU without introducing a second PMTU table or NDP state machine.

Runner evidence proves a routed 1500 -> 1280 PMTU change and SYN-ACK MSS change 1440 -> 1220.

## Bridge data model

`src/bridge/bridge.*` accepts lwIP public TCP streams and opens one nonblocking `AF_INET` socket to `127.0.0.1:<backend-port>` per flow. IPv4 and IPv6 listeners use the same bridge state machine.

The bridge does not maintain fixed application-data buffers in both directions.

Public -> backend data stays in lwIP-delivered pbufs until backend `writev()` commits bytes. `tcp_recved()` advances only for committed bytes. Host-socket `EAGAIN` therefore leaves pressure in the lwIP receive window instead of an unbounded userspace queue.

Backend -> public uses a small stack scratch buffer plus `MSG_PEEK`. Host bytes are consumed only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes. When lwIP send memory is full, backend readable interest is suppressed until `tcp_sent`/`tcp_poll` signals progress.

Backend sockets request 16-KiB send/receive buffers; Linux currently reports 32 KiB each on the runner. Half-close is directional. Once backend EOF is known, level-triggered `EPOLLRDHUP` is not left armed without useful work, preventing readiness spin.

Flow failure is isolated. Backend refusal/reset or public reset tears down only that flow. `bridge_stop()` explicitly aborts active flows and clears pending pbuf residency before process exit.

## Congestion-control policy boundary

P4 establishes `src/cc/` as a separately buildable pure-C library. It must not depend on lwIP objects, Linux/POSIX APIs, TUN, epoll, timerfd, nftables, bridge objects, or process lifecycle. Controller state is caller-owned and the core has no controller-owned heap allocation.

The generic transport observation currently includes:

- MSS;
- bytes in flight;
- peer send window;
- transport-representable cwnd limit.

The event surface currently includes init, ACK, loss, and retransmission timeout. Policy output includes cwnd, ssthresh, and optional pacing rate in bytes/second. The conventional Reno baseline publishes zero pacing rate and uses 16 bytes of caller-owned state.

`cwnd_limit_bytes` is a transport capability, not an lwIP-specific field. It prevents 32-bit controller state from silently diverging from the current unscaled 16-bit lwIP `tcpwnd_size_t`.

The standalone CC archive is built with `-ffreestanding -fno-builtin`; its include surface is allowlisted and the archive must have zero undefined external symbols.

## lwIP CC adapter and patch boundary

Pinned lwIP remains at commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. tcp-shift does not vendor a broad lwIP fork. `scripts/fetch-lwip.sh` records pristine critical-source hashes, then applies the repository-owned `patches/lwip-p4-cc-hooks.patch`.

The controlled patch changes exactly three upstream policy sites:

1. ACK cwnd growth in `tcp_in.c`;
2. fast-retransmit loss cwnd/ssthresh policy in `tcp_out.c`;
3. RTO cwnd/ssthresh reduction in `tcp.c`.

Unbound PCBs execute the native upstream policy path. The patch does not move retransmission execution, duplicate-ACK processing, fast-recovery flags/inflation/deflation, SACK/recovery, RTT/RTO calculation, segment queues, sequence-space management, packet construction, or `tcp_output()` out of lwIP.

One PCB ext-arg slot stores a small tcp-shift hook pointer. `src/lwip/cc_adapter.c` translates lwIP PCB state/events into generic observations and applies returned cwnd/ssthresh policy. The adapter may depend on lwIP; `src/cc/` may not.

Passive-open timing is explicit: pinned lwIP calls the accept callback before assigning `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter initializes the controller with the same pinned-lwIP initial-cwnd formula so the generic invariant `initial_cwnd >= MSS` remains intact. lwIP writes the same value immediately after the callback.

Controller state allocated for an accepted public PCB follows PCB lifetime through the ext-arg destroy callback. A bind failure rejects the child rather than silently falling back to native policy, preventing false-positive bridge tests.

## Qualified P4 ownership

Final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` proves integrated ownership:

- P2 requires `cc_bindings == bridge_accepts`, `cc_ack_events > 0`, `cc_bind_failures = 0`, and `cc_controller_errors = 0` across all bridge workloads;
- external fast-loss injection completes a 262144-byte stream with `cc_loss_events=1` and `cc_timeout_events=0`;
- external RTO injection completes the same-sized stream with `cc_timeout_events=2`;
- no test calls generic loss/timeout handlers directly to manufacture those events.

This means tcp-shift policy owns public-side base cwnd/ssthresh decisions for ACK, fast loss, and RTO, while lwIP still owns recovery mechanics.

## Provenance contract

The lwIP provenance workflow now proves both the upstream pin and the controlled modification:

- `HEAD` equals `.lwip-baseline` exactly;
- pristine critical-source hashes are retained before patching;
- patch path and SHA256 are retained;
- only `tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- reverse-apply check succeeds;
- an independent worktree created from the same pinned commit, after applying the repository patch, is byte-identical for all three modified files.

This is the permitted upstream modification surface. Additional lwIP patches require explicit architecture review and new provenance evidence.

## Memory/CPU planning boundary

P3 is the pre-CC baseline, not a full-host guarantee. It measures tcp-shift process PSS separately from backend Linux TCP/kernel memory and backend application memory.

Original P3 admission values were approximately:

```text
warm fixed process PSS: 315 KiB
fully-window-resident process slope: 37.523438 KiB/flow
32-MiB host / 25% process budget: 8192 KiB
128-active projected PSS: 5118 KiB
remaining process budget: 3074 KiB ~= 24.0 KiB/flow
```

With the integrated P4 adapter, final P3 rerun `34815149825` retained:

```text
warm fixed process PSS: 335 KiB
conservative idle slope: 0.523438 KiB/flow
controlled active payload delta: 36.625 KiB/flow
fully-window-resident process slope: 37.148438 KiB/flow
128-active projected PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
three-round 128-flow drain growth: 5 KiB
maximum warm drain floor above ready: 69 KiB
```

The adapter therefore adds a small fixed cost but does not consume the active-flow admission headroom. Window/pbuf/send-segment residency still dominates active memory.

P5/P6 must report incremental fixed, per-flow, and per-segment memory against this baseline. The remaining process budget cannot be treated as available host RAM because kernel/backend/provider residency is excluded.

## Qualification state

Runner-qualified milestones:

- P0: constrained lwIP build/config/source surface;
- P1: IPv4/IPv6 L3 TUN, product-owned ingress lifecycle, PMTU;
- P2: dual-stack public stream bridge to `127.0.0.1`, backpressure and lifecycle;
- P3: process memory/CPU/capacity baseline;
- P4: generic pure-C CC boundary plus real lwIP ACK/loss/RTO integration.

Final P4 behavior-head runs:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

This is GitHub-runner qualification, not provider/OpenVZ qualification. Provider qualification must separately prove TUN, capabilities, nftables/conntrack, forwarding, timing, and memory behavior on the target VPS class.

## P5: delivery-rate sampling and pacing prerequisites

P5 is next. It must add the information and scheduling primitives needed by BBR without moving Linux runtime dependencies into `src/cc/`.

Required capabilities include:

- high-resolution monotonic send/ACK timestamps;
- cumulative delivered-byte accounting;
- per-segment metadata sufficient to reconstruct delivery intervals;
- ACK delivery-rate samples;
- app-limited detection/marking;
- loss/inflight observations suitable for later model-based controllers;
- a process-wide pacing queue/scheduler with one timerfd rather than one timer per flow;
- mechanical memory/CPU accounting for fixed, per-flow, and per-segment additions.

The sampler should publish transport-neutral observations to controllers. The runtime owns timerfd/epoll pacing. BBR-specific state/modes are deferred until these primitives are independently qualified.

## Stop criteria

Stop and reassess before BBR if any of the following becomes necessary:

- rebuilding lwIP retransmission, SACK, or recovery machinery in project code;
- a large or hard-to-rebase permanent lwIP fork;
- memory approaching the hosted-Linux alternative for the target connection counts;
- unavoidable BDP/window buffers dominating constrained-host RAM;
- per-flow timers or scheduler structures that violate the single-owner/low-fixed-cost model.
