# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after they have measurable evidence.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests are attempted.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 already builds once and smoke-tests the same artifact on another runner. P1 currently rebuilds its temporary bring-up binary in the privileged job; before release-oriented P1 completion it should adopt the same artifact handoff pattern.
4. **Make the product surface mechanical.** P0 has an explicit IPv4/TCP source allowlist. Later source expansion is deliberate and milestone-qualified.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbols, ELF metadata, memory samples, packet-path state, runtime counters, and milestone reports are retained where applicable.
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

This is the privileged P1 packet-path workflow. It builds `tcp-shift-p1` from pinned lwIP, grants only `cap_net_admin=ep` to that temporary binary, creates a nonpersistent TUN, configures a private host/TUN subnet, and drives real packets through lwIP. Diagnostics retain capability state, runtime stdout/stderr, host addresses, routes, link counters, ICMP output, and TCP-connect output.

Retained evidence:

- run `34740740077`: direct IPv4 ICMP path passed 3/3 with 0% loss; runtime `rx_packets=4`, `tx_packets=3`, no queued TX bytes or queue drops; TUN disappeared after process exit;
- run `34740867645`: direct IPv4 ICMP remained 3/3, host TCP `connect()` to lwIP port 18080 succeeded, and runtime reported `tcp_accepts=1`, `tcp_errors=0` with `rx_packets=8`, `tx_packets=5`.

These prove direct IPv4 L3 TUN ingress/egress plus lwIP ICMP and TCP handshake/accept behavior on the CI runner. They do not prove DNAT/public routing, backend bridge semantics, queue-pressure behavior, or idle CPU/wakeup properties.

For commit `0a420757aa41c4d1eaeeb2f8c68d652ca4d0982a`, `lwIP P0`, `lwIP upstream provenance`, and `lwIP P1 IPv4 TUN` all passed.

Remaining P1 gates include DNAT/conntrack/public-route qualification, explicit idle-wakeup measurement, queue backpressure/failure paths, transactional host resource ownership, and the IPv6 P1b matrix including ICMPv6/TCP and PMTU.

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
