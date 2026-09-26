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
        "usage: p6-cubic-first-send-loss-evaluate.py "
        "<tcp-shift-summary> <linux-summary>"
    )

ts_text, ts = parse(sys.argv[1])
linux_text, linux = parse(sys.argv[2])

if need(ts, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift summary is not successful")
if need(linux, "linux_cubic_pacing_reference") != "ok":
    raise SystemExit("Linux CUBIC summary is not successful")
if need(ts, "cc") != "cubic":
    raise SystemExit(f"unexpected tcp-shift cc={ts.get('cc')}")
if need(linux, "cc") != "cubic":
    raise SystemExit(f"unexpected Linux cc={linux.get('cc')}")

for key in (
    "base_rtt_ms",
    "rate_mbit",
    "loss_mode",
    "fault_marker_count",
    "fault_marker_gap_packets",
    "fault_marker_prefix",
    "bdp_bytes",
    "queue_pkts",
):
    if need(ts, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if need(ts, "loss_mode") != "deterministic-first-send":
    raise SystemExit(f"unexpected tcp-shift loss mode={ts['loss_mode']}")
if need(linux, "loss_mode") != "deterministic-first-send":
    raise SystemExit(f"unexpected Linux loss mode={linux['loss_mode']}")

expected = int(need(ts, "fault_marker_count"))
ts_fault = int(need(ts, "fault_drops"))
linux_fault = int(need(linux, "fault_drops"))
if ts_fault != expected or linux_fault != expected:
    raise SystemExit(
        f"fault count mismatch: expected={expected} "
        f"tcp-shift={ts_fault} linux={linux_fault}"
    )

ts_qdisc = need(ts, "qdisc_drops").split("/")
if len(ts_qdisc) != 2:
    raise SystemExit(f"invalid tcp-shift qdisc_drops={ts['qdisc_drops']}")
ts_data_drops, ts_ack_drops = map(int, ts_qdisc)
linux_data_drops = int(need(linux, "data_qdisc_drops"))
linux_ack_drops = int(need(linux, "ack_qdisc_drops"))
if ts_data_drops != 0 or ts_ack_drops != 0:
    raise SystemExit(
        f"tcp-shift had unrelated qdisc drops: "
        f"data={ts_data_drops} ack={ts_ack_drops}"
    )
if linux_data_drops != 0 or linux_ack_drops != 0:
    raise SystemExit(
        f"Linux had unrelated qdisc drops: "
        f"data={linux_data_drops} ack={linux_ack_drops}"
    )

ts_retrans = int(need(ts, "retransmit_events"))
linux_retrans = int(need(linux, "total_retrans"))
if ts_retrans != expected:
    raise SystemExit(
        f"tcp-shift retransmission mismatch: expected={expected} actual={ts_retrans}"
    )
if linux_retrans != expected:
    raise SystemExit(
        f"Linux retransmission mismatch: expected={expected} actual={linux_retrans}"
    )

ts_timeout = int(need(ts, "timeout_events"))
if ts_timeout != 0:
    raise SystemExit(f"tcp-shift CUBIC fell back to RTO: {ts_timeout}")

ts_goodput = float(need(ts, "goodput_mbps"))
linux_goodput = float(need(linux, "goodput_mbps"))
if ts_goodput <= 0.0 or linux_goodput <= 0.0:
    raise SystemExit("non-positive goodput")

print(
    "p6_cubic_first_send_loss_comparison=ok "
    f"base_rtt_ms={ts['base_rtt_ms']} rate_mbit={ts['rate_mbit']} "
    f"fault_marker_count={expected} "
    f"fault_marker_gap_packets={ts['fault_marker_gap_packets']} "
    f"tcp_shift_goodput_mbps={ts_goodput:.6f} "
    f"linux_cubic_goodput_mbps={linux_goodput:.6f} "
    f"goodput_ratio={ts_goodput / linux_goodput:.6f} "
    f"tcp_shift_retransmit_events={ts_retrans} "
    f"linux_total_retrans={linux_retrans} "
    f"tcp_shift_loss_events={need(ts, 'loss_events')} "
    f"tcp_shift_timeout_events={ts_timeout}"
)
print("tcp_shift_cubic_first_send_loss_summary=" + ts_text)
print("linux_cubic_first_send_loss_summary=" + linux_text)
