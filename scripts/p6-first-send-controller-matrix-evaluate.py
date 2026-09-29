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


def qdisc_zero(fields, linux=False):
    if linux:
        data = int(need(fields, "data_qdisc_drops"))
        ack = int(need(fields, "ack_qdisc_drops"))
    else:
        parts = need(fields, "qdisc_drops").split("/")
        if len(parts) != 2:
            raise SystemExit(f"invalid qdisc_drops={fields.get('qdisc_drops')}")
        data, ack = map(int, parts)
    if data != 0 or ack != 0:
        raise SystemExit(f"unrelated qdisc drops: data={data} ack={ack}")


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-first-send-controller-matrix-evaluate.py "
        "<tcp-shift-bbr> <linux-bbr> <tcp-shift-cubic> <linux-cubic>"
    )

ts_bbr_text, ts_bbr = parse(sys.argv[1])
linux_bbr_text, linux_bbr = parse(sys.argv[2])
ts_cubic_text, ts_cubic = parse(sys.argv[3])
linux_cubic_text, linux_cubic = parse(sys.argv[4])

if need(ts_bbr, "p6_bbr_long_flow") != "ok" or need(ts_cubic, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift summary is not successful")
if need(linux_bbr, "linux_bbr_pacing_reference") != "ok":
    raise SystemExit("Linux BBR summary is not successful")
if need(linux_cubic, "linux_cubic_pacing_reference") != "ok":
    raise SystemExit("Linux CUBIC summary is not successful")
if need(ts_bbr, "cc") != "bbr-internal":
    raise SystemExit(f"unexpected tcp-shift BBR cc={ts_bbr.get('cc')}")
if need(ts_cubic, "cc") != "cubic":
    raise SystemExit(f"unexpected tcp-shift CUBIC cc={ts_cubic.get('cc')}")
if need(linux_bbr, "cc") != "bbr" or need(linux_cubic, "cc") != "cubic":
    raise SystemExit("unexpected Linux controller identity")

records = [ts_bbr, linux_bbr, ts_cubic, linux_cubic]
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
    expected = need(ts_bbr, key)
    for fields in records[1:]:
        actual = need(fields, key)
        if actual != expected:
            raise SystemExit(
                f"path mismatch for {key}: expected={expected} actual={actual}"
            )

if need(ts_bbr, "loss_mode") != "deterministic-first-send":
    raise SystemExit(f"unexpected loss mode={ts_bbr['loss_mode']}")

expected_drops = int(need(ts_bbr, "fault_marker_count"))
for label, fields in (
    ("tcp-shift-bbr", ts_bbr),
    ("linux-bbr", linux_bbr),
    ("tcp-shift-cubic", ts_cubic),
    ("linux-cubic", linux_cubic),
):
    fault_drops = int(need(fields, "fault_drops"))
    if fault_drops != expected_drops:
        raise SystemExit(
            f"{label} fault count mismatch: expected={expected_drops} actual={fault_drops}"
        )

qdisc_zero(ts_bbr)
qdisc_zero(ts_cubic)
qdisc_zero(linux_bbr, linux=True)
qdisc_zero(linux_cubic, linux=True)

for label, fields, key in (
    ("tcp-shift-bbr", ts_bbr, "retransmit_events"),
    ("linux-bbr", linux_bbr, "total_retrans"),
    ("tcp-shift-cubic", ts_cubic, "retransmit_events"),
    ("linux-cubic", linux_cubic, "total_retrans"),
):
    retrans = int(need(fields, key))
    if retrans != expected_drops:
        raise SystemExit(
            f"{label} retransmission mismatch: expected={expected_drops} actual={retrans}"
        )

for label, fields in (("tcp-shift-bbr", ts_bbr), ("tcp-shift-cubic", ts_cubic)):
    timeout_events = int(need(fields, "timeout_events"))
    if timeout_events != 0:
        raise SystemExit(f"{label} fell back to RTO: {timeout_events}")
    loss_events = int(need(fields, "loss_events"))
    if loss_events < 1 or loss_events > expected_drops:
        raise SystemExit(
            f"{label} recovery episode count outside bounded range: "
            f"loss={loss_events} drops={expected_drops}"
        )

ts_bbr_goodput = float(need(ts_bbr, "goodput_mbps"))
linux_bbr_goodput = float(need(linux_bbr, "goodput_mbps"))
ts_cubic_goodput = float(need(ts_cubic, "goodput_mbps"))
linux_cubic_goodput = float(need(linux_cubic, "goodput_mbps"))
for value in (
    ts_bbr_goodput,
    linux_bbr_goodput,
    ts_cubic_goodput,
    linux_cubic_goodput,
):
    if value <= 0.0:
        raise SystemExit("non-positive goodput")

bbr_ratio = ts_bbr_goodput / linux_bbr_goodput
cubic_ratio = ts_cubic_goodput / linux_cubic_goodput

print(
    "p6_first_send_controller_matrix=ok "
    f"base_rtt_ms={ts_bbr['base_rtt_ms']} rate_mbit={ts_bbr['rate_mbit']} "
    f"fault_marker_count={expected_drops} "
    f"fault_marker_gap_packets={ts_bbr['fault_marker_gap_packets']} "
    f"tcp_shift_bbr_goodput_mbps={ts_bbr_goodput:.6f} "
    f"linux_bbr_goodput_mbps={linux_bbr_goodput:.6f} "
    f"bbr_goodput_ratio={bbr_ratio:.6f} "
    f"tcp_shift_cubic_goodput_mbps={ts_cubic_goodput:.6f} "
    f"linux_cubic_goodput_mbps={linux_cubic_goodput:.6f} "
    f"cubic_goodput_ratio={cubic_ratio:.6f} "
    f"ratio_difference={bbr_ratio - cubic_ratio:.6f} "
    f"tcp_shift_bbr_loss_events={need(ts_bbr, 'loss_events')} "
    f"tcp_shift_cubic_loss_events={need(ts_cubic, 'loss_events')} "
    f"tcp_shift_bbr_timeout_events={need(ts_bbr, 'timeout_events')} "
    f"tcp_shift_cubic_timeout_events={need(ts_cubic, 'timeout_events')}"
)
print("tcp_shift_bbr_summary=" + ts_bbr_text)
print("linux_bbr_summary=" + linux_bbr_text)
print("tcp_shift_cubic_summary=" + ts_cubic_text)
print("linux_cubic_summary=" + linux_cubic_text)
