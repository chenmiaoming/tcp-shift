# P4: generic congestion-control boundary

Status: **active; standalone policy-library increment runner-qualified, lwIP adapter next**.

## Goal

Introduce a mechanically enforceable congestion-control policy boundary before delivery-rate sampling, pacing, or BBR-specific state exists.

P4 is successful when congestion-control policy can be built and tested as pure C independently from lwIP and the Linux runtime, and when a thin lwIP adapter can delegate conventional congestion-window decisions through that boundary without regressing the P0-P3 transport behavior or consuming the P3 memory headroom unexpectedly.

## Hard boundary

`src/cc/` is policy code, not a second network stack and not a runtime module. It may use ISO C integer/size types and its own headers only. It must not include or call:

- lwIP headers or `tcp_pcb`/`tcp_seg` objects;
- Linux, POSIX socket, TUN, epoll, timerfd, or nftables APIs;
- bridge/runtime/process lifecycle objects;
- heap allocation owned by the controller;
- a pacing scheduler.

Controller state is caller-owned. The controller consumes transport observations and publishes policy. P5 may extend ACK observations with qualified delivery-rate samples, but the runtime remains responsible for timestamps and pacing mechanics.

## First increment: standalone policy library — complete

The initial `src/cc/` API contains:

- `tcp_shift_cc_transport`: MSS, bytes in flight, and peer send-window observations;
- init parameters: initial cwnd, initial ssthresh, and minimum cwnd;
- ACK, loss, and retransmission-timeout events;
- policy output: cwnd, ssthresh, and optional pacing rate in bytes/second;
- an ops table and caller-owned opaque controller state.

`ssthresh` is explicit policy so a native transport recovery engine can consume the controller's threshold without inspecting opaque controller state. A pacing rate of zero means the controller does not request pacing. P4's conventional controller leaves pacing zero; no timer or scheduler is introduced here.

The first controller is a small byte-counting Reno baseline. It exists to validate the generic boundary, not to claim Linux Reno equivalence. Its state is four `uint32_t` values (16 bytes): cwnd, ssthresh, congestion-avoidance ACK accumulator, and configured minimum cwnd.

The conventional behavior contract covers:

- capped byte-counted slow-start growth;
- additive congestion-avoidance growth after one cwnd of acknowledged bytes;
- loss reduction using the effective min(cwnd, peer window) and a two-MSS ssthresh floor;
- timeout collapse to minimum cwnd;
- path-MSS changes without platform callbacks;
- saturating 32-bit byte accounting;
- rejected invalid input/state-size contracts;
- failed initialization leaving the generic CC handle non-callable;
- explicit cwnd/ssthresh publication;
- no pacing request.

## Standalone qualification

`scripts/validate-p4-cc.sh` configures `src/cc/` as its own CMake project. It compiles with warnings-as-errors plus `-ffreestanding -fno-builtin`, runs the controller contract binary, records archive symbols/size, and requires the static archive to have no undefined external symbols.

The script also scans every C/header include in `src/cc/` against an explicit allowlist. This prevents a later convenience include from silently coupling the policy library to lwIP or Linux.

Final standalone behavior head `9e8cb2fa6091418ac4ed3dcb52f963337fdc25d0` passed P4 run `34810033939`, job `103869414629`; artifact `10334775895` retained the standalone evidence. The key output was:

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
controller=reno
state_bytes=16
external_symbols=0
P4 standalone congestion-control boundary passed
```

P0 run `34810033921` and full P1 run `34810033970` also passed on the same head. This qualifies the standalone generic boundary only. The archive size is an object-code diagnostic, not a P3 process-memory replacement; native lwIP still owns the public-side cwnd until the adapter increment is qualified.

## Next increment: lwIP adapter

Pinned lwIP currently owns cwnd directly in several places:

- connection establishment initializes `pcb->cwnd` with lwIP's initial-cwnd calculation;
- ACK processing in `tcp_in.c` performs slow start and congestion avoidance;
- fast retransmit/recovery changes ssthresh/cwnd;
- retransmission timeout handling in `tcp.c` reduces ssthresh/cwnd;
- `tcp_out.c` gates transmission with `min(snd_wnd, cwnd)`.

P4 must not fork these paths wholesale. The next increment will identify the smallest explicit hook/adapter surface that lets the conventional controller own policy decisions while lwIP keeps retransmission, recovery, queueing, sequence-space, packet output, and SACK/recovery mechanics.

The adapter may depend on lwIP; `src/cc/` may not. Adapter state cost and any `tcp_pcb`/extension-state cost must be measured against the P3 24-KiB/active-flow planning headroom. Integrated CI must demonstrate real public-side traffic, ACK/loss/timeout transitions, and unchanged dual-stack/P2 bridge behavior where affected.

## Exit criteria

P4 is complete when all of the following are retained in CI:

- standalone pure-C `tcp_shift_cc` library with no lwIP/Linux dependency and no unresolved external symbol — **qualified**;
- conventional controller state-machine contracts — **qualified**;
- a narrow, documented lwIP adapter/hook surface rather than scattered CC policy;
- conventional controller ownership of public-side cwnd through the adapter under real P2 bridge traffic;
- loss/timeout/ACK state-transition evidence through the integrated transport path;
- P0/P1/P2/P3 regressions remain green where behavior is affected;
- incremental fixed/per-flow process memory remains inside the P3 planning budget;
- repository docs describe exactly which recovery semantics remain native lwIP responsibilities.

## Explicitly deferred to P5

P4 does not implement high-resolution send timestamps, delivered-byte snapshots, ACK delivery-rate estimation, app-limited detection, a pacing heap, timerfd scheduling, or BBR state/modes. Those are P5 prerequisites and need their own memory/CPU qualification.

## Stop signal

If conventional-controller integration already requires invasive rewrites of lwIP retransmission/recovery/SACK mechanics rather than narrow policy hooks, stop and reassess the architecture before adding BBR-specific complexity.
