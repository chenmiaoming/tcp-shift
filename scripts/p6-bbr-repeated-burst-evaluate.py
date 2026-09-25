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


if len(sys.argv) != 4:
    raise SystemExit(
        "usage: p6-bbr-repeated-burst-evaluate.py <repeats> "
        "<tcp-shift-summary> <linux-summary>"
    )

repeats, tcp_shift_path, linux_path = sys.argv[1:]
ts_text, ts = parse(tcp_shift_path)
linux_text, linux = parse(linux_path)

if need(ts, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift repeated-burst summary is not successful")
if need(linux, "linux_bbr_pacing_reference") != "ok":
    raise SystemExit("Linux BBR repeated-burst summary is not successful")
if need(ts, "cc") != "bbr-internal":
    raise SystemExit(f"unexpected tcp-shift cc={ts.get('cc')}")
if need(linux, "cc") != "bbr":
    raise SystemExit(f"unexpected Linux cc={linux.get('cc')}")

for key in (
    "base_rtt_ms",
    "rate_mbit",
    "loss_mode",
    "fault_burst_packets",
    "fault_burst_repeats",
    "fault_burst_gap_packets",
    "bdp_bytes",
    "queue_pkts",
):
    if need(ts, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if need(ts, "loss_mode") != "deterministic-repeated-burst":
    raise SystemExit(f"unexpected tcp-shift loss mode={ts['loss_mode']}")
if need(linux, "loss_mode") != "deterministic-repeated-burst":
    raise SystemExit(f"unexpected Linux loss mode={linux['loss_mode']}")
if need(ts, "fault_burst_repeats") != repeats:
    raise SystemExit(
        f"requested repeats={repeats} but tcp-shift summary has "
        f"{ts['fault_burst_repeats']}"
    )

burst_packets = int(need(ts, "fault_burst_packets"))
repeat_count = int(repeats)
expected_drops = burst_packets * repeat_count
ts_fault_drops = int(need(ts, "fault_drops"))
linux_fault_drops = int(need(linux, "fault_drops"))
if ts_fault_drops != expected_drops or linux_fault_drops != expected_drops:
    raise SystemExit(
        f"fault count mismatch: expected={expected_drops} "
        f"tcp-shift={ts_fault_drops} linux={linux_fault_drops}"
    )

ts_qdisc = need(ts, "qdisc_drops").split("/")
if len(ts_qdisc) != 2:
    raise SystemExit(f"invalid tcp-shift qdisc_drops={ts['qdisc_drops']}")
ts_data_drops, ts_ack_drops = map(int, ts_qdisc)
linux_data_drops = int(need(linux, "data_qdisc_drops"))
linux_ack_drops = int(need(linux, "ack_qdisc_drops"))
if ts_data_drops != 0 or ts_ack_drops != 0:
    raise SystemExit(
        f"tcp-shift had unrelated qdisc drops: data={ts_data_drops} "
        f"ack={ts_ack_drops}"
    )
if linux_data_drops != 0 or linux_ack_drops != 0:
    raise SystemExit(
        f"Linux had unrelated qdisc drops: data={linux_data_drops} "
        f"ack={linux_ack_drops}"
    )

ts_retrans = int(need(ts, "retransmit_events"))
linux_retrans = int(need(linux, "total_retrans"))
ts_loss = int(need(ts, "loss_events"))
ts_timeout = int(need(ts, "timeout_events"))
if ts_retrans < expected_drops:
    raise SystemExit(
        f"tcp-shift retransmissions below explicit drops: "
        f"retrans={ts_retrans} drops={expected_drops}"
    )
if linux_retrans < expected_drops:
    raise SystemExit(
        f"Linux retransmissions below explicit drops: "
        f"retrans={linux_retrans} drops={expected_drops}"
    )
if ts_loss + ts_timeout < 1:
    raise SystemExit("tcp-shift observed no recovery event")

ts_goodput = float(need(ts, "goodput_mbps"))
linux_goodput = float(need(linux, "goodput_mbps"))
if ts_goodput <= 0.0 or linux_goodput <= 0.0:
    raise SystemExit("non-positive goodput")

print(
    f"p6_bbr_repeated_burst_comparison=ok repeats={repeats} "
    f"base_rtt_ms={ts['base_rtt_ms']} rate_mbit={ts['rate_mbit']} "
    f"fault_burst_packets={burst_packets} fault_burst_gap_packets={ts['fault_burst_gap_packets']} "
    f"fault_drops={expected_drops} "
    f"tcp_shift_recovery_expectation={need(ts, 'recovery_expectation')} "
    f"tcp_shift_goodput_mbps={ts_goodput:.6f} "
    f"linux_bbr_goodput_mbps={linux_goodput:.6f} "
    f"goodput_ratio={ts_goodput / linux_goodput:.6f} "
    f"tcp_shift_retransmit_events={ts_retrans} "
    f"linux_total_retrans={linux_retrans} "
    f"tcp_shift_retransmit_amplification={ts_retrans / expected_drops:.6f} "
    f"linux_retransmit_amplification={linux_retrans / expected_drops:.6f} "
    f"tcp_shift_loss_events={ts_loss} "
    f"tcp_shift_timeout_events={ts_timeout}"
)
print("tcp_shift_repeated_burst_summary=" + ts_text)
print("linux_bbr_repeated_burst_summary=" + linux_text)
