# P4: lwIP congestion-control adapter increment

Status: **active; not yet qualified**.

## Scope

This increment connects the already-qualified pure-C controller boundary to real public-side lwIP TCP without moving retransmission, fast-recovery, SACK, sequence-space, segment-queue, or packet-output mechanics out of lwIP.

The integration shape is deliberately narrow:

- `src/cc/` remains platform independent and independently buildable;
- one `tcp_pcb` ext-arg slot carries an opaque tcp-shift hook pointer;
- patched pinned lwIP calls that hook only at normal ACK congestion-policy update, fast-retransmit loss policy, and RTO policy update sites;
- an unbound PCB executes the original native lwIP blocks unchanged;
- the bridge raw-API accept boundary binds the conventional controller before handing an established child PCB to the existing bridge callback;
- controller state is owned by the adapter and freed by the PCB ext-arg destroy callback;
- fast-recovery cwnd inflation/deflation remains native lwIP behavior around the controller-owned base cwnd/ssthresh policy.

The pinned upstream commit remains unchanged as provenance. `patches/lwip-p4-cc-hooks.patch` is applied only after pristine critical-source hashes are recorded, and its own SHA-256 plus the applied diff are retained as build evidence.

## Current implementation decisions

The generic transport observation now carries `cwnd_limit_bytes`. This is a platform-independent capability, not an lwIP type leak: the current no-window-scale lwIP build has a 16-bit `tcpwnd_size_t`, so the controller must converge internally to the transport's representable limit instead of relying on adapter-side truncation.

The bridge target alone redirects its `tcp_accept()` registration to `tcp_shift_lwip_cc_accept()`. P0/P1/probe PCBs never bind controller state and therefore remain native lwIP CC behavior, although all PCBs carry the one reserved ext-arg slot.

## Qualification still required

Before this increment is mergeable CI must retain:

- exact patch apply/check evidence against the pinned lwIP commit;
- standalone P4 controller contract still green;
- P0 and full P1 regressions green with the reserved ext-arg slot and patched-but-unbound hooks;
- real P2 IPv4 and IPv6 bridge traffic with controller bindings and ACK policy updates observed;
- deterministic integrated fast-loss and RTO policy events, not synthetic direct controller calls;
- no controller errors/fallback on qualified traffic;
- P2 bridge/backpressure/half-close/reset/reuse regressions unchanged;
- fixed/per-flow process-memory delta measured against the P3 baseline and comfortably inside the ~24 KiB/active-flow planning headroom.

If deterministic loss/RTO qualification requires rewriting lwIP recovery rather than packet-path fault injection around the existing transport, stop and reassess instead of weakening the gate.
