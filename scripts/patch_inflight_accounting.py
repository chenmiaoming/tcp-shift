#!/usr/bin/env python3
"""Add tcp-shift Linux-like inflight accounting and recovery diagnostics.

This patch is intentionally applied only to the full tcp-shift patchset. The
vanilla and rack-tiebreak-only profiles remain useful controls.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def patch_segment(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\trateAppLimited    bool                  `state:"nosave"`\n'
        '\trateSampleValid   bool                  `state:"nosave"`\n\n'
        '\t// acked indicates if the segment has already been SACKed.',
        '\trateAppLimited    bool                  `state:"nosave"`\n'
        '\trateSampleValid   bool                  `state:"nosave"`\n\n'
        '\t// Linux-like in-flight shadow state. RACK loss must survive the\n'
        '\t// retransmission path clearing seg.lost, while retransActive tracks\n'
        '\t// whether the latest retransmitted copy is still in flight.\n'
        '\tinflightRACKLost      bool `state:"nosave"`\n'
        '\tinflightRetransActive bool `state:"nosave"`\n\n'
        '\t// acked indicates if the segment has already been SACKed.',
        "segment inflight metadata",
    )
    text = replace_once(
        text,
        '\tt.rateAppLimited = s.rateAppLimited\n'
        '\tt.rateSampleValid = s.rateSampleValid\n'
        '\tt.ep = s.ep',
        '\tt.rateAppLimited = s.rateAppLimited\n'
        '\tt.rateSampleValid = s.rateSampleValid\n'
        '\tt.inflightRACKLost = s.inflightRACKLost\n'
        '\tt.inflightRetransActive = s.inflightRetransActive\n'
        '\tt.ep = s.ep',
        "segment clone inflight metadata",
    )
    path.write_text(text)


def patch_tcp_stats(path: Path) -> None:
    text = path.read_text()
    fields = '''type TCPStats struct {
	// tcp-shift experimental diagnostics. These counters are deliberately
	// cumulative so existing Stack.Stats() logging can expose recovery behavior
	// without adding per-packet logging to hot paths.
	TCPShiftRACKLossMarks              *StatCounter
	TCPShiftRACKEqualTimeCandidates    *StatCounter
	TCPShiftRACKRecoveryRetransmits    *StatCounter
	TCPShiftRecoveryEntries            *StatCounter
	TCPShiftRecoveryExits              *StatCounter
	TCPShiftSetPipeCalls               *StatCounter
	TCPShiftSetPipeMismatchCalls       *StatCounter
	TCPShiftSetPipeAbsGapSum           *StatCounter
	TCPShiftBBRSamples                 *StatCounter
	TCPShiftBBRInflightMismatchSamples *StatCounter
	TCPShiftBBRPriorInflightSum        *StatCounter
	TCPShiftBBRCurrentInflightSum      *StatCounter
	TCPShiftBBROutstandingSum          *StatCounter
'''
    text = replace_once(text, "type TCPStats struct {\n", fields, "TCP diagnostic stats")
    path.write_text(text)


def patch_rack(path: Path) -> None:
    text = path.read_text()
    old = '''\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && endSeq.LessThan(rc.EndSequence)) {
\t\t\ttimeRemaining := seg.xmitTime.Sub(rcvTime) + rc.RTT + rc.ReoWnd
\t\t\tif timeRemaining <= 0 {
\t\t\t\tseg.lost = true
\t\t\t\tnumLost++
'''
    new = '''\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && endSeq.LessThan(rc.EndSequence)) {
\t\t\tif seg.xmitTime == rc.XmitTime {
\t\t\t\trc.snd.ep.stack.Stats().TCP.TCPShiftRACKEqualTimeCandidates.Increment()
\t\t\t}
\t\t\ttimeRemaining := seg.xmitTime.Sub(rcvTime) + rc.RTT + rc.ReoWnd
\t\t\tif timeRemaining <= 0 {
\t\t\t\trc.snd.inflightMarkRACKLost(seg)
\t\t\t\trc.snd.ep.stack.Stats().TCP.TCPShiftRACKLossMarks.Increment()
\t\t\t\tseg.lost = true
\t\t\t\tnumLost++
'''
    text = replace_once(text, old, new, "RACK inflight loss accounting")
    path.write_text(text)


def patch_sender(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\tdeliveryRate      deliveryRateSample   `state:"nosave"`\n'
        '\trateCandidate     deliveryRateCandidate `state:"nosave"`\n',
        '\tdeliveryRate      deliveryRateSample   `state:"nosave"`\n'
        '\trateCandidate     deliveryRateCandidate `state:"nosave"`\n'
        '\tratePriorInFlight int                   `state:"nosave"`\n',
        "sender ACK-start prior inflight snapshot",
    )
    text = replace_once(
        text,
        '\ts.Outstanding = pipe\n}',
        '\ts.Outstanding = pipe\n\ts.recordSetPipeDiagnostics()\n}',
        "SetPipe diagnostics",
    )
    text = replace_once(
        text,
        '\ts.FastRecovery.Active = true\n\t// Save state to reflect we\'re now in fast recovery.',
        '\ts.FastRecovery.Active = true\n'
        '\ts.ep.stack.Stats().TCP.TCPShiftRecoveryEntries.Increment()\n'
        '\t// Save state to reflect we\'re now in fast recovery.',
        "recovery entry diagnostics",
    )
    text = replace_once(
        text,
        'func (s *sender) leaveRecovery() {\n\ts.FastRecovery.Active = false',
        'func (s *sender) leaveRecovery() {\n'
        '\ts.ep.stack.Stats().TCP.TCPShiftRecoveryExits.Increment()\n'
        '\ts.FastRecovery.Active = false',
        "recovery exit diagnostics",
    )
    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_inflight_accounting.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a gVisor source tree: {root}")

    patch_root = Path(__file__).resolve().parents[1] / "patches/gvisor/tcp"
    shutil.copy2(patch_root / "inflight.go", tcp / "inflight.go")
    patch_segment(tcp / "segment.go")
    patch_tcp_stats(root / "pkg/tcpip/tcpip.go")
    patch_rack(tcp / "rack.go")
    patch_sender(tcp / "snd.go")
    print(f"patched Linux-like inflight accounting at {root}")


if __name__ == "__main__":
    main()
