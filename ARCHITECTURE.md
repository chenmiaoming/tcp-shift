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

The public and backend TCP connections are distinct. Public-side congestion control belongs to tcp-shift/lwIP. Backend loopback congestion control is outside the product contract. Public IPv6 therefore does not require an IPv6-capable application backend.

The deployment target includes small 32/64/128-MiB systems and IPv6-only low-cost VPSes. Memory, idle CPU, cleanup behavior, forwarding prerequisites, firewall ownership, and wakeup behavior are product properties.

## Host and packet-path ownership

The production path is routed L3 TUN, not TAP/Ethernet. The outer Linux stack continues to own physical ARP/NDP. TUN carries complete IPv4/IPv6 packets only.

The product-owned public ingress resource is narrow DNAT/conntrack state. IPv4 uses an exact public destination/port match. IPv6 uses exact destination plus extension-header-safe TCP classification. tcp-shift does not enable broad host forwarding, SNAT/masquerade, or unrelated firewall policy.

`net.ipv4.ip_forward` and `net.ipv6.conf.all.forwarding` are read-only prerequisites. Setup fails before mutation when the selected family's forwarding prerequisite is disabled.

TUN interfaces are nonpersistent. nftables ownership is exclusive: a pre-existing product table is a collision, never an adopted resource. Cleanup deletes only state recorded as owned by the current process.

## Single-owner event-driven runtime

lwIP runs with `NO_SYS=1`. There is no lwIP socket API, netconn layer, `tcpip_thread`, or per-flow forwarding thread. One event loop owns all mutable lwIP state.

The event loop is readiness/deadline driven:

- `epoll_wait()` blocks until fd readiness or the next lwIP timeout;
- `sys_check_timeouts()` runs after a real readiness/timeout wakeup;
- TUN `EPOLLOUT` is armed only while the bounded complete-packet TX queue is non-empty;
- backend read/write interest is removed or suppressed when a flow cannot make progress;
- one ready fd is consumed per `epoll_wait` call so callbacks may safely unregister/free their watcher;
- app-limited marking is triggered by the actual backend read path reaching `EAGAIN`, never by a periodic flow scan;
- pacing uses one process-wide deadline heap and one one-shot `CLOCK_MONOTONIC` timerfd in the same epoll owner;
- the pacing timer is armed to the earliest pending deadline and disarmed when the heap is empty;
- no fixed 1-ms/10-ms pacing tick, per-flow timerfd/thread, flow scan, or busy spin exists.

P5b's application-pause gate observed zero runtime CPU ticks during its pause sample. P5c's sustained high-BDP/window-pressure gate used one 100-Hz CPU tick over about 15 seconds while producing real pacing wakeups.

A later cleanup may unify lwIP and pacing deadlines behind one absolute monotonic timerfd only if CI proves fewer/equal wakeups without changing lwIP timeout semantics. That is an optimization, not a prerequisite.

## Source/module boundaries

The current product is one process, but source responsibilities remain separate:

```text
host/       Linux TUN/interface/netfilter/lifecycle integration
runtime/    epoll owner, timers, process-wide pacing scheduler, lifecycle
lwip/       L3/TCP integration, PMTU, CC/delivery/pacing adapter
bridge/     public-stream <-> loopback-backend forwarding
cc/         platform-independent congestion-control policy core
```

`src/cc/` is separately buildable pure C. It must not depend on lwIP objects, Linux/POSIX APIs, TUN, epoll, timerfd, nftables, bridge objects, or process lifecycle. Controller state is caller-owned and the core has no controller-owned heap allocation.

A later privileged helper may own TUN/netfilter setup and pass a TUN fd to an unprivileged runtime. That is a security-hardening boundary, not a congestion-control requirement.

## L3 adapter and IPv6 model

The lwIP netif is a pure L3 adapter with MTU 1500. IPv4 and IPv6 share the same TUN/event-loop path. Static internal addressing is used; DHCPv6, SLAAC, router solicitation, MLD, ND6 packet queueing, RA MTU updates, endpoint IPv6 fragmentation, and reassembly are disabled in the low-memory profile.

The TUN TX queue is mechanically bounded to 64 packets / 96 KiB. On `EAGAIN` it holds a pbuf reference to the whole packet; queue exhaustion returns `ERR_MEM`. Packet splitting is not allowed.

A pure L3 output path bypasses lwIP's Ethernet next-hop code, so tcp-shift seeds/refreshes lwIP's existing fixed IPv6 destination cache for unicast output. Upstream ICMPv6 PTB processing can therefore update PMTU and upstream TCP MSS calculation can consume the learned value without a second PMTU table or NDP state machine.

Runner evidence proves a routed 1500 -> 1280 PMTU change and SYN-ACK MSS change 1440 -> 1220.

## Bridge data model

`src/bridge/bridge.*` accepts lwIP public TCP streams and opens one nonblocking `AF_INET` socket to `127.0.0.1:<backend-port>` per flow. IPv4 and IPv6 listeners use the same bridge state machine.

Public -> backend data stays in lwIP-delivered pbufs until backend `writev()` commits bytes. `tcp_recved()` advances only for committed bytes. Host-socket `EAGAIN` therefore leaves pressure in the lwIP receive window instead of an unbounded userspace queue.

Backend -> public uses a small stack scratch buffer plus `MSG_PEEK`. Host bytes are consumed only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts the same bytes. When lwIP send memory is full, backend readable interest is suppressed until `tcp_sent`/`tcp_poll` signals progress. When `MSG_PEEK` reaches `EAGAIN` while public TCP still has capacity, the bridge emits an app-limited observation; the adapter performs the final transport-state checks.

Backend sockets request 16-KiB send/receive buffers; Linux currently reports 32 KiB each on the runner. Half-close is directional. Once backend EOF is known, level-triggered `EPOLLRDHUP` is not left armed without useful work, preventing readiness spin.

Flow failure is isolated. Backend refusal/reset or public reset tears down only that flow. `bridge_stop()` explicitly aborts active flows and clears pending pbuf residency before process exit.

## Congestion-control policy boundary

The generic controller consumes transport-neutral MSS, bytes in flight, peer send window, and transport-representable cwnd limit. Events are init, ACK, loss, and retransmission timeout. Policy output contains cwnd, ssthresh, and optional pacing rate in bytes/second.

Conventional Reno uses 16 bytes of caller-owned state and publishes zero pacing rate. `cwnd_limit_bytes` is a transport capability, not an lwIP-specific field. Upstream lwIP window scaling is enabled, so `tcpwnd_size_t` is 32-bit and the adapter exposes the 32-bit representational limit to controllers; this is deliberately separate from how many payload bytes the current memory profile permits to be queued.

P5 extends the ACK observation with a transport-neutral delivery-rate sample: selected bytes/second, sampling interval, send interval, ACK interval, RTT when valid, newly delivered payload bytes, prior inflight, and VALID / APP_LIMITED / RETRANSMITTED / RTT_VALID flags. Reno deliberately ignores this sample and continues to use `acked_bytes` only.

The standalone CC archive is built with `-ffreestanding -fno-builtin`; its include surface is allowlisted and it must have zero undefined external symbols.

## Controlled lwIP integration surface

Pinned lwIP remains at:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

`tcp-shift` does not vendor a broad lwIP fork. `scripts/fetch-lwip.sh` records pristine critical-source hashes and then applies the controlled patch chain: `patches/lwip-p4-cc-hooks.patch` followed by `patches/lwip-sack-recovery.patch`. The second patch is compile-time dormant unless `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY=ON`; the build definition is propagated through the lwIP target because enabling upstream SACK changes the public `struct tcp_pcb` layout.

The permitted upstream modification surface remains exactly:

- `src/core/tcp.c`;
- `src/core/tcp_in.c`;
- `src/core/tcp_out.c`.

P4 delegates ACK/loss/RTO base congestion policy. P5 adds send/ACK observations and the narrow data-send eligibility hook in the already-controlled surface. P5b exposes host-order segment sequence information to project sidecar accounting without exposing private `tcp_seg` layout to the generic controller. P5c gates eligible data sends but resumes through native `tcp_output()`. P6 keeps that same ownership split: internal BBR may own its recovery cwnd policy, while lwIP still owns retransmission execution and sequence-space recovery. PR #35 adds bounded RFC 6582/NewReno partial-ACK continuation inside the existing `tcp_in.c` patch surface rather than moving recovery into `src/cc/`.

Window scaling uses upstream lwIP configuration rather than any additional source patch. The qualified low-memory profile sets `LWIP_WND_SCALE=1` and `TCP_RCV_SCALE=0`: sender-side window/cwnd accounting is 32-bit, while the local receive window and send-buffer profile remain 32 KiB at this checkpoint. Real SYN/SYN-ACK qualification observes a peer scale offer and an lwIP `wscale 0` response. Increasing sender buffering and local receive-window residency are separate memory-qualified milestones.

Unbound PCBs execute native upstream behavior. Project hooks do not move retransmission execution, duplicate-ACK processing, fast recovery, SACK/recovery, RTT/RTO calculation, segment queues, sequence-space management, packet construction, or `tcp_output()` out of lwIP.

One PCB ext-arg slot stores the tcp-shift hook pointer. `src/lwip/cc_adapter.c` translates lwIP state/events into generic observations, applies policy, and registers generation-safe pacing identity. Adapter/controller state follows PCB lifetime through the ext-arg destroy callback.

Pinned lwIP requires a non-NULL ext-arg callback table. Manual adapter unbind therefore clears only the ext-arg data pointer while retaining the static callback table; later PCB destruction invokes the callback with NULL data, which is a no-op. This behavior is covered by the deterministic P5c lifecycle contract.

Passive-open timing is explicit: pinned lwIP calls the accept callback before assigning `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter initializes the controller with the same pinned-lwIP formula so `initial_cwnd >= MSS` remains true; lwIP publishes the same value immediately after the callback.

A bind failure rejects the child rather than silently falling back to native policy, preventing false-positive bridge tests.

## Delivery ledger and rate sampling

P5 keeps delivery metadata outside upstream `struct tcp_seg` in a lazy sidecar keyed by stable `tcp_seg *`, initially 8 slots and bounded by `TCP_SND_QUEUELEN`.

Current sidecar entries are 56 bytes and retain segment identity, host-order payload sequence/progress, first-transmit/send-phase timestamps, delivered snapshots, prior inflight, and app-limited/retransmission flags.

Pinned lwIP updates `pcb->lastack` before the project ACK policy hook and frees acknowledged segments afterwards. The adapter therefore computes exact newly delivered payload from cumulative ACK overlap while relevant sidecars are still live. FIN sequence space is excluded. Retransmission reuses the same sidecar and cannot duplicate delivered bytes.

Rate sampling derives send-phase and ACK-phase intervals and selects the larger interval to resist ACK compression. Retransmitted candidates do not publish RTT.

App-limited state is entered from the bridge's real `EAGAIN` observation only after the adapter confirms no unsent data and available public transport capacity. Delivery past the delivered+inflight marker clears the state.

## Event-driven pacing architecture

P5c is runner-qualified. A nonzero controller pacing rate becomes actual data-send eligibility through this path:

```text
controller pacing policy
        |
        v
lwIP CC adapter
        |
        v
data-only eligibility hook inside native tcp_output() loop
        |
        v
process-wide min-heap: (deadline, flow_id, generation, bytes)
        |
        v
one one-shot CLOCK_MONOTONIC timerfd
        |
        v
existing epoll owner
        |
        v
generation lookup -> native tcp_output() resume
```

SYN/FIN/control traffic is not held behind the data pacing gate. A deferred segment remains in lwIP's native unsent queue; tcp-shift does not copy or re-own it.

A successful data transmission advances the next eligible-send deadline from payload bytes and the current policy rate. This observation occurs before first-send/retransmit classification, so fast retransmissions and RTO retransmissions remain paced while the delivery ledger still marks them as retransmitted.

Queued scheduler entries never store flow/PCB pointers. Teardown cancels by `(flow_id,generation)` and unregisters the generation. A late release is counted stale and cannot dereference freed flow state.

Production Reno publishes zero pacing rate and bypasses scheduler deferral. The qualification-only fixed-paced Reno publishes `65536 B/s` through the same generic policy path; there is no runtime force-rate hook.

## Resource planning boundary

P3 measures tcp-shift process PSS separately from backend Linux TCP/kernel state and backend application memory. It is not a full-host guarantee.

Latest P5c-head measurements:

```text
warm fixed process PSS:           367 KiB
conservative active slope:        37.773438 KiB/flow
128-active projected PSS:        5202 KiB
8-MiB process budget remaining:  2990 KiB = 23.359 KiB/flow
3x128 drain growth:              5 KiB
idle CPU:                        0 ticks/s
```

High-BDP pacing additionally consumed `0.01 s` runtime CPU over `15.067 s` wall time (`0.066%`) with no loss/RTO/scheduler/stale error.

P6 must continue reporting incremental fixed, per-flow, per-segment, and model-state memory. The retained process budget cannot be treated as free host RAM.

## Current qualification state

Runner-qualified:

- P0: constrained lwIP build/config/source surface, including upstream window scaling with 32-bit `tcpwnd_size_t` and a low-memory `TCP_RCV_SCALE=0` profile;
- P1: IPv4/IPv6 L3 TUN, ingress lifecycle, PMTU;
- P2: dual-stack public stream bridge to `127.0.0.1`, backpressure/lifecycle;
- P3: process memory/CPU/capacity baseline;
- P4: generic pure-C CC boundary plus real ACK/loss/RTO integration;
- P5a: high-resolution delivery ledger and retransmission-safe metadata;
- P5b: ACK delivery-rate sampling and event-driven app-limited classification;
- P5c: one process-wide event-driven pacer, deterministic teardown safety, multi-flow scheduling, paced loss/RTO recovery, and high-BDP/window-pressure qualification;
- P6: compact internal BBR runtime with live cwnd/pacing publication, clean Linux BBR reference comparison, multi-flow/app-limited qualification, explicit-loss ProbeBW semantics, NewReno recovery, bounded sender-SACK selective recovery, deterministic first-send-loss qualification, SACK-aware delivery/rate accounting, and SACK-aware effective-cwnd send gating.

Current merged behavior head:

```text
90e2c973cc5b72dc0a2ae9296b566fee1b7e3291
```

The sender-SACK extension remains compile-time experimental and OFF by default. When enabled for qualification, it still lives inside the transport-owned lwIP recovery surface: inbound SACK blocks mark existing outstanding segments, selective requeue remains bounded, and sequence space / queues / retransmission execution / RTO stay in lwIP. PR #45 lets the generic delivery sidecar charge newly SACKed out-of-order payload exactly once so internal BBR sees delivery progress before cumulative ACK repair. PR #49 closes the remaining accounting mismatch between that BBR inflight view and native `tcp_output()`: in the SACK experiment only, the hook supplies an effective cwnd that credits bytes already proven delivered by SACK while preserving the peer `snd_wnd` limit. The initial fast retransmit and later selective holes now use the same three-later-SACK loss proof, preventing the temporary SACK-block ambiguity exposed by the larger flight. Reno/CUBIC native policy and default SACK-OFF builds remain unchanged.

Production `tcp-shift-p2` still exposes only `reno|cubic`; `bbr` remains an internal qualification controller. The current product boundary is to qualify the experimental BBR + sender-SACK combination on the target provider/OpenVZ class and continue measurement-led work on the remaining deterministic-loss gap. Public `bbr` exposure remains a separate explicit decision after that evidence.

This is GitHub-runner qualification, not provider/OpenVZ qualification. Provider qualification must separately prove TUN, capabilities, nftables/conntrack, forwarding, timing, loss behavior, and memory on the target VPS class.

## Stop criteria

Stop and reassess if acceptable model-based behavior requires replacing most lwIP recovery/SACK machinery, the project accumulates a large hard-to-rebase lwIP fork, controller/sampler/pacer metadata approaches hosted-Linux memory cost, unavoidable BDP/window buffering dominates the fixed-memory advantage, or pacing/controller accuracy requires per-flow timers, periodic polling, or busy spinning.
