# CUBIC RFC 9438 / HyStart++ qualification

Status: hosted-runner standards-conformance checkpoint complete for the current RFC 9438/RFC 9406 controller profile; provider/OpenVZ exposure remains separate.

Production `cubic` is implemented as an RFC 9438 CUBIC controller over the shared lwIP transport/recovery boundary. Initial slow start uses RFC 9406 HyStart++. Reno remains the production default controller.

## Normative algorithm profile

The controller intentionally follows the current IETF CUBIC profile rather than treating Linux implementation details as the primary specification:

- CUBIC window constant: `C = 0.4`;
- multiplicative decrease: `beta_cubic = 0.7`;
- Reno-friendly alpha: `9/17` while `W_est < cwnd_prior`, then `1`;
- fast-convergence factor: `(1 + beta) / 2 = 0.85`;
- loss response uses `0.7 * flight_size` with the normal TCP lower bound;
- timeout response returns cwnd to one SMSS, retains the CUBIC beta rule for ssthresh, and starts the next CA epoch with `K = 0`;
- application-limited intervals do not advance CUBIC epoch time;
- the controller changes congestion-window policy but leaves TCP retransmission/recovery execution in the transport.

The fixed-point implementation uses Q16 windows and Q10 seconds. `TCP_SHIFT_CUBIC_SCALE=40960` is the exact integer form required for `C=0.4` in that representation.

## RFC 9406 HyStart++

Initial slow start implements RFC 9406 HyStart++ using the recommended constants:

- delay threshold: `clamp(lastRoundMinRTT / 8, 4 ms, 16 ms)`;
- minimum RTT samples per round: 8;
- CSS growth divisor: 4;
- CSS duration: at most 5 packet-timed rounds;
- non-paced ACK-growth limit: `L = 8`;
- actively paced ACK-growth limit: `L = infinity`;
- HyStart++ is used only for the initial slow start.

Production Reno/CUBIC now use the real shared transport pacer. The CUBIC adapter marks HyStart++ as paced only when the flow is registered with the scheduler and a finite nonzero pacing rate is actually installed; if pacing is unavailable, the model falls back to the RFC 9406 non-paced `L=8` rule.

The transport-neutral model represents the RFC 9406 sequence-number round boundary using cumulative delivered bytes plus the current in-flight snapshot. This is a deliberate abstraction and remains a conformance point to re-check under reordering/SACK qualification.

When HyStart++ confirms exit from the initial slow start without loss, the controller sets `ssthresh = cwnd`, `cwnd_prior = cwnd`, `Wmax = cwnd`, and `K = 0`, matching RFC 9438's lossless-startup handoff.

## Current conformance status

The core window algorithm is RFC-aligned and covered by deterministic contracts for CUBIC growth, Reno-friendly growth, beta reduction, K calculation, fast convergence, timeout behavior, application-limited epoch pausing, and HyStart++ CSS behavior.

The current scoped status is:

- RFC 3168 ECN negotiation/ECE/CWR/ECT(0) transport state and the CUBIC ECN congestion-event path are implemented behind `TCP_SHIFT_EXPERIMENTAL_ECN`. Hosted-runner real-TUN qualification covers single CE plus repeated independent CE episodes through the RFC 3168 one-SMSS retransmit-timer gate. Provider/OpenVZ exposure remains pending.
- Fast convergence remains enabled by default. RFC 9438 recommends disabling it in a known single-CUBIC-flow/no-cross-traffic environment. PR #96 proves that profile improves the controlled single-flow benchmark, while PR #97 shows no basis for globally disabling it under shared-bottleneck multi-flow traffic. tcp-shift has no reliable topology signal proving absence of path competition.
- HyStart++ packet-timed rounds use the delivery-domain abstraction described above rather than a literal transport `windowEnd = SND.NXT` sequence-number marker.
- Spurious-loss undo (for example DSACK/Eifel/F-RTO based restoration) is not implemented. RFC 9438 makes this optional, so its absence is not a normative violation, but it remains a robustness gap.
- TCP loss recovery remains transport-owned. Experimental/default-OFF RFC 8985 RACK-TLP supplies loss/retransmission selection, and RFC 9937 PRR controls RACK+SACK Reno/CUBIC recovery send credit. These are tracked independently from the CUBIC controller.

PRs #94-#101 also close the deterministic Linux CUBIC differential audit. Independent evaluators reproduce the RFC 9438 fixed-point congestion-avoidance state and FlightSize reduction with zero mismatches. RFC 9937 recovery reaches tcp-shift's own ssthresh within one MSS in every steady episode on the 28-drop path. Linux remains useful differential evidence, but the remaining throughput gap does not establish an RFC error in tcp-shift and is not a justification for replacing cumulative-new-ACK, FlightSize, or PRR semantics.

Accordingly, the current claim remains **RFC 9438 core-algorithm aligned with RFC 9406 HyStart++**, with experimental RFC 3168 ECN and separately qualified RFC 8985/RFC 9937 recovery integration; it is not a blanket whole-stack conformance claim.

## Linux differential benchmarks

Linux `tcp_cubic` remains a secondary differential/reference implementation, not the normative source of CUBIC semantics. CI compares tcp-shift CUBIC with Linux CUBIC across clean, loss, send-envelope, pacing, and same-stack controls to catch integration differences that pure model contracts cannot expose.

The same-stack Reno benchmark keeps runtime, lwIP transport, TUN path, RTT, bottleneck rate, queue, payload, and deterministic loss pattern identical, isolating controller selection more directly. Throughput ratios remain diagnostic; payload integrity, controller selection, controller errors, and controlled-loss evidence are hard failures.
