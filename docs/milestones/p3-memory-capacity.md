# P3: memory and capacity baseline

Status: **runner-qualified**.

## Goal

Establish the real memory and capacity envelope of the P2 dual-stack bridge before adding congestion-control metadata, delivery-rate sampling, or pacing.

The target deployment class is 32/64/128-MiB constrained VPS/container hosts. P3 replaces rough one-process RSS observations with staged measurements that separate tcp-shift process residency from backend Linux TCP state and controlled application-data residency.

P3 does not claim a full-host capacity number from process RSS/PSS. Backend Linux kernel socket memory, the backend application, and provider-specific host overhead remain outside tcp-shift process PSS and must be budgeted separately.

## Measurement topology

The qualification harness separates public client Linux sockets from the runtime/backend namespace:

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

Process memory comes from `/proc/<pid>/status` and `/proc/<pid>/smaps_rollup`. Runtime-namespace `ss` and `/proc/net/sockstat` describe the backend Linux TCP leg. This prevents the public client transport and backend transport from being collapsed into one socket count.

## Qualified evidence

Behavior head `775ea5832f7e308e2c908c2f5abedfa4175c69be` passed P3 run `34805306193`, job `103855926986`. The retained diagnostics artifact is `10332963208`.

### Idle established-flow slope

`scripts/p3-memory-baseline.sh` samples one long-lived process at 0, 8, 32, 64, and 128 established idle flows, then after full drain.

The final run retained:

```text
ready PSS:     262 KiB
8-flow PSS:    265 KiB
32-flow PSS:   273 KiB
64-flow PSS:   284 KiB
128-flow PSS:  307 KiB
drained PSS:   311 KiB
max idle PSS delta: 45 KiB
max idle PSS slope: 0.3515625 KiB/flow
fd count at 128 flows: 133
fd count after drain: 5
backend tcp_inuse at 128 flows: 256
```

The low idle PSS slope is only tcp-shift userspace residency. It is not total host memory per connection; every backend flow also creates ordinary Linux TCP kernel state.

### Controlled public-to-backend residency

`scripts/p3-public-to-backend-residency.sh` holds eight flows while the backend does not read. Each public client attempts a 1-MiB send. The bridge must encounter real backend `EAGAIN` and retain public lwIP pbufs instead of growing an application buffer.

Final retained evidence:

```text
idle PSS: 257 KiB
active PSS: 549 KiB
active PSS delta: 292 KiB = 36.5 KiB/flow
bridge_peak_pending_public_bytes=262144  # 32 KiB/flow
bridge_backend_write_blocked_events=877
transferred: 8 MiB exactly
bridge errors: 0
```

### Controlled backend-to-public residency

`scripts/p3-backend-to-public-residency.sh` requests a 4-KiB public receive buffer (Linux reports 8 KiB), half-closes the public write direction, then blocks public reads while the backend sends 1 MiB per flow. Server-side backend sockets are counted through the half-close state, including `CLOSE-WAIT`.

Final retained evidence:

```text
idle PSS: 261 KiB
active PSS: 558 KiB
active PSS delta: 297 KiB = 37.125 KiB/flow
backend Send-Q observation: 8019976 bytes
bridge_backend_read_blocked_events=3530
transferred: 8 MiB exactly
bridge errors: 0
```

The roughly 36-37 KiB/flow directional increments show that demand-backed TCP window/pbuf/send-segment residency dominates the idle bridge control-object cost.

### Repeated load/drain floor

`scripts/p3-repeated-drain.sh` keeps one tcp-shift process alive for three rounds of 128 established flows. Each round must return backend established count to zero and fd count to the ready floor.

Final retained evidence:

```text
ready PSS: 258 KiB, fd=5
round 1 idle/drain: 305 / 310 KiB
round 2 idle/drain: 309 / 310 KiB
round 3 idle/drain: 309 / 315 KiB
first-to-last drain growth: 5 KiB
maximum drain floor above ready: 57 KiB
```

The capacity model promotes this from observation to a regression gate:

```text
first-to-last drain PSS growth <= 32 KiB
maximum warm drain floor above ready <= 128 KiB
```

These limits are intentionally wider than the retained 1-5 KiB ratchet and 53-57 KiB floor observations from multiple runner executions, while still detecting material allocator/fd lifecycle regressions.

### CPU baseline

`scripts/p3-cpu-baseline.sh` records process CPU ticks for one second of idle runtime and for four concurrent request/reply flows. The representative workload is 2048 synchronous 64-byte request/echo operations with TCP_NODELAY.

Final run:

```text
idle CPU over 1 second: 0 ticks / 0 ms
work CPU: 7 ticks / 70 ms
work CPU per operation: 34.1796875 us/op
wall elapsed: 78.57 ms
wall throughput: ~26.1k ops/s
bytes each direction: 131072
```

This is a runner baseline for regression comparison, not a provider performance guarantee.

## Capacity model and P4 admission

`scripts/p3-capacity-model.py` combines the retained idle, active, repeated-drain, and CPU artifacts. It deliberately models tcp-shift process PSS only.

Final conservative inputs from run `34805306193`:

```text
warm fixed process PSS: 315 KiB
conservative idle PSS slope: 0.398438 KiB/flow
controlled active payload PSS delta: 37.125 KiB/flow
fully-window-resident process slope: 37.523438 KiB/flow
qualified window residency: 32768 bytes/flow
```

The model evaluates 25% and 50% tcp-shift process budgets for 32/64/128-MiB hosts. Those are planning fractions, not product limits; the remaining RAM is intentionally reserved for kernel/backend/application and other unmeasured residency.

For the conservative P4 admission case — a 32-MiB host with only 25% (8 MiB) assigned to tcp-shift process PSS — 128 fully-window-resident flows project to 5118 KiB. The model therefore retains 3074 KiB of process-budget headroom, or about 24.016 KiB per active flow, before future CC/sampler/pacer structures.

The same model projects approximately 209 fully-window-resident flows inside that 8-MiB tcp-shift process budget. This is not a 209-connection full-host guarantee because backend kernel/application memory is explicitly excluded.

## Exit criteria

P3 exit criteria are satisfied on the GitHub runner:

- fixed ready/warm process memory is measured;
- idle established-flow memory is measured at multiple connection-count stages;
- both active data directions have controlled window-pressure residency measurements;
- three repeated 128-flow load/drain rounds have hard no-ratchet/floor gates;
- idle and representative small-operation CPU are retained;
- a machine-readable 32/64/128-MiB process-budget sensitivity model exists;
- the conservative 32-MiB/25% planning case leaves explicit per-flow headroom for P4/P5 metadata.

P4 may therefore introduce the generic congestion-control boundary. Provider/OpenVZ qualification remains separate and P3 must not be cited as a full-host memory guarantee.

## BBR relevance

P3 establishes the pre-BBR memory and CPU baseline. BBR still requires high-resolution timestamps, delivered accounting, per-segment metadata, rate sampling, app-limited detection, pacing, and loss/inflight signals. Future phases must measure their incremental cost against this baseline instead of assuming the metadata is free.
