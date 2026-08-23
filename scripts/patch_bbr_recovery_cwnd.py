#!/usr/bin/env python3
"""Run BBR cwnd control during gVisor fast recovery after packet conservation.

Linux BBR's custom cong_control path runs bbr_set_cwnd() on ACKs even while TCP
is in Recovery. Only the first packet-timed recovery round is restricted to
packet conservation; after bbr_update_bw() starts the next round and clears
packet_conservation, normal BBR cwnd growth resumes while Recovery may still be
active.

gVisor's generic congestionControl.Update() is deliberately skipped whenever
FastRecovery.Active is true. tcp-shift already runs its delivery-rate consumer
before legacy/RACK recovery retransmits, so this narrow adapter performs only
the missing BBR cwnd step there after the conservation round. Open-state ACKs
continue to use Update(), avoiding double growth.
"""

from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def patch_tcp_stats(path: Path) -> None:
    text = path.read_text()
    anchor = "\tTCPShiftBBRStartupPacingFloorHolds         *StatCounter\n"
    addition = anchor + """\tTCPShiftBBRRecoveryCwndUpdates             *StatCounter
\tTCPShiftBBRRecoveryCwndAckedPacketsSum     *StatCounter
\tTCPShiftBBRRecoveryCwndGrowthPacketsSum    *StatCounter
\tTCPShiftBBRRecoveryCwndNoGrowth            *StatCounter
"""
    text = replace_once(text, anchor, addition, "BBR recovery cwnd diagnostics")
    path.write_text(text)


def patch_bbr(path: Path) -> None:
    text = path.read_text()

    # Factor the existing open-state cwnd rule into a helper so the recovery
    # custom-control path can apply exactly the same model, without duplicating
    # policy or changing generic Update() semantics.
    old_update = """// Update implements congestionControl.Update.
func (b *bbrState) Update(packetsAcked int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
\tb.updateMinRTT(rtt, ackTime)
\tb.maybeReinitPacingFromRTT(rtt)
\tb.updateMode(ackTime)

\tif packetsAcked <= 0 || b.mode == bbrProbeRTT || b.packetConservation || b.recoveryEntryPending {
\t\treturn
\t}

\ttarget := b.bdpPackets(bbrCwndGain)
\tinFlight := b.s.linuxLikePacketsInFlight()
\tswitch b.mode {
\tcase bbrStartup:
\t\tb.s.SndCwnd += packetsAcked
\t\tif b.maxBW != 0 && b.s.SndCwnd > 2*target {
\t\t\tb.s.SndCwnd = 2 * target
\t\t}
\tcase bbrDrain, bbrProbeBW:
\t\tif b.s.SndCwnd < target {
\t\t\tb.s.SndCwnd += packetsAcked
\t\t\tif b.s.SndCwnd > target {
\t\t\t\tb.s.SndCwnd = target
\t\t\t}
\t\t} else if b.s.SndCwnd > 2*target && inFlight < b.s.SndCwnd {
\t\t\tb.s.SndCwnd = max(target, inFlight+packetsAcked)
\t\t}
\t}
\tif b.s.SndCwnd < 4 {
\t\tb.s.SndCwnd = 4
\t}
\tb.s.Ssthresh = b.s.SndCwnd
}
"""
    new_update = """func (b *bbrState) applyCwndControl(packetsAcked int) {
\tif packetsAcked <= 0 || b.mode == bbrProbeRTT || b.packetConservation || b.recoveryEntryPending {
\t\treturn
\t}

\ttarget := b.bdpPackets(bbrCwndGain)
\tinFlight := b.s.linuxLikePacketsInFlight()
\tswitch b.mode {
\tcase bbrStartup:
\t\tb.s.SndCwnd += packetsAcked
\t\tif b.maxBW != 0 && b.s.SndCwnd > 2*target {
\t\t\tb.s.SndCwnd = 2 * target
\t\t}
\tcase bbrDrain, bbrProbeBW:
\t\tif b.s.SndCwnd < target {
\t\t\tb.s.SndCwnd += packetsAcked
\t\t\tif b.s.SndCwnd > target {
\t\t\t\tb.s.SndCwnd = target
\t\t\t}
\t\t} else if b.s.SndCwnd > 2*target && inFlight < b.s.SndCwnd {
\t\t\tb.s.SndCwnd = max(target, inFlight+packetsAcked)
\t\t}
\t}
\tif b.s.SndCwnd < 4 {
\t\tb.s.SndCwnd = 4
\t}
\tb.s.Ssthresh = b.s.SndCwnd
}

// Update implements congestionControl.Update for the generic open-state path.
func (b *bbrState) Update(packetsAcked int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
\tb.updateMinRTT(rtt, ackTime)
\tb.maybeReinitPacingFromRTT(rtt)
\tb.updateMode(ackTime)
\tb.applyCwndControl(packetsAcked)
}
"""
    text = replace_once(text, old_update, new_update, "factor BBR cwnd control")

    old_tail = """\tif b.packetConservation {
\t\t// Subsequent ACKs in the first recovery round may grow cwnd only enough
\t\t// to replace packets proven delivered, matching Linux's
\t\t// max(cwnd, tcp_packets_in_flight(tp) + acked).
\t\tif target > b.s.SndCwnd {
\t\t\tb.s.SndCwnd = target
\t\t}
\t\tb.s.Ssthresh = b.s.SndCwnd
\t}
}

func (b *bbrState) updateBandwidth(rs deliveryRateSample) {
"""
    new_tail = """\tif b.packetConservation {
\t\t// Subsequent ACKs in the first recovery round may grow cwnd only enough
\t\t// to replace packets proven delivered, matching Linux's
\t\t// max(cwnd, tcp_packets_in_flight(tp) + acked).
\t\tif target > b.s.SndCwnd {
\t\t\tb.s.SndCwnd = target
\t\t}
\t\tb.s.Ssthresh = b.s.SndCwnd
\t\treturn
\t}

\t// gVisor suppresses congestionControl.Update() while FastRecovery.Active.
\t// Linux BBR does not suppress its custom bbr_set_cwnd() there: once
\t// bbr_update_bw() has started the second recovery round it clears packet
\t// conservation and BBR resumes normal slow-start/target cwnd control. The
\t// delivery consumer is tcp-shift's custom cong_control-equivalent, so apply
\t// that missing cwnd step here before recovery transmits more data.
\tbefore := b.s.SndCwnd
\tb.applyCwndControl(acked)
\tstats.TCPShiftBBRRecoveryCwndUpdates.Increment()
\tstats.TCPShiftBBRRecoveryCwndAckedPacketsSum.IncrementBy(nonNegativeUint(acked))
\tif b.s.SndCwnd > before {
\t\tstats.TCPShiftBBRRecoveryCwndGrowthPacketsSum.IncrementBy(nonNegativeUint(b.s.SndCwnd - before))
\t} else {
\t\tstats.TCPShiftBBRRecoveryCwndNoGrowth.Increment()
\t}
}

func (b *bbrState) updateBandwidth(rs deliveryRateSample) {
"""
    text = replace_once(text, old_tail, new_tail, "BBR cwnd control after conservation round")

    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_bbr_recovery_cwnd.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a patched gVisor source tree: {root}")

    patch_tcp_stats(root / "pkg/tcpip/tcpip.go")
    patch_bbr(tcp / "bbr.go")
    print(f"patched BBR cwnd control after recovery conservation round at {root}")


if __name__ == "__main__":
    main()
