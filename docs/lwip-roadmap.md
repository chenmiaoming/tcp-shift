# lwIP route: architecture and milestones

This roadmap defines implementation order and exit evidence. `ARCHITECTURE.md` is the current product source of truth; this file explains how the project gets there.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. Fixed memory cost is the first optimization target.

The target shape is:

```text
public IPv4/IPv6 packet -> host routing/netfilter -> L3 TUN -> lwIP TCP
                                                        -> raw TCP callbacks
                                                        -> single-owner bridge
                                                        -> 127.0.0.1 backend
```

The public and backend TCP legs remain distinct. Congestion control belongs to the lwIP public leg. The public side is dual-stack/IPv6-only capable; the initial backend side remains IPv4 loopback only.

## Hard boundaries

- L3 TUN, not TAP/Ethernet, for the current product path.
- One mutable owner for lwIP state; no per-flow forwarding threads.
- `NO_SYS=1`; no lwIP TCP/IP thread, socket API, or netconn API.
- IPv6-only operation is a product requirement, not an optional compatibility add-on.
- Linux host integration, lwIP transport integration, bridge logic, and congestion-control policy remain separate source modules.
- The future CC core is pure C and independently buildable; library separation does not imply process separation.
- Do not patch congestion control before packet path, bridge, shutdown behavior, and memory accounting are observable.
- Do not call an experimental controller "Linux BBR" merely because its state names resemble Linux BBR.

## P0: reproducible lwIP userspace build — complete

Exit evidence includes exact upstream pinning, clean reproducible fetch, an explicit TCP/dual-stack `NO_SYS=1` source allowlist, configuration/source/binary/RSS gates, and clean-runner artifact smoke.

The initial Linux baseline intentionally uses libc allocation (`MEM_LIBC_MALLOC` and `MEMP_MEM_MALLOC`) so host RSS reflects demand.

## P1a: IPv4 L3 TUN and ingress lifecycle — complete

The IPv4 implementation uses one nonblocking exclusive `IFF_TUN | IFF_NO_PI` fd:

```text
RX: TUN read -> packet pbuf -> ip4_input -> TCP/ICMP
TX: lwIP ip4 output -> netif output -> whole-packet TUN write
```

The event loop integrates `sys_timeouts_sleeptime()` / `sys_check_timeouts()` rather than polling on a fixed timer. Writable interest is armed only after backpressure and the whole-packet retry queue is bounded to 64 packets / 96 KiB.

The host lifecycle owns TUN configuration and one exact-match IPv4 nftables DNAT table. The nft batch is checked read-only before mutation, installed atomically with exclusive table creation, and removed before TUN teardown. Existing resources are rejected rather than adopted. Global IPv4 forwarding and broad host forwarding policy remain operator-managed prerequisites.

Run `34744304038` qualified ICMP/TCP, DNAT/conntrack, idle wakeups, backpressure, MTU, oversize RX, and checksum behavior. Run `34763055040` qualified product-owned IPv4 ingress lifecycle.

## P1b: IPv6 L3 TUN and ingress lifecycle — complete

P1b extends the same adapter/event loop/lifecycle rather than creating a second runtime. The constrained lwIP profile enables the IPv6/IP6/ICMP6/ND6 core while keeping DHCPv6, SLAAC, Router Solicitation, MLD, ND6 packet queueing, RA MTU updates, IPv6 endpoint fragmentation, and IPv6 reassembly disabled.

The L3 adapter dispatches RX by IP version and sends IPv6 through the same bounded whole-packet writer. Static host/lwIP IPv6 addresses qualify direct ICMPv6 and raw-TCP ownership. Product ingress uses an exclusive `ip6` nftables table with exact public IPv6 + TCP-port DNAT into an internal static IPv6 TUN address. IPv6 forwarding is operator-managed and only diagnosed by tcp-shift.

Extension-header safety is a behavior gate: CI sends a TCP SYN carrying a Hop-by-Hop extension header and requires it to traverse the exact ingress rule to the TUN. It does not infer safety from nft's canonical printed form.

Pure L3 output bypasses the Ethernet ND next-hop path that normally creates lwIP destination-cache entries. The adapter therefore seeds/refreshes lwIP's existing fixed ND6 destination cache for unicast IPv6 output, with no separate PMTU allocation or neighbor-discovery traffic. This allows native ICMPv6 PTB handling to update PMTU and native TCP MSS calculation to consume it.

Behavior head `6a9d82feb1f3faead580f560ae1d076999a64b0f` passed P0 and the full P1 workflow. Run `34767386662` retained:

```text
ipv6_forwarding_disabled_preflight=ok
ipv6_exclusive_collision_rejection=ok
ipv6_extension_header_dnat=ok
product IPv6 DNAT connected [2001:db8:101::1]:18083
ipv6_signal_cleanup=ok unrelated_ruleset_unchanged=ok
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
P1b routed IPv6 Packet Too Big/PMTU qualification passed
```

P1 as a whole is therefore runner-qualified for both address families without regressing the IPv4 gates. Provider/OpenVZ capability qualification remains a separate deployment task.

## P2: dual-stack TCP listener and backend bridge — complete

P2 replaces the P1 probe listener with the real bridge. Public IPv4 and IPv6 both use lwIP raw TCP callbacks and the same address-family-independent flow state machine. Every accepted public flow connects through an ordinary nonblocking `AF_INET` host socket to `127.0.0.1:<backend-port>`.

The bridge adds no fixed application-data direction buffer. Public-to-backend data remains in lwIP pbufs until `writev()` commits it, and `tcp_recved()` advances only for committed bytes. Backend-to-public uses bounded stack scratch plus `MSG_PEEK`, consuming host-socket bytes only after `tcp_write(..., TCP_WRITE_FLAG_COPY)` accepts them. Backend readable interest is suppressed while lwIP send memory is blocked.

P2 also qualifies EOF/half-close ordering, backend refusal, public/backend reset recovery, explicit shutdown with active flows, and simultaneous-flow teardown. A discovered level-triggered `EPOLLRDHUP` spin hazard was fixed by removing backend watchers when no useful read/write interest remains and re-arming them only when progress requires it.

Behavior head `601a49648610513d98173e3e3add722326591ffc` passed P0 run `34769960299`, full P1 run `34769960302`, and P2 run `34769960275`. Retained P2 evidence includes:

```text
# blocked-peer 1 MiB gate
bridge_peak_pending_public_bytes=32768
bridge_backend_write_blocked_events=172
bridge_backend_read_blocked_events=357
bridge_backend_socket_sndbuf_bytes=32768
bridge_backend_socket_rcvbuf_bytes=32768

# eight simultaneous flows
bridge_accepts=8
bridge_backend_connects=8
bridge_peak_active_flows=8
bridge_active_flows=0

# active-flow process shutdown
pre_stop_active_flows=1
shutdown_bridge_active_flows=0
shutdown_bridge_pending_public_bytes=0

# 64 sequential reuse flows
rss_warmup_kb=1800
rss_mid_kb=1800
rss_final_kb=1800
bridge_reuse_no_ratcheting=ok
```

The reuse RSS result is a lifecycle/no-ratcheting gate, not a connection-capacity claim. Exact established-flow memory cost is intentionally measured in P3.

## P3: memory/capacity baseline — active next milestone

Measure idle RSS/PSS/private dirty, incremental memory per idle established connection, active-flow residency at fixed in-flight data, peak/post-drain floors, and CPU under idle/small-packet loads. Test stages are chosen for 32/64/128-MiB target hosts.

P3 should distinguish at least four memory contributors instead of collapsing them into one RSS number:

1. fixed process/lwIP/runtime cost;
2. per-public-flow lwIP PCB/segment/pbuf/control cost;
3. per-backend-flow Linux socket/kernel cost where observable from the host;
4. application data residency caused by configured TCP windows and controlled inflight traffic.

The first P3 increment should establish a long-lived measurement harness with `/proc/<pid>/smaps_rollup` plus process RSS, explicit connection-count stages, a stable idle-established state, and repeated load/drain rounds. Capacity thresholds must come from those measurements before being promoted to hard product limits.

## P4: generic congestion-control library boundary

Introduce an independently buildable pure-C CC interface instead of scattering policy through lwIP TCP code. The core consumes transport events/samples and emits cwnd/pacing policy. It does not call Linux host APIs and it does not own the pacing scheduler.

The lwIP adapter may require small, explicit additions to `tcp_pcb` and transmitted-segment metadata. Keep that patch surface mechanically testable. Validate the interface with a conventional controller before BBR.

## P5: delivery-rate sampler and pacer

Add high-resolution monotonic timestamps, delivered-byte accounting, per-segment delivery snapshots/send timestamps, ACK-derived delivery-rate samples, latest RTT samples, app-limited detection, and loss/inflight accounting.

The Linux runtime pacer is one process-wide scheduler, preferably min-heap plus timerfd, keyed by each flow's next eligible send time. The generic controller sees pacing rate, not timerfd.

## P6: experimental BBR

Reference order: current IETF BBR specification, Google QUICHE, ns-3 `TcpBbr`, Picoquic, then Linux `tcp_bbr.c`/`tcp_rate.c` as TCP behavior cross-checks.

Validation compares cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, loss response, throughput, retransmission behavior, CPU, and memory against native Linux baselines under reproducible RTT/loss/bandwidth scenarios.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, or unavoidable BDP buffering dominates the fixed-memory advantage.

The project is still successful if the result is a very small conventional-CC userspace TCP endpoint and BBR is rejected by evidence.
