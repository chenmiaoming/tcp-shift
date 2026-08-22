#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


OLD = """\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && rc.EndSequence.LessThan(endSeq)) {\n"""
NEW = """\t\tif seg.xmitTime.Before(rc.XmitTime) || (seg.xmitTime == rc.XmitTime && endSeq.LessThan(rc.EndSequence)) {\n"""


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_rack_tiebreak.py <gvisor-dir>")

    gvisor = Path(sys.argv[1])
    rack = gvisor / "pkg/tcpip/transport/tcp/rack.go"
    text = rack.read_text()

    # RACK's reference update defines the newer packet at an equal transmit
    # timestamp as the one with the larger ending sequence number. Loss
    # detection must therefore regard an equal-time candidate as older only
    # when candidate.endSeq < reference.endSeq. Linux encodes the same ordering
    # in tcp_skb_sent_after(ref_time, cand_time, ref_end, cand_end).
    #
    # Keep this patch deliberately narrow: if upstream changes this exact
    # predicate, fail the build so the compatibility job forces a review rather
    # than silently applying a stale RACK fix.
    count = text.count(OLD)
    if count != 1:
        raise SystemExit(
            f"expected exactly one gVisor RACK equal-time loss predicate, found {count}"
        )

    # The reference-update predicate should remain the opposite comparison:
    # rc.EndSequence < endSeq means the newly ACKed segment is newer.
    update_predicate = (
        "(seg.xmitTime == rc.XmitTime && rc.EndSequence.LessThan(endSeq))"
    )
    if text.count(update_predicate) < 2:
        raise SystemExit(
            "unexpected gVisor RACK ordering structure; refusing blind patch"
        )

    rack.write_text(text.replace(OLD, NEW, 1))
    print(f"patched gVisor RACK equal-time ordering at {rack}")


if __name__ == "__main__":
    main()
