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
// sample. Besides delivery rate, it carries the ACK/SACK delivery count and the
// pre-ACK in-flight estimate needed by congestion controls with a custom
// per-delivery control loop such as BBR.
type deliveryRateSample struct {
	valid          bool
	delivered      uint64
	priorDelivered uint64
	totalDelivered uint64
	interval       time.Duration
	rate           uint64 // delivered bytes/second
	ackedSacked    int    // packets newly ACKed or SACKed by this ACK event
	priorInFlight  int    // sender Outstanding before this ACK event delivered data
	ackTime        tcpip.MonotonicTime
	isAppLimited   bool
}

// deliveryRateConsumer is deliberately independent of BBR. TCP owns delivery
// accounting and invokes this after ACK processing has established the current
// recovery state but before recovery transmits more data. This mirrors Linux's
// custom cong_control(..., struct rate_sample *) ordering.
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

	// ackedBytes is delivery caused by the current ACK event, not the longer
	// interval represented by delivered=totalDelivered-priorDelivered.
	ackedBytes    int
	priorInFlight int
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
	// This fallback handles an early-return path that produced SACK delivery but
	// did not reach the normal end-of-ACK finalization hook. Normally the prior
	// event has already been finalized and notified before this point.
	if s.rateCandidate.valid {
		s.rateSampleEnd(s.rateCandidate.ackTime)
		s.rateSampleNotify()
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

	ackedBytes := deliveredBytes
	priorInFlight := s.Outstanding
	if s.rateCandidate.valid {
		ackedBytes += s.rateCandidate.ackedBytes
		priorInFlight = s.rateCandidate.priorInFlight
	}

	// Linux chooses the newly-delivered skb carrying the most recent delivery
	// snapshot. rateDelivered is monotonically increasing, so the largest prior
	// value is the appropriate timing candidate. ACK-event accounting is carried
	// forward even when that timing candidate changes.
	if !s.rateCandidate.valid || seg.rateDelivered > s.rateCandidate.priorDelivered {
		s.rateCandidate = deliveryRateCandidate{
			valid:          true,
			priorDelivered: seg.rateDelivered,
			priorTime:      seg.rateDeliveredTime,
			firstTxTime:    seg.rateFirstTxTime,
			txTime:         seg.xmitTime,
			ackTime:        ackTime,
			isAppLimited:   seg.rateAppLimited,
			ackedBytes:     ackedBytes,
			priorInFlight:  priorInFlight,
		}
	} else {
		s.rateCandidate.ackedBytes = ackedBytes
		s.rateCandidate.ackTime = ackTime
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

	ackedSacked := 0
	if c.ackedBytes > 0 {
		mss := s.MaxPayloadSize
		if mss <= 0 {
			mss = 1
		}
		ackedSacked = (c.ackedBytes + mss - 1) / mss
	}

	s.deliveryRate = deliveryRateSample{
		valid:          true,
		delivered:      delivered,
		priorDelivered: c.priorDelivered,
		totalDelivered: s.rateDelivered,
		interval:       interval,
		rate:           rate,
		ackedSacked:    ackedSacked,
		priorInFlight:  c.priorInFlight,
		ackTime:        ackTime,
		isAppLimited:   c.isAppLimited,
	}

	// Advance the sender delivery/send epochs after producing the sample, as
	// Linux advances delivered_mstamp/first_tx_mstamp between rate samples.
	s.rateDeliveredTime = ackTime
	if c.txTime != (tcpip.MonotonicTime{}) {
		s.rateFirstTxTime = c.txTime
	}

	// A finalized sample must not be finalized again. Notification is separate
	// so sender.handleRcvdSegment can first establish CA/recovery state and then
	// invoke the congestion-control loop before retransmission.
	s.rateCandidate = deliveryRateCandidate{}
}

// +checklocks:s.ep.mu
func (s *sender) rateSampleNotify() {
	if !s.deliveryRate.valid {
		return
	}
	if consumer, ok := s.cc.(deliveryRateConsumer); ok {
		consumer.OnDeliveryRateSample(s.deliveryRate)
	}
	s.deliveryRate = deliveryRateSample{}
}
