# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after measurable evidence exists.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 builds once and smoke-tests the same artifact on another runner. P1/P2/P3 still build milestone bring-up binaries inside privileged jobs; later release-oriented milestones should move toward explicit artifact handoff.
4. **Make the product surface mechanical.** Source expansion is deliberate and milestone-qualified rather than inherited from broad upstream build targets.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbols, ELF metadata, memory samples, packet captures, routes, counters, firewall state, conntrack state, and milestone reports are retained where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned production baseline.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds come from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

Validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes critical upstream TCP files, and uploads those records.

### `lwip-p0.yml`

Builds declared project libraries with warnings as errors, runs P0 configuration/source/binary/RSS contracts, and emits the P0 runtime plus diagnostics. A separate `artifact-smoke` job downloads and executes that exact P0 runtime.

The pinned `nd6.c` translation unit has two unused diagnostics when tcp-shift deliberately disables RS/SLAAC. CI keeps those warnings visible but marks only those exact upstream diagnostics `-Wno-error`; project code and all other warnings remain `-Werror`.

The first passing P0 evidence recorded a 41,288-byte executable and maximum RSS of 1,472 KiB for the original `lwip_init()` smoke process. These are initialization numbers, not current dual-stack capacity claims.

### `lwip-next-canary.yml`

Checks the same P0 contracts weekly against current upstream lwIP `master` without changing the pinned production baseline.

### `lwip-p1.yml`

This is the privileged packet/lifecycle workflow. It preserves distinct privilege shapes so packet-path behavior is not conflated with host-resource ownership.

The original IPv4 packet-path phase grants only `cap_net_admin=ep` to `tcp-shift-p1`. Before privileged behavior it runs two unprivileged contracts: TX backpressure fills a packet-oriented fd to real `EAGAIN` and exercises `netif.output -> writev -> bounded FIFO -> flush`; RX-drop injects an over-MTU packet and proves the packet is dropped without terminating the runtime.

The capability-only IPv4 phase qualifies idle wakeups, ICMP, MTU, checksum handling, direct TCP, and a harness-owned namespace/veth/DNAT path. GitHub-hosted runners carry Docker's IPv4 `FORWARD` policy `DROP`, so the harness owns exact temporary forwarding ACCEPT rules scoped to its test interfaces/IP/port. Those rules are runner scaffolding, not product policy.

CI then removes the file capability and runs the product-owned IPv4 lifecycle as root. The runtime briefly execs the system `nft` binary during setup/cleanup and still does not change broad host forwarding policy.

P1b adds two IPv6 behavior phases using the same lwIP/L3/runtime implementation:

- direct static IPv6 TUN qualification: 3/3 ICMPv6, 1500/1501 MTU boundary, real AF_INET6 TCP connect/accept, zero TCP errors, and nonpersistent TUN cleanup;
- product-owned IPv6 ingress qualification: forwarding-disabled preflight without mutation, exclusive `ip6` table collision rejection, exact public IPv6 DNAT/conntrack, actual Hop-by-Hop extension-header TCP traversal, SIGTERM cleanup, and unrelated-ruleset preservation.

The final IPv6 PMTU gate creates a routed asymmetric MTU path. Both client veth ends remain MTU 1500 so a 1500-byte request can enter the router; the host's /128 route back to the client carries MTU 1280. CI captures both the request and the resulting ICMPv6 Packet Too Big on the TUN, then establishes a second TCP connection and verifies that native lwIP MSS calculation consumes the learned PMTU.

P1 diagnostics retain capability state, packet contracts, host/namespace addresses and routes, ping/MTU output, checksum output, tcpdump text, connect output, nftables/iptables state, conntrack state, lifecycle evidence, extension-header evidence, egress-route state, PTB wire capture, and before/after SYN-ACK captures.

### `lwip-p2.yml`

P2 qualifies the actual public-stream-to-loopback bridge while keeping P0 and the entire P1 workflow as separate regression gates on the same PR.

The P2 job builds the IPv4 and IPv6 bring-up runtimes against the pinned lwIP baseline, grants only `cap_net_admin=ep` to the IPv4 bring-up binary for TUN configuration, and runs a sequence of deterministic bridge contracts. The backend is always an ordinary `AF_INET` listener at `127.0.0.1`.

The retained P2 gate set is:

- IPv4 128-KiB bidirectional echo with natural flow teardown;
- 1-MiB delayed-peer backpressure that must hit both backend `EAGAIN` and lwIP send-memory pressure while public pending pbuf residency stays at or below the 32-KiB receive window;
- backend-first half-close with a deliberate 0.5-second open public write direction and a wait-count ceiling to reject level-triggered RDHUP spin;
- backend connect refusal followed by a successful fresh flow on the same listener;
- public RST, backend RST, then a healthy third flow in the same runtime;
- eight simultaneously established flows with self-identifying payloads so backend accept order cannot be mistaken for client creation order;
- SIGTERM while a flow is active, requiring explicit bridge cleanup before process exit;
- 64 sequential connect/drain/reuse flows with warm-up/mid/final VmRSS samples and no sustained RSS ratchet;
- IPv6 public ingress through the same bridge state machine to the same IPv4 loopback backend.

P2 diagnostics retain all client/backend/runtime stdout/stderr plus summaries for each gate. The latest behavior run is `34769960275`, on head `601a49648610513d98173e3e3add722326591ffc`.

The blocked-peer evidence included:

```text
bridge_peak_pending_public_bytes=32768
bridge_backend_write_blocked_events=172
bridge_backend_read_blocked_events=357
bridge_backend_socket_sndbuf_bytes=32768
bridge_backend_socket_rcvbuf_bytes=32768
```

The active-shutdown gate retained:

```text
pre_stop_active_flows=1
shutdown_bridge_active_flows=0
shutdown_bridge_pending_public_bytes=0
client_close=ok
backend_close=ok
tun_cleanup=ok
```

The sequential-reuse gate retained:

```text
flows=64
rss_warmup_kb=1800
rss_mid_kb=1800
rss_final_kb=1800
rss_allowance_kb=1024
bridge_reuse_no_ratcheting=ok
```

That VmRSS result is only a lifecycle/no-ratcheting gate. It must not be reused as the P3 per-connection memory baseline.

### `lwip-p3.yml`

P3 is the dedicated memory/capacity/CPU qualification workflow. It builds the unchanged P2 bridge runtime against the pinned lwIP baseline and then uses separate runtime/backend and public-client network namespaces so public client Linux sockets cannot be mistaken for backend socket state.

The retained P3 gate set is:

- staged idle-established memory at 0/8/32/64/128 flows using `VmRSS` plus `smaps_rollup` RSS/PSS/private-clean/private-dirty/anonymous metrics;
- exact backend socket counts and runtime-namespace `sockstat` observations beside every memory stage;
- public-to-backend pressure with eight 1-MiB senders, a deliberately blocked backend, real backend `EAGAIN`, and 32-KiB/flow lwIP pending-pbuf residency;
- backend-to-public pressure with eight 1-MiB senders, deliberately blocked public reads, small advertised receive windows, and real lwIP send-memory backpressure;
- three rounds of 128 connect/idle/drain flows in one long-lived process, requiring fd count and backend established count to return to their ready floors;
- a one-second idle CPU sample plus 2048 synchronous 64-byte request/echo operations across four flows;
- a machine-readable constrained-host process-PSS model for 32/64/128-MiB planning targets.

Behavior head `775ea5832f7e308e2c908c2f5abedfa4175c69be` passed run `34805306193`, job `103855926986`; artifact `10332963208` retains the complete P3 workload diagnostics.

Final retained memory evidence included:

```text
ready_pss_kb=262
idle_128_pss_kb=307
max_idle_pss_kb_per_flow=0.3515625
public_to_backend_active_pss_delta_kb_per_flow=36.5
backend_to_public_active_pss_delta_kb_per_flow=37.125
bridge_peak_pending_public_bytes=262144
```

Repeated drain evidence included:

```text
round_1_drain_pss_kb=310
round_2_drain_pss_kb=310
round_3_drain_pss_kb=315
first_to_last_drain_pss_growth_kb=5
max_drain_pss_delta_from_ready_kb=57
ratchet_gate_kb=32
warm_floor_gate_kb=128
```

CPU evidence included:

```text
idle_cpu_ms=0.0
operations=2048
payload_bytes=64
work_cpu_ms=70.0
work_cpu_us_per_operation=34.1796875
wall_operations_per_second~=26066
```

The capacity model deliberately covers tcp-shift process PSS only. It excludes backend Linux kernel memory, backend application memory, public-client kernel memory, and provider-specific overhead. Its conservative 32-MiB planning case assigns only 25% of host RAM (8 MiB) to tcp-shift process PSS. With a 315-KiB warm fixed floor and about 37.52 KiB/flow fully-window-resident slope, 128 active flows project to 5118 KiB, leaving 3074 KiB (about 24.0 KiB/flow) for P4/P5 CC/sampler/pacer process structures.

P3 therefore qualifies entry to P4. It is not a full-host 32-MiB connection-capacity guarantee.

### `lwip-p4.yml`

The first P4 job qualifies the generic congestion-control core before any lwIP adapter is added. `scripts/validate-p4-cc.sh` configures `src/cc/` as an independent CMake project and compiles it with warnings-as-errors, `-ffreestanding`, and `-fno-builtin`.

The source-surface gate permits only the controller's own headers plus ISO C integer/size/limits headers. It therefore fails if lwIP, Linux, socket/runtime, bridge, TUN, epoll, timerfd, or nftables dependencies leak into `src/cc/`. The resulting static archive must have zero undefined external symbols. A conventional byte-counting Reno contract exercises init, slow start, congestion avoidance, loss, RTO, MSS changes, saturation, invalid arguments, explicit `cwnd`/`ssthresh` policy publication, failed-init invalid-handle semantics, and the no-pacing policy.

Final standalone behavior head `9e8cb2fa6091418ac4ed3dcb52f963337fdc25d0` passed P4 run `34810033939`, job `103869414629`; artifact `10334775895` retained the standalone evidence. Output included:

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
controller=reno
state_bytes=16
external_symbols=0
P4 standalone congestion-control boundary passed
```

P0 run `34810033921` and full P1 run `34810033970` also passed on the same head. This proves only the standalone policy-library boundary. It does not prove that lwIP is yet delegating public-side cwnd/ssthresh to the controller, and it does not qualify P5 delivery-rate or pacing behavior.

The next P4 CI increment must add integrated adapter evidence under real public-side lwIP traffic, preserve the standalone gate unchanged, retain ACK/loss/RTO policy transitions, and remeasure fixed/per-flow process cost against the P3 baseline.

## Retained P1 evidence

### IPv4 progression

- `34740740077`: direct IPv4 ICMP 3/3 and TUN fd-lifetime cleanup;
- `34740867645`: direct TCP connect and lwIP raw-API accept;
- `34742949259`: retained failure isolated Docker's host `FORWARD` policy as the first DNAT blocker;
- `34743049605`: direct and harness-DNAT TCP, conntrack tuples, cleanup;
- `34743203062`: two-second idle process with four waits and zero TUN writable wakeups;
- `34743392756`: 64 × 1500-byte / 96,000-byte TX ceiling, 65th `ERR_MEM`, 17 real `EAGAIN`, FIFO and detach cleanup;
- `34744304038`: consolidated RX oversize, MTU 1500/1501, bad/good checksum, direct TCP and harness DNAT qualification;
- `34763055040`: product-owned IPv4 forwarding preflight, exclusive table ownership, exact DNAT, rollback/signal cleanup, and unrelated nftables preservation.

### P1b behavior head: `6a9d82feb1f3faead580f560ae1d076999a64b0f`

Both P0 run `34767386591` and P1 run `34767386662` completed successfully on this behavior head. The P1 run retained all prior IPv4 gates and the full IPv6 qualification.

Product-owned IPv6 ingress output included:

```text
ipv6_forwarding_disabled_preflight=ok
ipv6_exclusive_collision_rejection=ok
ipv6_extension_header_dnat=ok
product IPv6 DNAT connected [2001:db8:101::1]:18083
tcp-shift-p1-ipv6: ready tun=tsp1v6nft0 host-ipv6=fd00:198:18::1/126 lwip-ipv6=fd00:198:18::2 mtu=1500 tcp-port=18083 public-ipv6=2001:db8:101::1 nft-table=tcp_shift_p1
ipv6_signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1b product-owned IPv6 nft ingress lifecycle passed
```

The routed PMTU gate retained:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
P1b routed IPv6 Packet Too Big/PMTU qualification passed
```

Before the L3 adapter fix, the same valid PTB reached lwIP but a subsequent SYN-ACK remained at MSS 1440. That failure proved the gap was real: pure L3 output bypassed the ND next-hop path that normally seeds lwIP's destination cache. The adapter now seeds/refreshes the existing fixed ND6 destination cache without neighbor discovery or a second PMTU table, after which upstream PTB handling reduced the next MSS to 1220.

P1 dual-stack packet/lifecycle qualification is complete on GitHub-hosted runners. This does not prove that every OpenVZ/VPS provider exposes the required TUN, IPv4/IPv6 forwarding, nftables, conntrack, and capabilities; provider-specific qualification remains separate.

## P2 completion evidence

Behavior head `601a49648610513d98173e3e3add722326591ffc` completed successfully in all three relevant workflows:

- P0 `34769960299`;
- full P1 regression `34769960302`;
- P2 bridge `34769960275`.

The P2 artifact for run `34769960275` retains 72 diagnostic files covering the bridge gate set.

## Milestone CI growth

### P4: generic congestion-control boundary — active

The standalone core is runner-qualified. The next gate is the thin lwIP adapter: preserve the pure-C boundary, demonstrate conventional-controller ownership of public-side cwnd/ssthresh under real traffic, retain structured ACK/loss/RTO policy evidence, and measure adapter/per-flow overhead against P3.

### P5-P6: sampler, pacing, and BBR

Congestion-control CI separates correctness from headline throughput. Retain structured traces for delivery-rate samples, RTT samples, cwnd, pacing rate, inflight/loss state, app-limited state, and mode transitions. Native Linux CUBIC/BBR runs are reference baselines, not byte-for-byte expected output.

CPU and timer behavior become dedicated gates once the pacer exists. Tests run under explicit CPU quotas and include idle, small-packet, and high-BDP workloads so pacing accuracy cannot be obtained by busy spinning.

## Gate policy

A workflow failure must say which contract failed. Resource thresholds live near the validation script and are emitted beside observed values. If a limit changes, the change should include evidence explaining whether the product legitimately grew or the old limit was unstable.

CI artifacts are part of the engineering record. Passing status without corresponding provenance and milestone evidence is not sufficient qualification for a release-oriented change.
