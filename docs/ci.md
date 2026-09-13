# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after they have measurable evidence.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests are attempted.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 already builds once and smoke-tests the same artifact on another runner. P1 currently rebuilds its temporary bring-up binary in the privileged job; before release-oriented P1 completion it should adopt the same artifact handoff pattern.
4. **Make the product surface mechanical.** P0 has an explicit IPv4/TCP source allowlist. Later source expansion is deliberate and milestone-qualified.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbols, ELF metadata, memory samples, packet-path state, runtime counters, firewall state, conntrack state, and milestone reports are retained where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned production baseline.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds come from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

Validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes critical upstream TCP files, and uploads those records.

### `lwip-p0.yml`

Builds all currently declared project libraries with warnings as errors, runs the P0 configuration/source/binary/RSS contracts, and emits the P0 runtime plus diagnostics. A separate `artifact-smoke` job downloads and executes that exact P0 runtime.

The first passing P0 evidence recorded an executable size of 41,288 bytes and maximum RSS of 1,472 KiB for the `lwip_init()` smoke process. Those are initialization numbers, not flow-capacity claims.

### `lwip-next-canary.yml`

Checks the same P0 contracts weekly against current upstream lwIP `master` without changing the pinned production baseline.

### `lwip-p1.yml`

This is the privileged P1 packet-path workflow. It builds `tcp-shift-p1` from pinned lwIP and grants only `cap_net_admin=ep` to that temporary binary. The binary itself creates the nonpersistent TUN and configures the host-side IPv4 address, MTU, and link-up state; the CI script does not configure the product TUN for it.

The test then exercises two paths:

```text
direct host -> TUN -> lwIP
```

and

```text
client network namespace
    -> veth
    -> host IPv4:port
    -> nftables PREROUTING DNAT
    -> TUN
    -> lwIP TCP
```

The namespace path uses normal Linux forwarding and conntrack. GitHub-hosted runners carry Docker's IPv4 `FORWARD` policy `DROP`, so the harness inserts exactly two temporary forwarding ACCEPT rules scoped to the test interfaces, lwIP address, and TCP port. They are deleted during cleanup. This is runner scaffolding and must not be mistaken for product firewall ownership.

Diagnostics currently retain capability state, runtime stdout/stderr, host and namespace addresses/routes/link counters, ICMP output, direct/DNAT TCP-connect output, nftables rules, iptables state, and conntrack state.

Retained evidence:

- run `34740740077`: direct IPv4 ICMP passed 3/3 with 0% loss; runtime `rx_packets=4`, `tx_packets=3`, no TX queue use/drops; TUN disappeared after exit;
- run `34740867645`: direct IPv4 TCP `connect()` completed and lwIP raw API reported `tcp_accepts=1`, `tcp_errors=0`, `rx_packets=8`, `tx_packets=5`;
- run `34742949259`: first DNAT attempt failed because the runner's Docker-managed IPv4 `FORWARD` chain had policy `DROP`; retained nftables, route, conntrack, and runtime diagnostics identified the environment prerequisite rather than a lwIP failure;
- run `34743049605`: after adding exact temporary forwarding rules, both direct and DNAT TCP connections completed. Runtime reported `rx_packets=12`, `tx_packets=7`, `tcp_accepts=2`, `tcp_errors=0`, no TX queue use/drops. The harness also required conntrack evidence for the external original tuple and the lwIP reply tuple and verified cleanup of the TUN, namespace, veth, nftables table, temporary forwarding rules, and forwarding sysctl state.

For commit `4d3141499cfd1624f6a15552d509edf73a30e11a`, `lwIP P0`, `lwIP upstream provenance`, and `lwIP P1 IPv4 TUN` all passed.

This now proves product-owned IPv4 TUN setup, direct ICMP/TCP, and a routed DNAT/conntrack TCP path into lwIP on the CI runner. It does not yet prove production firewall-rule ownership, queue-pressure behavior, idle-wakeup bounds, MTU edge cases, or IPv6.

## Remaining P1 gates

P1a still needs:

- explicit idle-wakeup measurement showing no busy polling beyond lwIP timer deadlines;
- forced TUN TX backpressure/queue-ceiling behavior and cleanup;
- checksum/MTU edge cases;
- production lifecycle ownership for narrow NAT/forwarding rules rather than CI-only harness rules.

P1b then adds IPv6 TUN addressing, ICMPv6/TCP, IPv6-only operation, Packet Too Big/PMTU, and extension-header-safe host filtering.

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
