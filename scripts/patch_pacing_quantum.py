#!/usr/bin/env python3
"""Batch tcp-shift pacing wakeups into small byte quanta.

The base patch installs a token-bucket pacer. Waking as soon as credit for one
MSS is available makes a userspace Go timer behave like a per-packet hardware
pacer, which is both expensive and imprecise. This patch keeps the same byte
rate/token-bucket semantics but arms the timer until roughly 1 ms of credit is
available, so each wake can release a small batch. An already-armed earlier
deadline is also preserved.
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
        raise SystemExit("usage: patch_pacing_quantum.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    snd = root / "pkg/tcpip/transport/tcp/snd.go"
    if not snd.is_file():
        raise SystemExit(f"not a gVisor source tree: {root}")

    text = snd.read_text()
    old = '''func (s *sender) schedulePacing(rate uint64, need int64) {
\tif rate == 0 || need <= s.pacingBudget {
\t\treturn
\t}
\tdeficit := uint64(need - s.pacingBudget)
\tns := (deficit*uint64(time.Second) + rate - 1) / rate
\td := time.Duration(ns)
\tif d < 50*time.Microsecond {
\t\td = 50 * time.Microsecond
\t}
\ts.pacingTimer.enable(d)
}
'''
    new = '''func (s *sender) schedulePacing(rate uint64, need int64) {
\tif rate == 0 || need <= s.pacingBudget {
\t\treturn
\t}

\t// A userspace timer should pace a small byte quantum, not one MSS per
\t// wake. At 100 Mbit/s, one-MSS pacing asks Go/netstack to wake roughly
\t// every 100-150us; timer/scheduler latency then becomes part of the wire
\t// rate. Accumulate about 1ms of credit so one wake can release a short
\t// batch while the token bucket still enforces the long-term byte rate.
\twakeBudget := int64(rate / 1000) // about 1ms worth of traffic.
\tminQuantum := int64(2 * s.MaxPayloadSize)
\tif wakeBudget < minQuantum {
\t\twakeBudget = minQuantum
\t}
\tif wakeBudget > 32<<10 {
\t\twakeBudget = 32 << 10
\t}
\tif wakeBudget < need {
\t\twakeBudget = need
\t}

\tdeficit := wakeBudget - s.pacingBudget
\tif deficit <= 0 {
\t\treturn
\t}
\tns := (uint64(deficit)*uint64(time.Second) + rate - 1) / rate
\td := time.Duration(ns)
\tif d < 50*time.Microsecond {
\t\td = 50 * time.Microsecond
\t}

\t// ACK processing can re-enter sendData while a pacing wake is already
\t// armed. Keep the earliest deadline: a later calculation may pull the
\t// wake earlier, but must not postpone credit that was already scheduled.
\tnow := s.ep.stack.Clock().NowMonotonic()
\tnewTarget := now.Add(d)
\tif s.pacingTimer.enabled() && !newTarget.Before(s.pacingTimer.target) {
\t\treturn
\t}
\ts.pacingTimer.enable(d)
}
'''
    text = replace_once(text, old, new, "userspace pacing quantum")
    snd.write_text(text)
    print(f"patched millisecond pacing quantum at {root}")


if __name__ == "__main__":
    main()
