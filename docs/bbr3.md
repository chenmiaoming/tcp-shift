# BBRv3 implementation track

Status: **model foundation only; not selectable for production**.

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

## Required before exposing `bbr3`

1. Implement the complete BBRv3 ProbeBW state machine, including
   phase transitions, ACK-phase feedback, per-cycle time and long-term
   inflight probing. Keep transport-independent state separate from the
   shared epoll/pacer and lwIP adapter.
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

The first increment is safe to merge ahead of these tasks precisely because
it has **no effect on live traffic or controller selection**.
