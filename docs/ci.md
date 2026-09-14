# CI architecture

`tcp-shift` CI is evidence-driven: upstream provenance is independent from product behavior, diagnostics survive failure, and observed resource behavior becomes a hard gate only after a workload is defined and measured.

## Principles

1. Pin upstream exactly and retain provenance.
2. Keep project-owned lwIP modifications explicit and mechanically bounded.
3. Validate source/configuration contracts before network behavior.
4. Preserve P0/P1/P2/P3/P4 regressions while adding later transport features.
5. Retain failure artifacts rather than rerunning away useful evidence.
6. Separate process-PSS planning from full-host memory claims.
7. Keep pure controller logic independently buildable from lwIP/Linux runtime code.
8. Qualify real packet-path events for ACK/loss/RTO/rate sampling; synthetic controller calls are not enough for integration claims.
9. Make harness failures fail closed: parser/check failures must propagate nonzero status even when summaries are retained.
10. Pacing/timing CI must reject busy spin and periodic-polling designs that inflate wakeups.

## `lwip-upstream.yml`

The production baseline is `.lwip-baseline`, currently pinning lwIP commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`.

`scripts/fetch-lwip.sh` checks out that exact commit, records `.build/upstream.env`, and hashes critical pristine TCP sources before applying `patches/lwip-p4-cc-hooks.patch`.

The provenance workflow proves:

- dependency `HEAD` equals the pin;
- patch path and SHA256 match the repository artifact;
- `git diff --check` and reverse-apply checks pass;
- only `src/core/tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- an independent worktree created from the same pinned commit, after applying the repository patch, is byte-identical to all modified files in the build workspace.

P5a continues to use the same three-file patch boundary; it adds send/ACK observation calls only inside already-controlled `tcp_in.c` / `tcp_out.c`.

## `lwip-p0.yml`

P0 builds the constrained lwIP source allowlist with warnings-as-errors, runs compile-time configuration contracts, source-surface checks, binary-size/RSS gates, and stages an exact runtime artifact for a separate smoke job.

The pinned `nd6.c` unit emits two diagnostics when RS/SLAAC are deliberately disabled. Only those upstream diagnostics are downgraded from errors; they remain visible. Project code and other warnings stay under `-Werror`.

## `lwip-next-canary.yml`

The scheduled canary tests the constrained build against moving upstream lwIP independently from the pinned production baseline. A canary failure does not change `.lwip-baseline`.

## `lwip-p1.yml`

P1 is the privileged L3 packet/lifecycle workflow. It retains:

- real TUN TX `EAGAIN` and bounded FIFO qualification;
- nonfatal oversized RX drops;
- direct IPv4 ICMP/TCP, MTU/checksum behavior;
- product-owned IPv4 nft ingress lifecycle;
- direct static IPv6 ICMP/TCP and MTU behavior;
- product-owned IPv6 exact DNAT/conntrack;
- actual Hop-by-Hop extension-header TCP traversal;
- forwarding-disabled preflight/collision/cleanup invariants;
- routed ICMPv6 PTB learning and subsequent TCP MSS reduction.

## `lwip-p2.yml`

P2 qualifies the real public-stream -> `127.0.0.1` backend bridge for both public address families.

The gate set includes natural IPv4/IPv6 echo, real bidirectional backpressure, half-close without `EPOLLRDHUP` spin, refusal/reset recovery, concurrent flows, active-flow shutdown, and sequential reuse without sustained VmRSS ratcheting.

P4 overlays a controller-ownership gate on every P2 workload:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

This prevents a green bridge test from silently exercising native lwIP congestion policy.

## `lwip-p3.yml`

P3 measures tcp-shift process residency and CPU separately from backend Linux kernel/application memory. Runtime/backend and public-client sockets are separated in namespaces so socket-state accounting remains interpretable.

Workloads include 0/8/32/64/128 idle flows, both active-window directions, three 128-flow connect/idle/drain rounds, idle/small-operation CPU, and constrained-host process-PSS modeling.

Final P4 adapter baseline:

```text
warm fixed process PSS: 335 KiB
fully-window-resident slope: 37.148438 KiB/flow
128 active projected PSS: 5090 KiB
8-MiB process budget remaining: 3102 KiB = 24.234 KiB/flow
```

P5a rerun:

```text
warm fixed process PSS: 343 KiB
fully-window-resident slope: 37.679688 KiB/flow
128 active projected PSS: 5166 KiB
8-MiB process budget remaining: 3026 KiB = 23.641 KiB/flow
```

The model remains tcp-shift process PSS only. It is not a full-host connection-capacity guarantee.

## `lwip-p4.yml`

P4 has two independent jobs.

### `pure-c-contract`

`scripts/validate-p4-cc.sh` builds `src/cc/` as its own CMake project with `-ffreestanding -fno-builtin`, an explicit include allowlist, warnings-as-errors, and a zero-undefined-symbol archive gate.

The conventional Reno contract covers init, slow start, congestion avoidance, loss, timeout, MSS changes, transport cwnd limits, saturation, invalid arguments, failed-init handle safety, explicit cwnd/ssthresh publication, and no pacing request.

### `integrated-recovery`

This job builds `tcp-shift-p2` against the pinned+controlled-patch lwIP tree and runs real TUN fault injection.

Retained P4 evidence includes:

```text
fast-loss: payload_bytes=262144 cc_loss_events=1 cc_timeout_events=0 recovery=ok
RTO:       payload_bytes=262144 cc_loss_events=0 cc_timeout_events=2 recovery=ok
```

Both require exact stream integrity and external packet drops. They never call controller loss/RTO functions directly.

## `lwip-p5.yml`: P5a delivery ledger

P5a builds the real P2 runtime with send/fully-ACKed segment observation hooks and qualifies the delivery ledger through three workloads:

1. natural 256-KiB echo;
2. the already-qualified fast-loss fault injection;
3. the already-qualified RTO fault injection.

Every workload requires:

```text
delivered_payload_bytes == 262144
metadata_bytes_per_slot == 32
metadata_alloc_failures == 0
metadata_misses == 0
metadata_abandoned_slots == 0
clock_errors == 0
timestamp_regressions == 0
live_slots == 0
```

The natural path requires zero retransmit events. Fast-loss/RTO require at least one retransmit event while `acked_segment_events == first_tx_events` and delivered bytes remain unique.

Final P5a behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed:

```text
upstream provenance  34821205386  success
P0                   34821205401  success
P1                   34821205357  success
P2                   34821205366  success
P3                   34821205396  success
P4                   34821205367  success
P5                   34821205375  success
```

P5 run `34821205375`, job `103903019956`, artifact `10338108552` retained:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
RTO:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
```

### Harness failure found before qualification

The first P5 workflow run was marked success by GitHub even though the log printed `P5 delivery-ledger qualification failed`. Two harness defects caused the false green:

- a greedy `sed` expression looking for `live_slots=` matched the later token `peak_live_slots=`;
- `check_delivery ... | tee ...` returned `tee`'s zero status rather than the failed checker's status under POSIX `sh`.

Qualification was withheld. The harness now parses exact whitespace-delimited `key=value` tokens with `awk`, writes the summary directly from `check_delivery`, then `cat`s it. A checker failure therefore terminates the step nonzero.

The artifact upload also originally found no files because `.build` is hidden and `actions/upload-artifact` excluded hidden paths. P5 now sets `include-hidden-files: true`, and the final run retained the full diagnostics.

This failure is part of the CI evidence: workflow conclusion alone is not accepted when log semantics contradict the intended gate.

## P5b CI: rate sample + app-limited — next

P5b must keep all prior gates and retain structured rate-sample evidence for:

- newly delivered bytes per ACK;
- delivery interval and send interval;
- selected bytes/second sample and validity flag;
- delayed ACK / ACK aggregation;
- ACKs covering multiple segments;
- retransmitted segments without duplicate delivered accounting;
- partial ACK handling;
- latest valid RTT observation;
- prior inflight/loss observations;
- app-limited enter/exit state.

Normal, delayed/aggregated ACK, fast-loss, RTO, and app-limited workloads must produce inspectable sample traces. A rate value is not qualified merely because it is nonzero.

## P5c CI: event-driven pacing

Pacing CI must prove both timing correctness and efficient wakeup behavior.

Target implementation is one process-wide deadline heap plus one one-shot `CLOCK_MONOTONIC` timerfd registered in the existing epoll owner.

Required observations include:

- timerfd arm/disarm/rearm counts;
- timer expirations and pacing wakeups;
- packets/bytes released per wakeup;
- requested vs actual send deadline and lateness distribution;
- heap size/peak size;
- idle epoll wakeups with no paced traffic;
- CPU under idle, small-packet, and high-BDP loads.

Hard design constraints:

- no one-timerfd-per-flow model;
- no periodic 1-ms/10-ms tick;
- no busy spin;
- timer is disarmed when no pacing deadline exists;
- TUN/backend readiness remains event-driven.

A later experiment may replace epoll's lwIP timeout argument with one unified one-shot timerfd for both lwIP and pacing deadlines, but only if regression CI proves equal timeout semantics and fewer/equal idle wakeups.

## Gate policy

A failure must identify the violated contract. Threshold changes require evidence explaining whether the implementation legitimately grew or the previous threshold was wrong. Do not weaken a gate solely to make CI green.

When a workflow exposes a harness assumption rather than a product defect, fix the harness while retaining or strengthening the intended assertion. Preserve the failing diagnostics that established the distinction.

For merge decisions, a GitHub `success` conclusion is necessary but not sufficient when the log itself reports a qualification failure; the intended assertions must be demonstrably fail-closed.
