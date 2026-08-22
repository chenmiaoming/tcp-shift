#!/usr/bin/env python3
"""Apply the tcp-shift BBR/pacing integration to a resolved gVisor checkout.

The patcher intentionally preserves as much upstream source text as possible.
In particular, sender.sendData() is modified with narrow insertions rather than
being replaced wholesale. Delivery-rate sampling is also integrated through
small hooks in segment send/ACK/SACK paths. This keeps unrelated upstream TCP
changes in place and makes the floating-go CI job a useful compatibility signal.
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
        "\trate := s.pacingRate()\n"
        "\tif rate == 0 {\n"
        "\t\ts.pacingTimer.disable()\n"
        "\t\ts.pacingBudget = 0\n"
        "\t\ts.pacingLast = tcpip.MonotonicTime{}\n"
        "\t} else {\n"
        "\t\ts.refillPacingBudget(rate, s.ep.stack.Clock().NowMonotonic())\n"
        "\t}\n\n"
        "\tvar dataSent bool\n\tfor seg := s.writeNext;",
        "sendData pacing initialization",
    )

    block = replace_once(
        block,
        "\n\t\tif sent := s.maybeSendSegment(seg, limit, end); !sent {",
        "\n\t\tif rate != 0 {\n"
        "\t\t\tneed := seg.payloadSize()\n"
        "\t\t\tif need <= 0 || need > s.MaxPayloadSize {\n"
        "\t\t\t\tneed = s.MaxPayloadSize\n"
        "\t\t\t}\n"
        "\t\t\tif s.pacingBudget < int64(need) {\n"
        "\t\t\t\ts.schedulePacing(rate, int64(need))\n"
        "\t\t\t\tbreak\n"
        "\t\t\t}\n"
        "\t\t}\n\n"
        "\t\tif sent := s.maybeSendSegment(seg, limit, end); !sent {",
        "sendData pacing admission",
    )

    block = replace_once(
        block,
        "\t\ts.updateWriteNext(seg.Next())\n\t}\n\n\ts.postXmit(dataSent, true /* shouldScheduleProbe */)",
        "\t\ts.updateWriteNext(seg.Next())\n"
        "\t\tif rate != 0 {\n"
        "\t\t\ts.pacingBudget -= int64(seg.payloadSize())\n"
        "\t\t\tif s.pacingBudget < 0 {\n"
        "\t\t\t\ts.pacingBudget = 0\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n\n\ts.postXmit(dataSent, true /* shouldScheduleProbe */)",
        "sendData pacing accounting",
    )

    return text[:start] + block + text[end:]


def patch_segment(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\t// xmitTime is the last transmit time of this segment.\n\txmitTime  tcpip.MonotonicTime\n\txmitCount uint32\n\n\t// acked indicates if the segment has already been SACKed.',
        '\t// xmitTime is the last transmit time of this segment.\n\txmitTime  tcpip.MonotonicTime\n\txmitCount uint32\n\n'
        '\t// Delivery-rate sampling metadata. These fields mirror the per-skb\n'
        '\t// delivery snapshot used by Linux TCP rate sampling and are populated\n'
        '\t// only for outgoing data.\n'
        '\trateDelivered     uint64                `state:"nosave"`\n'
        '\trateDeliveredTime tcpip.MonotonicTime `state:"nosave"`\n'
        '\trateFirstTxTime   tcpip.MonotonicTime `state:"nosave"`\n'
        '\trateAppLimited    bool                  `state:"nosave"`\n'
        '\trateSampleValid   bool                  `state:"nosave"`\n\n'
        '\t// acked indicates if the segment has already been SACKed.',
        "segment rate metadata",
    )
    text = replace_once(
        text,
        '\tt.xmitTime = s.xmitTime\n\tt.xmitCount = s.xmitCount\n\tt.ep = s.ep',
        '\tt.xmitTime = s.xmitTime\n'
        '\tt.xmitCount = s.xmitCount\n'
        '\tt.rateDelivered = s.rateDelivered\n'
        '\tt.rateDeliveredTime = s.rateDeliveredTime\n'
        '\tt.rateFirstTxTime = s.rateFirstTxTime\n'
        '\tt.rateAppLimited = s.rateAppLimited\n'
        '\tt.rateSampleValid = s.rateSampleValid\n'
        '\tt.ep = s.ep',
        "segment clone rate metadata",
    )
    path.write_text(text)


def patch_sender(path: Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        '\t// cc is the congestion control algorithm in use for this sender.\n\tcc congestionControl\n',
        '\t// cc is the congestion control algorithm in use for this sender.\n'
        '\tcc congestionControl\n\n'
        '\t// Generic delivery-rate sampling state. BBR consumes deliveryRate,\n'
        '\t// while Reno/CUBIC simply ignore it. Keep it outside bbrState so the\n'
        '\t// TCP sender owns delivery accounting, matching Linux TCP.\n'
        '\trateDelivered     uint64                `state:"nosave"`\n'
        '\trateDeliveredTime tcpip.MonotonicTime `state:"nosave"`\n'
        '\trateFirstTxTime   tcpip.MonotonicTime `state:"nosave"`\n'
        '\tdeliveryRate      deliveryRateSample   `state:"nosave"`\n'
        '\trateCandidate     deliveryRateCandidate `state:"nosave"`\n',
        "sender rate state",
    )
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

    text = replace_once(
        text,
        'func (s *sender) handleRcvdSegment(rcvdSeg *segment) {\n\tbestRTT := unknownRTT',
        'func (s *sender) handleRcvdSegment(rcvdSeg *segment) {\n'
        '\ts.rateSampleBegin()\n'
        '\tbestRTT := unknownRTT',
        "ACK rate sample begin",
    )
    text = replace_once(
        text,
        '\t\t\tif sb.Start.LessThanEq(seg.sequenceNumber) && !seg.acked {\n\t\t\t\ts.rc.update(seg, rcvdSeg)',
        '\t\t\tif sb.Start.LessThanEq(seg.sequenceNumber) && !seg.acked {\n'
        '\t\t\t\ts.rateSampleDelivered(seg, seg.payloadSize(), rcvdSeg.rcvdTime)\n'
        '\t\t\t\ts.rc.update(seg, rcvdSeg)',
        "SACK delivery accounting",
    )
    text = replace_once(
        text,
        '\t\t\tif datalen > ackLeft {\n\t\t\t\tprevCount := s.pCount(seg, s.MaxPayloadSize)',
        '\t\t\tif datalen > ackLeft {\n'
        '\t\t\t\tif !s.rateSegmentAlreadyDelivered(seg) {\n'
        '\t\t\t\t\tdelivered := int(ackLeft)\n'
        '\t\t\t\t\tif delivered > seg.payloadSize() {\n'
        '\t\t\t\t\t\tdelivered = seg.payloadSize()\n'
        '\t\t\t\t\t}\n'
        '\t\t\t\t\ts.rateSampleDelivered(seg, delivered, rcvdSeg.rcvdTime)\n'
        '\t\t\t\t}\n'
        '\t\t\t\tprevCount := s.pCount(seg, s.MaxPayloadSize)',
        "partial cumulative ACK delivery accounting",
    )
    text = replace_once(
        text,
        '\t\t\ts.writeList.Remove(seg)\n\n\t\t\t// If SACK is enabled then only reduce outstanding if',
        '\t\t\tif !s.rateSegmentAlreadyDelivered(seg) {\n'
        '\t\t\t\ts.rateSampleDelivered(seg, seg.payloadSize(), rcvdSeg.rcvdTime)\n'
        '\t\t\t}\n'
        '\t\t\ts.writeList.Remove(seg)\n\n'
        '\t\t\t// If SACK is enabled then only reduce outstanding if',
        "full cumulative ACK delivery accounting",
    )
    text = replace_once(
        text,
        '\t\t// Clear SACK information for all acked data.\n\t\ts.ep.scoreboard.Delete(s.SndUna)',
        '\t\ts.rateSampleEnd(rcvdSeg.rcvdTime)\n\n'
        '\t\t// Clear SACK information for all acked data.\n'
        '\t\ts.ep.scoreboard.Delete(s.SndUna)',
        "delivery rate sample finalize",
    )
    text = replace_once(
        text,
        '\tseg.xmitTime = s.ep.stack.Clock().NowMonotonic()\n\tseg.xmitCount++',
        '\tnow := s.ep.stack.Clock().NowMonotonic()\n'
        '\ts.rateSampleOnSend(seg, now)\n'
        '\tseg.xmitTime = now\n'
        '\tseg.xmitCount++',
        "send delivery snapshot",
    )
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

    patch_root = Path(__file__).resolve().parents[1] / "patches/gvisor/tcp"
    shutil.copy2(patch_root / "bbr.go", tcp / "bbr.go")
    shutil.copy2(patch_root / "rate.go", tcp / "rate.go")
    patch_protocol(tcp / "protocol.go")
    patch_segment(tcp / "segment.go")
    patch_sender(tcp / "snd.go")
    patch_restore(tcp / "endpoint_state.go")
    patch_cleanup(tcp / "endpoint.go")
    print(f"patched gVisor TCP at {root}")


if __name__ == "__main__":
    main()
