#!/usr/bin/env python3
"""Batch tcp-shift pacing wakeups and expose compact pacing diagnostics.

The base patch installs a token-bucket pacer. Waking as soon as credit for one
MSS is available makes a userspace Go timer behave like a per-packet hardware
pacer, which is both expensive and imprecise. This patch keeps the same byte
rate/token-bucket semantics but arms the timer until roughly 2 ms of credit is
available, matching the token bucket's burst window so each wake can release a
small batch. An already-armed earlier deadline is also preserved.

Diagnostics are cumulative counters only: timer arms/wakeups, callback lateness,
and bytes actually emitted by timer-driven versus other sendData invocations.
They are intended to distinguish scheduler/timer latency from send-path credit
fragmentation without adding per-packet logging.
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
    text = replace_once(
        text,
        "type TCPStats struct {\n",
        '''type TCPStats struct {
\t// tcp-shift userspace pacing diagnostics. These stay cumulative so the
\t// existing periodic Stack.Stats() dump can expose pacing behavior without
\t// logging on every send.
\tTCPShiftPacingTimerArms              *StatCounter
\tTCPShiftPacingTimerWakeups           *StatCounter
\tTCPShiftPacingTimerLatenessMicrosSum *StatCounter
\tTCPShiftPacingTimerBytes             *StatCounter
\tTCPShiftPacingOtherBytes             *StatCounter
''',
        "pacing TCP stats",
    )
    path.write_text(text)


def patch_sender(path: Path) -> None:
    text = path.read_text()

    # Remember whether the current sendData invocation came from the pacing
    # timer so byte accounting can separate timer-driven progress from ACK/
    # application-driven progress.
    text = replace_once(
        text,
        '''\tpacingTimer  timer               `state:"nosave"`
\tpacingBudget int64                `state:"nosave"`
\tpacingLast   tcpip.MonotonicTime `state:"nosave"`
''',
        '''\tpacingTimer         timer               `state:"nosave"`
\tpacingBudget        int64                `state:"nosave"`
\tpacingLast          tcpip.MonotonicTime `state:"nosave"`
\tpacingTimerDispatch bool                 `state:"nosave"`
''',
        "pacing dispatch state",
    )

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

\t// Pace a userspace batch, not an individual packet. The token bucket
\t// already caps stored credit to about 2ms of traffic in
\t// refillPacingBudget(). Waiting for the same 2ms quantum amortizes Go
\t// timer/runtime/endpoint-lock overhead while preserving the long-term
\t// byte rate. At 100 Mbit/s this is only about 25KB, so the batch remains
\t// small compared with the BDP of the long-fat paths tcp-shift targets.
\twakeBudget := int64(rate / 500) // about 2ms worth of traffic.
\tminQuantum := int64(2 * s.MaxPayloadSize)
\tif wakeBudget < minQuantum {
\t\twakeBudget = minQuantum
\t}
\tif wakeBudget > 64<<10 {
\t\twakeBudget = 64 << 10
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
\ts.ep.stack.Stats().TCP.TCPShiftPacingTimerArms.Increment()
\ts.pacingTimer.enable(d)
}
'''
    text = replace_once(text, old, new, "userspace pacing quantum")

    text = replace_once(
        text,
        '''func (s *sender) pacingTimerExpired() tcpip.Error {
\tif s.pacingTimer.isUninitialized() || !s.pacingTimer.checkExpiration() {
\t\treturn nil
\t}
\ts.sendData()
\treturn nil
}
''',
        '''func (s *sender) pacingTimerExpired() tcpip.Error {
\tif s.pacingTimer.isUninitialized() {
\t\treturn nil
\t}
\ttarget := s.pacingTimer.target
\tif !s.pacingTimer.checkExpiration() {
\t\treturn nil
\t}

\tstats := s.ep.stack.Stats().TCP
\tstats.TCPShiftPacingTimerWakeups.Increment()
\tnow := s.ep.stack.Clock().NowMonotonic()
\tif late := now.Sub(target); late > 0 {
\t\tstats.TCPShiftPacingTimerLatenessMicrosSum.IncrementBy(uint64(late / time.Microsecond))
\t}

\ts.pacingTimerDispatch = true
\ts.sendData()
\ts.pacingTimerDispatch = false
\treturn nil
}
''',
        "pacing timer diagnostics",
    )

    text = replace_once(
        text,
        '''\t\tif rate != 0 {
\t\t\ts.pacingBudget -= int64(seg.payloadSize())
\t\t\tif s.pacingBudget < 0 {
\t\t\t\ts.pacingBudget = 0
\t\t\t}
\t\t}
''',
        '''\t\tif rate != 0 {
\t\t\tsentBytes := uint64(seg.payloadSize())
\t\t\tstats := s.ep.stack.Stats().TCP
\t\t\tif s.pacingTimerDispatch {
\t\t\t\tstats.TCPShiftPacingTimerBytes.IncrementBy(sentBytes)
\t\t\t} else {
\t\t\t\tstats.TCPShiftPacingOtherBytes.IncrementBy(sentBytes)
\t\t\t}

\t\t\ts.pacingBudget -= int64(sentBytes)
\t\t\tif s.pacingBudget < 0 {
\t\t\t\ts.pacingBudget = 0
\t\t\t}
\t\t}
''',
        "pacing byte source diagnostics",
    )

    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_pacing_quantum.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    snd = root / "pkg/tcpip/transport/tcp/snd.go"
    stats = root / "pkg/tcpip/tcpip.go"
    if not snd.is_file() or not stats.is_file():
        raise SystemExit(f"not a gVisor source tree: {root}")

    patch_tcp_stats(stats)
    patch_sender(snd)
    print(f"patched 2ms userspace pacing quantum with diagnostics at {root}")


if __name__ == "__main__":
    main()
