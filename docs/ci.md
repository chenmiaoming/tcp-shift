# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, the production artifact is built once, downstream tests consume that exact artifact, diagnostics survive failure, and resource claims become hard gates only after they have measurable evidence.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes the TCP files whose behavior matters most to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests are attempted.
3. **Build the runtime once.** The primary build job produces the candidate `tcp-shift` executable. Later jobs must download and test that exact artifact rather than rebuilding a possibly different binary.
4. **Make the product surface mechanical.** P0 has an explicit IPv4/TCP source allowlist. IPv6, socket/netconn, PPP, 6LoWPAN, and Unix `sys_arch.c` are forbidden until a milestone deliberately adds them.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbol lists, ELF metadata, memory samples, and milestone reports are uploaded with `if: always()` where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned release baseline. Canary breakage does not silently change the production dependency.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds are introduced from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

This is the provenance gate. It validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes the critical upstream TCP files, and uploads those records.

Changing the pinned lwIP commit or fetch logic must pass this workflow independently of the runtime tests.

### `lwip-p0.yml`

The `build-contract` job is the P0 qualification job. It:

- validates local scripts and baseline syntax;
- fetches the pinned upstream source;
- builds with warnings as errors;
- compiles and runs the project-local lwIP configuration contract;
- verifies the actual `tcp_shift_lwip` target source manifest rather than the global CMake compile database;
- rejects forbidden socket/netconn/UDP symbols from the final executable;
- enforces binary-size and idle-RSS ceilings;
- emits `tcp-shift.p0.v1` JSON evidence;
- uploads the candidate runtime and diagnostics.

The `artifact-smoke` job runs on a separate runner. It downloads the exact runtime artifact produced by `build-contract`, verifies its checksum, and executes it. This catches accidental dependencies on the original build tree.

The first passing P0 evidence on Ubuntu 24.04 recorded:

- executable size: 41,288 bytes;
- maximum RSS for the `lwip_init()` smoke process: 1,472 KiB;
- source surface: IPv4/TCP, `NO_SYS=1`, no window scaling.

These numbers describe only the P0 initialization workload. They are not connection-capacity claims.

### `lwip-next-canary.yml`

The weekly canary checks the same P0 build and contracts against current upstream lwIP `master`. It is an early-warning mechanism for API/configuration drift, analogous to the moving-kernel canary in `linux-tcp-cc`. Production remains pinned until an explicit baseline update is reviewed and qualified.

## Milestone CI growth

CI grows with the implementation rather than front-loading tests for features that do not exist yet.

### P1: L3 TUN ingress

Add a privileged packet-path job that consumes the built runtime artifact and verifies:

- TUN ownership/setup and cleanup;
- ICMP echo through lwIP;
- TCP SYN/SYN-ACK packet flow;
- IPv4 checksum and MTU behavior;
- bounded idle wakeups with no fixed-rate polling.

Always retain packet captures, runtime logs, and interface/routing state on failure.

### P2: stream bridge lifecycle

Add an ingress/bridge qualification job that verifies bidirectional payload integrity, partial I/O, half-close, reset, backend failure, deterministic shutdown, and fresh-flow reuse. The job must assert zero live bridge objects after teardown.

### P3: memory and capacity

Introduce staged connection counts chosen for 32/64/128-MiB target hosts. Record at least:

- ready/idle RSS, PSS, and private dirty memory;
- established-idle and active-flow residency;
- per-stage connection count and verified traffic;
- peak, post-drain, and post-idle floors;
- repeated load/drain rounds in one long-lived process.

The hard lifecycle criterion should be the same kind of property used by `linux-tcp-cc`: repeated rounds must not create a steadily rising resident-memory floor. A single fast RSS drop is useful telemetry but not sufficient evidence by itself.

Only after several stable runs should per-flow or absolute-RSS ceilings become hard gates.

### P4-P6: congestion control, sampler, and pacing

Congestion-control CI must separate correctness from headline throughput. It should retain structured traces for delivery-rate samples, RTT samples, cwnd, pacing rate, inflight/loss state, and mode transitions. Native Linux CUBIC/BBR runs remain reference baselines rather than being treated as byte-for-byte expected output.

CPU and timer behavior become dedicated gates once the pacer exists. The preferred tests run under explicit CPU quotas and include idle, small-packet, and high-BDP workloads so pacing accuracy cannot be obtained by busy spinning.

## Gate policy

A workflow failure must say which contract failed. Resource thresholds are kept close to the validation script and emitted in the JSON report alongside the observed value. If a limit needs to change, the change should include evidence explaining whether the product got legitimately larger or the old limit was unstable.

CI artifacts are part of the engineering record. Passing status without the corresponding provenance and milestone evidence is not considered sufficient qualification for a release-oriented change.
