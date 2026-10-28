# BBRv3 implementation track

Status: **independent model, ProbeBW and controller policy under qualification; not selectable for production**.

## User-visible controller names

- `cc = "bbr"` continues to select the existing compact BBRv1-style
  implementation. Its policy, default availability, recovery behavior and
  GitHub/OpenVZ regression baseline are unchanged.
- `cc = "bbr3"` is the **reserved future name** for a new controller with
  independent state and algorithms. It must fail closed until the entire
  BBRv3 controller is implemented and separately qualified; do not quietly
  alias it to the existing `bbr`.
- `reno` and `cubic` remain unchanged.

BBRv3 is specified by an active Internet-Draft, not an RFC:
[draft-ietf-ccwg-bbr-06](https://datatracker.ietf.org/doc/html/draft-ietf-ccwg-bbr-06)
(July 2026). Its model, recovery and ProbeBW design differs materially from
the Linux-mainline BBRv1-style baseline used by tcp-shift today. Google's
[BBRv3 Linux release](https://github.com/google/bbr/blob/v3/net/ipv4/tcp_bbr.c)
is the implementation reference. Pin exact reference revisions before
differential testing and record deviations from the draft explicitly.

## Implemented in the first increment

`src/cc/bbr3_model.h` and `.c` are a new standalone pure-C model.
Nothing in `src/cc/bbr.*`, `src/cc/bbr_controller.*`, or the production
selector was replaced.

- Independent BBRv3 phase vocabulary: STARTUP, DRAIN, ProbeBW_DOWN,
  ProbeBW_CRUISE, ProbeBW_REFILL, ProbeBW_UP, ProbeRTT.
- A two-ProbeBW-cycle maximum delivery-rate filter, deliberately distinct
  from the existing BBR model's 10-packet-round horizon.
- Application-limited admission for bandwidth samples, packet-round
  observation and a three-round / 25%-growth full-pipe estimator.
- Independent 5-second probe-RTT and 10-second minimum-RTT filters.
- Fixed-rational mode pacing gains and the 15% inflight headroom helper
  (minimum one MSS, floor four MSS), with saturating arithmetic.
- A deterministic freestanding C contract executed alongside existing P6
  model qualification.

These are *components*, not a complete congestion controller. The cycle
advance function is intentionally explicit: the eventual ProbeBW state
machine decides when one complete ProbeBW cycle finishes. The model does not
advance cycles on every ACK or unilaterally schedule ProbeRTT.

## Second increment: ProbeBW scheduler (separate from production)

`src/cc/bbr3_probe.h` and `.c` implement a transport-independent
scheduler for the DOWN/CRUISE/REFILL/UP phases, checked with
`scripts/bbr3-probe-contract.sh`. It uses:

- Randomized **2–3 second** ProbeBW wait or **Reno-coexistence packet-round**
  bound (at most 63 rounds), with a per-flow seed supplied by the eventual
  adapter; deterministic in tests.
- DOWN → CRUISE only after both estimated BDP and long-term inflight
  headroom constraints hold; time-to-probe has precedence in DOWN.
- Exactly one new packet-timed round in REFILL before UP; UP leaves after
  qualified excess loss or a persistent 25% growth plateau. Application-limited
  samples cannot falsely end the UP plateau check.
- ACK-phase states to guard max-bandwidth filter aging: an initial DOWN from
  Startup must not expire a ProbeBW cycle, and filter advancement after UP
  requires complete, non-application-limited ACK feedback.
- Explicit qualified congestion input, *not* invented loss from iperf3's
  loopback socket. The actual loss/ECN detector and long-term bound adaptation
  still require another independently qualified step.

This pure-C scheduler does **not** own an lwIP PCB, modify the current BBR
selector, or activate a listener. The full 2.11/5.3.3 behavior additionally
requires the real SACK/RACK loss/ECN integration, long-term inflight slope,
ACK aggregation and congestion model described below.

## Third increment: congestion model (not yet bound to lwIP)

`src/cc/bbr3_congestion.h` and `.c` provide an independent
pure-C loss/ECN congestion model. Inputs are explicitly **public-leg**
transport observations, not the loopback `iperf3` TCP_INFO retransmit field.
The future lwIP adapter must supply validated SACK/RACK loss evidence,
transmit-time inflight, authenticated packet-timed delivery boundaries,
and CE bytes **only after ECN negotiation and transport eligibility**.

- Short-term `bw_shortterm` / `inflight_shortterm` loss adaptation occurs
  at most once per completed loss round, and never while probing bandwidth.
  The update preserves the highest delivery demonstrated in that round.
  `inflight_latest` uses **RS.delivered over the sampled flight**, not just
  the bytes newly ACKed by one ACK; the latter is separately tracked for CE
  and ACK accounting.
- Explicit independent `inflight_longterm` upper-bound tracking rejects
  unqualified, repeated, or app-limited shrinkage; safe ProbeBW_UP feedback
  may raise an established bound conservatively with an increasing slope.
- The CE input is gated by explicit ECN eligibility; a per-round alpha
  estimator can adjust short-term inflight limits. This **does not** turn
  on BBRv3 ECN on public TCP connections.
- Recovery snapshot/undo, unbounded sentinel handling, stage-specific
  short-term rate limits and cwnd caps are unit-tested with 64-bit bytes.
- `scripts/bbr3-congestion-contract.sh` runs as a freestanding strict-C
  contract beside the existing P4/P6 quality gates.

The actual `bbr3` selector and runtime remain unavailable. The remaining
work includes connecting validated evidence to the adapter, aligning
congestion timing with the ProbeBW ACK-phase machine, full ProbeRTT and
ACK aggregation, and differential testing against a pinned Linux BBRv3
revision. In particular this module does not inspect kernel `TCP_INFO` or
infer public-path retransmission counts from an application socket.

## Fourth increment: independent controller orchestration

`src/cc/bbr3_controller.h` / `.c` now compose the isolated
delivery/RTT model, ProbeBW scheduler and congestion bounds in a pure-C
controller. The input requires a validated public-leg SACK/RACK rate sample
and separately qualified loss/CE counters; the output describes an intended
cwnd, pacing rate, send quantum and app-limited marker. No lwIP PCB or
transport timer is passed into the CC core.

- Startup → Drain → ProbeBW scheduling, including the three-round Drain
  fallback once the bandwidth/RTT model is populated.
- A ten-packet-round ACK-aggregation maximum filter (one round in Startup).
  The aggregation episode restarts when delivery drops below expectation,
  rather than inflating the allowance indefinitely.
- ProbeRTT is triggered by the independent 5-second probe-RTT filter expiry,
  using a 0.5 BDP target, a four-MSS floor, at least 200ms spent below the
  target, **and** a completed packet-timed round. Exiting restores cwnd and
  returns to ProbeBW_CRUISE or Startup as appropriate.
- BBRv3 pacing gain with a 1% margin, slow increase before filling the pipe,
  inflight based on gain × BDP plus ACK aggregation, and an adaptive
  1ms/2-MSS–64KiB send-quantum policy. These are policy outputs only.
- `scripts/bbr3-controller-contract.sh` exercises the resulting phase
  machine, aggregation, ProbeRTT and validated-loss rejection independently
  of the existing `bbr` controller.

**Not production-ready:** the final lwIP adapter still needs loss recovery
conservation, explicit transport pacing, long-flow/netns fault testing,
idle/transmit handling, and differential reference tests. In particular,
the current `cc = "bbr"` remains completely separate and the `bbr3`
selector stays disabled.

## Fifth increment: pinned-lwIP ACK observation bridge (qualification only)

The new `src/lwip/bbr3_binding.{h,c}` uses the *actual pinned lwIP*
`struct tcp_pcb` and the existing P5 SACK-aware
`tcp_shift_cc_ack` / `tcp_shift_cc_rate_sample` interfaces to generate
BBRv3 controller **policy candidates**. It is built and tested in the
default-build P6 exposure job, but is **not part of the runtime adapter
library or production selection path**.

The binding checks PCB/adapter identity and lifetime, MSS, monotonic ACK
timestamps and delivery counters, SACK-aware newly delivered payload,
valid interval and send-time inflight evidence. It explicitly preserves
the distinction between `RS.delivered` (sampling-flight volume) and
this ACK's uniquely delivered bytes; a SACK-only delivery cannot be
counted again on cumulative ACK. Invalid snapshots do not reach the
controller or advance its delivery model. The computed policy is never
written to `pcb->cwnd`, `pcb->ssthresh`, or the actual pacer.

The currently available generic lwIP hook ABI only delivers a coarse
`on_loss(lost_bytes)` event and a boolean `on_ecn()` event. These lack
the necessary per-packet send-time loss context, ECN-qualified CE *bytes*,
and delivery-ledger loss-round boundaries. The bridge therefore always
passes zero for loss/CE and marks their provenance unavailable. It does
**not** fabricate these values from `iperf3` or backend Linux TCP_INFO.

Before enabling real BBRv3 sends, extend the transport evidence path
with qualified RACK loss provenance, per-ACK CE volume (only if
negotiated), recovery state and cwnd-limited/ProbeBW transmit provenance;
then add an internal, opt-in BBRv3 adapter binding with real pacing and
conservation tests. The current `cc="bbr"` path remains unchanged.

## Sixth increment (part A): RACK-validated per-transmission loss evidence

`src/lwip/bbr3_rack_evidence.{h,c}` adds a small, **per-transmission**
qualification state record that consumes the existing RFC 8985
`tcp_shift_rack_loss_remaining()` verdict. Unlike coarse
`on_loss(lost_bytes)`, it associates first-time RACK-confirmed loss
with the original send-time `tx_ns`, inflight snapshot, sequence range,
retransmission generation, app-limited flag and ProbeBW transmission
provenance. Partial SACK-delivered payload is subtracted, and later
cumulative ACKs cannot count those same bytes as another loss.

Repeated RACK timer callbacks for one transmit generation produce zero
new loss, but a *new* lost retransmission can be classified independently.
A separately proven DSACK/spurious-loss event may revoke the corresponding
generation once. The RFC 8985 RTO special rule and an unqualified TLP
event are explicitly **not** treated as RACK-verified BBRv3 evidence.

The deterministic contract calls the **actual** `rack_tlp.c` implementation
and tests immature/mature timing, SACK/ACK deduplication, retransmitted
attempts, sequence wrap, repeated callbacks and spurious-loss undo.
See `scripts/bbr3-rack-evidence-contract.sh`.

**Integration still required:** wire this state into the existing P5
delivery slots without duplicating the segment queue, generate and
validate per-ACK loss rounds, and feed the correct loss/inflight pair to
the separate BBRv3 controller. For now the module is qualification-only:
it is not registered as a live CC hook, and does not change cwnd,
pacing, Reno/CUBIC, or compact BBR. In particular the pending loss
model must not infer a sample-wide loss ratio by summing unrelated
per-segment denominators.

## Sixth increment (part B): real P5 delivery-slot observation

The existing P5 segment sidecar now refreshes `prior_inflight_bytes` on
**every retransmission** using its unique outstanding payload ledger;
a retransmitted range is not counted as newly transmitted payload again.
Each slot and adapter also carry a small monotonically advancing
per-transmission ID. This avoids treating the original transmission's
send-time inflight as the retransmission's measurement.

`tcp_shift_lwip_cc_bbr3_observe_rack_loss()` is an **explicit, opt-in
qualification interface** over those exact P5 slots. It requires the
real connection's SACK negotiation, its active SACK-aware delivery
policy, a valid pinned RACK timestamp and a mature RFC 8985 verdict.
It returns the loss bytes not already SACK/ACK-delivered and a send-time
snapshot, deduplicated by slot attempt; no second segment allocation
or new send queue is required. On retransmission the slot gets a fresh
transmission ID and the once-only gate resets. Without known ProbeBW
send-time provenance, `probe_sample` is always false — no inference.

The new `tcp-shift-bbr3-p5-loss-observation-contract` uses actual
pinned-lwIP send/SACK/ACK hooks and checks that first-send loss,
partially delivered retransmissions and repeated RACK polling preserve
inflight, SACK deduplication and immutable live cwnd. The existing
P6 delivery-snapshot contract separately asserts that RTO and
partial-ACK retransmission rate samples use the **current** inflight.

**Still excluded:** this observer is not invoked by the live TCP hook
table, has no CE or real ProbeBW send-phase provenance, and does not yet
publish a packet-timed loss round to the independent BBRv3 controller.
Loss records from different TX inflight denominators must not be
naively summed into a fabricated loss ratio. The next integration
gate is a validated loss-round event ledger and recovery lifecycle.

## Required before exposing `bbr3`

1. Complete BBRv3 ProbeBW integration: bind per-flow observations to the
   pure-C scheduler, implement cautious exponential inflight_longterm slope,
   upper/lower bounds, pacing/cwnd coupling, and reference differential tests.
2. Implement loss/ECN congestion signals, inflight_longterm/shortterm,
   bw_shortterm, recovery-aware cwnd, ACK aggregation and ProbeRTT
   scheduling. Extend transport observations only when they have a
   defined source in the existing SACK/RACK delivery ledger.
3. Bind the complete controller as a **separate** `bbr3` listener choice
   through the ordinary p2 IPv4/IPv6 path; do not add `bbr3` to the
   pure-C Reno/CUBIC registry merely to bypass adapter recovery semantics.
4. Qualify lossless, app-limited, high-BDP, ACK compression, loss,
   ECN-on/off, cross-traffic fairness, first-send loss, RTO/ProbeRTT,
   netns/TUN teardown and memory cost against pinned Linux BBRv3.
5. Only then make `cc = "bbr3"` valid at runtime in default builds.
   `cc = "bbr"` must remain binary/behaviorally unchanged.

Both increments remain safe to merge ahead of these tasks precisely because
neither has any effect on live traffic or controller selection.
