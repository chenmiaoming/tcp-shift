# lwIP route: architecture and milestones

This roadmap defines implementation order and exit evidence. `ARCHITECTURE.md` is the current product source of truth.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. Fixed memory cost is the first optimization target.

```text
public IPv4/IPv6 packet -> host routing/netfilter -> L3 TUN -> lwIP TCP
                                                        -> raw TCP callbacks
                                                        -> single-owner bridge
                                                        -> 127.0.0.1 backend
```

The public and backend TCP legs are distinct. Congestion control belongs to the lwIP public leg. The public side is dual-stack/IPv6-only capable; the backend remains IPv4 loopback.

## Hard boundaries

- L3 TUN, not TAP/Ethernet.
- One mutable owner for lwIP; no per-flow forwarding threads.
- `NO_SYS=1`; no lwIP socket/netconn/tcpip-thread path.
- IPv6-only public operation is required.
- Linux host integration, lwIP transport integration, bridge logic, and congestion-control policy remain separate modules.
- `src/cc/` stays pure C and independently buildable.
- Linux timerfd/epoll pacing stays outside `src/cc/`.
- Pacing is deadline/event driven: no periodic pacing poll loop, no per-flow timerfd/thread, no busy spin.
- Do not call an experimental controller Linux BBR unless transport semantics actually match.

## P0: reproducible lwIP userspace build — complete

Exact upstream pinning, explicit dual-stack TCP `NO_SYS=1` source allowlist, configuration/source/binary/RSS gates, and clean-runner artifact smoke are qualified.

## P1: dual-stack L3 TUN and ingress lifecycle — complete

P1 qualifies one nonpersistent `IFF_TUN | IFF_NO_PI` L3 adapter, lwIP timer-driven epoll ownership, bounded whole-packet TX retry, nonfatal RX drops, direct IPv4/IPv6 ICMP/TCP, product-owned exact IPv4/IPv6 DNAT/conntrack lifecycle, forwarding prerequisite checks, collision/rollback/cleanup, extension-header-safe IPv6 TCP matching, and routed ICMPv6 PTB learning.

Representative retained PMTU evidence:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
```

Provider/OpenVZ capability qualification remains separate.

## P2: dual-stack TCP listener and backend bridge — complete

P2 replaces the probe listener with a real bridge. Public IPv4/IPv6 use the same flow state machine and connect to an ordinary nonblocking `127.0.0.1:<backend-port>` socket.

Public->backend data remains in lwIP pbufs until backend `writev()` commits it. Backend->public removes host bytes only after `tcp_write(..., COPY)` accepts them. The bridge therefore adds no fixed bidirectional application buffer and preserves bounded transport backpressure.

P2 qualifies IPv4/IPv6 stream integrity, real bidirectional blocking, half-close, backend refusal recovery, public/backend reset recovery, concurrent flows, active-flow shutdown, and long-lived reuse without RSS ratcheting.

## P3: memory/capacity baseline — complete

P3 separates tcp-shift process residency from backend Linux TCP/kernel state and measures idle flows, both active-window directions, repeated load/drain floors, and representative CPU.

Original admission baseline:

```text
warm fixed process PSS: 315 KiB
fully-window-resident slope: 37.523438 KiB/flow
32-MiB host / 25% tcp-shift process budget: 8192 KiB
128-active projected PSS: 5118 KiB
remaining process budget: 3074 KiB ~= 24.0 KiB/flow
```

This is a process-PSS planning model, not a full-host capacity guarantee. Backend kernel/application memory and provider overhead remain outside it.

## P4: generic CC boundary + lwIP adapter — complete

P4 is fully runner-qualified on behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac`.

`src/cc/` is independently buildable with caller-owned state, an allowlisted ISO-C include surface, `-ffreestanding -fno-builtin`, and zero unresolved archive symbols. The generic controller consumes MSS, inflight, peer send window, and a transport cwnd limit. Events are init/ACK/loss/RTO. Policy publishes cwnd, ssthresh, and optional pacing rate. The conventional Reno baseline uses 16 bytes and requests no pacing.

Pinned lwIP remains at `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. A repository-owned patch is confined to `tcp.c`, `tcp_in.c`, and `tcp_out.c`. P4 delegates ACK cwnd growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh while retransmission execution, fast recovery, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and output remain native lwIP mechanics.

External TUN fault injection qualifies:

```text
fast-loss: cc_loss_events=1 cc_timeout_events=0 payload_bytes=262144 recovery=ok
RTO:       cc_loss_events=0 cc_timeout_events=2 payload_bytes=262144 recovery=ok
```

## P5a: retransmission-safe delivery ledger — complete

P5a supplied the accounting substrate for rate estimation without changing controller behavior or adding pacing.

The project keeps metadata outside upstream `struct tcp_seg`, using a lazy per-flow sidecar keyed by stable `tcp_seg *`, initially 8 slots and bounded by `TCP_SND_QUEUELEN=90`. P5a slots were 32 bytes. Retransmission reuses a slot and cannot double-count delivered payload; teardown must leave zero live slots.

Final P5a behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed provenance/P0/P1/P2/P3/P4 and P5 run `34821205375`, job `103903019956`, artifact `10338108552`:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
RTO:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

The first P5 workflow exposed and then fixed a false-green harness bug: a parser matched `live_slots` inside `peak_live_slots`, and `check_delivery | tee` masked the checker exit status. Qualification was withheld until the checker became fail-closed.

## P5b: ACK delivery-rate sampler + app-limited — complete

P5b converts the P5a ledger into a transport-neutral ACK rate observation without changing conventional Reno policy.

The ACK observation now carries delivery rate, selected interval, send interval, ACK interval, RTT when valid, newly delivered payload, prior inflight, and validity/app-limited/retransmission flags. The lwIP adapter owns all segment/sequence/timestamp mechanics; `src/cc/` sees transport-neutral values only.

The lazy sidecar grew to 56 bytes to retain sequence/progress, send-phase snapshots, prior inflight, and app-limited/retransmission state. ACK delivery is calculated from cumulative sequence progress while acknowledged sidecars are still live, before pinned lwIP frees segments. This defines partial-ACK payload accounting without moving queue ownership into project code and excludes FIN sequence space from delivered payload.

Rate selection uses the larger of the send-phase and ACK-phase intervals to resist ACK compression. Retransmitted candidates carry a retransmission flag and do not publish an RTT sample; RTO retains unique delivery accounting.

App-limited detection is event-driven. The bridge invokes the marker only when its actual backend read path reaches `EAGAIN`; the adapter then verifies there is no unsent data and that the public TCP still has transport capacity before setting a delivered+inflight marker. There is no product-side periodic flow scan.

Final behavior head `327efe7e29adfc230e7d201b466f2bd4980e976c` passed provenance/P0/P1/P2/P3/P4/P5. P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained:

```text
normal:    samples=138 valid=138 invalid=0 max_rate=696998778 B/s
fast-loss: samples=121 valid=121 invalid=0 loss_events=1
RTO:       samples=135 valid=135 invalid=0 retransmitted_samples=3 timeout_events=2

app-pause:
first_burst=4096 second_burst=131072 delivered=135168
app_limited_samples=7 app_limited_enters=2 app_limited_exits=2
pause_cpu_ticks=0 event_driven=ok
```

Current P3 resource evidence, run `34843587033`, job `103974029078`, artifact `10346739278`:

```text
warm fixed process PSS: 347 KiB
fully-window-resident slope: 37.710938 KiB/flow
128-active projected process PSS: 5174 KiB
remaining 8-MiB process budget: 3018 KiB = 23.578 KiB/flow
3x128 first-to-last drain growth: 5 KiB
idle CPU: 0 ticks/s
```

Thus the sampler remains well inside the constrained-process planning budget.

## P5c: event-driven pacer — next

P5c turns controller pacing policy into actual transmission timing without introducing periodic polling.

Target runtime shape:

```text
all paced flows
      |
      v
process-wide min-heap of next eligible send deadlines
      |
      v
one one-shot CLOCK_MONOTONIC timerfd
      |
      v
existing epoll owner
```

Rules:

- one timerfd for the process, not per flow;
- arm to the earliest pending deadline only;
- use monotonic absolute one-shot deadlines where practical;
- disarm when no pacing deadline exists;
- no fixed pacing tick;
- no busy spin;
- fd readiness and lwIP timers remain under the same owner;
- controller code publishes pacing policy but never sees timerfd/epoll objects;
- CI records timer arms/disarms/expirations, pacing wakeups, released bytes, deadline lateness, heap residency, idle wakeups, CPU, and memory.

The current runtime is already event/deadline driven: epoll timeout comes from `sys_timeouts_sleeptime()`, TUN `EPOLLOUT` is enabled only for real backlog, and P5b app-limited qualification consumed zero CPU ticks during the application pause. P5c must preserve or improve that wakeup profile.

A later optimization may unify lwIP deadlines and the pacer behind one one-shot timerfd only if regression CI proves equal timeout semantics and fewer/equal idle wakeups.

## P6: tcp-shift BBR

After P5c is qualified, BBR becomes the active controller milestone.

P6 implements tcp-shift's bandwidth/min-RTT model, pacing/cwnd policy, mode transitions, probing, loss behavior, and app-limited treatment over the generic CC boundary.

Reference order: current IETF BBR specification, Google QUICHE, ns-3 `TcpBbr`, Picoquic, then Linux `tcp_bbr.c` / `tcp_rate.c` as TCP behavior cross-checks.

Validation compares cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, app-limited behavior, loss response, throughput, retransmissions, CPU, timer wakeups, and memory against native Linux reference runs under reproducible RTT/loss/bandwidth scenarios.

## Distance to BBR

The remaining pre-BBR path is now one infrastructure increment:

```text
P5a delivery ledger        complete
P5b rate/app-limited       complete
P5c event-driven pacing    next
P6 tcp-shift BBR           then active
```

The difficult TCP ownership/recovery and rate-sampling boundaries are qualified. The largest remaining pre-BBR risk is pacing accuracy/wakeup efficiency under high BDP, ACK aggregation, and loss.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, unavoidable BDP buffering dominates the fixed-memory advantage, or pacing accuracy requires per-flow timers / periodic polling / busy spinning.

The project is still successful if evidence supports only a very small conventional-CC userspace TCP endpoint and rejects BBR.
