#!/usr/bin/env python3
"""Add narrow legacy-SACK recovery pacing hooks to patched gVisor.

The initial experiment also paced RACK DoRecovery(), but CI showed worse
throughput and substantially more retransmission/DSACK activity. Keep RACK on
upstream timing while its loss-inference interaction with BBR is investigated.
This layer therefore touches only RFC6675 SACK recovery plus the shared pacing
timer resume hook.
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


def patch_pacing_timer(path: Path) -> None:
    text = path.read_text()
    old = '''func (s *sender) pacingTimerExpired() tcpip.Error {
\tif s.pacingTimer.isUninitialized() || !s.pacingTimer.checkExpiration() {
\t\treturn nil
\t}
\ts.sendData()
\treturn nil
}'''
    new = '''func (s *sender) pacingTimerExpired() tcpip.Error {
\tif s.pacingTimer.isUninitialized() || !s.pacingTimer.checkExpiration() {
\t\treturn nil
\t}
\tif s.resumePacedRecovery() {
\t\treturn nil
\t}
\ts.sendData()
\treturn nil
}'''
    path.write_text(replace_once(text, old, new, "pacing timer recovery resume"))


def patch_sack(path: Path) -> None:
    text = path.read_text()
    start_marker = "func (sr *sackRecovery) handleSACKRecovery(limit int, end seqnum.Value) (dataSent bool) {"
    end_marker = "// +checklocks:sr.s.ep.mu\nfunc (sr *sackRecovery) DoRecovery"
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    block = text[start:end]

    block = replace_once(
        block,
        "\tsnd := sr.s\n\tsnd.SetPipe()",
        "\tsnd := sr.s\n"
        "\trate := snd.preparePacedSend()\n"
        "\tsnd.SetPipe()",
        "SACK pacing prepare",
    )
    block = replace_once(
        block,
        "\t\t\tif sent := snd.maybeSendSegment(nextSeg, limit, end); !sent {",
        "\t\t\tif !snd.allowPacedSend(rate, nextSeg.payloadSize()) {\n"
        "\t\t\t\treturn dataSent\n"
        "\t\t\t}\n"
        "\t\t\tif sent := snd.maybeSendSegment(nextSeg, limit, end); !sent {",
        "SACK new-data pacing admission",
    )
    block = replace_once(
        block,
        "\t\t\tdataSent = true\n\t\t\tsnd.Outstanding++\n\t\t\tsnd.updateWriteNext(nextSeg.Next())",
        "\t\t\tdataSent = true\n"
        "\t\t\tsnd.Outstanding++\n"
        "\t\t\tsnd.accountPacedSend(rate, nextSeg.payloadSize())\n"
        "\t\t\tsnd.updateWriteNext(nextSeg.Next())",
        "SACK new-data pacing accounting",
    )
    block = replace_once(
        block,
        "\t\t// Now handle the retransmission case where we matched either step 1,3 or 4\n",
        "\t\t// Now handle the retransmission case where we matched either step 1,3 or 4\n"
        "\t\tif !snd.allowPacedSend(rate, nextSeg.payloadSize()) {\n"
        "\t\t\treturn dataSent\n"
        "\t\t}\n",
        "SACK retransmit pacing admission",
    )
    block = replace_once(
        block,
        "\t\tsnd.sendSegment(nextSeg)\n\n\t\tsegEnd := nextSeg.sequenceNumber.Add(nextSeg.logicalLen())",
        "\t\tsnd.sendSegment(nextSeg)\n"
        "\t\tsnd.accountPacedSend(rate, nextSeg.payloadSize())\n\n"
        "\t\tsegEnd := nextSeg.sequenceNumber.Add(nextSeg.logicalLen())",
        "SACK retransmit pacing accounting",
    )

    path.write_text(text[:start] + block + text[end:])


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_recovery_pacing.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a gVisor source tree: {root}")

    patch_root = Path(__file__).resolve().parents[1] / "patches/gvisor/tcp"
    shutil.copy2(patch_root / "pacing_recovery.go", tcp / "pacing_recovery.go")
    patch_pacing_timer(tcp / "snd.go")
    patch_sack(tcp / "sack_recovery.go")
    print(f"patched gVisor legacy SACK recovery pacing at {root}")


if __name__ == "__main__":
    main()
