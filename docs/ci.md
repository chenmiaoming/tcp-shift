# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after measurable evidence exists.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 builds once and smoke-tests the same artifact on another runner. P1 still builds its temporary lifecycle binaries inside the privileged job; release-oriented milestones should move toward explicit artifact handoff.
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

## Milestone CI growth

### P2: stream bridge lifecycle — active next

Add public-stream/backend qualification for bidirectional payload integrity, partial I/O, bounded backpressure in both directions, half-close, reset, backend-connect failure, deterministic shutdown, and fresh-flow reuse. The job must assert zero live bridge objects after teardown. Public IPv4 and IPv6 must exercise the same bridge implementation; the backend can remain `AF_INET` `127.0.0.1`.

### P3: memory and capacity

Introduce staged connection counts chosen for 32/64/128-MiB target hosts. Record ready/idle RSS, PSS/private dirty, established-idle and active-flow residency, verified traffic, peak/post-drain floors, and repeated load/drain rounds in one long-lived process. A single fast RSS drop is not sufficient evidence by itself.

### P4-P6: congestion control, sampler, and pacing

Congestion-control CI separates correctness from headline throughput. Retain structured traces for delivery-rate samples, RTT samples, cwnd, pacing rate, inflight/loss state, and mode transitions. Native Linux CUBIC/BBR runs are reference baselines, not byte-for-byte expected output.

CPU and timer behavior become dedicated gates once the pacer exists. Tests run under explicit CPU quotas and include idle, small-packet, and high-BDP workloads so pacing accuracy cannot be obtained by busy spinning.

## Gate policy

A workflow failure must say which contract failed. Resource thresholds live near the validation script and are emitted beside observed values. If a limit changes, the change should include evidence explaining whether the product legitimately grew or the old limit was unstable.

CI artifacts are part of the engineering record. Passing status without corresponding provenance and milestone evidence is not sufficient qualification for a release-oriented change.
