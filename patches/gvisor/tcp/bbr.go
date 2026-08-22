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

type bbrBandwidthBucket struct {
	round uint64
	rate  uint64
}

// bbrState implements a compact BBRv1-inspired model on top of netstack's
// packet-count congestion window. Bandwidth and cwnd are driven by the
// TCP-owned per-delivery rate sample, following Linux BBR's custom
// cong_control() model rather than gVisor's Reno/CUBIC-oriented Update hook.
type bbrState struct {
	s *sender

	mode bbrMode

	minRTT      time.Duration
	minRTTStamp tcpip.MonotonicTime

	bwRounds [bbrBandwidthWindow]bbrBandwidthBucket
	maxBW    uint64 // bytes/second

	roundCount         uint64
	nextRoundDelivered uint64
	roundStart         bool
	fullBW             uint64
	fullBWRound        int

	cycleIndex int
	cycleStamp tcpip.MonotonicTime

	probeRTTPriorCwnd int
	probeRTTDone      tcpip.MonotonicTime
	probeRTTStarted   bool

	// Recovery state mirrors the essential BBRv1 semantics: remember the model
	// cwnd, conserve packets for the first packet-timed recovery round, then
	// allow cwnd to slow-start toward the BDP target. This differs from the
	// Reno/CUBIC contract assumed by gVisor's generic recovery code.
	inRecovery        bool
	recoveryPriorCwnd int
	packetConservation bool
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

// OnDeliveryRateSample is tcp-shift's equivalent of Linux BBR's custom
// cong_control() callback: it runs for ACK/SACK delivery even during recovery,
// after the sender has established the current recovery state and before the
// recovery path transmits more data.
func (b *bbrState) OnDeliveryRateSample(rs deliveryRateSample) {
	if !rs.valid {
		return
	}
	b.updateBandwidth(rs)
	b.checkFullBandwidth(rs)
	b.updateMode(rs.ackTime)
	b.updateCwndFromDelivery(rs)
}

func (b *bbrState) updateBandwidth(rs deliveryRateSample) {
	b.roundStart = false
	if rs.rate == 0 || rs.interval <= 0 || rs.delivered == 0 {
		return
	}

	// Linux BBR starts a new packet-timed round when the packet that generated
	// this rate sample was sent after the previous round boundary in delivered
	// space. Use cumulative bytes rather than packets; the ordering invariant is
	// identical.
	if rs.priorDelivered >= b.nextRoundDelivered {
		b.nextRoundDelivered = rs.totalDelivered
		b.roundCount++
		b.roundStart = true
		// Linux clears packet_conservation at the next packet-timed round.
		if b.packetConservation {
			b.packetConservation = false
		}
	}

	// Application-limited samples are ignored when they are below the current
	// network model, exactly so application think-time cannot drag maxBW down.
	if rs.isAppLimited && rs.rate < b.maxBW {
		return
	}

	idx := int(b.roundCount % uint64(len(b.bwRounds)))
	bucket := &b.bwRounds[idx]
	if bucket.round != b.roundCount {
		bucket.round = b.roundCount
		bucket.rate = rs.rate
	} else if rs.rate > bucket.rate {
		bucket.rate = rs.rate
	}

	var maxSample uint64
	for _, v := range b.bwRounds {
		if v.round == 0 || b.roundCount < v.round || b.roundCount-v.round >= bbrBandwidthWindow {
			continue
		}
		if v.rate > maxSample {
			maxSample = v.rate
		}
	}
	b.maxBW = maxSample
}

// checkFullBandwidth follows Linux BBR's STARTUP full-pipe test: only one test
// per packet-timed round, ignore app-limited rounds, require 25% growth to reset
// the counter, and declare the pipe full after three rounds without that growth.
func (b *bbrState) checkFullBandwidth(rs deliveryRateSample) {
	if b.mode != bbrStartup || !b.roundStart || rs.isAppLimited || b.maxBW == 0 {
		return
	}
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

func (b *bbrState) updateMode(now tcpip.MonotonicTime) {
	if now == (tcpip.MonotonicTime{}) {
		return
	}
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

// updateCwndFromDelivery mirrors the central BBRv1 recovery behavior. During
// the first recovery round, P newly delivered packets may release at most P
// packets (packet conservation). After that, cwnd can grow toward the model BDP
// target rather than remaining pinned to a loss-based ssthresh.
func (b *bbrState) updateCwndFromDelivery(rs deliveryRateSample) {
	acked := rs.ackedSacked
	if acked <= 0 {
		return
	}
	if b.mode == bbrProbeRTT {
		b.s.SndCwnd = 4
		b.s.Ssthresh = 4
		return
	}

	if b.inRecovery && b.s.FastRecovery.Active && b.packetConservation {
		cwnd := b.s.Outstanding + acked
		if cwnd > b.s.SndCwnd {
			b.s.SndCwnd = cwnd
		}
		if b.s.SndCwnd < 4 {
			b.s.SndCwnd = 4
		}
		b.s.Ssthresh = b.s.SndCwnd
		return
	}

	target := b.bdpPackets(bbrCwndGain)
	switch b.mode {
	case bbrStartup:
		b.s.SndCwnd += acked
		if b.maxBW != 0 && b.s.SndCwnd > 2*target {
			b.s.SndCwnd = 2 * target
		}
	case bbrDrain, bbrProbeBW:
		if b.s.SndCwnd < target {
			b.s.SndCwnd += acked
			if b.s.SndCwnd > target {
				b.s.SndCwnd = target
			}
		} else if b.s.SndCwnd > 2*target && b.s.Outstanding < b.s.SndCwnd {
			b.s.SndCwnd = max(target, b.s.Outstanding+acked)
		}
	}
	if b.s.SndCwnd < 4 {
		b.s.SndCwnd = 4
	}
	b.s.Ssthresh = b.s.SndCwnd
}

// Update implements gVisor's legacy Reno/CUBIC-oriented congestionControl
// interface. BBR cwnd/model updates are intentionally not done here anymore:
// gVisor suppresses Update while FastRecovery is active, while Linux BBR's
// custom cong_control callback continues to run on ACK/SACK delivery in
// recovery. Keep this hook only for RTT/minRTT information that gVisor already
// computes on cumulative ACKs.
func (b *bbrState) Update(_ int, rtt time.Duration, ackTime tcpip.MonotonicTime) {
	b.updateMinRTT(rtt, ackTime)
	b.updateMode(ackTime)
}

// HandleLossDetected saves the model cwnd and establishes a one-round packet
// conservation phase. gVisor's enterRecovery() immediately assigns
// cwnd=ssthresh+3, so provide the current pipe estimate as the temporary
// ssthresh; per-delivery BBR control then governs further cwnd changes.
func (b *bbrState) HandleLossDetected() {
	if !b.inRecovery {
		b.recoveryPriorCwnd = max(b.s.SndCwnd, 4)
		b.inRecovery = true
	}
	b.packetConservation = true
	b.nextRoundDelivered = b.s.rateDelivered
	b.s.Ssthresh = max(b.s.Outstanding, 4)
}

// HandleRTOExpired follows an important Linux BBR invariant: an RTO may collapse
// the sending cwnd, but it does not erase the bottleneck-bandwidth model.
func (b *bbrState) HandleRTOExpired() {
	b.inRecovery = false
	b.recoveryPriorCwnd = 0
	b.packetConservation = false
	b.roundStart = false

	// gVisor delegates the RFC5681 cwnd collapse to the congestion-control
	// implementation. Collapse to one packet, then let per-delivery BBR control
	// grow back toward the preserved model target.
	b.s.SndCwnd = 1
	if b.s.Ssthresh < 4 {
		b.s.Ssthresh = 4
	}
}

func (b *bbrState) PostRecovery() {
	if !b.inRecovery {
		return
	}

	// Restore the model-driven window saved on entry. A following delivery
	// sample can then bring it back toward the current BDP target.
	restored := max(b.recoveryPriorCwnd, 4)
	if b.s.SndCwnd > restored {
		restored = b.s.SndCwnd
	}
	b.s.SndCwnd = restored
	b.s.Ssthresh = restored
	b.recoveryPriorCwnd = 0
	b.inRecovery = false
	b.packetConservation = false
}
