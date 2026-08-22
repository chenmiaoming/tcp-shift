#!/usr/bin/env python3
"""Batch tcp-shift pacing wakeups and tolerate userspace scheduler lateness.

The base patch installs a byte-rate token bucket. Waking as soon as credit for
one MSS is available makes a userspace Go timer behave like a per-packet
hardware pacer, which is both expensive and imprecise. This patch keeps a
roughly 2 ms wake quantum, but deliberately decouples that scheduling quantum
from the amount of pacing credit that may be retained.

That distinction matters for a userspace stack: a timer callback can be several
milliseconds late because of host scheduling even when the requested deadline
is precise. If the bucket stores only one wake quantum, all credit accumulated
while the callback is late is discarded and scheduler jitter becomes permanent
throughput loss. The reservoir below keeps up to roughly 10 ms of credit (with
an absolute cap), while a fresh sender still starts with only one 2 ms quantum.
Normal timely wakes therefore stay small; only a late sender is allowed to
catch up using byte credit that was actually earned over elapsed wall time.

This patch owns the shared pacing-timer callback, including resuming a paced
legacy-SACK recovery episode. Diagnostics are cumulative counters only: timer
arms/wakeups, callback lateness, bytes emitted by timer-driven versus other
sendData invocations, and pacing credit discarded at the reservoir cap.
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
\tTCPShiftPacingCreditClampedBytes     *StatCounter
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

    old_refill = '''func (s *sender) refillPacingBudget(rate uint64, now tcpip.MonotonicTime) {
\tburst := int64(rate / 500) // about 2ms worth of traffic.
\tminBurst := int64(2 * s.MaxPayloadSize)
\tif burst < minBurst {
\t\tburst = minBurst
\t}
\tif burst > 64<<10 {
\t\tburst = 64 << 10
\t}
\tif s.pacingLast == (tcpip.MonotonicTime{}) {
\t\ts.pacingLast = now
\t\ts.pacingBudget = burst
\t\treturn
\t}
\telapsed := now.Sub(s.pacingLast)
\tif elapsed <= 0 {
\t\treturn
\t}
\tadd := int64(rate * uint64(elapsed) / uint64(time.Second))
\ts.pacingBudget += add
\tif s.pacingBudget > burst {
\t\ts.pacingBudget = burst
\t}
\ts.pacingLast = now
}
'''
    new_refill = '''func (s *sender) pacingWakeBudget(rate uint64) int64 {
\tbudget := int64(rate / 500) // about 2ms worth of traffic.
\tminBudget := int64(2 * s.MaxPayloadSize)
\tif budget < minBudget {
\t\tbudget = minBudget
\t}
\tif budget > 64<<10 {
\t\tbudget = 64 << 10
\t}
\treturn budget
}

// pacingBurstCap is deliberately larger than pacingWakeBudget. The wake
// quantum controls how often a timely userspace timer should run; the reservoir
// controls how much already-earned credit survives a late callback. Keeping
// these equal made scheduler lateness translate directly into permanent
// throughput loss. Ten milliseconds is still small relative to the long-fat
// BDPs targeted by tcp-shift, and the absolute cap bounds burst size at higher
// rates.
func (s *sender) pacingBurstCap(rate uint64) int64 {
\tburst := int64(rate / 100) // about 10ms worth of traffic.
\tminBurst := int64(2 * s.MaxPayloadSize)
\tif burst < minBurst {
\t\tburst = minBurst
\t}
\tif burst > 512<<10 {
\t\tburst = 512 << 10
\t}
\treturn burst
}

// +checklocks:s.ep.mu
func (s *sender) refillPacingBudget(rate uint64, now tcpip.MonotonicTime) {
\tburst := s.pacingBurstCap(rate)
\tif s.pacingLast == (tcpip.MonotonicTime{}) {
\t\ts.pacingLast = now
\t\t// Do not start with the full lateness reservoir: a fresh flow should
\t\t// still release only one normal userspace pacing quantum.
\t\ts.pacingBudget = s.pacingWakeBudget(rate)
\t\tif s.pacingBudget > burst {
\t\t\ts.pacingBudget = burst
\t\t}
\t\treturn
\t}
\telapsed := now.Sub(s.pacingLast)
\tif elapsed <= 0 {
\t\treturn
\t}
\tadd := int64(rate * uint64(elapsed) / uint64(time.Second))
\ts.pacingBudget += add
\tif s.pacingBudget > burst {
\t\tdiscarded := s.pacingBudget - burst
\t\ts.ep.stack.Stats().TCP.TCPShiftPacingCreditClampedBytes.IncrementBy(uint64(discarded))
\t\ts.pacingBudget = burst
\t}
\ts.pacingLast = now
}
'''
    text = replace_once(text, old_refill, new_refill, "lateness-tolerant pacing reservoir")

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

\t// Pace a userspace batch, not an individual packet. The wake threshold is
\t// intentionally smaller than the stored-credit reservoir: normally we wake
\t// around every 2ms, while a late callback may retain enough earned credit to
\t// catch up instead of permanently losing throughput.
\twakeBudget := s.pacingWakeBudget(rate)
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
\tif s.resumePacedRecovery() {
\t\ts.pacingTimerDispatch = false
\t\treturn nil
\t}
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
    print(f"patched lateness-tolerant 2ms userspace pacing with diagnostics at {root}")


if __name__ == "__main__":
    main()
