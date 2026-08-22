#!/usr/bin/env python3
"""Align tcp-shift's independent in-flight shadow with Linux RTO semantics.

This is intentionally a narrow post-patch experiment. gVisor rewinds the send
queue and clears its SACK scoreboard on RTO, while BBR consumes tcp-shift's
independent Linux-like packets_in_flight estimate. Linux marks the old flight
lost at RTO, so packets_in_flight becomes primarily retransmitted copies rather
than the entire pre-RTO queue. Preserve that invariant in the shadow without
changing gVisor's recovery algorithm.
"""

from __future__ import annotations

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
        '''\tinflightRACKLost      bool `state:"nosave"`
\tinflightRetransActive bool `state:"nosave"`
''',
        '''\tinflightRACKLost      bool `state:"nosave"`
\tinflightRTOLost       bool `state:"nosave"`
\tinflightRetransActive bool `state:"nosave"`
''',
        "segment RTO-loss shadow",
    )
    text = replace_once(
        text,
        '''\tt.inflightRACKLost = s.inflightRACKLost
\tt.inflightRetransActive = s.inflightRetransActive
''',
        '''\tt.inflightRACKLost = s.inflightRACKLost
\tt.inflightRTOLost = s.inflightRTOLost
\tt.inflightRetransActive = s.inflightRetransActive
''',
        "clone RTO-loss shadow",
    )
    path.write_text(text)


def patch_inflight(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '''\t\t\tif seg.inflightRACKLost || (s.ep.SACKPermitted && s.ep.scoreboard.IsRangeLost(sb)) {
''',
        '''\t\t\tif seg.inflightRACKLost || seg.inflightRTOLost || (s.ep.SACKPermitted && s.ep.scoreboard.IsRangeLost(sb)) {
''',
        "RTO lost_out accounting",
    )

    marker = '''// inflightOnSend updates the shadow state immediately before sendSegment bumps
'''
    helper = '''// inflightEnterRTO mirrors Linux tcp_enter_loss/tcp_timeout_mark_lost for the
// independent accounting coordinate consumed by BBR. gVisor clears the SACK
// scoreboard and rewinds writeNext after HandleRTOExpired(), so without this
// shadow all pre-RTO packets would incorrectly reappear as packets_in_flight.
// Mark only data that has actually been transmitted; unsent write-list entries
// remain normal new data. Any older retransmitted copy is no longer counted as
// active when a new RTO epoch starts. A later retransmission adds retrans_out
// again through inflightOnSend(), yielding packets_out-lost_out+retrans_out.
//
// +checklocks:s.ep.mu
func (s *sender) inflightEnterRTO() {
\tfor seg := s.writeList.Front(); seg != nil; seg = seg.Next() {
\t\tif seg.payloadSize() <= 0 || seg.xmitCount == 0 {
\t\t\tcontinue
\t\t}
\t\tseg.inflightRTOLost = true
\t\tseg.inflightRetransActive = false
\t}
}

'''
    if text.count(marker) != 1:
        raise RuntimeError(f"RTO helper insertion: expected exactly one marker, got {text.count(marker)}")
    text = text.replace(marker, helper + marker, 1)
    path.write_text(text)


def patch_sender(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '''\ts.state = tcpip.RTORecovery
\ts.cc.HandleRTOExpired()
''',
        '''\ts.state = tcpip.RTORecovery
\t// Keep BBR's independent packets_in_flight coordinate consistent with the
\t// RTO transition before congestion control observes it. This changes only
\t// tcp-shift shadow accounting; gVisor still owns the actual RTO recovery.
\ts.inflightEnterRTO()
\ts.cc.HandleRTOExpired()
''',
        "RTO inflight transition",
    )
    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_rto_inflight.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a gVisor source tree: {root}")

    patch_segment(tcp / "segment.go")
    patch_inflight(tcp / "inflight.go")
    patch_sender(tcp / "snd.go")
    print(f"patched Linux-like RTO inflight shadow at {root}")


if __name__ == "__main__":
    main()
