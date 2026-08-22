#!/usr/bin/env python3
"""Order tcp-shift delivery-rate CC callbacks like Linux cong_control().

patch_gvisor.py wires delivery accounting into ACK/SACK processing. This narrow
follow-up patch separates sample generation from congestion-control notification:
we finalize any pure-SACK sample after ACK bookkeeping, establish RACK/legacy
recovery state, notify the rate-sample consumer, and only then let recovery send
or retransmit data.
"""

from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_delivery_control.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    path = root / "pkg/tcpip/transport/tcp/snd.go"
    if not path.is_file():
        raise SystemExit(f"not a patched gVisor source tree: {root}")

    text = path.read_text()

    # Cumulative ACKs are already finalized by patch_gvisor.py. This additional
    # call is a no-op for those ACKs, but finalizes pure-SACK delivery in the
    # same ACK event instead of deferring it until the next ACK.
    marker = '''\tif s.ep.SACKPermitted && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 {
\t\t// Update RACK reorder window.'''
    replacement = '''\t// Finalize the current delivery sample after ACK/SACK bookkeeping. Keep
\t// notification separate until loss detection establishes the CA state.
\ts.rateSampleEnd(rcvdSeg.rcvdTime)

\tif s.ep.SACKPermitted && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 {
\t\t// Update RACK reorder window.'''
    text = replace_once(text, marker, replacement, "delivery sample finalize before RACK")

    # Linux invokes custom cong_control after CA-state processing but before
    # retransmission. At this point RACK has already called HandleLossDetected
    # and enterRecovery() if this ACK proved a loss.
    marker = '''\t\tif s.FastRecovery.Active {
\t\t\ts.rc.DoRecovery(nil, fastRetransmit)
\t\t}
\t}'''
    replacement = '''\t\t// BBR-like custom congestion controls must observe ACK/SACK delivery
\t\t// with the current recovery state before RACK retransmits anything.
\t\ts.rateSampleNotify()

\t\tif s.FastRecovery.Active {
\t\t\ts.rc.DoRecovery(nil, fastRetransmit)
\t\t}
\t}'''
    text = replace_once(text, marker, replacement, "delivery notify before RACK recovery")

    # When RACK is not active, notify once before legacy recovery. This covers
    # both open-state ACKs and RFC6675/NewReno recovery ACKs.
    marker = '''\t// Now that we've popped all acknowledged data from the retransmit
\t// queue, retransmit if needed.
\tif s.FastRecovery.Active && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection == 0 {'''
    replacement = '''\t// If RACK did not consume the sample above, run the same custom control
\t// loop before legacy recovery transmits.
\tif !(s.ep.SACKPermitted && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0) {
\t\ts.rateSampleNotify()
\t}

\t// Now that we've popped all acknowledged data from the retransmit
\t// queue, retransmit if needed.
\tif s.FastRecovery.Active && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection == 0 {'''
    text = replace_once(text, marker, replacement, "delivery notify before legacy recovery")

    path.write_text(text)
    print(f"patched delivery-control ordering at {root}")


if __name__ == "__main__":
    main()
