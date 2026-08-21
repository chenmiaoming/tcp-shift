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

// bbrState implements a small BBRv1-inspired model on top of netstack's
// packet-count congestion window. Bandwidth is sampled from cumulatively ACKed
// data over ACK arrival intervals. A generic sender pacer consumes PacingRate.
//
// The ACK-rate sampler is intentionally simpler than Linux's per-packet
// delivery-rate sampler. This keeps the first tcp-shift experiment small while
// preserving the two properties that matter for the experiment: a bandwidth /
// min-RTT model and pacing independent of cwnd growth.
type bbrState struct {
	s *sender

	mode bbrMode

	minRTT      time.Duration
	minRTTStamp tcpip.MonotonicTime

	bwSamples [bbrBandwidthWindow]uint64
	bwIndex   int
	maxBW     uint64 // bytes/second

	lastAckTime tcpip.MonotonicTime
	roundStart  tcpip.MonotonicTime
	fullBW      uint64
	fullBWRound int

	cycleIndex int
	cycleStamp tcpip.MonotonicTime

	priorCwnd       int
	probeRTTDone    tcpip.MonotonicTime
	probeRTTStarted bool
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

func (b *bbrState) updateBandwidth(packetsAcked int, ackTime tcpip.MonotonicTime) {
	if packetsAcked <= 0 {
		return
	}
	if b.lastAckTime != (tcpip.MonotonicTime{}) {
		interval := ackTime.Sub(b.lastAckTime)
		if interval > 0 {
			ackedBytes := uint64(packetsAcked) * uint64(b.s.MaxPayloadSize)
			sample := ackedBytes * uint64(time.Second) / uint64(interval)
			b.bwSamples[b.bwIndex] = sample
			b.bwIndex = (b.bwIndex + 1) % len(b.bwSamples)
			var maxSample uint64
			for _, v := range b.bwSamples {
				if v > maxSample {
					maxSample = v
				}
			}
			b.maxBW = maxSample
		}
	}
	b.lastAckTime = ackTime
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
		b.priorCwnd = b.s.SndCwnd
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
			if b.priorCwnd > target {
				target = b.priorCwnd
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

// HandleLossDetected keeps BBR model-driven rather than applying Reno/CUBIC's
// multiplicative decrease. Netstack recovery still handles retransmission.
func (b *bbrState) HandleLossDetected() {
	b.s.Ssthresh = max(b.s.SndCwnd, 4)
}

// HandleRTOExpired restarts model acquisition after a hard timeout.
func (b *bbrState) HandleRTOExpired() {
	b.mode = bbrStartup
	b.maxBW = 0
	b.bwSamples = [bbrBandwidthWindow]uint64{}
	b.bwIndex = 0
	b.fullBW = 0
	b.fullBWRound = 0
	b.lastAckTime = tcpip.MonotonicTime{}
	b.roundStart = tcpip.MonotonicTime{}
	b.s.SndCwnd = 4
	b.s.Ssthresh = 4
}

func (b *bbrState) PostRecovery() {
	// Netstack restores cwnd from ssthresh when leaving recovery. Keep
	// ssthresh synchronized with the model-driven cwnd in Update/loss paths.
}
