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
        "usage: p6-bbr-loss-reference-evaluate.py <case> "
        "<tcp-shift-summary> <linux-summary>"
    )

case, tcp_shift_path, linux_path = sys.argv[1:]
ts_text, ts = parse(tcp_shift_path)
linux_text, linux = parse(linux_path)

if need(ts, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift BBR loss summary is not successful")
if need(linux, "linux_bbr_pacing_reference") != "ok":
    raise SystemExit("Linux BBR loss summary is not successful")
if need(linux, "cc") != "bbr":
    raise SystemExit(f"unexpected Linux reference cc={linux.get('cc')}")

for key in (
    "base_rtt_ms",
    "rate_mbit",
    "loss_pct",
    "loss_mode",
    "bdp_bytes",
    "queue_pkts",
):
    if need(ts, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if need(ts, "loss_mode") != "random":
    raise SystemExit(f"unexpected tcp-shift loss mode: {ts['loss_mode']}")
if need(linux, "loss_mode") != "random":
    raise SystemExit(f"unexpected Linux loss mode: {linux['loss_mode']}")

ts_drops = need(ts, "qdisc_drops").split("/")
if len(ts_drops) != 2:
    raise SystemExit(f"invalid tcp-shift qdisc_drops={ts['qdisc_drops']}")
ts_data_drops, ts_ack_drops = map(int, ts_drops)
linux_data_drops = int(need(linux, "data_qdisc_drops"))
linux_ack_drops = int(need(linux, "ack_qdisc_drops"))

if ts_data_drops < 1 or ts_ack_drops != 0:
    raise SystemExit(
        f"tcp-shift loss direction invalid: data={ts_data_drops} ack={ts_ack_drops}"
    )
if linux_data_drops < 1 or linux_ack_drops != 0:
    raise SystemExit(
        f"Linux loss direction invalid: data={linux_data_drops} ack={linux_ack_drops}"
    )

ts_retrans = int(need(ts, "retransmit_events"))
ts_loss = int(need(ts, "loss_events"))
ts_timeout = int(need(ts, "timeout_events"))
linux_retrans = int(need(linux, "total_retrans"))
if ts_retrans < 1:
    raise SystemExit("tcp-shift observed no retransmission")
if ts_loss + ts_timeout < 1:
    raise SystemExit("tcp-shift observed no recovery event")
if linux_retrans < 1:
    raise SystemExit("Linux BBR observed no retransmission")

ts_goodput = float(need(ts, "goodput_mbps"))
linux_goodput = float(need(linux, "goodput_mbps"))
if ts_goodput <= 0.0 or linux_goodput <= 0.0:
    raise SystemExit("non-positive goodput")

print(
    f"p6_bbr_loss_reference_comparison=ok case={case} "
    f"base_rtt_ms={ts['base_rtt_ms']} rate_mbit={ts['rate_mbit']} "
    f"loss_pct={ts['loss_pct']} loss_mode={ts['loss_mode']} "
    f"bdp_bytes={ts['bdp_bytes']} queue_pkts={ts['queue_pkts']} "
    f"tcp_shift_goodput_mbps={ts_goodput:.6f} "
    f"linux_bbr_goodput_mbps={linux_goodput:.6f} "
    f"goodput_ratio={ts_goodput / linux_goodput:.6f} "
    f"tcp_shift_data_qdisc_drops={ts_data_drops} "
    f"linux_data_qdisc_drops={linux_data_drops} "
    f"tcp_shift_retransmit_events={ts_retrans} "
    f"linux_total_retrans={linux_retrans} "
    f"tcp_shift_loss_events={ts_loss} "
    f"tcp_shift_timeout_events={ts_timeout} "
    f"tcp_shift_max_rate_Bps={need(ts, 'max_rate_bytes_per_sec')} "
    f"linux_delivery_rate_median_Bps={need(linux, 'delivery_rate_median_Bps')} "
    f"linux_pacing_rate_median_Bps={need(linux, 'pacing_rate_median_Bps')}"
)
print("tcp_shift_loss_summary=" + ts_text)
print("linux_bbr_loss_summary=" + linux_text)
