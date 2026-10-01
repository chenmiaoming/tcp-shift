#!/usr/bin/env python3
import sys
from pathlib import Path


def read_summary(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"empty summary: {path}")
    fields = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return text, fields


def need(row, key):
    value = row.get(key)
    if value is None or value == "":
        raise SystemExit(f"missing {key}")
    return value


def goodput(row):
    return float(need(row, "goodput_mbps"))


if len(sys.argv) != 4:
    raise SystemExit(
        "usage: p6-cubic-single-flow-residual-evaluate.py "
        "<default-summary> <no-fast-summary> <linux-summary>"
    )

default_text, default = read_summary(sys.argv[1])
single_text, single = read_summary(sys.argv[2])
linux_text, linux = read_summary(sys.argv[3])

for name, row in (("default", default), ("single-flow", single)):
    if need(row, "cc") != "cubic":
        raise SystemExit(f"{name} is not CUBIC")
    if int(need(row, "fault_drops")) != 28:
        raise SystemExit(f"{name} fault count mismatch")
    if int(need(row, "loss_events")) != 28:
        raise SystemExit(f"{name} loss count mismatch")
    if int(need(row, "retransmit_events")) != 28:
        raise SystemExit(f"{name} retransmit count mismatch")
    if int(need(row, "timeout_events")) != 0:
        raise SystemExit(f"{name} used RTO")
    if need(row, "qdisc_drops") != "0/0":
        raise SystemExit(f"{name} has unrelated qdisc drops")
    if need(row, "payload_integrity") != "ok":
        raise SystemExit(f"{name} payload integrity failed")

if int(need(linux, "fault_drops")) != 28:
    raise SystemExit("Linux fault count mismatch")
if int(need(linux, "total_retrans")) != 28:
    raise SystemExit("Linux retransmit count mismatch")
if need(linux, "data_qdisc_drops") != "0" or need(linux, "ack_qdisc_drops") != "0":
    raise SystemExit("Linux has unrelated qdisc drops")

for key in ("base_rtt_ms", "rate_mbit"):
    if need(default, key) != need(single, key) or need(default, key) != need(linux, key):
        raise SystemExit(f"path mismatch for {key}")

default_g = goodput(default)
single_g = goodput(single)
linux_g = goodput(linux)
if default_g <= 0 or single_g <= 0 or linux_g <= 0:
    raise SystemExit("non-positive goodput")

original_gap = linux_g - default_g
remaining_gap = linux_g - single_g
closed_gap = single_g - default_g
closed_fraction = closed_gap / original_gap if original_gap > 0 else 0.0

print(
    "p6_cubic_single_flow_residual=ok "
    f"default_goodput_mbps={default_g:.6f} "
    f"single_flow_goodput_mbps={single_g:.6f} "
    f"linux_goodput_mbps={linux_g:.6f} "
    f"default_over_linux={default_g / linux_g:.6f} "
    f"single_flow_over_linux={single_g / linux_g:.6f} "
    f"original_gap_mbps={original_gap:.6f} "
    f"remaining_gap_mbps={remaining_gap:.6f} "
    f"fast_convergence_closed_gap_mbps={closed_gap:.6f} "
    f"fast_convergence_closed_fraction={closed_fraction:.6f}"
)
print("default_summary=" + default_text)
print("single_flow_summary=" + single_text)
print("linux_summary=" + linux_text)
