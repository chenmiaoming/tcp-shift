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
// retransmission needs an additional retrans_out copy.
// +checklocks:s.ep.mu
func (s *sender) inflightOnSend(seg *segment) {
	if seg == nil || seg.payloadSize() <= 0 || seg.xmitCount == 0 {
		return
	}
	seg.inflightRetransActive = true
	if s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 && s.FastRecovery.Active {
		s.ep.stack.Stats().TCP.TCPShiftRACKRecoveryRetransmits.Increment()
	}
}

// inflightMarkRACKLost records the Linux-equivalent state transition when RACK
// declares a sequence range lost. The original packet remains in packets_out
// and becomes lost_out. If the latest retransmitted copy was in flight, that
// copy is now lost too and must leave retrans_out until another retransmission
// is sent.
// +checklocks:s.ep.mu
func (s *sender) inflightMarkRACKLost(seg *segment) {
	if seg == nil || seg.payloadSize() <= 0 {
		return
	}
	seg.inflightRACKLost = true
	seg.inflightRetransActive = false
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
