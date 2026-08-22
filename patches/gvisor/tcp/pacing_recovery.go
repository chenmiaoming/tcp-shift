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
// This helper is currently used only by legacy RFC6675 SACK recovery. The RACK
// experiment showed that externally pacing RACK's recovery loop increased
// spurious retransmission/DSACK activity, so RACK is intentionally left on its
// upstream send timing while we debug its loss-inference interaction with BBR.
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

// allowPacedSend checks the token budget and arms pacingTimer when legacy SACK
// recovery must stop.
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

// resumePacedRecovery resumes a pacing-blocked legacy SACK recovery episode.
// RACK deliberately returns false here and therefore remains entirely on its
// upstream recovery timing.
// +checklocks:s.ep.mu
func (s *sender) resumePacedRecovery() bool {
	if !s.FastRecovery.Active || !s.ep.SACKPermitted {
		return false
	}
	if s.ep.tcpRecovery&tcpip.TCPRACKLossDetection != 0 {
		return false
	}
	if sr, ok := s.lr.(*sackRecovery); ok {
		end := s.SndUna.Add(s.SndWnd)
		dataSent := sr.handleSACKRecovery(s.MaxPayloadSize, end)
		s.postXmit(dataSent, true /* shouldScheduleProbe */)
		return true
	}
	return false
}
