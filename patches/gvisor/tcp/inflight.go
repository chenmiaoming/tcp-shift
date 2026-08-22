// Copyright 2026 tcp-shift authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0

package tcp

import (
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/seqnum"
)

// linuxFlightSnapshot mirrors the four counters used by Linux
// tcp_packets_in_flight():
//
//   packets_out - sacked_out - lost_out + retrans_out
//
// This accounting is deliberately independent of sender.Outstanding. gVisor
// rewrites Outstanding with RFC 6675 SetPipe() during SACK/RACK recovery, so it
// is a recovery pipe estimate rather than the congestion-control in-flight
// quantity used by Linux BBR.
type linuxFlightSnapshot struct {
	packetsOut      int
	sackedOut       int
	lostOut         int
	retransOut      int
	packetsInFlight int
}

// recoveryFlightControl lets a congestion control select the in-flight
// coordinate system used for recovery cwnd admission. Loss-based controls do
// not implement this interface and retain gVisor's RFC6675 SetPipe/Outstanding
// semantics. BBR implements it so both its cwnd calculation and recovery send
// admission use the same Linux tcp_packets_in_flight-like quantity.
type recoveryFlightControl interface {
	recoveryPacketsInFlight() int
}

// recoveryPacketsInFlight returns the quantity that must be compared with
// SndCwnd when RACK/RFC6675 recovery decides whether another packet may be
// transmitted. Keeping this indirection at the admission point is important:
// SetPipe/Outstanding is still maintained for gVisor recovery bookkeeping, but
// it must not be compared against a BBR cwnd expressed in Linux-like inflight
// units.
//
// +checklocks:s.ep.mu
func (s *sender) recoveryPacketsInFlight() int {
	if cc, ok := s.cc.(recoveryFlightControl); ok {
		return cc.recoveryPacketsInFlight()
	}
	return s.Outstanding
}

// recoveryPacketsInFlight makes BBR recovery admission use the same independent
// quantity used by BBR packet conservation and rate_sample.prior_in_flight.
//
// +checklocks:b.s.ep.mu
func (b *bbrState) recoveryPacketsInFlight() int {
	return b.s.linuxLikePacketsInFlight()
}

// linuxLikeFlight reconstructs Linux-style TCP in-flight state from netstack's
// retransmission queue and SACK scoreboard. RACK loss state that would
// otherwise disappear when sendSegment clears seg.lost is kept separately in
// seg.inflightRACKLost. Likewise inflightRetransActive tracks whether the most
// recent retransmitted copy is still considered in the network.
//
// +checklocks:s.ep.mu
func (s *sender) linuxLikeFlight() linuxFlightSnapshot {
	var out linuxFlightSnapshot
	mss := s.MaxPayloadSize
	if mss <= 0 {
		mss = 1
	}

	for seg := s.writeList.Front(); seg != nil && seg.xmitCount != 0; seg = seg.Next() {
		payload := seg.payloadSize()
		if payload <= 0 {
			continue
		}

		start := seg.sequenceNumber
		segEnd := start.Add(seqnum.Size(payload))
		for start.LessThan(segEnd) {
			end := start.Add(seqnum.Size(mss))
			if segEnd.LessThan(end) {
				end = segEnd
			}
			sb := header.SACKBlock{Start: start, End: end}
			out.packetsOut++

			// A SACKed packet has left the network from TCP's point of view.
			// Do not also count stale loss/retransmission metadata for it.
			if s.ep.SACKPermitted && s.ep.scoreboard.IsSACKED(sb) {
				out.sackedOut++
				start = end
				continue
			}

			// Legacy RFC6675 loss is represented by the scoreboard. RACK marks
			// loss directly on segments, so retain that state across the
			// retransmission that clears seg.lost.
			if seg.inflightRACKLost || (s.ep.SACKPermitted && s.ep.scoreboard.IsRangeLost(sb)) {
				out.lostOut++
			}

			// Linux retrans_out counts a retransmitted copy that is itself still
			// outstanding. If RACK subsequently marks that copy lost,
			// inflightMarkRACKLost clears this bit until the next retransmission.
			if seg.inflightRetransActive {
				out.retransOut++
			}
			start = end
		}
	}

	out.packetsInFlight = out.packetsOut - out.sackedOut - out.lostOut + out.retransOut
	if out.packetsInFlight < 0 {
		// A negative value means our shadow state is inconsistent. Clamp the
		// control input but record the mismatch through the normal diagnostics.
		out.packetsInFlight = 0
	}
	return out
}

// +checklocks:s.ep.mu
func (s *sender) linuxLikePacketsInFlight() int {
	return s.linuxLikeFlight().packetsInFlight
}

// inflightOnSend updates the shadow state immediately before sendSegment bumps
// xmitCount. A first transmission is already represented by writeList; only a
// retransmission needs an additional retrans_out copy. It also classifies the
// retransmission depth so CI can distinguish one recovery retransmission from a
// segment being sent repeatedly.
// +checklocks:s.ep.mu
func (s *sender) inflightOnSend(seg *segment) {
	if seg == nil || seg.payloadSize() <= 0 || seg.xmitCount == 0 {
		return
	}

	stats := s.ep.stack.Stats().TCP
	switch seg.xmitCount {
	case 1:
		stats.TCPShiftRetransmitFirst.Increment()
	case 2:
		stats.TCPShiftRetransmitSecond.Increment()
	default:
		stats.TCPShiftRetransmitThirdPlus.Increment()
	}

	// RTORecovery is a sender state, not just the synchronous call from
	// retransmitTimerExpired(). An RTO rewinds writeNext and subsequent ACK or
	// pacing callbacks may continue retransmitting that recovery flight. Count
	// the whole episode so diagnostics do not mislabel those packets as normal
	// sender traffic.
	if s.state == tcpip.RTORecovery {
		stats.TCPShiftRTORetransmits.Increment()
	}

	seg.inflightRetransActive = true
	if s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 && s.FastRecovery.Active {
		stats.TCPShiftRACKRecoveryRetransmits.Increment()
	}
}

// inflightMarkRACKLost records the Linux-equivalent state transition when RACK
// declares a sequence range lost. The return value is true when the same
// sequence range had already been declared lost before and has since been
// retransmitted; this is the signal needed to detect repeated loss inference on
// retransmitted copies.
// +checklocks:s.ep.mu
func (s *sender) inflightMarkRACKLost(seg *segment) bool {
	if seg == nil || seg.payloadSize() <= 0 {
		return false
	}
	repeated := seg.inflightRACKLost && seg.xmitCount > 1
	seg.inflightRACKLost = true
	seg.inflightRetransActive = false
	return repeated
}

// recordSetPipeDiagnostics measures how far gVisor's RFC6675 pipe estimator is
// from the Linux-style in-flight quantity. The values are cumulative so the
// existing periodic TCP stats logger can expose them without per-packet logs.
// +checklocks:s.ep.mu
func (s *sender) recordSetPipeDiagnostics() {
	stats := s.ep.stack.Stats().TCP
	stats.TCPShiftSetPipeCalls.Increment()
	flight := s.linuxLikePacketsInFlight()
	gap := s.Outstanding - flight
	if gap < 0 {
		gap = -gap
	}
	stats.TCPShiftSetPipeAbsGapSum.IncrementBy(uint64(gap))
	if gap != 0 {
		stats.TCPShiftSetPipeMismatchCalls.Increment()
	}
}
