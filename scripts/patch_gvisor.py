#!/usr/bin/env python3
"""Apply the tcp-shift BBR/pacing integration to a resolved gVisor checkout.

The patcher intentionally preserves as much upstream source text as possible.
In particular, sender.sendData() and RACK recovery are modified with narrow
insertions rather than wholesale replacements. This keeps unrelated upstream
sender/recovery changes in place and makes the floating-go CI job a useful
compatibility signal.
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


PACING_HELPERS = r'''// pacedCongestionControl is implemented by congestion controls that want the
// generic TCP sender to pace transmissions. The rate is bytes per second.
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

// preparePacing refreshes the sender-wide token budget. The same budget is
// shared by normal data and recovery retransmissions so recovery cannot bypass
// the model's pacing rate.
// +checklocks:s.ep.mu
func (s *sender) preparePacing() uint64 {
	rate := s.pacingRate()
	if rate == 0 {
		s.pacingTimer.disable()
		s.pacingBudget = 0
		s.pacingLast = tcpip.MonotonicTime{}
		return 0
	}
	s.refillPacingBudget(rate, s.ep.stack.Clock().NowMonotonic())
	return rate
}

// pacingBytes returns a conservative estimate of the bytes the next send will
// place on the wire. maybeSendSegment may split a larger buffered segment at
// limit; overestimating slightly is safe and only delays the send.
func (s *sender) pacingBytes(seg *segment, limit int) int64 {
	if seg == nil {
		return 0
	}
	need := seg.payloadSize()
	if need <= 0 {
		return 0
	}
	if limit > 0 && need > limit {
		need = limit
	}
	return int64(need)
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

// +checklocks:s.ep.mu
func (s *sender) admitPacedSend(rate uint64, need int64) bool {
	if rate == 0 || need <= 0 {
		return true
	}
	if s.pacingBudget >= need {
		return true
	}
	s.schedulePacing(rate, need)
	return false
}

// +checklocks:s.ep.mu
func (s *sender) accountPacedSend(rate uint64, sent int64) {
	if rate == 0 || sent <= 0 {
		return
	}
	s.pacingBudget -= sent
	if s.pacingBudget < 0 {
		s.pacingBudget = 0
	}
}

// pacingTimerExpired lets paced congestion controls resume the path that was
// actually blocked by pacing. RACK recovery must not fall through to the normal
// sendData path, otherwise retransmissions can remain stalled while new data is
// emitted instead.
// +checklocks:s.ep.mu
func (s *sender) pacingTimerExpired() tcpip.Error {
	if s.pacingTimer.isUninitialized() || !s.pacingTimer.checkExpiration() {
		return nil
	}
	if s.FastRecovery.Active && s.ep.SACKPermitted && s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 {
		s.rc.DoRecovery(nil, false)
		return nil
	}
	s.sendData()
	return nil
}

'''


def patch_send_data(text: str) -> str:
    start_marker = "// sendData sends new data segments."
    end_marker = "// +checklocks:s.ep.mu\nfunc (s *sender) enterRecovery()"
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    block = text[start:end]

    block = replace_once(
        block,
        "\tvar dataSent bool\n\tfor seg := s.writeNext;",
        "\trate := s.preparePacing()\n\n"
        "\tvar dataSent bool\n\tfor seg := s.writeNext;",
        "sendData pacing initialization",
    )

    block = replace_once(
        block,
        "\n\t\tif sent := s.maybeSendSegment(seg, limit, end); !sent {",
        "\n\t\tif !s.admitPacedSend(rate, s.pacingBytes(seg, limit)) {\n"
        "\t\t\tbreak\n"
        "\t\t}\n\n"
        "\t\tif sent := s.maybeSendSegment(seg, limit, end); !sent {",
        "sendData pacing admission",
    )

    block = replace_once(
        block,
        "\t\ts.updateWriteNext(seg.Next())\n\t}\n\n\ts.postXmit(dataSent, true /* shouldScheduleProbe */)",
        "\t\ts.updateWriteNext(seg.Next())\n"
        "\t\ts.accountPacedSend(rate, int64(seg.payloadSize()))\n"
        "\t}\n\n\ts.postXmit(dataSent, true /* shouldScheduleProbe */)",
        "sendData pacing accounting",
    )

    return text[:start] + block + text[end:]


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
    text = replace_once(
        text,
        "// sendData sends new data segments.",
        PACING_HELPERS + "// sendData sends new data segments.",
        "sender pacing helpers",
    )
    text = patch_send_data(text)
    path.write_text(text)


def patch_rack(path: Path) -> None:
    text = path.read_text()
    start_marker = "func (rc *rackControl) DoRecovery(_ *segment, fastRetransmit bool) {"
    start = text.index(start_marker)
    end = text.index("\n}\n", start) + 2
    block = text[start:end]

    block = replace_once(
        block,
        "\tsnd := rc.snd\n\tif fastRetransmit {\n\t\tsnd.resendSegment()\n\t}\n",
        "\tsnd := rc.snd\n"
        "\trate := snd.preparePacing()\n"
        "\tif fastRetransmit {\n"
        "\t\tfront := snd.writeList.Front()\n"
        "\t\tif front != nil && !snd.admitPacedSend(rate, snd.pacingBytes(front, snd.MaxPayloadSize)) {\n"
        "\t\t\treturn\n"
        "\t\t}\n"
        "\t\tsnd.resendSegment()\n"
        "\t\tif front != nil {\n"
        "\t\t\tsnd.accountPacedSend(rate, int64(front.payloadSize()))\n"
        "\t\t}\n"
        "\t}\n",
        "RACK fast retransmit pacing",
    )

    block = replace_once(
        block,
        "\t\tif sent := snd.maybeSendSegment(seg, int(snd.ep.scoreboard.SMSS()), snd.SndUna.Add(snd.SndWnd)); !sent {",
        "\t\tlimit := int(snd.ep.scoreboard.SMSS())\n"
        "\t\tif !snd.admitPacedSend(rate, snd.pacingBytes(seg, limit)) {\n"
        "\t\t\tbreak\n"
        "\t\t}\n\n"
        "\t\tif sent := snd.maybeSendSegment(seg, limit, snd.SndUna.Add(snd.SndWnd)); !sent {",
        "RACK recovery pacing admission",
    )

    block = replace_once(
        block,
        "\t\tdataSent = true\n\t\tsnd.Outstanding += snd.pCount(seg, snd.MaxPayloadSize)\n",
        "\t\tdataSent = true\n"
        "\t\tsnd.Outstanding += snd.pCount(seg, snd.MaxPayloadSize)\n"
        "\t\tsnd.accountPacedSend(rate, int64(seg.payloadSize()))\n",
        "RACK recovery pacing accounting",
    )

    path.write_text(text[:start] + block + text[end:])


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
    patch_rack(tcp / "rack.go")
    patch_restore(tcp / "endpoint_state.go")
    patch_cleanup(tcp / "endpoint.go")
    print(f"patched gVisor TCP at {root}")


if __name__ == "__main__":
    main()
