# CI architecture

`tcp-shift` CI is evidence-driven: upstream provenance is independent from product behavior, diagnostics survive failure, and observed resource behavior becomes a hard gate only after a workload is defined and measured.

## Principles

1. Pin upstream exactly and retain provenance.
2. Keep project-owned lwIP modifications explicit and mechanically bounded.
3. Validate source/configuration contracts before network behavior.
4. Preserve P0/P1/P2/P3 regressions while adding later transport features.
5. Retain failure artifacts rather than rerunning away useful evidence.
6. Separate process-PSS planning from full-host memory claims.
7. Keep pure controller logic independently buildable from lwIP/Linux runtime code.
8. Qualify real packet-path events for ACK/loss/RTO/rate sampling; synthetic controller calls are not enough for integration claims.

## `lwip-upstream.yml`

The production baseline is `.lwip-baseline`, currently pinning lwIP commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`.

`scripts/fetch-lwip.sh` checks out that exact commit, records `.build/upstream.env`, and hashes critical pristine TCP sources before applying `patches/lwip-p4-cc-hooks.patch`.

The provenance workflow then proves:

- dependency `HEAD` equals the pin;
- patch path and SHA256 match the repository artifact;
- `git diff --check` and reverse-apply checks pass;
- only `src/core/tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- an independent worktree created from the same pinned commit, after applying the repository patch, is byte-identical to all three modified files in the build workspace.

Final P4 behavior-head provenance run `34815149550`, job `103884173736`, passed; artifact `10335752874` retains the pin, pristine hashes, patch metadata, applied diff, and modified-file list.

This replaces the old pre-P4 assumption that `.deps/lwip` must remain a clean working tree after fetch.

## `lwip-p0.yml`

P0 builds the constrained lwIP source allowlist with warnings-as-errors, runs compile-time configuration contracts, source-surface checks, binary-size/RSS gates, and stages an exact runtime artifact for a separate smoke job.

The pinned `nd6.c` unit emits two diagnostics when RS/SLAAC are deliberately disabled. Only those upstream diagnostics are downgraded from errors; they remain visible. Project code and other warnings stay under `-Werror`.

Final P4 behavior-head P0 run: `34815149474` — success.

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

Final P4 behavior-head P1 run `34815149646`, job `103884174271`, passed the full regression suite.

## `lwip-p2.yml`

P2 qualifies the real public-stream -> `127.0.0.1` backend bridge for both public address families.

The gate set includes:

- natural IPv4 and IPv6 bidirectional echo/teardown;
- real bidirectional backpressure;
- backend-first half-close without `EPOLLRDHUP` spin;
- backend refusal then healthy fresh flow;
- public/backend reset recovery;
- eight simultaneously established flows;
- SIGTERM with an active flow and explicit bridge cleanup;
- 64 sequential reuse flows without sustained VmRSS ratcheting.

P4 adds a controller-ownership gate over every P2 workload. For every runtime diagnostics file:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

This prevents a green bridge test from silently exercising native lwIP congestion policy instead of the project controller.

Final P4 behavior-head P2 run `34815149444`, job `103884173371`, passed; artifact `10335932665` retains bridge and CC diagnostics.

## `lwip-p3.yml`

P3 measures tcp-shift process residency and CPU separately from backend Linux kernel/application memory. Runtime/backend and public-client sockets are placed in separate network namespaces so socket-state accounting remains interpretable.

Workloads include:

- 0/8/32/64/128 idle established flows;
- public->backend active pressure with real backend `EAGAIN`;
- backend->public active pressure with blocked public reads;
- three rounds of 128 connect/idle/drain flows in one process;
- idle CPU plus 2048 small request/echo operations;
- constrained-host process-PSS sensitivity modeling.

Pre-P4 admission baseline:

```text
warm fixed process PSS: 315 KiB
fully-window-resident slope: 37.523438 KiB/flow
128 active projected PSS: 5118 KiB
8-MiB process budget remaining: 3074 KiB
```

With the integrated P4 adapter, final behavior-head P3 run `34815149825`, job `103884174726`, artifact `10335968830` retained:

```text
warm fixed process PSS: 335 KiB
conservative idle slope: 0.523438 KiB/flow
controlled active payload delta: 36.625 KiB/flow
fully-window-resident slope: 37.148438 KiB/flow
128 active projected PSS: 5090 KiB
8-MiB process budget remaining: 3102 KiB = 24.234 KiB/flow
three-round drain growth: 5 KiB
maximum warm drain floor above ready: 69 KiB
```

The model remains tcp-shift process PSS only. It is not a full-host connection-capacity guarantee.

## `lwip-p4.yml`

P4 has two independent jobs.

### `pure-c-contract`

`scripts/validate-p4-cc.sh` builds `src/cc/` as its own CMake project with `-ffreestanding -fno-builtin`, an explicit include allowlist, warnings-as-errors, and a zero-undefined-symbol archive gate.

The conventional Reno state-machine contract covers init, slow start, congestion avoidance, loss, timeout, MSS changes, transport cwnd limits, saturation, invalid arguments, failed-init handle safety, explicit cwnd/ssthresh publication, and no pacing request.

Expected core evidence includes:

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
external_symbols=0
```

### `integrated-recovery`

This job builds `tcp-shift-p2` against the pinned+controlled-patch lwIP tree and runs `scripts/p4-cc-recovery-smoke.sh` as root so the harness can create TUN state and isolated host firewall fault injection.

`fast-loss` drops one data packet while later packets pass, forcing duplicate ACKs and native fast retransmit/recovery. Final behavior-head evidence:

```text
payload_bytes=262144
cc_bindings=1
cc_ack_events=108
cc_loss_events=1
cc_timeout_events=0
cc_controller_errors=0
recovery=ok
```

`rto` blocks response data past the pinned initial RTO and then restores delivery. Final evidence:

```text
payload_bytes=262144
cc_bindings=1
cc_ack_events=144
cc_loss_events=0
cc_timeout_events=2
cc_controller_errors=0
recovery=ok
```

Both workloads require exact client/backend echo integrity and at least one packet dropped by the external fault rule. They never call controller loss/RTO functions directly.

Final P4 behavior-head run `34815149645` passed both jobs. Artifacts:

- pure-C boundary: `10335288981`;
- integrated recovery: `10336213268`.

## Final P4 behavior-head matrix

Behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` retained:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

After this head, documentation-only commits may rely on these behavioral runs if a compare proves no product source, scripts, patches, or workflows changed.

## P5 CI growth

P5 must keep all P0-P4 gates and add structured evidence for delivery-rate sampling and pacing prerequisites.

Required retained observations should include:

- monotonic send/ACK timestamp samples;
- cumulative delivered bytes;
- per-segment send/delivery snapshots;
- ACK-derived delivery interval and rate;
- app-limited transitions;
- inflight/loss observations consumed by the controller boundary;
- pacing enqueue/dequeue/deadline/error counters;
- timerfd wakeups and idle behavior;
- fixed/per-flow/per-segment process-memory increments;
- CPU under idle, small-packet, and high-BDP workloads.

Pacing accuracy must not be obtained by busy spinning. Prefer one process-wide scheduler/timerfd and qualify idle wakeups explicitly.

## Gate policy

A failure must identify the violated contract. Threshold changes require evidence explaining whether the implementation legitimately grew or the previous threshold was wrong. Do not weaken a gate solely to make CI green.

When a workflow exposes a harness assumption rather than a product defect, fix the harness while retaining or strengthening the intended behavioral assertion. Preserve the failing diagnostics that established the distinction.
