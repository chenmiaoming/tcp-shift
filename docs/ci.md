# CI architecture

`tcp-shift` CI follows the same evidence-driven structure used by `linux-tcp-cc`: upstream provenance is independent from product validation, diagnostics survive failure, and resource or behavior claims become hard gates only after they have measurable evidence.

## Principles

1. **Pin and prove upstream provenance.** A normal product build consumes one exact lwIP commit. CI records that commit and hashes TCP files whose behavior matters to the project.
2. **Validate contracts before behavior.** Shell syntax, baseline format, compile-time lwIP options, and source-surface boundaries fail before network tests are attempted.
3. **Reuse exact artifacts once a milestone has a stable artifact boundary.** P0 already builds once and smoke-tests the same artifact on another runner. P1 still builds its temporary bring-up binary in the privileged job; release-oriented milestones should move toward exact artifact handoff.
4. **Make the product surface mechanical.** Source expansion is deliberate and milestone-qualified rather than inherited from broad upstream build targets.
5. **Preserve diagnostics on failure.** Provenance, source manifests, symbols, ELF metadata, memory samples, packet captures, packet-path state, runtime counters, firewall state, conntrack state, and milestone reports are retained where applicable.
6. **Separate pinned qualification from moving-upstream compatibility.** A scheduled canary tests current lwIP independently of the pinned production baseline.
7. **Promote observations to gates deliberately.** Memory, CPU, throughput, and capacity thresholds come from measured baselines and named workloads, not guessed limits.

## Current workflows

### `lwip-upstream.yml`

Validates `.lwip-baseline`, fetches the exact commit, verifies a clean detached checkout, records `.build/upstream.env`, hashes critical upstream TCP files, and uploads those records.

### `lwip-p0.yml`

Builds the declared project libraries with warnings as errors, runs the P0 configuration/source/binary/RSS contracts, and emits the P0 runtime plus diagnostics. A separate `artifact-smoke` job downloads and executes that exact P0 runtime.

The first passing P0 evidence recorded a 41,288-byte executable and maximum RSS of 1,472 KiB for the `lwip_init()` smoke process. These are initialization numbers, not flow-capacity claims.

### `lwip-next-canary.yml`

Checks the same P0 contracts weekly against current upstream lwIP `master` without changing the pinned production baseline.

### `lwip-p1.yml`

This is the privileged P1 packet-path workflow. It builds `tcp-shift-p1` from pinned lwIP and grants only `cap_net_admin=ep` to that temporary binary. The binary creates the nonpersistent TUN and configures the host-side IPv4 address, MTU, and link-up state; the CI script does not configure the product TUN for it.

Before privileged behavior, the job runs two unprivileged contracts:

- TX backpressure: fills a packet-oriented fd to `EAGAIN` and exercises the production `netif.output -> writev -> bounded FIFO -> flush` path;
- RX drop: injects an over-MTU packet and proves it is counted as a nonfatal drop rather than terminating the runtime.

The behavior phase first measures a separate idle-only process and then starts a fresh active process. The active path exercises:

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

GitHub-hosted runners carry Docker's IPv4 `FORWARD` policy `DROP`, so the harness inserts exactly two temporary forwarding ACCEPT rules scoped to the test interfaces, lwIP address, and TCP port. They are deleted during cleanup. This is runner scaffolding and must not be mistaken for product firewall ownership.

The active phase also qualifies MTU and checksum semantics. A 1500-byte IPv4 DF packet must succeed; a 1501-byte DF packet must be rejected at the configured MTU boundary. A raw ICMP probe emits one deliberately invalid checksum and one valid checksum while `tcpdump` retains wire evidence; the bad request must receive no reply and the valid request must receive an echo reply.

Diagnostics retain capability state, runtime stdout/stderr, idle summary, host/namespace addresses and routes, link counters, ping/MTU output, checksum script output, tcpdump capture text, direct/DNAT connect output, nftables/iptables state, and conntrack state.

## Retained P1 evidence

- `34740740077`: direct IPv4 ICMP passed 3/3 with 0% loss; TUN disappeared after exit.
- `34740867645`: direct IPv4 TCP connect completed and lwIP raw API reported `tcp_accepts=1` with no TCP error.
- `34742949259`: first DNAT attempt failed because the runner's Docker-managed `FORWARD` chain had policy `DROP`; retained diagnostics identified the environment prerequisite rather than a lwIP failure.
- `34743049605`: after exact temporary forwarding rules were added, both direct and DNAT TCP connections completed; conntrack original/reply tuples and cleanup were verified.
- `34743203062`: a separate two-second idle process recorded 4 `epoll_wait` calls, 2 timer timeouts, and zero idle TUN writable wakeups; the loose ceiling of 32 waits/two seconds catches fixed-rate/busy polling regressions.
- `34743392756`: bounded TX contract queued 64 × 1500-byte packets for a 96,000-byte peak, rejected the 65th with `ERR_MEM`, observed 17 real `EAGAIN` events, preserved FIFO ordering, and cleaned queued references on detach.
- `34744304038`: the complete current IPv4 packet-path qualification passed. The RX contract reported `mtu=1500 oversize=1501 rx_drops=1 rx_errors=0 runtime_survives_drop=ok`; 1500-byte DF ICMP succeeded while 1501 bytes failed with `message too long, mtu=1500`; checksum software/wire verification showed the deliberately bad request was invalid and the good request verified to `0x0000`; lwIP ignored the bad request and replied to the good request; direct TCP and DNAT TCP both connected. Final active runtime counters were `rx_packets=16 rx_drops=0 rx_errors=0 tx_packets=9 tx_queue_peak_bytes=0 tx_queue_drops=0 tcp_accepts=2 tcp_rx_bytes=0 tcp_errors=0 loop_wait_calls=18 loop_ready_wakeups=12 loop_timeout_wakeups=5 loop_eintr_wakeups=1 loop_tun_readable_wakeups=12 loop_tun_writable_wakeups=0`.

For commit `153ebaa348c28465dafc793d4f820fa355280252`, `lwIP P0`, `lwIP upstream provenance`, and `lwIP P1 IPv4 TUN` all passed.

This proves the current IPv4 packet path: product-owned TUN setup, bounded event-loop wakeups, bounded TX queue behavior, nonfatal oversize RX handling, direct ICMP/TCP, routed DNAT/conntrack TCP, MTU boundary behavior, and checksum rejection/acceptance.

It does **not** yet prove product-owned firewall resource acquisition/rollback. The current nftables DNAT resource and forwarding exceptions are created by the CI harness to qualify the data path.

## Remaining P1 gates

P1a still needs product lifecycle ownership for a narrow exact-match nftables DNAT resource, including read-only prerequisite checks, collision handling, startup rollback, signal cleanup, forced-failure cleanup, and proof that unrelated firewall state is unchanged. Global forwarding sysctls and broad host forwarding policy should remain operator-managed prerequisites rather than silent product mutations.

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
