# CUBIC HyStart and Reno benchmark

Status: active qualification checkpoint.

This increment keeps `cubic` as a selectable peer of `reno` and adds the classic HyStart detector used by current Linux `tcp_cubic` as a startup behavior checkpoint. It does not change the production default controller, which remains Reno.

## HyStart scope

The implementation uses the current Linux classic HyStart defaults:

- lower cwnd threshold: 16 SMSS,
- ACK-train spacing threshold: 2 ms,
- delay detector minimum samples: 8,
- delay threshold: `clamp(min_rtt / 8, 4 ms, 16 ms)`,
- both ACK-train and delay detectors enabled.

CUBIC currently requests no pacing rate, so the ACK-train detector uses the unpaced Linux threshold of half the minimum observed RTT. Raw RTT samples and cumulative delivery snapshots come through the generic transport-neutral ACK observation.

When HyStart exits the first slow start without loss, the controller sets `ssthresh = cwnd`, `cwnd_prior = cwnd`, `Wmax = cwnd`, and `K = 0` before entering CUBIC congestion avoidance, matching RFC 9438's first-lossless-exit rule.

This is intentionally classic HyStart, not HyStart++. RFC 9438 recommends HyStart++, but Linux `tcp_cubic` currently retains classic HyStart. HyStart++ remains a separate future qualification decision rather than a silent semantic replacement.

## Same-stack Reno benchmark

The CUBIC benchmark CI now has two independent references:

1. tcp-shift `cubic` versus Linux `tcp_cubic`, which exposes differences in both congestion control and the surrounding TCP stack;
2. tcp-shift `cubic` versus tcp-shift `reno`, which keeps the runtime, lwIP transport, TUN path, RTT, bottleneck rate, queue, payload, and deterministic loss pattern identical and therefore isolates the effect of controller selection more directly.

The same-stack benchmark reports completion goodput, CUBIC/Reno goodput ratio, final cwnd, controller loss events, RTO events, and deterministic forced-drop counters. Throughput ratio remains informational; payload integrity, controller selection, controller errors, and forced-loss evidence are hard failures.
