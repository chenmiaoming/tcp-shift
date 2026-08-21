#!/usr/bin/env python3
"""Apply the small tcp-shift BBR/pacing patch to a pinned gVisor checkout."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def patch_protocol(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        'const (\n\tccReno  = "reno"\n\tccCubic = "cubic"\n)',
        'const (\n\tccReno  = "reno"\n\tccCubic = "cubic"\n\tccBBR   = "bbr"\n)',
        "protocol cc constants",
    )
    text = replace_once(
        text,
        'availableCongestionControl: []string{ccReno, ccCubic},',
        'availableCongestionControl: []string{ccReno, ccCubic, ccBBR},',
        "protocol available cc list",
    )
    path.write_text(text)


def patch_sender(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\tcorkTimer timer `state:"nosave"`\n}',
        '\tcorkTimer timer `state:"nosave"`\n\n'
        '\t// pacingTimer and its token bucket are used only by congestion controls\n'
        '\t// that expose a PacingRate method (currently tcp-shift BBR).\n'
        '\tpacingTimer  timer               `state:"nosave"`\n'
        '\tpacingBudget int64                `state:"nosave"`\n'
        '\tpacingLast   tcpip.MonotonicTime `state:"nosave"`\n}',
        "sender pacing fields",
    )
    text = replace_once(
        text,
        '\tep.snd.corkTimer.init(ep.snd.ep.stack.Clock(), timerHandler(ep.snd.ep, ep.snd.corkTimerExpired))',
        '\tep.snd.corkTimer.init(ep.snd.ep.stack.Clock(), timerHandler(ep.snd.ep, ep.snd.corkTimerExpired))\n'
        '\tep.snd.pacingTimer.init(ep.snd.ep.stack.Clock(), timerHandler(ep.snd.ep, ep.snd.pacingTimerExpired))',
        "sender pacing timer init",
    )
    text = replace_once(
        text,
        '\tcase ccCubic:\n\t\treturn newCubicCC(s)\n\tcase ccReno:',
        '\tcase ccCubic:\n\t\treturn newCubicCC(s)\n\tcase ccBBR:\n\t\treturn newBBRCC(s)\n\tcase ccReno:',
        "sender cc switch",
    )

    start = text.index('// sendData sends new data segments.')
    end = text.index('// +checklocks:s.ep.mu\nfunc (s *sender) enterRecovery()', start)
    old = text[start:end]
    new = r'''// pacedCongestionControl is implemented by congestion controls that want the
// generic TCP sender to pace new data. The rate is bytes per second.
type pacedCongestionControl interface {
	PacingRate() uint64
}

func (s *sender) pacingRate() uint64 {
	p, ok := s.cc.(pacedCongestionControl)
	if !ok {
		return 0
	}
	return p.PacingRate()
}

// +checklocks:s.ep.mu
func (s *sender) refillPacingBudget(rate uint64, now tcpip.MonotonicTime) {
	burst := int64(rate / 500) // about 2ms worth of traffic.
	minBurst := int64(2 * s.MaxPayloadSize)
	if burst < minBurst {
		burst = minBurst
	}
	if burst > 64<<10 {
		burst = 64 << 10
	}
	if s.pacingLast == (tcpip.MonotonicTime{}) {
		s.pacingLast = now
		s.pacingBudget = burst
		return
	}
	elapsed := now.Sub(s.pacingLast)
	if elapsed <= 0 {
		return
	}
	add := int64(rate * uint64(elapsed) / uint64(time.Second))
	s.pacingBudget += add
	if s.pacingBudget > burst {
		s.pacingBudget = burst
	}
	s.pacingLast = now
}

// +checklocks:s.ep.mu
func (s *sender) schedulePacing(rate uint64, need int64) {
	if rate == 0 || need <= s.pacingBudget {
		return
	}
	deficit := uint64(need - s.pacingBudget)
	ns := (deficit*uint64(time.Second) + rate - 1) / rate
	d := time.Duration(ns)
	if d < 50*time.Microsecond {
		d = 50 * time.Microsecond
	}
	s.pacingTimer.enable(d)
}

// pacingTimerExpired lets paced congestion controls resume sending.
// +checklocks:s.ep.mu
func (s *sender) pacingTimerExpired() tcpip.Error {
	if s.pacingTimer.isUninitialized() || !s.pacingTimer.checkExpiration() {
		return nil
	}
	s.sendData()
	return nil
}

// sendData sends new data segments. It is called when data becomes available or
// when the send window opens up.
// +checklocks:s.ep.mu
func (s *sender) sendData() {
	limit := s.MaxPayloadSize
	if s.gso {
		limit = int(s.ep.gso.MaxSize - header.TCPTotalHeaderMaximumSize - 1)
	}
	end := s.SndUna.Add(s.SndWnd)

	// Reduce the congestion window to min(IW, cwnd) per RFC 5681, page 10.
	// "A TCP SHOULD set cwnd to no more than RW before beginning
	// transmission if the TCP has not sent data in the interval exceeding
	// the retrasmission timeout."
	if !s.FastRecovery.Active && s.state != tcpip.RTORecovery && s.ep.stack.Clock().NowMonotonic().Sub(s.LastSendTime) > s.RTO {
		if s.SndCwnd > InitialCwnd {
			s.SndCwnd = InitialCwnd
		}
	}

	rate := s.pacingRate()
	if rate == 0 {
		s.pacingTimer.disable()
		s.pacingBudget = 0
		s.pacingLast = tcpip.MonotonicTime{}
	} else {
		s.refillPacingBudget(rate, s.ep.stack.Clock().NowMonotonic())
	}

	var dataSent bool
	for seg := s.writeNext; seg != nil && s.Outstanding < s.SndCwnd; seg = seg.Next() {
		// NOTE(gvisor.dev/issue/11632): Use uint64 to avoid overflow.
		cwndLimit := uint64(s.SndCwnd-s.Outstanding) * uint64(s.MaxPayloadSize)
		if cwndLimit < uint64(limit) {
			limit = int(cwndLimit)
		}
		if s.isAssignedSequenceNumber(seg) && s.ep.SACKPermitted && s.ep.scoreboard.IsSACKED(seg.sackBlock()) {
			// Move writeNext along so that we don't try and scan data that
			// has already been SACKED.
			s.updateWriteNext(seg.Next())
			continue
		}

		if rate != 0 {
			need := seg.payloadSize()
			if need <= 0 || need > s.MaxPayloadSize {
				need = s.MaxPayloadSize
			}
			if s.pacingBudget < int64(need) {
				s.schedulePacing(rate, int64(need))
				break
			}
		}

		if sent := s.maybeSendSegment(seg, limit, end); !sent {
			break
		}
		dataSent = true
		s.Outstanding += s.pCount(seg, s.MaxPayloadSize)
		s.updateWriteNext(seg.Next())
		if rate != 0 {
			s.pacingBudget -= int64(seg.payloadSize())
			if s.pacingBudget < 0 {
				s.pacingBudget = 0
			}
		}
	}

	s.postXmit(dataSent, true /* shouldScheduleProbe */)
}

'''
    if 'func (s *sender) sendData()' not in old:
        raise RuntimeError("sendData block not found")
    text = text[:start] + new + text[end:]
    path.write_text(text)


def patch_restore(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\t\tsnd.corkTimer.init(s.Clock(), timerHandler(e, e.snd.corkTimerExpired))',
        '\t\tsnd.corkTimer.init(s.Clock(), timerHandler(e, e.snd.corkTimerExpired))\n'
        '\t\tsnd.pacingTimer.init(s.Clock(), timerHandler(e, e.snd.pacingTimerExpired))',
        "restore pacing timer",
    )
    path.write_text(text)


def patch_cleanup(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\t\te.snd.corkTimer.cleanup()',
        '\t\te.snd.corkTimer.cleanup()\n\t\te.snd.pacingTimer.cleanup()',
        "cleanup pacing timer",
    )
    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_gvisor.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a gVisor source tree: {root}")

    bbr_src = Path(__file__).resolve().parents[1] / "patches/gvisor/tcp/bbr.go"
    shutil.copy2(bbr_src, tcp / "bbr.go")
    patch_protocol(tcp / "protocol.go")
    patch_sender(tcp / "snd.go")
    patch_restore(tcp / "endpoint_state.go")
    patch_cleanup(tcp / "endpoint.go")
    print(f"patched gVisor TCP at {root}")


if __name__ == "__main__":
    main()
