#!/usr/bin/env python3
"""Add measurement-only delivery-rate and BBR STARTUP diagnostics.

This post-patch intentionally changes no congestion-control decision. It adds
cumulative counters to the already-patched gVisor tree so CI can distinguish
low delivery-rate samples caused by ACK/send interval construction from an
early BBR full-bandwidth decision. Keeping this as a narrow post-patch makes
the experiment easy to remove without perturbing the canonical BBR/rate files.
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
    anchor = "\tTCPShiftBBRPostRTORoundStarts      *StatCounter\n"
    addition = anchor + """\tTCPShiftRateInvalidSegmentDeliveredCalls *StatCounter
\tTCPShiftRateInvalidSegmentDeliveredBytes *StatCounter
\tTCPShiftRateSamplesFinalized              *StatCounter
\tTCPShiftRateRetransCandidateSamples       *StatCounter
\tTCPShiftRateAckDominatedSamples           *StatCounter
\tTCPShiftRateSendDominatedSamples          *StatCounter
\tTCPShiftRateAckElapsedMicrosSum           *StatCounter
\tTCPShiftRateSendElapsedMicrosSum          *StatCounter
\tTCPShiftRateIntervalMicrosSum             *StatCounter
\tTCPShiftRateDeliveredBytesSum             *StatCounter
\tTCPShiftRateSampleRateSum                 *StatCounter
\tTCPShiftBBRSubMinRTTSamplesRejected       *StatCounter
\tTCPShiftBBRFullBWChecks                   *StatCounter
\tTCPShiftBBRFullBWGrowthUpdates            *StatCounter
\tTCPShiftBBRFullBWNoGrowthRounds           *StatCounter
\tTCPShiftBBRFullBWExits                    *StatCounter
\tTCPShiftBBRFullBWExitMaxBWSum             *StatCounter
\tTCPShiftBBRFullBWExitRoundSum             *StatCounter
"""
    text = replace_once(text, anchor, addition, "rate/STARTUP diagnostic counters")
    path.write_text(text)


def patch_rate(path: Path) -> None:
    text = path.read_text()

    text = replace_once(
        text,
        "\tackTime        tcpip.MonotonicTime\n\tisAppLimited   bool\n\n\t// ackedBytes is delivery caused by the current ACK event, not the longer\n",
        "\tackTime        tcpip.MonotonicTime\n\tisAppLimited   bool\n\tretransmitted  bool\n\n\t// ackedBytes is delivery caused by the current ACK event, not the longer\n",
        "rate candidate retransmission provenance",
    )

    text = replace_once(
        text,
        "\ts.rateDelivered += uint64(deliveredBytes)\n"
        "\tif s.rateAppLimitedUntil != 0 && s.rateDelivered > s.rateAppLimitedUntil {\n"
        "\t\ts.rateAppLimitedUntil = 0\n"
        "\t}\n\n"
        "\tif !seg.rateSampleValid {\n"
        "\t\treturn\n"
        "\t}\n",
        "\ts.rateDelivered += uint64(deliveredBytes)\n"
        "\tif s.rateAppLimitedUntil != 0 && s.rateDelivered > s.rateAppLimitedUntil {\n"
        "\t\ts.rateAppLimitedUntil = 0\n"
        "\t}\n\n"
        "\tif !seg.rateSampleValid {\n"
        "\t\t// Measurement only: invalid per-segment metadata can make an ACK\n"
        "\t\t// advance delivered without contributing a usable rate candidate.\n"
        "\t\tif _, ok := s.cc.(deliveryRateConsumer); ok {\n"
        "\t\t\tstats := s.ep.stack.Stats().TCP\n"
        "\t\t\tstats.TCPShiftRateInvalidSegmentDeliveredCalls.Increment()\n"
        "\t\t\tstats.TCPShiftRateInvalidSegmentDeliveredBytes.IncrementBy(uint64(deliveredBytes))\n"
        "\t\t}\n"
        "\t\treturn\n"
        "\t}\n",
        "invalid delivered-segment diagnostics",
    )

    text = replace_once(
        text,
        "\t\t\tackTime:        ackTime,\n\t\t\tisAppLimited:   seg.rateAppLimited,\n\t\t\tackedBytes:     ackedBytes,\n",
        "\t\t\tackTime:        ackTime,\n"
        "\t\t\tisAppLimited:   seg.rateAppLimited,\n"
        "\t\t\tretransmitted:  seg.xmitCount > 1,\n"
        "\t\t\tackedBytes:     ackedBytes,\n",
        "rate candidate retransmission capture",
    )

    text = replace_once(
        text,
        "\trate := delivered * uint64(time.Second) / uint64(interval)\n\tif rate == 0 {\n\t\treturn\n\t}\n\n\tackedSacked := 0\n",
        "\trate := delivered * uint64(time.Second) / uint64(interval)\n"
        "\tif rate == 0 {\n"
        "\t\treturn\n"
        "\t}\n\n"
        "\t// These counters observe the exact numerator and the two interval\n"
        "\t// candidates used by the Linux-style max(send_elapsed, ack_elapsed)\n"
        "\t// estimator. They are gated on a rate-sample consumer so CUBIC control\n"
        "\t// runs do not pay the additional per-ACK measurement cost.\n"
        "\tif _, ok := s.cc.(deliveryRateConsumer); ok {\n"
        "\t\tstats := s.ep.stack.Stats().TCP\n"
        "\t\tstats.TCPShiftRateSamplesFinalized.Increment()\n"
        "\t\tstats.TCPShiftRateAckElapsedMicrosSum.IncrementBy(uint64(ackElapsed / time.Microsecond))\n"
        "\t\tstats.TCPShiftRateSendElapsedMicrosSum.IncrementBy(uint64(sendElapsed / time.Microsecond))\n"
        "\t\tstats.TCPShiftRateIntervalMicrosSum.IncrementBy(uint64(interval / time.Microsecond))\n"
        "\t\tstats.TCPShiftRateDeliveredBytesSum.IncrementBy(delivered)\n"
        "\t\tstats.TCPShiftRateSampleRateSum.IncrementBy(rate)\n"
        "\t\tif c.retransmitted {\n"
        "\t\t\tstats.TCPShiftRateRetransCandidateSamples.Increment()\n"
        "\t\t}\n"
        "\t\tif ackElapsed >= sendElapsed {\n"
        "\t\t\tstats.TCPShiftRateAckDominatedSamples.Increment()\n"
        "\t\t} else {\n"
        "\t\t\tstats.TCPShiftRateSendDominatedSamples.Increment()\n"
        "\t\t}\n"
        "\t}\n\n"
        "\tackedSacked := 0\n",
        "finalized rate-sample diagnostics",
    )

    path.write_text(text)


def patch_bbr(path: Path) -> None:
    text = path.read_text()

    text = replace_once(
        text,
        "\tif b.minRTT > 0 && b.minRTT != time.Duration(math.MaxInt64) && rs.interval < b.minRTT {\n\t\treturn\n\t}\n",
        "\tif b.minRTT > 0 && b.minRTT != time.Duration(math.MaxInt64) && rs.interval < b.minRTT {\n"
        "\t\tb.s.ep.stack.Stats().TCP.TCPShiftBBRSubMinRTTSamplesRejected.Increment()\n"
        "\t\treturn\n"
        "\t}\n",
        "sub-minRTT sample diagnostics",
    )

    old = """\tif b.mode != bbrStartup || !b.roundStart || rs.isAppLimited || b.maxBW == 0 {
\t\treturn
\t}
\tif b.fullBW == 0 || b.maxBW >= b.fullBW*5/4 {
\t\tb.fullBW = b.maxBW
\t\tb.fullBWRound = 0
\t\treturn
\t}
\tb.fullBWRound++
\tif b.fullBWRound >= bbrFullBWRounds {
\t\tb.mode = bbrDrain
\t}
"""
    new = """\tif b.mode != bbrStartup || !b.roundStart || rs.isAppLimited || b.maxBW == 0 {
\t\treturn
\t}
\tstats := b.s.ep.stack.Stats().TCP
\tstats.TCPShiftBBRFullBWChecks.Increment()
\tif b.fullBW == 0 || b.maxBW >= b.fullBW*5/4 {
\t\tstats.TCPShiftBBRFullBWGrowthUpdates.Increment()
\t\tb.fullBW = b.maxBW
\t\tb.fullBWRound = 0
\t\treturn
\t}
\tstats.TCPShiftBBRFullBWNoGrowthRounds.Increment()
\tb.fullBWRound++
\tif b.fullBWRound >= bbrFullBWRounds {
\t\tstats.TCPShiftBBRFullBWExits.Increment()
\t\tstats.TCPShiftBBRFullBWExitMaxBWSum.IncrementBy(b.maxBW)
\t\tstats.TCPShiftBBRFullBWExitRoundSum.IncrementBy(b.roundCount)
\t\tb.mode = bbrDrain
\t}
"""
    text = replace_once(text, old, new, "BBR full-bandwidth diagnostics")
    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_rate_diagnostics.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a patched gVisor source tree: {root}")

    patch_tcp_stats(root / "pkg/tcpip/tcpip.go")
    patch_rate(tcp / "rate.go")
    patch_bbr(tcp / "bbr.go")
    print(f"patched measurement-only BBR rate/STARTUP diagnostics at {root}")


if __name__ == "__main__":
    main()
