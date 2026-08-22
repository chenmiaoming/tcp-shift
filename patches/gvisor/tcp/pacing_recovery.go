// Copyright 2026 tcp-shift authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0

package tcp

import "gvisor.dev/gvisor/pkg/tcpip"

// preparePacedSend refills the same sender pacing budget used by sendData.
// Recovery must share this budget: Linux BBR paces retransmissions and new data
// from one socket pacing clock rather than allowing recovery to bypass pacing.
// +checklocks:s.ep.mu
func (s *sender) preparePacedSend() uint64 {
	rate := s.pacingRate()
	if rate == 0 {
		return 0
	}
	s.refillPacingBudget(rate, s.ep.stack.Clock().NowMonotonic())
	return rate
}

func (s *sender) pacedPayloadSize(payload int) int64 {
	if payload <= 0 || payload > s.MaxPayloadSize {
		payload = s.MaxPayloadSize
	}
	return int64(payload)
}

// allowPacedSend checks the token budget and arms pacingTimer when recovery
// must stop. The timer resumes the active recovery algorithm instead of merely
// calling sendData, so a loss-recovery episode can make forward progress even
// if no further ACK arrives while it is pacing-blocked.
// +checklocks:s.ep.mu
func (s *sender) allowPacedSend(rate uint64, payload int) bool {
	if rate == 0 {
		return true
	}
	need := s.pacedPayloadSize(payload)
	if s.pacingBudget >= need {
		return true
	}
	s.schedulePacing(rate, need)
	return false
}

// accountPacedSend consumes tokens only after a segment was actually emitted.
// +checklocks:s.ep.mu
func (s *sender) accountPacedSend(rate uint64, payload int) {
	if rate == 0 {
		return
	}
	s.pacingBudget -= s.pacedPayloadSize(payload)
	if s.pacingBudget < 0 {
		s.pacingBudget = 0
	}
}

// resumePacedRecovery resumes the exact recovery path that was pacing-blocked.
// It returns true when an active recovery algorithm handled the timer event.
// +checklocks:s.ep.mu
func (s *sender) resumePacedRecovery() bool {
	if !s.FastRecovery.Active {
		return false
	}

	if s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 {
		s.rc.DoRecovery(nil, false /* fastRetransmit */)
		return true
	}

	if s.ep.SACKPermitted {
		if sr, ok := s.lr.(*sackRecovery); ok {
			end := s.SndUna.Add(s.SndWnd)
			dataSent := sr.handleSACKRecovery(s.MaxPayloadSize, end)
			s.postXmit(dataSent, true /* shouldScheduleProbe */)
			return true
		}
	}

	return false
}
