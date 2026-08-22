#!/usr/bin/env python3
"""Restore BBR's last-known-good cwnd after gVisor RTO recovery.

Linux BBR saves prior_cwnd before loss recovery and restores it when TCP exits
Loss/Recovery. gVisor's congestion-control interface has a PostRecovery hook
for fast recovery, but the RTORecovery -> Open transition does not call it.
This narrow BBR-only adapter uses gVisor's existing recover boundary
(FastRecovery.Last < SndUna) from sender ACK processing to perform the missing
restore inside BBR.Update(). It does not alter gVisor loss detection,
retransmission, or recovery admission.
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
    anchor = "\tTCPShiftBBRFullBWExitRoundSum             *StatCounter\n"
    addition = anchor + """\tTCPShiftBBRRTORecoveryEntries             *StatCounter
\tTCPShiftBBRRTOPriorCwndSum                *StatCounter
\tTCPShiftBBRRTORecoveryExits               *StatCounter
\tTCPShiftBBRRTORecoveredCwndSum             *StatCounter
"""
    text = replace_once(text, anchor, addition, "BBR RTO cwnd diagnostic counters")
    path.write_text(text)


def patch_bbr(path: Path) -> None:
    text = path.read_text()

    text = replace_once(
        text,
        "\trecoveryEntryPending bool\n}\n",
        "\trecoveryEntryPending bool\n\trtoRecovery          bool\n}\n",
        "BBR RTO recovery state",
    )

    old_update = """func (b *bbrState) Update(packetsAcked int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
\tb.updateMinRTT(rtt, ackTime)
\tb.updateMode(ackTime)
"""
    new_update = """func (b *bbrState) Update(packetsAcked int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
\t// gVisor ends timeout recovery when cumulative ACK progress passes the
\t// recover point saved in FastRecovery.Last. Unlike fast-recovery exit, that
\t// RTORecovery -> Open transition does not invoke congestionControl.PostRecovery.
\t// Restore BBR's saved model cwnd here, on the same ACK immediately before
\t// sender.handleRcvdSegment changes the generic state to Open.
\tif b.rtoRecovery && b.s.state == tcpip.RTORecovery && b.s.FastRecovery.Last.LessThan(b.s.SndUna) {
\t\trestored := max(b.recoveryPriorCwnd, 4)
\t\tif restored > b.s.SndCwnd {
\t\t\tb.s.SndCwnd = restored
\t\t}
\t\tb.s.Ssthresh = b.s.SndCwnd
\t\tstats := b.s.ep.stack.Stats().TCP
\t\tstats.TCPShiftBBRRTORecoveryExits.Increment()
\t\tstats.TCPShiftBBRRTORecoveredCwndSum.IncrementBy(nonNegativeUint(b.s.SndCwnd))
\t\tb.recoveryPriorCwnd = 0
\t\tb.rtoRecovery = false
\t}

\tb.updateMinRTT(rtt, ackTime)
\tb.updateMode(ackTime)
"""
    text = replace_once(text, old_update, new_update, "BBR RTO cwnd restore on ACK")

    # patch_inflight_accounting.py owns the RTO diagnostic timestamp and is
    # applied before this adapter, so match the generated post-patch source.
    old_rto = """func (b *bbrState) HandleRTOExpired() {
\t// Diagnostic timestamp only: BBR's bandwidth/round state intentionally
\t// survives RTO exactly as before this instrumentation.
\tb.lastRTO = b.s.ep.stack.Clock().NowMonotonic()
\tb.inRecovery = false
\tb.recoveryPriorCwnd = 0
\tb.packetConservation = false
\tb.recoveryEntryPending = false
\tb.roundStart = false

\tb.s.SndCwnd = 1
"""
    new_rto = """func (b *bbrState) HandleRTOExpired() {
\t// Diagnostic timestamp only: BBR's bandwidth/round state intentionally
\t// survives RTO exactly as before this instrumentation.
\tb.lastRTO = b.s.ep.stack.Clock().NowMonotonic()

\t// Linux BBR's ssthresh callback saves the last-known-good cwnd before the
\t// TCP core collapses cwnd for timeout recovery. If this RTO interrupted fast
\t// recovery, gVisor has just called leaveRecovery(), so SndCwnd is already the
\t// BBR cwnd restored by PostRecovery. Preserve the maximum across repeated
\t// RTOs until cumulative ACK progress passes the recover boundary.
\tprior := max(b.recoveryPriorCwnd, b.s.SndCwnd, 4)
\tb.recoveryPriorCwnd = prior
\tb.rtoRecovery = true
\tstats := b.s.ep.stack.Stats().TCP
\tstats.TCPShiftBBRRTORecoveryEntries.Increment()
\tstats.TCPShiftBBRRTOPriorCwndSum.IncrementBy(nonNegativeUint(prior))

\tb.inRecovery = false
\tb.packetConservation = false
\tb.recoveryEntryPending = false
\tb.roundStart = false

\tb.s.SndCwnd = 1
"""
    text = replace_once(text, old_rto, new_rto, "BBR save cwnd on RTO")

    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_bbr_rto_cwnd.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a patched gVisor source tree: {root}")

    patch_tcp_stats(root / "pkg/tcpip/tcpip.go")
    patch_bbr(tcp / "bbr.go")
    print(f"patched BBR RTO prior-cwnd save/restore at {root}")


if __name__ == "__main__":
    main()
