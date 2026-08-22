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
// packet-count congestion window. Bandwidth comes from the TCP-owned delivery
// sampler and is filtered over packet-timed rounds, following Linux BBR's
// next_rtt_delivered/rtt_cnt model. In particular, this is a 10-round window,
// not a 10-ACK window; the distinction is critical on high-BDP paths.
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

// OnDeliveryRateSample consumes the TCP-owned delivery sample. Linux BBR does
// its bandwidth/round accounting directly from struct rate_sample before cwnd
// is updated; this callback gives tcp-shift the same ordering without making the
// generic sampler depend on BBR.
func (b *bbrState) OnDeliveryRateSample(rs deliveryRateSample) {
	b.updateBandwidth(rs)
	b.checkFullBandwidth(rs)
}

func (b *bbrState) updateBandwidth(rs deliveryRateSample) {
	b.roundStart = false
	if !rs.valid || rs.rate == 0 || rs.interval <= 0 || rs.delivered == 0 {
		return
	}

	// Linux BBR starts a new packet-timed round when the packet that generated
	// this rate sample was sent before next_rtt_delivered. Use cumulative bytes
	// rather than packets because the tcp-shift sampler's delivered counter is
	// byte-based; the ordering invariant is identical.
	if rs.priorDelivered >= b.nextRoundDelivered {
		b.nextRoundDelivered = rs.totalDelivered
		b.roundCount++
		b.roundStart = true
	}

	// Application-limited samples are ignored when they are below the current
	// network model, exactly so application think-time cannot drag maxBW down.
	if rs.isAppLimited && rs.rate < b.maxBW {
		return
	}

	// Keep one maximum sample for each packet-timed round. A bucket is reused
	// only after its round number has aged out; recomputing the maximum across
	// the ten buckets is tiny and avoids embedding Linux's minmax helper.
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
	b.updateMinRTT(rtt, ackTime)
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
		// Linux BBR slow-starts cwnd back toward the model target after loss or
		// an RTO rather than permanently carrying a loss-based multiplicative
		// reduction. Do the same here.
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

// HandleRTOExpired follows an important Linux BBR invariant: an RTO may collapse
// the sending cwnd, but it does not erase the bottleneck-bandwidth model. The
// prior implementation reset maxBW and STARTUP on every RTO, which made random
// loss destroy the path model and forced BBR to relearn the link repeatedly.
func (b *bbrState) HandleRTOExpired() {
	b.inRecovery = false
	b.recoveryPriorCwnd = 0
	b.roundStart = false

	// gVisor delegates the RFC5681 cwnd collapse to the congestion-control
	// implementation. Use one packet here, as its Reno path does, then let BBR's
	// normal ACK processing grow back toward the preserved model target.
	b.s.SndCwnd = 1
	if b.s.Ssthresh < 4 {
		b.s.Ssthresh = 4
	}
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
