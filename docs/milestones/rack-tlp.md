# RFC 8985 RACK-TLP

Status: active transport-recovery implementation; timer-driven RACK repair, tail-loss TLP, application-limited tail repair, lost-retransmission recovery, deterministic 28-drop recovery across Reno/CUBIC/internal-BBR, reordering/D-SACK adaptation, recovery-timer teardown, and incremental resource cost are live-qualified, experimental/default-OFF.

tcp-shift is moving sender loss detection from a fixed DupAck/SACK-count heuristic toward RFC 8985 RACK-TLP. This work is deliberately transport-level and independent of Reno, CUBIC, and BBR controller policy.

## Normative reference

The normative specification is RFC 8985, not Linux implementation behavior. Linux TCP remains a differential reference after the RFC contracts are implemented.

RFC 8985 requires SACK plus per-segment most-recent transmit timestamps. tcp-shift already has both prerequisites in the experimental sender-SACK path and the delivery sidecar:

- a sender SACK scoreboard/tagging path;
- stable sent-segment identity;
- nanosecond monotonic send timestamps;
- retransmission identity;
- cumulative and selective delivery accounting.

The implementation keeps RACK/TLP metadata out of upstream `struct tcp_seg` where practical so the lwIP fork remains bounded.

## Implementation phases

The PR is intentionally dedicated to RACK-TLP. It must not contain BBR gain changes, CUBIC changes, IW10 policy changes, or unrelated memory tuning.

Implementation phases:

1. **RFC 8985 core contracts**
   - RACK.segment ordering by latest transmit timestamp plus end-sequence tie break;
   - recent RTT and reordering-window state;
   - time-based loss deadline;
   - DSACK reordering-window adaptation;
   - PTO calculation and one-probe invariant;
   - TLP repaired-loss ACK classification.

2. **Per-segment integration**
   - reuse existing sidecar `tx_ns` as Segment.xmit_ts;
   - reuse retransmission identity;
   - expose Segment.end_seq and RACK lost state;
   - update RACK.segment on cumulative and selective delivery.

3. **Loss detection and recovery selection**
   - make RACK time evidence the loss oracle instead of the current fixed three-later-SACK proof;
   - fail closed when RACK timing/sidecar state is unavailable instead of silently falling back to the legacy three-later-SACK detector;
   - preserve the SACK scoreboard as required input;
   - do not run RFC 6675's recovery selection algorithm unchanged alongside RACK-TLP: RFC 8985 section 9.2 explicitly prohibits that combination because RFC 6675 does not handle lost retransmissions;
   - drive a modified retransmission selector from RACK-marked loss state;
   - use the RACK reordering timer when loss is not yet mature.

4. **TLP / PTO**
   - multiplex RACK/PTO/RTO timing without per-flow busy polling;
   - prefer new data when peer receive window permits;
   - otherwise retransmit the highest-sequence sent segment;
   - permit at most one extra probe beyond cwnd;
   - always fall back to the ordinary RTO after a probe.

5. **RTO and congestion-control integration**
   - on RTO, mark SND.UNA lost unconditionally and only mark other segments when the RACK deadline has elapsed;
   - feed the initial congestion event into Reno/CUBIC/BBR without coupling RACK to controller internals;
   - when RACK proves that a retransmission was itself lost, deliver the additional congestion response required by RFC 8985 section 9.3 without starting a second transport recovery episode;
   - reset TLP state on connection start, fast recovery, and RTO recovery.

6. **Qualification**
   - tail loss that previously requires RTO;
   - lost retransmission, including the required additional congestion response;
   - reordering below and above the reordering window;
   - DSACK adaptation;
   - deterministic 28-drop reference with exact retransmission accounting;
   - memory and timer-cost regression gates.

## Current boundary

The implementation is beyond the core-math-only stage. The experimental build now provides:

- transport-neutral RFC 8985 RACK/TLP state and deterministic contracts;
- per-segment latest-transmit timestamps and delivery ordering through the existing sidecar;
- cumulative/SACK delivery processing in the RFC-required timing pass followed by the reordering pass;
- RACK loss deadlines and a process-wide one-shot recovery timer integrated with the existing epoll owner;
- timer-driven RACK repair when loss matures below the ordinary DupThresh path;
- on a RACK-enabled connection that negotiated SACK, the pinned lwIP DupAck threshold no longer independently enters fast recovery: RACK owns fast-loss entry, while RFC 8985's SACK-count rule still operates inside `RACK.reo_wnd`, a no-SACK DupAck is forwarded to the section 7.4.2 TLP ambiguity check, and native Reno/CUBIC recovery-window inflation is retained only after RACK has already entered `TF_INFR`; non-RACK or non-SACK connections keep the ordinary DupAck fallback;
- RACK-enabled retransmission selection now treats RFC 8985 time evidence as the sole fast-loss oracle: an unavailable RACK status is "not yet proven lost", not permission to fall back to the legacy fixed three-later-SACK rule; ordinary RTO remains the conservative terminal fallback;
- the build boundary now matches that ownership: `TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE` supplies SACK negotiation/scoreboard/delivery evidence, while `TCP_SHIFT_EXPERIMENTAL_RACK_TLP` selects RFC 8985 recovery. The former fixed-count compatibility option and deprecated recovery alias are removed and fail closed if supplied;
- the code boundary now matches the build boundary as well: RACK selects retransmissions only through `tcp_shift_tcp_select_rack_loss()` / `tcp_shift_tcp_rexmit_rack_loss()`; the fixed three-later-SACK selector and its retransmission wrapper are deleted, leaving only the neutral segment unlink/sorted-requeue/accounting primitive used by RACK;
- PTO/TLP scheduling with ordinary RTO as the conservative fallback;
- live tail-loss qualification proving one TLP retransmission repairs the tested tail loss without an RTO;
- live application-limited tail qualification where the final packet is first transmitted after the prior flight drains: the application-limited transition re-evaluates PTO, one TLP retransmission completes burst one before burst two is made available, the later ACK produces exactly one congestion indication, and the path records zero RTOs, zero unrelated qdisc drops, and exact payload integrity;
- an RFC-shaped lost-retransmission gate: one original segment and its first retransmission are dropped, a later lost original segment is successfully retransmitted to provide newer RACK timing evidence, and the first hole is then repaired again without RTO;
- the lost-retransmission gate requires exactly three injected drops, three retransmission events, two congestion-loss events, zero timeout fallback, zero unrelated qdisc drops, and exact payload integrity;
- the 28-drop gate exposed and now prevents a same-ACK ordering bug: lwIP parses SACK options before tcp_receive() advances the cumulative ACK, so processing each source as a separate RFC 8985 pass could advance RACK.fack from a higher SACK block and then misclassify the lower cumulatively ACKed segment from that same ACK as reordering; advancing mixed ACKs now defer RACK Step 2/Step 3 until both cumulative and selective delivery are marked, preserving the RFC-required combined ordering;
- after that fix, the established 260 ms / 10 Mbit/s / 4 MiB deterministic first-send reference is live-qualified with RACK-TLP enabled: 28 explicit drops, exactly 28 retransmissions, zero false reordering events, zero timeout/RTO fallback, zero unrelated qdisc drops, exact payload integrity, 7 controller recovery episodes, and 5.279809 Mbit/s goodput;
- on the same PR head, the sender-SACK-only tcp-shift reference measured 5.457767 Mbit/s with the same 7 recovery episodes, while the Linux BBR reference measured 5.493823 Mbit/s. The false recovery fragmentation is therefore closed; the remaining RACK-enabled goodput delta is retained as a performance follow-up rather than treated as an RFC correctness failure;
- the Reno/CUBIC controller-matrix gate exposed a second integration bug: the production transport-pacing selector replaced the base hook table without forwarding `on_sack` or `on_recovery_exit`, while the base SACK callback itself was incorrectly gated by the internal-BBR-only `sack_delivery_policy`; as a result, loss-based controllers negotiated SACK but RACK received no selective-delivery evidence and fell back to repeated RTO recovery;
- the selector now forwards the transport-recovery hooks, and RACK SACK observation is independent of whether the congestion controller consumes SACKed bytes as ACK credit. Reno/CUBIC continue to grow cwnd from cumulative ACKs, while RACK still receives the RFC 8985 scoreboard/timing evidence;
- the deterministic controller matrix is now live-qualified on the same 260 ms / 10 Mbit/s / 4 MiB / 28-drop shape: Reno completed with 28 retransmissions, zero false reordering, zero timeout fallback, zero qdisc drops, and exact payload integrity; CUBIC met the same invariants; internal BBR also met the same transport invariants. The measured Reno/CUBIC goodput on this roughly 1% deterministic-loss shape is intentionally not a parity target for BBR because their loss-based congestion responses differ by design;
- recovery-timer teardown is now qualified at both adapter and runtime layers: unbind cancels the exact `(flow_id, generation)` deadline before registry release; a stale callback after teardown is a no-op; registry-slot reuse increments the generation and an old callback cannot clear the new flow's live timer; the real epoll-owned recovery heap cancels one future event to an empty/disarmed state, then reuses the same single process-wide timerfd for one later release with one wakeup and one callback;
- incremental RACK resource cost is now fail-closed A/B qualified against the same default build. The validated 128-flow run measured +104 bytes of adapter state, +16 bytes of PCB state (+120 bytes static per flow total), +248 bytes of process-wide loop state, +20 KiB fixed process PSS, +15 KiB staged-idle PSS beyond default, and +12 KiB incremental post-drain PSS retention. Idle CPU remained zero ticks; small-operation CPU was identical to default at 31.73828125 us/op; the lossless workload created exactly one recovery timerfd and produced zero recovery timer arms, wakeups, expirations, callbacks, callback errors, or residual heap entries;
- the exposure-readiness audit identified an RFC 8985 section 7.3 timer-lifecycle gap: if a PTO fired but the fresh-RTT/probe guard prevented transmission, the callback returned without restarting lwIP's RTO age. The readiness fix resets the ordinary RTO age whenever that PTO opportunity still has data in flight, and also does so after a failed/successful probe attempt, preserving RTO as the last-resort timer required by the RFC;
- congestion-control callbacks kept separate from the transport loss detector; RACK reports the second congestion indication without moving the existing fast-recovery boundary;
- deterministic reordering below the active reordering window is live-qualified with observed reordering, zero retransmissions, zero congestion-loss events, zero RTOs, zero qdisc drops, and exact payload delivery;
- RFC 2883 D-SACK classification uses the cumulative ACK carried in the same packet rather than stale sender state;
- deterministic reordering beyond the initial reordering window is live-qualified to trigger bounded spurious recovery, D-SACK feedback, RACK.reo_wnd_mult growth, and persistence without RTO or qdisc loss.

The feature remains **experimental/default-OFF** while the RFC 8985 exposure-readiness audit closes remaining normative timer/recovery details and reruns full qualification. Provider/OpenVZ production evidence and any default/public exposure decision remain separate from the protocol implementation audit.

Accordingly, the correct current claim is **RFC 8985-driven experimental implementation with live timer-driven RACK repair, ordinary and application-limited tail-loss TLP, lost-retransmission recovery, exact 28-drop recovery accounting across Reno/CUBIC/internal-BBR, reordering/D-SACK adaptation, generation-safe recovery-timer teardown, and incremental resource cost qualified on deterministic paths**, not complete RFC 8985 or production conformance.
