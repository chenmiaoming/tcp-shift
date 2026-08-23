#!/usr/bin/env python3
"""Mirror Linux BBR's persistent STARTUP pacing-rate semantics.

Linux BBR does not expose pacing as a pure function of the current max_bw.
It initializes sk_pacing_rate from initial_cwnd and a nominal 1ms RTT, repeats
that initialization when the first real SRTT is available, and then refuses to
lower pacing_rate until full_bw has been reached. After STARTUP, pacing follows
current bw * mode gain normally.

The tcp-shift model previously returned maxBW * gain directly from PacingRate().
That lets a recovery-induced drop/aging of maxBW immediately lower the sender's
pacing rate while STARTUP is still trying to discover capacity, creating a
self-locking low-bandwidth model. This post-patch keeps the experiment isolated
from recovery, maxBW filtering, gains, and loss detection.
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
    anchor = "\tTCPShiftBBRFullBWPriorOnRTOSum             *StatCounter\n"
    addition = anchor + """\tTCPShiftBBRPacingNominalInits              *StatCounter
\tTCPShiftBBRPacingRTTReinits               *StatCounter
\tTCPShiftBBRPacingRTTReinitRateSum          *StatCounter
\tTCPShiftBBRStartupPacingRaises             *StatCounter
\tTCPShiftBBRStartupPacingFloorHolds         *StatCounter
"""
    text = replace_once(text, anchor, addition, "BBR startup pacing diagnostics")
    path.write_text(text)


def patch_bbr(path: Path) -> None:
    text = path.read_text()

    text = replace_once(
        text,
        "\tfullBW             uint64\n\tfullBWRound        int\n\n\tcycleIndex int\n",
        "\tfullBW             uint64\n\tfullBWRound        int\n\n"
        "\t// Linux BBR keeps sk_pacing_rate as state. Before full_bw is reached,\n"
        "\t// the rate may increase but must not fall with a transient maxBW dip.\n"
        "\tpacingRate    uint64\n"
        "\tpacingSeenRTT bool\n\n"
        "\tcycleIndex int\n",
        "BBR persistent pacing fields",
    )

    old_new = """func newBBRCC(s *sender) *bbrState {
\treturn &bbrState{
\t\ts:      s,
\t\tmode:   bbrStartup,
\t\tminRTT: time.Duration(math.MaxInt64),
\t}
}
"""
    new_new = """func newBBRCC(s *sender) *bbrState {
\tb := &bbrState{
\t\ts:      s,
\t\tmode:   bbrStartup,
\t\tminRTT: time.Duration(math.MaxInt64),
\t}
\t// Linux bbr_init_pacing_rate_from_rtt() uses a nominal 1ms RTT until a
\t// real SRTT exists. cwnd still limits the initial burst; this only avoids
\t// deriving the first bandwidth sample from an artificially tiny pace.
\tb.pacingRate = b.pacingRateFromCwnd(time.Millisecond)
\tif b.pacingRate != 0 {
\t\tb.s.ep.stack.Stats().TCP.TCPShiftBBRPacingNominalInits.Increment()
\t}
\treturn b
}
"""
    text = replace_once(text, old_new, new_new, "BBR nominal pacing initialization")

    old_pacing = """// PacingRate is consumed by sender.sendData's generic pacer.
func (b *bbrState) PacingRate() uint64 {
\tif b.maxBW == 0 {
\t\treturn 0
\t}
\tgain := uint64(1000)
\tswitch b.mode {
\tcase bbrStartup:
\t\tgain = bbrStartupGain
\tcase bbrDrain:
\t\tgain = bbrDrainGain
\tcase bbrProbeBW:
\t\tgain = bbrProbeBWGain[b.cycleIndex]
\tcase bbrProbeRTT:
\t\tgain = 1000
\t}
\treturn mulGain(b.maxBW, gain)
}
"""
    new_pacing = """// PacingRate is consumed by sender.sendData's generic pacer. Unlike the
// old pure maxBW*gain expression, this is persistent state, matching Linux's
// sk_pacing_rate behavior during STARTUP.
func (b *bbrState) PacingRate() uint64 {
\treturn b.pacingRate
}

func (b *bbrState) pacingGain() uint64 {
\tswitch b.mode {
\tcase bbrStartup:
\t\treturn bbrStartupGain
\tcase bbrDrain:
\t\treturn bbrDrainGain
\tcase bbrProbeBW:
\t\treturn bbrProbeBWGain[b.cycleIndex]
\tcase bbrProbeRTT:
\t\treturn 1000
\tdefault:
\t\treturn 1000
\t}
}

func (b *bbrState) pacingRateFromCwnd(rtt time.Duration) uint64 {
\tif rtt <= 0 || b.s.SndCwnd <= 0 || b.s.MaxPayloadSize <= 0 {
\t\treturn 0
\t}
\tbytes := uint64(b.s.SndCwnd) * uint64(b.s.MaxPayloadSize)
\tsecond := uint64(time.Second)
\tvar rate uint64
\tif bytes > math.MaxUint64/second {
\t\trate = math.MaxUint64 / uint64(rtt)
\t} else {
\t\trate = bytes * second / uint64(rtt)
\t}
\treturn mulGain(rate, bbrStartupGain)
}

// maybeReinitPacingFromRTT mirrors Linux's one-time transition from the
// nominal 1ms initialization to the first real smoothed RTT. sender.updateRTO
// has already updated SRTT before congestionControl.Update is called.
func (b *bbrState) maybeReinitPacingFromRTT(rtt time.Duration) {
\tif b.pacingSeenRTT {
\t\treturn
\t}
\tb.s.rtt.Lock()
\tsrtt := b.s.rtt.TCPRTTState.SRTT
\tb.s.rtt.Unlock()
\tif srtt <= 0 {
\t\tif rtt <= 0 {
\t\t\treturn
\t\t}
\t\tsrtt = rtt
\t}
\trate := b.pacingRateFromCwnd(srtt)
\tif rate == 0 {
\t\treturn
\t}
\tb.pacingRate = rate
\tb.pacingSeenRTT = true
\tstats := b.s.ep.stack.Stats().TCP
\tstats.TCPShiftBBRPacingRTTReinits.Increment()
\tstats.TCPShiftBBRPacingRTTReinitRateSum.IncrementBy(rate)
}

// updatePacingRateFromModel follows Linux bbr_set_pacing_rate(): before full
// bandwidth is reached, only raise the persistent pace. Once STARTUP exits,
// permit the current mode gain and maxBW model to move pacing in either
// direction.
func (b *bbrState) updatePacingRateFromModel() {
\tif b.maxBW == 0 {
\t\treturn
\t}
\tcandidate := mulGain(b.maxBW, b.pacingGain())
\tif b.mode == bbrStartup {
\t\tstats := b.s.ep.stack.Stats().TCP
\t\tif candidate > b.pacingRate {
\t\t\tb.pacingRate = candidate
\t\t\tstats.TCPShiftBBRStartupPacingRaises.Increment()
\t\t} else if candidate < b.pacingRate {
\t\t\tstats.TCPShiftBBRStartupPacingFloorHolds.Increment()
\t\t}
\t\treturn
\t}
\tb.pacingRate = candidate
}
"""
    text = replace_once(text, old_pacing, new_pacing, "BBR persistent pacing implementation")

    text = replace_once(
        text,
        "\tb.updateBandwidth(rs)\n\tb.checkFullBandwidth(rs)\n\n\tcurrentInFlight := b.s.linuxLikePacketsInFlight()\n",
        "\tb.updateBandwidth(rs)\n\tb.checkFullBandwidth(rs)\n\tb.updatePacingRateFromModel()\n\n\tcurrentInFlight := b.s.linuxLikePacketsInFlight()\n",
        "BBR model pacing update before recovery",
    )

    text = replace_once(
        text,
        "\tb.updateMinRTT(rtt, ackTime)\n\tb.updateMode(ackTime)\n",
        "\tb.updateMinRTT(rtt, ackTime)\n\tb.maybeReinitPacingFromRTT(rtt)\n\tb.updateMode(ackTime)\n",
        "BBR first RTT pacing reinitialization",
    )

    path.write_text(text)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_bbr_startup_pacing.py <gvisor-root>")
    root = Path(sys.argv[1]).resolve()
    tcp = root / "pkg/tcpip/transport/tcp"
    if not tcp.is_dir():
        raise SystemExit(f"not a patched gVisor source tree: {root}")

    patch_tcp_stats(root / "pkg/tcpip/tcpip.go")
    patch_bbr(tcp / "bbr.go")
    print(f"patched Linux-style persistent BBR STARTUP pacing at {root}")


if __name__ == "__main__":
    main()
