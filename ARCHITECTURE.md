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
- one ready fd is consumed per `epoll_wait` call so a callback may safely unregister/free its own watcher;
- P5b app-limited marking is triggered when the actual backend read path reaches `EAGAIN` while the public TCP still has transport capacity; there is no periodic flow scan.

The P5b application-pause gate observed zero runtime CPU ticks during a 300-ms measurement window while proving app-limited enter/sample/exit transitions.

P5c pacing must preserve this property. The scheduler target is one process-wide deadline heap plus one one-shot `CLOCK_MONOTONIC` timerfd registered in the same epoll owner. The timerfd is armed to the earliest pacing deadline and disarmed when no pacing work exists. There is no fixed 1-ms/10-ms pacing tick, no per-flow timerfd/thread, and no busy spin.

A later runtime cleanup may unify lwIP and pacing deadlines behind one absolute monotonic timerfd if CI proves fewer/equal wakeups without changing lwIP timeout behavior. That is an optimization, not a prerequisite for pacing correctness.

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

Public -> backend data stays in lwIP-delivered pbufs until backend `writev()` commits bytes. `tcp_recved()` advances only for committed bytes. Host-socket `EAGAIN` therefore leaves pressure in the lwIP receive window instead of an unbounded userspace queue.

Backend -> public uses a small stack scratch buffer plus `MSG_PEEK`. Host bytes are consumed only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes. When lwIP send memory is full, backend readable interest is suppressed until `tcp_sent`/`tcp_poll` signals progress. When `MSG_PEEK` reaches `EAGAIN` while the public TCP has space, the bridge emits an app-limited observation to the lwIP adapter; the adapter performs the final transport-state checks before setting a delivered+inflight marker.

Backend sockets request 16-KiB send/receive buffers; Linux currently reports 32 KiB each on the runner. Half-close is directional. Once backend EOF is known, level-triggered `EPOLLRDHUP` is not left armed without useful work, preventing readiness spin.

Flow failure is isolated. Backend refusal/reset or public reset tears down only that flow. `bridge_stop()` explicitly aborts active flows and clears pending pbuf residency before process exit.

## Congestion-control policy boundary

P4 establishes `src/cc/` as a separately buildable pure-C library. It must not depend on lwIP objects, Linux/POSIX APIs, TUN, epoll, timerfd, nftables, bridge objects, or process lifecycle. Controller state is caller-owned and the core has no controller-owned heap allocation.

The generic transport observation includes MSS, bytes in flight, peer send window, and the transport-representable cwnd limit. The event surface includes init, ACK, loss, and retransmission timeout. Policy output includes cwnd, ssthresh, and optional pacing rate in bytes/second. The conventional Reno baseline publishes zero pacing rate and uses 16 bytes of caller-owned state.

P5b extends the ACK observation with a transport-neutral delivery-rate sample: selected bytes/second, sampling interval, send interval, ACK interval, RTT when valid, newly delivered payload bytes, prior inflight, and validity/app-limited/retransmission flags. Reno deliberately ignores this sample and continues to use `acked_bytes` only.

`cwnd_limit_bytes` is a transport capability, not an lwIP-specific field. It prevents 32-bit controller state from silently diverging from the current unscaled 16-bit lwIP `tcpwnd_size_t`.

The standalone CC archive is built with `-ffreestanding -fno-builtin`; its include surface is allowlisted and the archive must have zero undefined external symbols.

## Controlled lwIP integration surface

Pinned lwIP remains at commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. tcp-shift does not vendor a broad lwIP fork. `scripts/fetch-lwip.sh` records pristine critical-source hashes, then applies the repository-owned `patches/lwip-p4-cc-hooks.patch`.

The permitted upstream modification surface remains exactly three files:

- `src/core/tcp.c`;
- `src/core/tcp_in.c`;
- `src/core/tcp_out.c`.

P4 inserted base congestion-policy delegation at ACK growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh sites. P5 adds send/ACK observations in the already-controlled `tcp_out.c` / `tcp_in.c` surface. P5b adds host-order segment sequence information to the successful-send observation so project sidecar state can account cumulative/partial ACK payload without exposing private `tcp_seg` layout to the generic controller. No new upstream file is patched.

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

P5a established retransmission-safe accounting without enlarging upstream `struct tcp_seg`. A project-owned lazy sidecar vector is keyed by stable `tcp_seg *`; it starts at 8 entries and is bounded by current `TCP_SND_QUEUELEN=90`.

At P5a each slot was 32 bytes and held segment identity plus first-transmit timestamp and delivery-state snapshots. Final behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38`, P5 run `34821205375`, job `103903019956`, artifact `10338108552` proved exact 262144-byte unique delivery across natural, fast-loss, and RTO paths with zero leaked slots or metadata errors.

The first P5 workflow version was not accepted even though GitHub marked it green: its parser matched `live_slots` inside `peak_live_slots`, and `check_delivery | tee` masked the checker's nonzero exit status. The final harness parses exact tokens and fails closed.

## P5b delivery-rate sampler + app-limited — qualified

P5b widens each lazy sidecar entry to 56 bytes so the adapter can retain segment sequence/progress, first-transmit/send-phase snapshots, delivered snapshots, prior inflight, and app-limited/retransmission state. It still does not enlarge upstream `struct tcp_seg`.

For a cumulative ACK, pinned lwIP updates `pcb->lastack` before the project ACK policy hook and frees acknowledged segments afterwards. The adapter therefore observes the new cumulative ACK while all matching sidecars are still live. It computes exact newly delivered payload from sequence overlap/progress, excluding FIN sequence space, then lets upstream free segments. This also defines partial-ACK behavior without moving queue ownership into project code.

The rate sample follows the same anti-ACK-compression principle used by mature TCP rate samplers: derive a send-phase interval and an ACK-phase interval, then use the larger interval. Per-segment snapshots carry app-limited and retransmission history. RTT is withheld for retransmitted candidates (Karn-style); retransmission does not duplicate delivered payload.

Final behavior head `327efe7e29adfc230e7d201b466f2bd4980e976c` passed provenance/P0/P1/P2/P3/P4/P5. P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained:

```text
normal:    samples=138 valid=138 invalid=0 max_rate=696998778 B/s
fast-loss: samples=121 valid=121 invalid=0 loss_events=1
RTO:       samples=135 valid=135 invalid=0 retransmitted_samples=3 timeout_events=2

app-pause:
delivered=135168 app_limited_samples=7
app_limited_enters=2 app_limited_exits=2
pause_cpu_ticks=0 event_driven=ok
```

The app-pause workload sends 4096 bytes, pauses the backend application for 0.8 s, then sends 131072 bytes. During a 300-ms window entirely inside the pause, the runtime consumed zero CPU ticks on the runner. This qualifies the app-limited path as readiness-driven rather than a periodic product scan.

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

P3 measures tcp-shift process PSS separately from backend Linux TCP/kernel state and backend application memory. It is not a full-host guarantee.

Representative evolution:

```text
                         P4         P5a        P5b
warm fixed PSS          335 KiB     343 KiB    347 KiB
active slope KiB/flow   37.148438   37.679688  37.710938
128-active projection   5090 KiB    5166 KiB   5174 KiB
8-MiB budget remaining  3102 KiB    3026 KiB   3018 KiB
headroom / active flow  24.234 KiB  23.641 KiB 23.578 KiB
```

The P5b P3 run was `34843587033`, job `103974029078`, artifact `10346739278`. Repeated 3x128-flow drain remained stable at 342 -> 346 -> 347 KiB PSS (first-to-last +5 KiB), and idle CPU remained 0 ticks/s. The process budget excludes backend kernel/application and provider residency.

P5c/P6 must continue reporting incremental fixed, per-flow, and per-segment memory. The remaining process budget cannot be treated as available host RAM.

## Current qualification state and next work

Runner-qualified:

- P0: constrained lwIP build/config/source surface;
- P1: IPv4/IPv6 L3 TUN, ingress lifecycle, PMTU;
- P2: dual-stack public stream bridge to `127.0.0.1`, backpressure/lifecycle;
- P3: process memory/CPU/capacity baseline;
- P4: generic pure-C CC boundary plus real lwIP ACK/loss/RTO integration;
- P5a: high-resolution delivery ledger and retransmission-safe segment metadata;
- P5b: delivery-rate sampling, retransmission/RTT metadata, exact payload accounting, and event-driven app-limited classification.

Next:

- P5c: one process-wide event-driven pacer and integrated pacing/sample publication;
- P6: tcp-shift BBR state/model implementation and native-Linux reference comparison.

This is GitHub-runner qualification, not provider/OpenVZ qualification. Provider qualification must separately prove TUN, capabilities, nftables/conntrack, forwarding, timing, and memory behavior on the target VPS class.

## Stop criteria

Stop and reassess before BBR if any of the following becomes necessary:

- rebuilding lwIP retransmission, SACK, or recovery machinery in project code;
- a large or hard-to-rebase permanent lwIP fork;
- memory approaching the hosted-Linux alternative for the target connection counts;
- unavoidable BDP/window buffers dominating constrained-host RAM;
- per-flow timers, periodic pacing polling, or busy spinning for pacing accuracy.
