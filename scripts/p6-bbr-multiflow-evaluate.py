#!/usr/bin/env python3
import sys
from pathlib import Path


def parse(path):
    text = Path(path).read_text(encoding="utf-8").strip()
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
        "usage: p6-bbr-multiflow-evaluate.py "
        "<tcp-shift-summary> <linux-summary>"
    )

ts_text, ts = parse(sys.argv[1])
linux_text, linux = parse(sys.argv[2])

if need(ts, "p6_bbr_multiflow") != "ok":
    raise SystemExit("tcp-shift multi-flow summary is not successful")
if need(linux, "linux_bbr_multiflow_reference") != "ok":
    raise SystemExit("Linux BBR multi-flow summary is not successful")

for key in (
    "flows",
    "base_rtt_ms",
    "rate_mbit",
    "bdp_bytes",
    "queue_pkts",
    "payload_bytes_per_flow",
    "total_wire_bytes",
):
    if need(ts, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if need(ts, "qdisc_drops") != "0/0":
    raise SystemExit(f"tcp-shift qdisc drops: {ts['qdisc_drops']}")
if need(linux, "qdisc_drops") != "0/0":
    raise SystemExit(f"Linux qdisc drops: {linux['qdisc_drops']}")
if int(need(ts, "loss_events")) != 0 or int(need(ts, "timeout_events")) != 0:
    raise SystemExit(
        f"tcp-shift recovery on clean path: "
        f"loss={ts['loss_events']} timeout={ts['timeout_events']}"
    )

ts_agg = float(need(ts, "aggregate_goodput_mbps"))
linux_agg = float(need(linux, "aggregate_goodput_mbps"))
ts_jain = float(need(ts, "jain_fairness"))
linux_jain = float(need(linux, "jain_fairness"))
if ts_agg <= 0.0 or linux_agg <= 0.0:
    raise SystemExit("non-positive aggregate goodput")
if not (0.0 < ts_jain <= 1.0) or not (0.0 < linux_jain <= 1.0):
    raise SystemExit("invalid Jain fairness")

print(
    "p6_bbr_multiflow_comparison=ok "
    f"flows={ts['flows']} base_rtt_ms={ts['base_rtt_ms']} "
    f"rate_mbit={ts['rate_mbit']} bdp_bytes={ts['bdp_bytes']} "
    f"queue_pkts={ts['queue_pkts']} "
    f"tcp_shift_aggregate_goodput_mbps={ts_agg:.6f} "
    f"linux_bbr_aggregate_goodput_mbps={linux_agg:.6f} "
    f"aggregate_goodput_ratio={ts_agg / linux_agg:.6f} "
    f"tcp_shift_jain_fairness={ts_jain:.6f} "
    f"linux_bbr_jain_fairness={linux_jain:.6f} "
    f"tcp_shift_min_flow_goodput_mbps={need(ts, 'min_flow_goodput_mbps')} "
    f"tcp_shift_max_flow_goodput_mbps={need(ts, 'max_flow_goodput_mbps')} "
    f"linux_min_flow_goodput_mbps={need(linux, 'min_flow_goodput_mbps')} "
    f"linux_max_flow_goodput_mbps={need(linux, 'max_flow_goodput_mbps')} "
    f"tcp_shift_heap_peak={need(ts, 'heap_peak')} "
    f"linux_total_retrans={need(linux, 'total_retrans')}"
)
print("tcp_shift_multiflow_summary=" + ts_text)
print("linux_bbr_multiflow_summary=" + linux_text)
