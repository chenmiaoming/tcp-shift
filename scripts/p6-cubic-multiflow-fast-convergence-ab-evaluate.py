#!/usr/bin/env python3
import sys
from pathlib import Path


def parse(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"empty summary: {path}")
    fields = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return text, fields


def need(fields, key):
    value = fields.get(key)
    if value is None or value == "":
        raise SystemExit(f"missing {key}")
    return value


if len(sys.argv) != 3:
    raise SystemExit(
        "usage: p6-cubic-multiflow-fast-convergence-ab-evaluate.py "
        "<default-summary> <no-fast-summary>"
    )

default_text, default = parse(sys.argv[1])
no_fast_text, no_fast = parse(sys.argv[2])

for name, row in (("default", default), ("no-fast", no_fast)):
    if need(row, "p6_bbr_multiflow") != "ok":
        raise SystemExit(f"{name} multi-flow summary is not successful")
    if need(row, "cc") != "cubic":
        raise SystemExit(f"{name} did not run CUBIC: cc={row['cc']}")
    if need(row, "qdisc_drops") != "0/0":
        raise SystemExit(f"{name} has unrelated qdisc drops: {row['qdisc_drops']}")
    if need(row, "payload_integrity") != "ok":
        raise SystemExit(f"{name} payload integrity failed")
    if int(need(row, "timeout_events")) != 0:
        raise SystemExit(f"{name} fell back to RTO")

for key in (
    "flows",
    "base_rtt_ms",
    "rate_mbit",
    "bdp_bytes",
    "queue_pkts",
    "payload_bytes_per_flow",
    "total_wire_bytes",
    "fault_markers_per_flow",
    "fault_drops",
    "retransmit_events",
    "loss_events",
):
    if need(default, key) != need(no_fast, key):
        raise SystemExit(
            f"A/B path mismatch for {key}: default={default[key]} no-fast={no_fast[key]}"
        )

flows = int(need(default, "flows"))
markers = int(need(default, "fault_markers_per_flow"))
expected_faults = flows * markers
if markers <= 0:
    raise SystemExit("multi-flow A/B did not inject deterministic losses")
for name, row in (("default", default), ("no-fast", no_fast)):
    for key in ("fault_drops", "retransmit_events", "loss_events"):
        if int(need(row, key)) != expected_faults:
            raise SystemExit(
                f"{name} {key} mismatch: {row[key]} expected={expected_faults}"
            )

default_fast = int(need(default, "fast_convergence_events"))
no_fast_count = int(need(no_fast, "fast_convergence_events"))
if default_fast <= 0:
    raise SystemExit("default multi-flow path did not exercise fast convergence")
if no_fast_count != 0:
    raise SystemExit("no-fast multi-flow target still applied fast convergence")

def metric(row, key):
    return float(need(row, key))


def ratio(a, b):
    return a / b if b else 0.0


default_agg = metric(default, "aggregate_goodput_mbps")
no_fast_agg = metric(no_fast, "aggregate_goodput_mbps")
default_jain = metric(default, "jain_fairness")
no_fast_jain = metric(no_fast, "jain_fairness")
default_min = metric(default, "min_flow_goodput_mbps")
default_max = metric(default, "max_flow_goodput_mbps")
no_fast_min = metric(no_fast, "min_flow_goodput_mbps")
no_fast_max = metric(no_fast, "max_flow_goodput_mbps")

if default_agg <= 0 or no_fast_agg <= 0:
    raise SystemExit("non-positive aggregate goodput")
if not 0.0 < default_jain <= 1.0 or not 0.0 < no_fast_jain <= 1.0:
    raise SystemExit("invalid Jain fairness")

print(
    "p6_cubic_multiflow_fast_convergence_ab=ok "
    f"flows={flows} markers_per_flow={markers} expected_faults={expected_faults} "
    f"default_fast_convergence_events={default_fast} "
    f"no_fast_convergence_events={no_fast_count} "
    f"default_aggregate_goodput_mbps={default_agg:.6f} "
    f"no_fast_aggregate_goodput_mbps={no_fast_agg:.6f} "
    f"no_fast_over_default_aggregate={ratio(no_fast_agg, default_agg):.6f} "
    f"default_jain_fairness={default_jain:.6f} "
    f"no_fast_jain_fairness={no_fast_jain:.6f} "
    f"default_min_flow_goodput_mbps={default_min:.6f} "
    f"default_max_flow_goodput_mbps={default_max:.6f} "
    f"default_max_over_min={ratio(default_max, default_min):.6f} "
    f"no_fast_min_flow_goodput_mbps={no_fast_min:.6f} "
    f"no_fast_max_flow_goodput_mbps={no_fast_max:.6f} "
    f"no_fast_max_over_min={ratio(no_fast_max, no_fast_min):.6f}"
)
print("default_multiflow_summary=" + default_text)
print("no_fast_multiflow_summary=" + no_fast_text)
