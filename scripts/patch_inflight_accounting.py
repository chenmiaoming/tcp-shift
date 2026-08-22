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
	TCPShiftRACKLossMarksFirst         *StatCounter
	TCPShiftRACKLossMarksRepeat        *StatCounter
	TCPShiftRACKLossMarksACK           *StatCounter
	TCPShiftRACKLossMarksTimer         *StatCounter
	TCPShiftRACKEqualTimeCandidates    *StatCounter
	TCPShiftRACKRecoveryRetransmits    *StatCounter
	TCPShiftRACKFastRetransmits        *StatCounter
	TCPShiftRACKLostLoopRetransmits    *StatCounter
	TCPShiftTLPRetransmits             *StatCounter
	TCPShiftRTORetransmits             *StatCounter
	TCPShiftRetransmitFirst            *StatCounter
	TCPShiftRetransmitSecond           *StatCounter
	TCPShiftRetransmitThirdPlus        *StatCounter
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
	TCPShiftBBRMaxBWSum                *StatCounter
	TCPShiftBBRPacingRateSum           *StatCounter
	TCPShiftBBRCwndSum                 *StatCounter
	TCPShiftBBRCwndTargetSum           *StatCounter
	TCPShiftBBRMinRTTMicrosSum         *StatCounter
	TCPShiftBBRStartupSamples          *StatCounter
	TCPShiftBBRDrainSamples            *StatCounter
	TCPShiftBBRProbeBWSamples          *StatCounter
	TCPShiftBBRProbeRTTSamples         *StatCounter
'''
    text = replace_once(text, "type TCPStats struct {\n", fields, "TCP diagnostic stats")
    path.write_text(text)


def patch_rack(path: Path) -> None:
    text = path.read_text()

    text = replace_once(
        text,
        '\t// snd is a reference to the sender.\n\tsnd *sender\n}',
        '\t// snd is a reference to the sender.\n\tsnd *sender\n\n'
        '\t// tcpShiftDetectFromTimer distinguishes reorder-timer loss inference\n'
        '\t// from ACK-driven detectLoss calls without emitting per-packet logs.\n'
        '\ttcpShiftDetectFromTimer bool `state:"nosave"`\n}',
        "RACK loss-origin context",
    )

    # Match unmodified upstream RACK here. The equal-timestamp tie-break remains
    # a standalone CI control and is deliberately not part of tcp-shift proper.
    old = '''\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && rc.EndSequence.LessThan(endSeq)) {
\t\t\ttimeRemaining := seg.xmitTime.Sub(rcvTime) + rc.RTT + rc.ReoWnd
\t\t\tif timeRemaining <= 0 {
\t\t\t\tseg.lost = true
\t\t\t\tnumLost++
'''
    new = '''\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && rc.EndSequence.LessThan(endSeq)) {
\t\t\tif seg.xmitTime == rc.XmitTime {
\t\t\t\trc.snd.ep.stack.Stats().TCP.TCPShiftRACKEqualTimeCandidates.Increment()
\t\t\t}
\t\t\ttimeRemaining := seg.xmitTime.Sub(rcvTime) + rc.RTT + rc.ReoWnd
\t\t\tif timeRemaining <= 0 {
\t\t\t\trepeated := rc.snd.inflightMarkRACKLost(seg)
\t\t\t\tstats := rc.snd.ep.stack.Stats().TCP
\t\t\t\tstats.TCPShiftRACKLossMarks.Increment()
\t\t\t\tif repeated {
\t\t\t\t\tstats.TCPShiftRACKLossMarksRepeat.Increment()
\t\t\t\t} else {
\t\t\t\t\tstats.TCPShiftRACKLossMarksFirst.Increment()
\t\t\t\t}
\t\t\t\tif rc.tcpShiftDetectFromTimer {
\t\t\t\t\tstats.TCPShiftRACKLossMarksTimer.Increment()
\t\t\t\t} else {
\t\t\t\t\tstats.TCPShiftRACKLossMarksACK.Increment()
\t\t\t\t}
\t\t\t\tseg.lost = true
\t\t\t\tnumLost++
'''
    text = replace_once(text, old, new, "RACK inflight loss accounting")

    text = replace_once(
        text,
        '\tnumLost := rc.detectLoss(rc.snd.reorderTimer.target)\n',
        '\trc.tcpShiftDetectFromTimer = true\n'
        '\tnumLost := rc.detectLoss(rc.snd.reorderTimer.target)\n'
        '\trc.tcpShiftDetectFromTimer = false\n',
        "RACK reorder-timer loss origin",
    )

    # A TLP can retransmit the highest transmitted segment outside
    # FastRecovery. probeTimerExpired lives in rack.go, not snd.go.
    text = replace_once(
        text,
        '''\t\tif highestSeqXmit != nil {
\t\t\tdataSent = s.maybeSendSegment(highestSeqXmit, int(s.ep.scoreboard.SMSS()), s.SndUna.Add(s.SndWnd))
\t\t\tif dataSent {
\t\t\t\ts.rc.tlpRxtOut = true''',
        '''\t\tif highestSeqXmit != nil {
\t\t\twasRetransmit := highestSeqXmit.xmitCount > 0
\t\t\tdataSent = s.maybeSendSegment(highestSeqXmit, int(s.ep.scoreboard.SMSS()), s.SndUna.Add(s.SndWnd))
\t\t\tif dataSent {
\t\t\t\tif wasRetransmit {
\t\t\t\t\ts.ep.stack.Stats().TCP.TCPShiftTLPRetransmits.Increment()
\t\t\t\t}
\t\t\t\ts.rc.tlpRxtOut = true''',
        "TLP retransmit classification",
    )

    text = replace_once(
        text,
        '''\tsnd := rc.snd
\tif fastRetransmit {
\t\tsnd.resendSegment()
\t}''',
        '''\tsnd := rc.snd
\tif fastRetransmit {
\t\tsnd.ep.stack.Stats().TCP.TCPShiftRACKFastRetransmits.Increment()
\t\tsnd.resendSegment()
\t}''',
        "RACK fast retransmit classification",
    )

    text = replace_once(
        text,
        '''\t\tif sent := snd.maybeSendSegment(seg, int(snd.ep.scoreboard.SMSS()), snd.SndUna.Add(snd.SndWnd)); !sent {
\t\t\tbreak
\t\t}
\t\tdataSent = true''',
        '''\t\tif sent := snd.maybeSendSegment(seg, int(snd.ep.scoreboard.SMSS()), snd.SndUna.Add(snd.SndWnd)); !sent {
\t\t\tbreak
\t\t}
\t\tsnd.ep.stack.Stats().TCP.TCPShiftRACKLostLoopRetransmits.Increment()
\t\tdataSent = true''',
        "RACK lost-loop retransmit classification",
    )

    text = replace_once(
        text,
        '''\t\t// Check the congestion window after entering recovery.
\t\tif snd.Outstanding >= snd.SndCwnd {''',
        '''\t\t// Compare cwnd and in-flight in the same coordinate system. Reno/CUBIC
\t\t// retain Outstanding; BBR supplies Linux-like packets_in_flight.
\t\tif snd.recoveryPacketsInFlight() >= snd.SndCwnd {''',
        "RACK recovery admission inflight",
    )
    path.write_text(text)


def patch_sack_recovery(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\tfor snd.Outstanding < snd.SndCwnd {',
        '\tfor snd.recoveryPacketsInFlight() < snd.SndCwnd {',
        "RFC6675 recovery admission inflight",
    )
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
    patch_sack_recovery(tcp / "sack_recovery.go")
    patch_sender(tcp / "snd.go")
    print(f"patched Linux-like inflight accounting at {root}")


if __name__ == "__main__":
    main()
