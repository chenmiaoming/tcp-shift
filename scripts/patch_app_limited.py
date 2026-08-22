#!/usr/bin/env python3
"""Add Linux-style application-limited delivery-rate sampling to tcp-shift.

Linux marks a delivery epoch app-limited when the sender drains its unsent data
while it is neither cwnd-limited nor waiting to retransmit known losses. BBR
then refuses to let a low app-limited rate sample reduce the bottleneck model,
while still allowing an app-limited sample that raises max_bw.

For tcp-shift the corresponding send-gap boundary is the point where sendData()
has actually transmitted data and drains writeNext. This avoids coupling the
sampler to relay/application code and keeps the decision in TCP, where the
in-flight and loss state are available.
"""

from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def patch_sender(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\tratePriorInFlight int                   `state:"nosave"`\n',
        '\tratePriorInFlight  int    `state:"nosave"`\n'
        '\trateAppLimitedUntil uint64 `state:"nosave"`\n',
        "sender app-limited marker",
    )

    text = replace_once(
        text,
        '\t}\n\n\ts.postXmit(dataSent, true /* shouldScheduleProbe */)\n}',
        '\t}\n\n'
        '\t// Linux checks for an application-limited send gap after TCP has\n'
        '\t// consumed the currently available unsent data. Do this only after\n'
        '\t// making forward progress so an empty queue observed repeatedly by\n'
        '\t// ACK callbacks does not extend the marker indefinitely.\n'
        '\tif dataSent {\n'
        '\t\ts.rateCheckAppLimited()\n'
        '\t}\n\n'
        '\ts.postXmit(dataSent, true /* shouldScheduleProbe */)\n}',
        "sendData app-limited boundary",
    )
    path.write_text(text)


def patch_rate(path: Path) -> None:
    text = path.read_text()

    marker = '// +checklocks:s.ep.mu\nfunc (s *sender) rateSampleOnSend(seg *segment, now tcpip.MonotonicTime) {'
    helper = '''// rateCheckAppLimited mirrors the intent of Linux
// tcp_rate_check_app_limited(): remember a delivery boundary when TCP has no
// unsent data, is not cwnd-limited, and has retransmitted every packet already
// known lost. Samples from packets sent after this gap carry isAppLimited until
// delivery advances past the marker.
//
// tcp-shift's delivered coordinate is bytes while its independent in-flight
// coordinate is packets, so convert the latter conservatively with the current
// MSS. The exact unit is unimportant as long as the marker and delivered count
// share one monotonic coordinate.
//
// +checklocks:s.ep.mu
func (s *sender) rateCheckAppLimited() {
\tif s.writeNext != nil {
\t\treturn
\t}

\tflight := s.linuxLikeFlight()
\tif flight.packetsInFlight >= s.SndCwnd {
\t\treturn
\t}
\tif flight.lostOut > flight.retransOut {
\t\treturn
\t}

\tmss := s.MaxPayloadSize
\tif mss <= 0 {
\t\tmss = 1
\t}
\tmarker := s.rateDelivered + uint64(flight.packetsInFlight)*uint64(mss)
\tif marker == 0 {
\t\tmarker = 1
\t}

\t// Do not move an existing marker forward merely because another sender
\t// callback observes the same gap. A new marker is created only after the
\t// previous application-limited epoch has been delivered through.
\tif s.rateAppLimitedUntil == 0 {
\t\ts.rateAppLimitedUntil = marker
\t\ts.ep.stack.Stats().TCP.TCPShiftRateAppLimitedMarks.Increment()
\t}
}

'''
    if text.count(marker) != 1:
        raise RuntimeError(f"app-limited helper insertion: expected exactly one marker, got {text.count(marker)}")
    text = text.replace(marker, helper + marker, 1)

    text = replace_once(
        text,
        '\tseg.rateAppLimited = false\n',
        '\tseg.rateAppLimited = s.rateAppLimitedUntil != 0\n',
        "per-segment app-limited snapshot",
    )

    text = replace_once(
        text,
        '\ts.rateDelivered += uint64(deliveredBytes)\n\n\tif !seg.rateSampleValid {',
        '\ts.rateDelivered += uint64(deliveredBytes)\n'
        '\tif s.rateAppLimitedUntil != 0 && s.rateDelivered > s.rateAppLimitedUntil {\n'
        '\t\ts.rateAppLimitedUntil = 0\n'
        '\t}\n\n'
        '\tif !seg.rateSampleValid {',
        "app-limited delivery marker clear",
    )
    path.write_text(text)


def patch_bbr(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\tstats.TCPShiftBBRSamples.Increment()\n'
        '\tstats.TCPShiftBBRPriorInflightSum.IncrementBy(nonNegativeUint(rs.priorInFlight))',
        '\tstats.TCPShiftBBRSamples.Increment()\n'
        '\tif rs.isAppLimited {\n'
        '\t\tstats.TCPShiftBBRAppLimitedSamples.Increment()\n'
        '\t}\n'
        '\tstats.TCPShiftBBRPriorInflightSum.IncrementBy(nonNegativeUint(rs.priorInFlight))',
        "BBR app-limited diagnostics",
    )
    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_app_limited.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a gVisor source tree: {root}")

    patch_sender(tcp / "snd.go")
    patch_rate(tcp / "rate.go")
    patch_bbr(tcp / "bbr.go")
    print(f"patched Linux-style application-limited rate sampling at {root}")


if __name__ == "__main__":
    main()
