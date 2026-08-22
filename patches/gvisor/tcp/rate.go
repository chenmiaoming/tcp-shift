// Copyright 2026 tcp-shift authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0

package tcp

import (
	"time"

	"gvisor.dev/gvisor/pkg/tcpip"
)

// deliveryRateSample is the sender-wide result of a Linux-style delivery-rate
// sample. It intentionally carries both the delta delivered by this sample and
// the sender's cumulative delivered counters. BBR uses priorDelivered and
// totalDelivered to define packet-timed round trips, matching Linux's
// next_rtt_delivered/rtt_cnt model instead of using wall-clock RTT boundaries.
type deliveryRateSample struct {
	valid          bool
	delivered      uint64
	priorDelivered uint64
	totalDelivered uint64
	interval       time.Duration
	rate           uint64 // delivered bytes/second
	isAppLimited   bool
}

// deliveryRateConsumer is deliberately independent of BBR. TCP owns delivery
// accounting; congestion controls may consume the resulting sample if they need
// a delivery-rate model. Reno and CUBIC simply do not implement this interface.
type deliveryRateConsumer interface {
	OnDeliveryRateSample(deliveryRateSample)
}

type deliveryRateCandidate struct {
	valid          bool
	priorDelivered uint64
	priorTime      tcpip.MonotonicTime
	firstTxTime    tcpip.MonotonicTime
	txTime         tcpip.MonotonicTime
	ackTime        tcpip.MonotonicTime
	isAppLimited   bool
}

// +checklocks:s.ep.mu
func (s *sender) rateSampleOnSend(seg *segment, now tcpip.MonotonicTime) {
	// Linux stores the delivery snapshot for newly transmitted data. Preserve
	// that snapshot across retransmissions so ACKing a retransmit does not make
	// the rate sample measure only the retransmission-to-ACK interval.
	if seg == nil || seg.xmitCount != 0 || seg.payloadSize() == 0 {
		return
	}

	// A new flight starts a new send phase. This is analogous to Linux
	// tcp_rate_skb_sent() resetting first_tx_mstamp when there were no packets
	// in flight.
	if s.rateFirstTxTime == (tcpip.MonotonicTime{}) || s.Outstanding == 0 {
		s.rateFirstTxTime = now
		if s.rateDeliveredTime == (tcpip.MonotonicTime{}) {
			s.rateDeliveredTime = now
		}
	}

	seg.rateDelivered = s.rateDelivered
	seg.rateDeliveredTime = s.rateDeliveredTime
	seg.rateFirstTxTime = s.rateFirstTxTime
	seg.rateAppLimited = false
	seg.rateSampleValid = true
}

// +checklocks:s.ep.mu
func (s *sender) rateSampleBegin() {
	// A pure SACK ACK does not advance SND.UNA, so gVisor's legacy congestion
	// control Update hook is not reached for that ACK. Do not throw away the
	// delivery candidate in that case: finalize it at the beginning of the next
	// ACK before starting a fresh sample. Linux accounts SACKed delivery in the
	// same rate-sampling machinery, and BBR depends heavily on those samples
	// during loss recovery.
	if s.rateCandidate.valid {
		s.rateSampleEnd(s.rateCandidate.ackTime)
	}
	s.rateCandidate = deliveryRateCandidate{}
	s.deliveryRate = deliveryRateSample{}
}

// rateSegmentAlreadyDelivered reports whether a segment was already counted as
// delivered by a previous SACK. A later cumulative ACK must not count it again.
// +checklocks:s.ep.mu
func (s *sender) rateSegmentAlreadyDelivered(seg *segment) bool {
	if seg == nil || !s.ep.SACKPermitted {
		return false
	}
	return seg.acked || s.ep.scoreboard.IsSACKED(seg.sackBlock())
}

// +checklocks:s.ep.mu
func (s *sender) rateSampleDelivered(seg *segment, deliveredBytes int, ackTime tcpip.MonotonicTime) {
	if seg == nil || deliveredBytes <= 0 {
		return
	}

	s.rateDelivered += uint64(deliveredBytes)

	if !seg.rateSampleValid {
		return
	}

	// Linux chooses the newly-delivered skb carrying the most recent delivery
	// snapshot. rateDelivered is monotonically increasing, so the largest prior
	// value is the appropriate candidate.
	if !s.rateCandidate.valid || seg.rateDelivered > s.rateCandidate.priorDelivered {
		s.rateCandidate = deliveryRateCandidate{
			valid:          true,
			priorDelivered: seg.rateDelivered,
			priorTime:      seg.rateDeliveredTime,
			firstTxTime:    seg.rateFirstTxTime,
			txTime:         seg.xmitTime,
			ackTime:        ackTime,
			isAppLimited:   seg.rateAppLimited,
		}
	}
}

// +checklocks:s.ep.mu
func (s *sender) rateSampleEnd(ackTime tcpip.MonotonicTime) {
	c := s.rateCandidate
	if !c.valid || s.rateDelivered <= c.priorDelivered {
		return
	}

	ackElapsed := ackTime.Sub(c.priorTime)
	sendElapsed := c.txTime.Sub(c.firstTxTime)
	if ackElapsed <= 0 || sendElapsed < 0 {
		return
	}

	interval := ackElapsed
	if sendElapsed > interval {
		interval = sendElapsed
	}
	if interval <= 0 {
		return
	}

	delivered := s.rateDelivered - c.priorDelivered
	if delivered == 0 || delivered > ^uint64(0)/uint64(time.Second) {
		return
	}

	rate := delivered * uint64(time.Second) / uint64(interval)
	if rate == 0 {
		return
	}

	s.deliveryRate = deliveryRateSample{
		valid:          true,
		delivered:      delivered,
		priorDelivered: c.priorDelivered,
		totalDelivered: s.rateDelivered,
		interval:       interval,
		rate:           rate,
		isAppLimited:   c.isAppLimited,
	}

	// Deliver the sample through a generic TCP-owned extension point. This is
	// the equivalent architectural boundary of Linux producing struct
	// rate_sample before a congestion-control algorithm consumes it.
	if consumer, ok := s.cc.(deliveryRateConsumer); ok {
		consumer.OnDeliveryRateSample(s.deliveryRate)
	}

	// Advance the sender delivery/send epochs after producing the sample, as
	// Linux advances delivered_mstamp/first_tx_mstamp between rate samples.
	s.rateDeliveredTime = ackTime
	if c.txTime != (tcpip.MonotonicTime{}) {
		s.rateFirstTxTime = c.txTime
	}

	// A finalized sample must not be finalized again at the next ACK. Pure SACK
	// candidates survive only until rateSampleBegin() on the following ACK.
	s.rateCandidate = deliveryRateCandidate{}
}
