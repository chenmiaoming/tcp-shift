# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, the production artifact is built once, downstream tests consume that exact artifact, diagnostics survive failure, and resource claims become hard gates only after they have measurable evidence.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes the TCP files whose behavior matters most to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests are attempted.
3. **Build the runtime once.** The primary build job produces the candidate `tcp-shift` executable. Later jobs must download and test that exact artifact rather than rebuilding a possibly different binary once the runtime artifact boundary exists for that milestone.
4. **Make the product surface mechanical.** P0 has an explicit IPv4/TCP source allowlist. IPv6, socket/netconn, PPP, 6LoWPAN, and Unix `sys_arch.c` are forbidden from the P0 lwIP core until a milestone deliberately adds them.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbol lists, ELF metadata, memory samples, packet-path state, and milestone reports are uploaded with `if: always()` where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned release baseline. Canary breakage does not silently change the production dependency.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds are introduced from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

This is the provenance gate. It validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes the critical upstream TCP files, and uploads those records.

### `lwip-p0.yml`

The `build-contract` job is the P0 qualification job. It builds all currently declared project libraries with warnings as errors, runs the P0 configuration/source/binary/RSS contracts, and emits the P0 runtime plus diagnostics. The separate `artifact-smoke` job downloads and executes that exact P0 runtime.

The first passing P0 evidence on Ubuntu 24.04 recorded an executable size of 41,288 bytes and maximum RSS of 1,472 KiB for the `lwip_init()` smoke process. Those are initialization numbers, not flow-capacity claims.

### `lwip-next-canary.yml`

The weekly canary checks the same P0 contracts against current upstream lwIP `master` without changing the pinned production baseline.

### `lwip-p1.yml`

P1 now has a dedicated privileged packet-path workflow rather than treating compilation as behavioral evidence.

The job builds `tcp-shift-p1` from the pinned lwIP source, grants only `cap_net_admin=ep` to that temporary bring-up binary, creates a nonpersistent TUN, configures a small host/TUN subnet, and exercises real packets through lwIP. Diagnostics retain capability state, runtime stdout/stderr, host addresses, routes, link counters, and probe output.

The first run, `34740740077`, passed the direct IPv4 ICMP path: 3/3 echo replies, 0% loss, runtime `rx_packets=4`, `tx_packets=3`, no queued TX bytes, no queue drops, and TUN disappearance after process exit. This proves L3 TUN packet ingress/egress and fd-lifetime cleanup on the CI runner; it does not prove DNAT/public routing or queue-pressure behavior.

The current workflow additionally requires a real host TCP `connect()` to the minimal lwIP probe listener and `tcp_accepts > 0`. Once that run passes, TCP SYN/SYN-ACK/accept becomes retained P1 evidence rather than a compile-only claim.

Remaining P1 gates include DNAT/conntrack/public-route qualification, explicit idle-wakeup measurement, queue backpressure/failure paths, transactional host resource ownership, then the IPv6 P1b matrix including ICMPv6/TCP and PMTU.

## Milestone CI growth

### P2: stream bridge lifecycle

Add an ingress/bridge qualification job that verifies bidirectional payload integrity, partial I/O, half-close, reset, backend failure, deterministic shutdown, and fresh-flow reuse. The job must assert zero live bridge objects after teardown.

### P3: memory and capacity

Introduce staged connection counts chosen for 32/64/128-MiB target hosts. Record ready/idle RSS, PSS/private dirty, established-idle and active-flow residency, verified traffic, peak/post-drain floors, and repeated load/drain rounds in one long-lived process. A single fast RSS drop is useful telemetry but not sufficient evidence by itself.

### P4-P6: congestion control, sampler, and pacing

Congestion-control CI separates correctness from headline throughput. Retain structured traces for delivery-rate samples, RTT samples, cwnd, pacing rate, inflight/loss state, and mode transitions. Native Linux CUBIC/BBR runs are reference baselines, not byte-for-byte expected output.

CPU and timer behavior become dedicated gates once the pacer exists. Tests run under explicit CPU quotas and include idle, small-packet, and high-BDP workloads so pacing accuracy cannot be obtained by busy spinning.

## Gate policy

A workflow failure must say which contract failed. Resource thresholds live near the validation script and are emitted beside observed values. If a limit changes, the change should include evidence explaining whether the product legitimately grew or the old limit was unstable.

CI artifacts are part of the engineering record. Passing status without corresponding provenance and milestone evidence is not considered sufficient qualification for a release-oriented change.
