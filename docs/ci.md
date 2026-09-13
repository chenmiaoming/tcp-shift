# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after measurable evidence exists.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 builds once and smoke-tests the same artifact on another runner. P1 still builds its temporary lifecycle binary inside the privileged job; release-oriented milestones should move toward explicit artifact handoff.
4. **Make the product surface mechanical.** Source expansion is deliberate and milestone-qualified rather than inherited from broad upstream build targets.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbols, ELF metadata, memory samples, packet captures, routes, counters, firewall state, conntrack state, and milestone reports are retained where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned production baseline.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds come from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

Validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes critical upstream TCP files, and uploads those records.

### `lwip-p0.yml`

Builds declared project libraries with warnings as errors, runs P0 configuration/source/binary/RSS contracts, and emits the P0 runtime plus diagnostics. A separate `artifact-smoke` job downloads and executes that exact P0 runtime.

The first passing P0 evidence recorded a 41,288-byte executable and maximum RSS of 1,472 KiB for the `lwip_init()` smoke process. These are initialization numbers, not flow-capacity claims.

### `lwip-next-canary.yml`

Checks the same P0 contracts weekly against current upstream lwIP `master` without changing the pinned production baseline.

### `lwip-p1.yml`

This is the privileged P1 packet/lifecycle workflow. It preserves two distinct privilege shapes so the tests do not conflate packet-path behavior with host-resource ownership.

The original packet-path phase grants only `cap_net_admin=ep` to `tcp-shift-p1`. Before privileged behavior it runs two unprivileged contracts: TX backpressure fills a packet-oriented fd to real `EAGAIN` and exercises `netif.output -> writev -> bounded FIFO -> flush`; RX-drop injects an over-MTU packet and proves the packet is dropped without terminating the runtime.

The capability-only behavior phase then qualifies idle wakeups, ICMP, MTU, checksum handling, direct TCP, and a harness-owned namespace/veth/DNAT path. GitHub-hosted runners carry Docker's IPv4 `FORWARD` policy `DROP`, so the harness owns two exact temporary forwarding ACCEPT rules scoped to its test interfaces/IP/port. Those rules are runner scaffolding, not product policy.

After that regression path, CI removes the file capability and runs `scripts/p1-nft-lifecycle.sh` as root. This second phase validates the product-owned host lifecycle because the runtime briefly execs the system `nft` binary during setup/cleanup. The product still does not change broad host forwarding policy.

The lifecycle phase checks three independent paths:

```text
operator prerequisite failure:
    ip_forward=0 -> startup fails -> sysctl unchanged -> no TUN/table leak

resource collision:
    pre-existing ip tcp_shift_p1 -> startup fails -> existing table unchanged -> TUN rolled back

normal public ingress:
    namespace client -> veth -> product-owned exact DNAT -> TUN -> lwIP TCP
    SIGTERM -> product table removed -> TUN removed -> unrelated nft table unchanged
```

P1 diagnostics retain capability state, packet contracts, idle summary, host/namespace addresses and routes, link counters, ping/MTU output, checksum script output, tcpdump text, connect output, nftables/iptables state, conntrack state, and `.build/p1-nft-ci` lifecycle evidence.

## Retained P1 evidence

Earlier runs progressively qualified the IPv4 packet path:

- `34740740077`: direct IPv4 ICMP 3/3 and TUN fd-lifetime cleanup;
- `34740867645`: direct TCP connect and lwIP raw-API accept;
- `34742949259`: retained failure isolated Docker's host `FORWARD` policy as the first DNAT blocker;
- `34743049605`: direct and harness-DNAT TCP, conntrack tuples, cleanup;
- `34743203062`: two-second idle process with four waits and zero TUN writable wakeups;
- `34743392756`: 64 × 1500-byte / 96,000-byte TX ceiling, 65th `ERR_MEM`, 17 real `EAGAIN`, FIFO and detach cleanup;
- `34744304038`: consolidated RX oversize, MTU 1500/1501, bad/good checksum, direct TCP and harness DNAT qualification.

### Run 34763055040: product-owned IPv4 ingress lifecycle

This run kept all previous P1 packet-path gates green and passed the new lifecycle step. Retained output:

```text
forwarding_disabled_preflight=ok
exclusive_collision_rejection=ok
product DNAT connected 198.51.101.1:18081
tcp-shift-p1: ready tun=tsp1nft0 host-ipv4=10.232.0.1 lwip-ipv4=10.232.0.2 mtu=1500 tcp-port=18081 public-ipv4=198.51.101.1 nft-table=tcp_shift_p1
tcp-shift-p1: rx_packets=5 rx_drops=0 rx_errors=0 tx_packets=2 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=1 tcp_rx_bytes=0 tcp_errors=0 loop_wait_calls=4 loop_ready_wakeups=3 loop_timeout_wakeups=0 loop_eintr_wakeups=1 loop_tun_readable_wakeups=3 loop_tun_writable_wakeups=0
signal_cleanup=ok unrelated_ruleset_unchanged=ok
P1 product-owned nft ingress lifecycle passed
```

The test deliberately set `net.ipv4.ip_forward=0` in the harness and proved tcp-shift only diagnosed it: the value stayed zero and product resources were absent after failure. It then occupied the exact product table name and proved startup rejected the collision without adopting or deleting that table. Finally the product created its own exact DNAT resource, a separate network namespace connected through it into lwIP, conntrack evidence was retained, SIGTERM removed product TUN/table, and the SHA-256 of an unrelated nft table was identical before and after.

The P1 diagnostic artifact for this run was `tcp-shift-p1-ipv4-diagnostics`, artifact ID `10319408011`.

P1a IPv4 is complete by the current gate policy. Remaining P1 work is P1b IPv6: IPv6 source/config expansion, IPv6-only TUN addressing, ICMPv6/TCP, exact product-owned IPv6 ingress, Packet Too Big/PMTU, extension-header-safe matching, capability diagnostics, and cleanup while retaining all IPv4 regression gates.

## Milestone CI growth

### P2: stream bridge lifecycle

Add ingress/bridge qualification for bidirectional payload integrity, partial I/O, half-close, reset, backend failure, deterministic shutdown, and fresh-flow reuse. The job must assert zero live bridge objects after teardown.

### P3: memory and capacity

Introduce staged connection counts chosen for 32/64/128-MiB target hosts. Record ready/idle RSS, PSS/private dirty, established-idle and active-flow residency, verified traffic, peak/post-drain floors, and repeated load/drain rounds in one long-lived process. A single fast RSS drop is not sufficient evidence by itself.

### P4-P6: congestion control, sampler, and pacing

Congestion-control CI separates correctness from headline throughput. Retain structured traces for delivery-rate samples, RTT samples, cwnd, pacing rate, inflight/loss state, and mode transitions. Native Linux CUBIC/BBR runs are reference baselines, not byte-for-byte expected output.

CPU and timer behavior become dedicated gates once the pacer exists. Tests run under explicit CPU quotas and include idle, small-packet, and high-BDP workloads so pacing accuracy cannot be obtained by busy spinning.

## Gate policy

A workflow failure must say which contract failed. Resource thresholds live near the validation script and are emitted beside observed values. If a limit changes, the change should include evidence explaining whether the product legitimately grew or the old limit was unstable.

CI artifacts are part of the engineering record. Passing status without corresponding provenance and milestone evidence is not sufficient qualification for a release-oriented change.
