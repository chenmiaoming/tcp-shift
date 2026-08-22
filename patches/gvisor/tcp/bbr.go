// Copyright 2026 tcp-shift authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// This is an original, compact BBRv1-inspired implementation for tcp-shift
// experiments. It is not copied from Linux and is not intended to be a
// bit-for-bit implementation of Linux BBR.

package tcp

import (
	"math"
	"time"

	"gvisor.dev/gvisor/pkg/tcpip"
)

const (
	bbrGainScale       = uint64(1000)
	bbrStartupGain     = uint64(2885)
	bbrDrainGain       = uint64(347)
	bbrCwndGain        = uint64(2000)
	bbrProbeRTTTime    = 200 * time.Millisecond
	bbrMinRTTWindow    = 10 * time.Second
	bbrFullBWRounds    = 3
	bbrBandwidthWindow = 10
)

var bbrProbeBWGain = [...]uint64{1250, 750, 1000, 1000, 1000, 1000, 1000, 1000}

type bbrMode uint8

const (
	bbrStartup bbrMode = iota
	bbrDrain
	bbrProbeBW
	bbrProbeRTT
)

// bbrState implements a compact BBRv1-inspired model on top of netstack's
// packet-count congestion window. Bandwidth comes from a TCP delivery-rate
// sampler: transmitted segments snapshot sender delivery state and ACK/SACK
// processing produces delivered/interval samples. This follows Linux BBR's
// measurement model much more closely than using adjacent ACK arrival times.
type bbrState struct {
	s *sender

	mode bbrMode

	minRTT      time.Duration
	minRTTStamp tcpip.MonotonicTime

	bwSamples [bbrBandwidthWindow]uint64
	bwIndex   int
	maxBW     uint64 // bytes/second

	roundStart  tcpip.MonotonicTime
	fullBW      uint64
	fullBWRound int

	cycleIndex int
	cycleStamp tcpip.MonotonicTime

	probeRTTPriorCwnd int
	probeRTTDone      tcpip.MonotonicTime
	probeRTTStarted   bool

	// Netstack's generic fast/SACK recovery assumes loss-based congestion
	// controls reduce ssthresh before enterRecovery(), which then sets cwnd to
	// ssthresh+3. BBR must not apply a Reno/CUBIC multiplicative decrease, but
	// it still needs packet conservation while recovery decides what to
	// retransmit. Save the model cwnd here, temporarily constrain recovery to
	// the estimated in-flight pipe, and restore the model cwnd in PostRecovery.
	inRecovery        bool
	recoveryPriorCwnd int
}

func newBBRCC(s *sender) *bbrState {
	return &bbrState{
		s:      s,
		mode:   bbrStartup,
		minRTT: time.Duration(math.MaxInt64),
	}
}

// PacingRate is consumed by sender.sendData's generic pacer.
func (b *bbrState) PacingRate() uint64 {
	if b.maxBW == 0 {
		return 0
	}
	gain := uint64(1000)
	switch b.mode {
	case bbrStartup:
		gain = bbrStartupGain
	case bbrDrain:
		gain = bbrDrainGain
	case bbrProbeBW:
		gain = bbrProbeBWGain[b.cycleIndex]
	case bbrProbeRTT:
		gain = 1000
	}
	return mulGain(b.maxBW, gain)
}

func mulGain(v, gain uint64) uint64 {
	if v > math.MaxUint64/gain {
		return math.MaxUint64 / bbrGainScale
	}
	return v * gain / bbrGainScale
}

func (b *bbrState) updateBandwidth(packetsAcked int, _ tcpip.MonotonicTime) {
	if packetsAcked <= 0 {
		return
	}

	rs := b.s.deliveryRate
	if !rs.valid || rs.rate == 0 || rs.interval <= 0 || rs.delivered == 0 {
		return
	}

	// Once app-limited detection is wired, a low app-limited sample must not
	// pull down the max filter; a higher sample is still useful evidence of
	// available bandwidth. Keeping this guard now makes the eventual app-limited
	// hook behavior explicit without changing the BBR API again.
	if rs.isAppLimited && rs.rate < b.maxBW {
		return
	}

	b.bwSamples[b.bwIndex] = rs.rate
	b.bwIndex = (b.bwIndex + 1) % len(b.bwSamples)
	var maxSample uint64
	for _, v := range b.bwSamples {
		if v > maxSample {
			maxSample = v
		}
	}
	b.maxBW = maxSample
}

func (b *bbrState) updateMinRTT(rtt time.Duration, now tcpip.MonotonicTime) {
	if rtt <= 0 {
		return
	}
	if rtt < b.minRTT || b.minRTT == time.Duration(math.MaxInt64) {
		b.minRTT = rtt
		b.minRTTStamp = now
	}
}

func (b *bbrState) bdpPackets(gain uint64) int {
	if b.maxBW == 0 || b.minRTT <= 0 || b.minRTT == time.Duration(math.MaxInt64) || b.s.MaxPayloadSize <= 0 {
		return max(InitialCwnd, 4)
	}
	bytes := b.maxBW * uint64(b.minRTT) / uint64(time.Second)
	bytes = mulGain(bytes, gain)
	packets := int((bytes + uint64(b.s.MaxPayloadSize) - 1) / uint64(b.s.MaxPayloadSize))
	if packets < 4 {
		packets = 4
	}
	return packets
}

func (b *bbrState) updateFullBandwidth(now tcpip.MonotonicTime) {
	if b.mode != bbrStartup || b.minRTT == time.Duration(math.MaxInt64) || b.maxBW == 0 {
		return
	}
	if b.roundStart == (tcpip.MonotonicTime{}) {
		b.roundStart = now
		b.fullBW = b.maxBW
		return
	}
	if now.Sub(b.roundStart) < b.minRTT {
		return
	}
	b.roundStart = now

	if b.fullBW == 0 || b.maxBW >= b.fullBW*5/4 {
		b.fullBW = b.maxBW
		b.fullBWRound = 0
		return
	}
	b.fullBWRound++
	if b.fullBWRound >= bbrFullBWRounds {
		b.mode = bbrDrain
	}
}

func (b *bbrState) updateMode(now tcpip.MonotonicTime) {
	if b.minRTT != time.Duration(math.MaxInt64) && b.minRTTStamp != (tcpip.MonotonicTime{}) && b.mode != bbrProbeRTT && now.Sub(b.minRTTStamp) >= bbrMinRTTWindow {
		b.probeRTTPriorCwnd = b.s.SndCwnd
		b.mode = bbrProbeRTT
		b.probeRTTDone = now.Add(bbrProbeRTTTime)
		b.probeRTTStarted = true
		b.s.SndCwnd = 4
		return
	}

	if b.mode == bbrProbeRTT {
		b.s.SndCwnd = 4
		if b.probeRTTStarted && !now.Before(b.probeRTTDone) {
			b.minRTTStamp = now
			b.mode = bbrProbeBW
			b.cycleIndex = 0
			b.cycleStamp = now
			target := b.bdpPackets(bbrCwndGain)
			if b.probeRTTPriorCwnd > target {
				target = b.probeRTTPriorCwnd
			}
			b.s.SndCwnd = max(target, 4)
			b.probeRTTStarted = false
		}
		return
	}

	if b.mode == bbrDrain && b.s.Outstanding <= b.bdpPackets(1000) {
		b.mode = bbrProbeBW
		b.cycleIndex = 0
		b.cycleStamp = now
	}

	if b.mode == bbrProbeBW && b.minRTT != time.Duration(math.MaxInt64) {
		cycle := b.minRTT
		if cycle < 10*time.Millisecond {
			cycle = 10 * time.Millisecond
		}
		if b.cycleStamp == (tcpip.MonotonicTime{}) {
			b.cycleStamp = now
		} else if now.Sub(b.cycleStamp) >= cycle {
			b.cycleIndex = (b.cycleIndex + 1) % len(bbrProbeBWGain)
			b.cycleStamp = now
		}
	}
}

// Update implements congestionControl.Update.
func (b *bbrState) Update(packetsAcked int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
	b.updateBandwidth(packetsAcked, ackTime)
	b.updateMinRTT(rtt, ackTime)
	b.updateFullBandwidth(ackTime)
	b.updateMode(ackTime)

	if packetsAcked <= 0 || b.mode == bbrProbeRTT {
		return
	}

	target := b.bdpPackets(bbrCwndGain)
	switch b.mode {
	case bbrStartup:
		// Packet conservation during STARTUP: grow by newly ACKed packets,
		// roughly doubling once per RTT, while avoiding unbounded growth if
		// the bandwidth estimate has already converged.
		b.s.SndCwnd += packetsAcked
		if b.maxBW != 0 && b.s.SndCwnd > 2*target {
			b.s.SndCwnd = 2 * target
		}
	case bbrDrain, bbrProbeBW:
		if b.s.SndCwnd < target {
			b.s.SndCwnd += packetsAcked
			if b.s.SndCwnd > target {
				b.s.SndCwnd = target
			}
		} else if b.s.SndCwnd > 2*target && b.s.Outstanding < b.s.SndCwnd {
			b.s.SndCwnd = max(target, b.s.Outstanding+packetsAcked)
		}
	}
	if b.s.SndCwnd < 4 {
		b.s.SndCwnd = 4
	}
	b.s.Ssthresh = b.s.SndCwnd
}

// HandleLossDetected enters packet conservation rather than applying the
// multiplicative decrease used by loss-based congestion controls. Netstack's
// enterRecovery() immediately sets cwnd=ssthresh+3, so using the model cwnd as
// ssthresh lets RFC6675 recovery send a large burst whenever SetPipe() falls
// below that model window. Use the current pipe estimate instead and restore
// the model window in PostRecovery.
func (b *bbrState) HandleLossDetected() {
	if !b.inRecovery {
		b.recoveryPriorCwnd = max(b.s.SndCwnd, 4)
		b.inRecovery = true
	}
	b.s.Ssthresh = max(b.s.Outstanding, 4)
}

// HandleRTOExpired restarts model acquisition after a hard timeout.
func (b *bbrState) HandleRTOExpired() {
	b.mode = bbrStartup
	b.maxBW = 0
	b.bwSamples = [bbrBandwidthWindow]uint64{}
	b.bwIndex = 0
	b.fullBW = 0
	b.fullBWRound = 0
	b.roundStart = tcpip.MonotonicTime{}
	b.inRecovery = false
	b.recoveryPriorCwnd = 0
	b.s.SndCwnd = 4
	b.s.Ssthresh = 4
}

func (b *bbrState) PostRecovery() {
	if !b.inRecovery {
		return
	}

	// leaveRecovery() has just assigned SndCwnd=Ssthresh. Restore the
	// model-driven window saved on entry instead of carrying the temporary
	// packet-conservation window into the open state.
	restored := max(b.recoveryPriorCwnd, 4)
	b.s.SndCwnd = restored
	b.s.Ssthresh = restored
	b.recoveryPriorCwnd = 0
	b.inRecovery = false
}
