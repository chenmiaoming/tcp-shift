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

The deployment target includes small 32/64/128-MiB systems and IPv6-only low-cost VPSes. Memory, idle CPU, cleanup behavior, forwarding prerequisites, firewall ownership, and wakeup behavior are product properties.

## Packet path and host ownership

The production direction is routed L3 TUN, not TAP/Ethernet. The outer Linux stack continues to own physical ARP/NDP. The TUN carries complete IPv4/IPv6 packets only.

The product-owned public ingress resource is narrow DNAT/conntrack state. IPv4 uses an exact public destination/port match. IPv6 uses exact destination plus `meta l4proto tcp` so extension headers do not invalidate TCP classification. tcp-shift does not enable broad host forwarding, SNAT/masquerade, or unrelated firewall policy.

`net.ipv4.ip_forward` and `net.ipv6.conf.all.forwarding` are read-only prerequisites. Product setup fails before mutation when the selected family's forwarding prerequisite is disabled.

TUN interfaces are nonpersistent. nftables ownership is exclusive: a pre-existing product table is a collision, never an adopted resource. Cleanup deletes only state recorded as owned by the current process.

## Single-owner event-driven runtime

lwIP runs with `NO_SYS=1`. There is no lwIP socket API, netconn layer, `tcpip_thread`, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The event loop is readiness/deadline driven rather than fixed-polling:

- `epoll_wait()` blocks until an fd becomes ready or the next lwIP timeout returned by `sys_timeouts_sleeptime()` expires;
- `sys_check_timeouts()` runs after that readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only while the bounded complete-packet TX queue is non-empty;
- backend read/write interest is removed or suppressed when the flow cannot make progress;
- one ready fd is consumed per `epoll_wait` call so a callback may safely unregister/free its own watcher.

P5 pacing must preserve this property. The target scheduler is one process-wide deadline heap plus one one-shot `timerfd` registered in the same epoll owner. The timerfd is armed to the earliest pacing deadline and disarmed when no pacing work exists. There is no fixed 1-ms/10-ms pacing tick, no per-flow timerfd/thread, and no busy spin.

A later runtime cleanup may unify lwIP and pacing deadlines behind one absolute monotonic timerfd if CI proves that this reduces wakeups without changing lwIP timeout behavior. That is an optimization, not a prerequisite for P5b sampling.

## Source/module boundaries

The current product is one process, but source responsibilities are separate:

```text
host/       Linux TUN/interface/netfilter/lifecycle integration
runtime/    event loop, timer/pacing scheduling, process lifecycle
lwip/       L3/TCP integration, PMTU, CC and delivery adapters
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

The generic transport observation currently includes MSS, bytes in flight, peer send window, and the transport-representable cwnd limit. The event surface currently includes init, ACK, loss, and retransmission timeout. Policy output includes cwnd, ssthresh, and optional pacing rate in bytes/second. The conventional Reno baseline publishes zero pacing rate and uses 16 bytes of caller-owned state.

`cwnd_limit_bytes` is a transport capability, not an lwIP-specific field. It prevents 32-bit controller state from silently diverging from the current unscaled 16-bit lwIP `tcpwnd_size_t`.

The standalone CC archive is built with `-ffreestanding -fno-builtin`; its include surface is allowlisted and the archive must have zero undefined external symbols.

## Controlled lwIP integration surface

Pinned lwIP remains at commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. tcp-shift does not vendor a broad lwIP fork. `scripts/fetch-lwip.sh` records pristine critical-source hashes, then applies the repository-owned `patches/lwip-p4-cc-hooks.patch`.

The permitted upstream modification surface remains exactly three files:

- `src/core/tcp.c`;
- `src/core/tcp_in.c`;
- `src/core/tcp_out.c`.

P4 inserted base congestion-policy delegation at ACK growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh sites. P5a adds segment-send and fully-ACKed-segment observation calls in the already-controlled `tcp_out.c` / `tcp_in.c` surface. No new upstream file is patched.

Unbound PCBs execute native upstream congestion control and transport behavior. Project hooks do not move retransmission execution, duplicate-ACK processing, fast-recovery flags/inflation/deflation, SACK/recovery, RTT/RTO calculation, segment queues, sequence-space management, packet construction, or `tcp_output()` out of lwIP.

One PCB ext-arg slot stores a tcp-shift hook pointer. `src/lwip/cc_adapter.c` translates lwIP PCB state/events into project observations and applies returned policy. The adapter may depend on lwIP; `src/cc/` may not.

Passive-open timing is explicit: pinned lwIP calls the accept callback before assigning `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter initializes the controller with the same pinned-lwIP initial-cwnd formula so the generic invariant `initial_cwnd >= MSS` remains intact. lwIP writes the same value immediately after the callback.

Controller/sidecar state allocated for an accepted public PCB follows PCB lifetime through the ext-arg destroy callback. A bind failure rejects the child rather than silently falling back to native policy, preventing false-positive bridge tests.

## P4 ownership boundary — qualified

Final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` proves integrated ownership:

- P2 requires `cc_bindings == bridge_accepts`, `cc_ack_events > 0`, `cc_bind_failures = 0`, and `cc_controller_errors = 0` across all bridge workloads;
- external fast-loss injection completes a 262144-byte stream with `cc_loss_events=1` and `cc_timeout_events=0`;
- external RTO injection completes the same-sized stream with `cc_timeout_events=2`;
- no test calls generic loss/timeout handlers directly to manufacture those events.

Thus tcp-shift policy owns public-side base cwnd/ssthresh decisions for ACK, fast loss, and RTO, while lwIP still owns recovery mechanics.

## P5a delivery ledger — qualified

P5a establishes the retransmission-safe accounting required by later delivery-rate sampling without enlarging upstream `struct tcp_seg`.

Per bound public flow, `src/lwip/cc_adapter.*` owns a lazy growable sidecar vector keyed by the stable `tcp_seg *`. The vector starts at 8 entries and grows only when needed, bounded by current `TCP_SND_QUEUELEN=90`. Each entry is 32 bytes and contains segment identity plus first-transmit timestamp and delivery-state snapshots.

Lifecycle rules:

- create metadata only for a successful transmitted data segment;
- retransmission of the same `tcp_seg *` reuses the existing entry and increments retransmission diagnostics only;
- consume the entry when the fully acknowledged segment is about to be freed;
- cumulative delivered payload advances only once per unique acknowledged segment;
- PCB teardown releases any remaining sidecar allocation;
- zero live entries after normal flow teardown is a hard gate.

Final P5a behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed provenance/P0/P1/P2/P3/P4 and P5 run `34821205375`, job `103903019956`. Artifact `10338108552` retains the corrected fail-closed normal / fast-loss / RTO evidence:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
RTO:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

All three paths additionally require zero allocation failures, metadata misses, abandoned slots, clock errors, and timestamp regressions.

The first P5 workflow version was not accepted as qualification even though GitHub marked it green: its parser matched `live_slots` inside `peak_live_slots`, and `check_delivery | tee` masked the checker's nonzero exit status. The final harness uses exact key tokens and fail-closed summary generation before `cat`; CI also explicitly includes hidden `.build` diagnostics in retained artifacts.

## Provenance contract

The lwIP provenance workflow proves both the upstream pin and the controlled modification:

- `HEAD` equals `.lwip-baseline` exactly;
- pristine critical-source hashes are retained before patching;
- patch path and SHA256 are retained;
- only `tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- reverse-apply check succeeds;
- an independent worktree created from the same pinned commit, after applying the repository patch, is byte-identical for all modified files.

Additional lwIP files in the patch require explicit architecture review and new provenance evidence.

## Memory/CPU planning boundary

P3 measures tcp-shift process PSS separately from backend Linux TCP/kernel memory and backend application memory. It is not a full-host guarantee.

Final P4 adapter baseline:

```text
warm fixed process PSS: 335 KiB
fully-window-resident process slope: 37.148438 KiB/flow
128-active projected PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
```

P5a rerun:

```text
warm fixed process PSS: 343 KiB
fully-window-resident process slope: 37.679688 KiB/flow
128-active projected PSS: 5166 KiB
remaining 8-MiB process budget: 3026 KiB = 23.641 KiB/flow
```

P5a therefore consumes about 8 KiB fixed PSS and about 76 KiB of the conservative 128-flow process budget in the retained runner sample. Window/pbuf/send-segment residency still dominates active memory.

P5b/P5c/P6 must continue reporting incremental fixed, per-flow, and per-segment memory. The remaining process budget cannot be treated as available host RAM because kernel/backend/provider residency is excluded.

## Current qualification state and next work

Runner-qualified:

- P0: constrained lwIP build/config/source surface;
- P1: IPv4/IPv6 L3 TUN, ingress lifecycle, PMTU;
- P2: dual-stack public stream bridge to `127.0.0.1`, backpressure/lifecycle;
- P3: process memory/CPU/capacity baseline;
- P4: generic pure-C CC boundary plus real lwIP ACK/loss/RTO integration;
- P5a: high-resolution delivery ledger and retransmission-safe segment metadata.

Next:

- P5b: ACK-derived delivery-rate sample + app-limited semantics;
- P5c: event-driven process-wide pacer and integrated loss/inflight/sample publication;
- P6: tcp-shift BBR state/model implementation and native-Linux reference comparison.

This is GitHub-runner qualification, not provider/OpenVZ qualification. Provider qualification must separately prove TUN, capabilities, nftables/conntrack, forwarding, timing, and memory behavior on the target VPS class.

## Stop criteria

Stop and reassess before BBR if any of the following becomes necessary:

- rebuilding lwIP retransmission, SACK, or recovery machinery in project code;
- a large or hard-to-rebase permanent lwIP fork;
- memory approaching the hosted-Linux alternative for the target connection counts;
- unavoidable BDP/window buffers dominating constrained-host RAM;
- per-flow timers, periodic pacing polling, or busy spinning for pacing accuracy.
