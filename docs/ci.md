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

## Upstream provenance

The production baseline is `.lwip-baseline`, currently pinning lwIP commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`.

`scripts/fetch-lwip.sh` checks out that exact commit, records `.build/upstream.env`, hashes critical pristine TCP sources, then applies `patches/lwip-p4-cc-hooks.patch`.

The provenance workflow proves:

- dependency `HEAD` equals the pin;
- patch path and SHA256 match the repository artifact;
- `git diff --check` and reverse-apply checks pass;
- only `src/core/tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- an independent worktree created from the same pinned commit, after applying the repository patch, is byte-identical to all modified files in the build workspace.

P5b remains within this same three-file patch boundary. It adds host-order sequence information to the existing successful-TX observation; it does not add a patched upstream file.

## P0/P1/P2 regression layers

P0 builds the constrained dual-stack `NO_SYS=1` lwIP source allowlist with warnings-as-errors, configuration/source contracts, binary-size/RSS gates, and staged smoke artifacts. The two diagnostics caused by intentionally disabled RS/SLAAC in pinned `nd6.c` remain visible but are source-specifically nonfatal; project code remains under `-Werror`.

P1 qualifies the privileged L3 packet/lifecycle path: real TUN TX `EAGAIN`, bounded FIFO, RX drops, IPv4/IPv6 ICMP/TCP, exact nft ingress, forwarding preflights, extension-header-safe IPv6 TCP traversal, cleanup/collision invariants, and routed ICMPv6 PTB learning.

P2 qualifies the real public-stream -> `127.0.0.1` bridge for IPv4/IPv6, bidirectional backpressure, half-close, refusal/reset recovery, concurrent flows, active shutdown, and reuse. P4 overlays controller ownership on every P2 workload:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

## P3 resource qualification

P3 measures tcp-shift process residency and CPU separately from backend Linux kernel/application memory. Runtime/backend and public-client sockets are separated in namespaces so socket-state accounting remains interpretable.

Workloads include 0/8/32/64/128 idle flows, both active-window directions, three 128-flow connect/idle/drain rounds, idle/small-operation CPU, and constrained-host process-PSS modeling.

Current P5b rerun, `34843587033`, job `103974029078`, artifact `10346739278`:

```text
warm fixed process PSS: 347 KiB
idle 128-flow PSS delta: 65 KiB
public->backend active: 36.5 KiB/flow
backend->public active: 37.125 KiB/flow
conservative active slope: 37.710938 KiB/flow
128 active projected PSS: 5174 KiB
8-MiB process budget remaining: 3018 KiB = 23.578 KiB/flow
3x128 first->last drain growth: 5 KiB
max warm-floor delta: 77 KiB
idle CPU: 0 ticks/s
```

The model remains tcp-shift process PSS only. Backend kernel/application and provider memory are excluded.

## P4 controller qualification

`pure-c-contract` builds `src/cc/` as its own CMake project with `-ffreestanding -fno-builtin`, an explicit include allowlist, warnings-as-errors, and zero undefined archive symbols.

`integrated-recovery` builds the real P2 runtime and uses external TUN packet loss. Retained P4 evidence includes:

```text
fast-loss: payload_bytes=262144 cc_loss_events=1 cc_timeout_events=0 recovery=ok
RTO:       payload_bytes=262144 cc_loss_events=0 cc_timeout_events=2 recovery=ok
```

The tests do not call controller loss/RTO handlers directly.

## P5a delivery-ledger qualification

P5a qualified a retransmission-safe lazy sidecar through natural, fast-loss, and RTO traffic. Final behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38`, run `34821205375`, job `103903019956`, artifact `10338108552` retained exact unique 262144-byte delivery with zero live slots/errors.

The first P5 workflow was falsely green despite logging a qualification failure. A greedy parser matched `live_slots` inside `peak_live_slots`, and a `check_delivery | tee` pipeline masked the checker exit status. Qualification was withheld until exact-token parsing and fail-closed summary generation were in place. Hidden `.build` artifacts are explicitly included in uploads.

## P5b rate-sampler qualification — complete

`.github/workflows/lwip-p5.yml` is now the P5 rate-sampler workflow. It builds the real P2 runtime and runs four fail-closed workloads:

1. natural 256-KiB delivery;
2. fast-loss fault injection;
3. RTO fault injection;
4. an event-driven application-pause workload.

The natural/loss/RTO checker requires both delivery and sampler invariants:

```text
delivered_payload_bytes == 262144
metadata_bytes_per_slot == 56
metadata_alloc_failures == 0
metadata_misses == 0
metadata_abandoned_slots == 0
clock_errors == 0
timestamp_regressions == 0
live_slots == 0
rate_samples > 0
valid_samples > 0
invalid_samples == 0
max_rate_bytes_per_sec > 0
last_interval_ns > 0
last_ack_interval_ns > 0
```

Fault paths also require real retransmission metadata. RTO must produce at least one retransmitted rate candidate; the retained run produced three.

Final behavior head `327efe7e29adfc230e7d201b466f2bd4980e976c` passed:

```text
upstream provenance  34843587049  success
P0                   34843587091  success
P1                   34843587026  success
P2                   34843587113  success
P3                   34843587033  success
P4                   34843587045  success
P5                   34843586990  success
```

P5 run `34843586990`, job `103974027867`, artifact `10347003115` retained:

```text
normal:
  samples=138 valid=138 invalid=0
  delivered=262144 max_rate=696998778 B/s

fast-loss:
  samples=121 valid=121 invalid=0
  retransmit_events=1 cc_loss_events=1 recovery=ok

RTO:
  samples=135 valid=135 invalid=0
  retransmit_events=4 retransmitted_rate_samples=3
  cc_timeout_events=2 recovery=ok
```

### Event-driven app-limited gate

`scripts/p5-rate-sampler-smoke.sh` uses a backend that sends 4096 bytes, pauses for 0.8 seconds, then sends 131072 bytes. The bridge marks app-limited only when the real `MSG_PEEK | MSG_DONTWAIT` path reaches `EAGAIN`; the adapter still checks transport capacity/no unsent data before setting its delivered+inflight marker.

During a 300-ms interval entirely inside the backend pause, the test samples `/proc/$PID/stat`. Hard gate: CPU delta <= 1 tick. Retained result:

```text
client/backend payload: 135168 bytes exact
rate_samples=71 valid_samples=71 invalid_samples=0
app_limited_samples=7
app_limited_enters=2
app_limited_exits=2
max_rate_bytes_per_sec=633180764
pause_cpu_ticks=0
event_driven=ok
```

This is evidence that app-limited classification does not require a periodic product polling loop.

## P5c CI: event-driven pacing — next

Pacing CI must prove timing correctness and efficient wakeup behavior. The target implementation is one process-wide deadline heap plus one one-shot `CLOCK_MONOTONIC` timerfd registered in the existing epoll owner.

Required observations:

- timerfd create/arm/disarm/rearm counts;
- timer expirations and pacing wakeups;
- packets/bytes released per wakeup;
- requested vs actual send deadline and lateness distribution;
- heap current/peak size;
- stale/cancelled entry handling;
- idle epoll wakeups with no paced traffic;
- CPU under idle, small-packet, and high-BDP loads;
- P3 memory deltas for scheduler state.

Hard design constraints:

- one process-wide timerfd, never one per flow;
- no periodic 1-ms/10-ms tick;
- no busy spin;
- timer disarmed when no pacing deadline exists;
- TUN/backend readiness remains event-driven;
- timerfd/epoll objects remain outside `src/cc/`.

A deterministic integration controller/test policy may request a fixed nonzero pacing rate to qualify scheduler mechanics before BBR exists. The production Reno baseline must continue to request zero pacing and preserve the unpaced regression path.

## Gate policy

A failure must identify the violated contract. Threshold changes require evidence explaining whether the implementation legitimately grew or the previous threshold was wrong. Do not weaken a gate solely to make CI green.

When a workflow exposes a harness assumption rather than a product defect, fix the harness while retaining or strengthening the intended assertion. Preserve the failing diagnostics that established the distinction.

For merge decisions, a GitHub `success` conclusion is necessary but not sufficient when the log itself reports a qualification failure; the intended assertions must be demonstrably fail-closed.
