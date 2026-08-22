#!/usr/bin/env python3
"""Prevent ACK-driven pacing calls from postponing an existing send deadline."""

from __future__ import annotations

import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one match, got {n}")
    return text.replace(old, new, 1)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_pacing_deadline.py <gvisor-root>")

    root = Path(sys.argv[1]).resolve()
    snd = root / "pkg/tcpip/transport/tcp/snd.go"
    if not snd.is_file():
        raise SystemExit(f"not a gVisor source tree: {root}")

    text = snd.read_text()
    old = '''func (s *sender) schedulePacing(rate uint64, need int64) {
	if rate == 0 || need <= s.pacingBudget {
		return
	}
	deficit := uint64(need - s.pacingBudget)
	ns := (deficit*uint64(time.Second) + rate - 1) / rate
	d := time.Duration(ns)
	if d < 50*time.Microsecond {
		d = 50 * time.Microsecond
	}
	s.pacingTimer.enable(d)
}
'''
    new = '''func (s *sender) schedulePacing(rate uint64, need int64) {
	if rate == 0 || need <= s.pacingBudget {
		return
	}
	deficit := uint64(need - s.pacingBudget)
	ns := (deficit*uint64(time.Second) + rate - 1) / rate
	d := time.Duration(ns)
	if d < 50*time.Microsecond {
		d = 50 * time.Microsecond
	}

	// ACK processing may call sendData repeatedly while a pacing timer is
	// already armed. timer.enable() always replaces timer.target, so blindly
	// enabling now+d can postpone the original deadline on every ACK. At high
	// ACK rates the 50us floor then turns into timer starvation. Preserve the
	// earliest known time at which enough pacing credit is available: a later
	// calculation may pull the deadline earlier, but must never push it later.
	now := s.ep.stack.Clock().NowMonotonic()
	newTarget := now.Add(d)
	if s.pacingTimer.enabled() && !newTarget.Before(s.pacingTimer.target) {
		return
	}
	s.pacingTimer.enable(d)
}
'''
    text = replace_once(text, old, new, "pacing deadline monotonicity")
    snd.write_text(text)
    print(f"patched non-postponing pacing deadline at {root}")


if __name__ == "__main__":
    main()
