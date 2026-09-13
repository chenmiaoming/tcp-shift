# P3: memory and capacity baseline

Status: **active**.

## Goal

Establish the real memory and capacity envelope of the P2 dual-stack bridge before adding congestion-control metadata, delivery-rate sampling, or pacing.

The target deployment class is 32/64/128-MiB constrained VPS/container hosts. P3 must replace rough one-process RSS observations with staged, reproducible measurements that separate fixed process cost, public lwIP flow cost, backend Linux socket cost, and application-data residency.

P3 does not tune memory merely to improve a headline number. Measurements come first; configuration changes require evidence that they improve the target workload without invalidating P1/P2 transport behavior.

## Measurement model

The baseline distinguishes four contributors:

1. fixed tcp-shift process/lwIP/runtime mappings and heap;
2. incremental public-side lwIP TCP/bridge state per established flow;
3. backend Linux TCP socket/kernel state;
4. demand-backed application data in lwIP pbuf/send queues and backend socket buffers.

`sizeof()` values alone are not accepted as per-flow memory evidence. The primary userspace metrics are process RSS, PSS, private clean, private dirty, and anonymous memory from `/proc/<pid>/smaps_rollup`, together with live bridge counters and flow counts.

Backend kernel TCP state is recorded separately from public client TCP state. The first harness uses two network namespaces:

```text
client namespace
  Linux public clients
        |
        | veth
        v
runtime namespace
  routing/forwarding -> TUN -> lwIP public TCP
                              |
                              v
                         bridge flow
                              |
                              v
                    127.0.0.1 backend TCP
```

The public client Linux sockets therefore live in the client namespace, while runtime-namespace `ss` and `/proc/net/sockstat` describe the backend leg. This prevents the two independent TCP transports from being collapsed into one kernel-socket count.

## First increment: idle established-flow slope

`scripts/p3-memory-baseline.sh` creates one long-lived tcp-shift runtime and samples these default stages:

```text
ready:    0 established flows
idle-8:   8 established flows
idle-32:  32 established flows
idle-64:  64 established flows
idle-128: 128 established flows
drained:  0 established flows after all 128 have closed
```

Each public client connection is held idle after both public and backend handshakes complete. No application payload is intentionally queued for the idle samples.

For every sample the harness records:

- `VmRSS` from `/proc/<pid>/status`;
- `Rss`, `Pss`, `Private_Clean`, `Private_Dirty`, and `Anonymous` from `smaps_rollup`;
- tcp-shift fd count and process CPU ticks;
- exact backend `ESTABLISHED` count from `ss` in the runtime namespace;
- runtime-namespace TCP `inuse`, `tw`, `alloc`, and `mem` values from `/proc/net/sockstat`.

The harness writes machine-readable `measurements.tsv` and `summary.json` artifacts. The summary computes max-stage deltas and per-flow slopes relative to the ready process, plus post-drain PSS/private-dirty deltas.

The first CI runs are observational. They enforce measurement consistency and transport cleanup, but do not invent a memory threshold before repeated data exists.

## Required invariants

Every P3 measurement run must preserve the already-qualified transport invariants:

- all expected public flows are accepted and all backend connects succeed;
- the backend established count at each idle stage exactly equals the staged flow count;
- no bridge backend/public errors occur;
- after drain, bridge active-flow and pending-public-byte counters return to zero;
- process shutdown reports zero bridge active/pending state;
- namespace/TUN resources are cleaned by the harness.

A measurement is invalid if the workload did not actually reach the intended transport state.

## Planned increments

After the idle-established slope is repeatable, P3 adds controlled workloads rather than mixing all variables at once:

- active-flow residency with fixed public-to-backend and backend-to-public inflight bytes;
- repeated load/drain rounds in one process to identify allocator floors or fragmentation;
- higher staged connection counts until an explicit resource or memory ceiling is reached;
- IPv4 versus IPv6 public-flow memory delta using the same backend shape;
- idle CPU and small-packet CPU measurements with explicit elapsed time/work counts;
- a capacity model for 32/64/128-MiB targets with reserved headroom for the application and future CC/sampler/pacer metadata.

Kernel socket memory must be interpreted separately from tcp-shift process PSS. If the runner does not expose a reliable per-socket kernel-memory signal, P3 records the observable socket counts and documents that limitation rather than attributing kernel memory to userspace RSS.

## Exit criteria

P3 is complete when the repository contains reproducible evidence for:

- fixed idle process memory;
- incremental idle-established flow memory across multiple connection-count stages;
- active-flow data residency under controlled inflight bytes;
- post-drain memory floors across repeated rounds;
- a measured connection-capacity curve sufficient to reason about 32/64/128-MiB targets;
- idle and representative small-packet CPU behavior;
- explicit remaining memory budget before P4/P5 add CC, sampler metadata, and pacing structures.

Only after those measurements show adequate headroom should the project proceed to the generic congestion-control boundary.

## BBR relevance

P3 is a prerequisite to BBR rather than BBR implementation work. BBR requires additional per-flow and per-segment metadata plus pacing infrastructure. Without a measured pre-BBR memory slope, later memory growth cannot be attributed or bounded.

If the existing lwIP/bridge design already consumes too much memory at realistic connection counts, the correct action is to fix or stop the design before adding BBR complexity.
