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
	// ssthresh+3. BBR instead follows Linux packet conservation once TCP has
	// established the recovery state: use independent tcp_packets_in_flight-like
	// accounting plus newly delivered packets, never RFC6675 SetPipe/Outstanding.
	inRecovery           bool
	recoveryPriorCwnd    int
	packetConservation   bool
	recoveryEntryPending bool
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

func nonNegativeUint(v int) uint64 {
	if v <= 0 {
		return 0
	}
	return uint64(v)
}

// recordModelDiagnostics exposes only the small set of values needed to decide
// whether throughput is limited by delivery-rate estimation or by the pacer.
// Values are cumulative sums over delivery samples so the existing periodic
// Stack.Stats() logger can report them without adding a new debug interface.
func (b *bbrState) recordModelDiagnostics() {
	stats := b.s.ep.stack.Stats().TCP
	stats.TCPShiftBBRMaxBWSum.IncrementBy(b.maxBW)
	stats.TCPShiftBBRPacingRateSum.IncrementBy(b.PacingRate())
	stats.TCPShiftBBRCwndSum.IncrementBy(nonNegativeUint(b.s.SndCwnd))
	stats.TCPShiftBBRCwndTargetSum.IncrementBy(nonNegativeUint(b.bdpPackets(bbrCwndGain)))
	if b.minRTT > 0 && b.minRTT != time.Duration(math.MaxInt64) {
		stats.TCPShiftBBRMinRTTMicrosSum.IncrementBy(uint64(b.minRTT / time.Microsecond))
	}
	switch b.mode {
	case bbrStartup:
		stats.TCPShiftBBRStartupSamples.Increment()
	case bbrDrain:
		stats.TCPShiftBBRDrainSamples.Increment()
	case bbrProbeBW:
		stats.TCPShiftBBRProbeBWSamples.Increment()
	case bbrProbeRTT:
		stats.TCPShiftBBRProbeRTTSamples.Increment()
	}
}

// OnDeliveryRateSample consumes the TCP-owned delivery sample. The callback is
// ordered after loss detection/enterRecovery and before RACK/SACK recovery
// transmits. This is the point where Linux BBR's custom cong_control sees the
// current CA state and struct rate_sample.
func (b *bbrState) OnDeliveryRateSample(rs deliveryRateSample) {
	b.updateBandwidth(rs)
	b.checkFullBandwidth(rs)

	currentInFlight := b.s.linuxLikePacketsInFlight()
	stats := b.s.ep.stack.Stats().TCP
	stats.TCPShiftBBRSamples.Increment()
	stats.TCPShiftBBRPriorInflightSum.IncrementBy(nonNegativeUint(rs.priorInFlight))
	stats.TCPShiftBBRCurrentInflightSum.IncrementBy(nonNegativeUint(currentInFlight))
	stats.TCPShiftBBROutstandingSum.IncrementBy(nonNegativeUint(b.s.Outstanding))
	if currentInFlight != b.s.Outstanding {
		stats.TCPShiftBBRInflightMismatchSamples.Increment()
	}
	b.recordModelDiagnostics()

	if !b.inRecovery || !b.s.FastRecovery.Active {
		return
	}

	acked := rs.ackedSacked
	if acked < 0 {
		acked = 0
	}
	target := max(currentInFlight+acked, 4)

	// Linux enters packet conservation in bbr_set_cwnd(), after model/round
	// updates for the ACK. HandleLossDetected runs earlier in netstack, so defer
	// activation until here; otherwise a round boundary on the recovery-entry
	// ACK could immediately clear the newly-created conservation state.
	if b.recoveryEntryPending {
		b.packetConservation = true
		b.recoveryEntryPending = false
		b.nextRoundDelivered = rs.totalDelivered
		b.s.SndCwnd = target
		b.s.Ssthresh = target
		return
	}

	if b.packetConservation {
		// Subsequent ACKs in the first recovery round may grow cwnd only enough
		// to replace packets proven delivered, matching Linux's
		// max(cwnd, tcp_packets_in_flight(tp) + acked).
		if target > b.s.SndCwnd {
			b.s.SndCwnd = target
		}
		b.s.Ssthresh = b.s.SndCwnd
	}
}

func (b *bbrState) updateBandwidth(rs deliveryRateSample) {
	b.roundStart = false
	if !rs.valid || rs.rate == 0 || rs.interval <= 0 || rs.delivered == 0 {
		return
	}

	// Linux tcp_rate_gen rejects delivery samples shorter than tcp_min_rtt().
	// Such samples are common after a spurious retransmission: the retransmitted
	// skb gets a fresh send timestamp even though the receiver may already have
	// the original data, so its apparent delivery interval can be less than one
	// physical RTT. Letting those samples advance BBR's packet-timed round would
	// age the 10-round maxBW filter much faster than the path can actually turn
	// over. tcp-shift currently exposes minRTT through BBR rather than generic
	// TCP state, so apply the same validity gate here before round accounting.
	if b.minRTT > 0 && b.minRTT != time.Duration(math.MaxInt64) && rs.interval < b.minRTT {
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
		// Linux packet conservation lasts only through the first packet-timed
		// recovery round. A recovery-entry transition pending on this same ACK
		// will be activated later in OnDeliveryRateSample().
		b.packetConservation = false
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

	if b.mode == bbrDrain && b.s.linuxLikePacketsInFlight() <= b.bdpPackets(1000) {
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

	if packetsAcked <= 0 || b.mode == bbrProbeRTT || b.packetConservation || b.recoveryEntryPending {
		return
	}

	target := b.bdpPackets(bbrCwndGain)
	inFlight := b.s.linuxLikePacketsInFlight()
	switch b.mode {
	case bbrStartup:
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
		} else if b.s.SndCwnd > 2*target && inFlight < b.s.SndCwnd {
			b.s.SndCwnd = max(target, inFlight+packetsAcked)
			}
	}
	if b.s.SndCwnd < 4 {
		b.s.SndCwnd = 4
	}
	b.s.Ssthresh = b.s.SndCwnd
}

// HandleLossDetected bridges netstack's Reno-shaped recovery entry to BBR.
// The temporary ssthresh is based on Linux-like in-flight state only so
// enterRecovery() cannot expose the large model cwnd to SetPipe. The precise
// Linux packet-conservation cwnd is installed by OnDeliveryRateSample after
// FastRecovery.Active becomes true and before recovery sends retransmissions.
func (b *bbrState) HandleLossDetected() {
	if !b.inRecovery {
		b.recoveryPriorCwnd = max(b.s.SndCwnd, 4)
		b.inRecovery = true
		recoveryFlight := b.s.linuxLikePacketsInFlight()
		b.s.Ssthresh = max(recoveryFlight, 4)
	}
	b.recoveryEntryPending = true
}

// HandleRTOExpired follows an important Linux BBR invariant: an RTO may collapse
// the sending cwnd, but it does not erase the bottleneck-bandwidth model.
func (b *bbrState) HandleRTOExpired() {
	b.inRecovery = false
	b.recoveryPriorCwnd = 0
	b.packetConservation = false
	b.recoveryEntryPending = false
	b.roundStart = false

	b.s.SndCwnd = 1
	if b.s.Ssthresh < 4 {
		b.s.Ssthresh = 4
	}
}

func (b *bbrState) PostRecovery() {
	if !b.inRecovery {
		return
	}

	restored := max(b.recoveryPriorCwnd, 4)
	b.s.SndCwnd = restored
	b.s.Ssthresh = restored
	b.recoveryPriorCwnd = 0
	b.inRecovery = false
	b.packetConservation = false
	b.recoveryEntryPending = false
}
