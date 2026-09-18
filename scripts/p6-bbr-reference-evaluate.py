#!/usr/bin/env python3
import sys
from pathlib import Path


def parse_summary(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"empty summary: {path}")
    fields = {}
    for token in text.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return text, fields


def require(fields, key):
    if key not in fields or fields[key] == "":
        raise SystemExit(f"missing {key}")
    return fields[key]


if len(sys.argv) != 4:
    raise SystemExit(
        "usage: p6-bbr-reference-evaluate.py <case> "
        "<tcp-shift-summary> <linux-summary>"
    )

case, tcp_shift_path, linux_path = sys.argv[1:]
tcp_shift_text, ts = parse_summary(tcp_shift_path)
linux_text, linux = parse_summary(linux_path)

if require(ts, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift BBR long-flow summary is not successful")
if require(linux, "linux_bbr_pacing_reference") != "ok":
    raise SystemExit("Linux BBR reference summary is not successful")
if require(linux, "cc") != "bbr":
    raise SystemExit(f"unexpected Linux reference cc={linux.get('cc')}")

for key in ("base_rtt_ms", "rate_mbit", "bdp_bytes", "queue_pkts"):
    if require(ts, key) != require(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if require(ts, "qdisc_drops") != "0/0":
    raise SystemExit(f"tcp-shift clean path had qdisc drops: {ts['qdisc_drops']}")
if int(require(ts, "loss_events")) != 0 or int(require(ts, "timeout_events")) != 0:
    raise SystemExit(
        "tcp-shift clean path entered recovery: "
        f"loss={ts['loss_events']} timeout={ts['timeout_events']}"
    )

ts_goodput = float(require(ts, "goodput_mbps"))
linux_goodput = float(require(linux, "goodput_mbps"))
if ts_goodput <= 0.0 or linux_goodput <= 0.0:
    raise SystemExit("non-positive goodput")
ratio = ts_goodput / linux_goodput

ts_max_rate = int(require(ts, "max_rate_bytes_per_sec"))
linux_delivery_median = int(require(linux, "delivery_rate_median_Bps"))
linux_pacing_median = int(require(linux, "pacing_rate_median_Bps"))
linux_pacing_final = int(require(linux, "pacing_rate_final_Bps"))
linux_retrans = int(require(linux, "total_retrans"))

print(
    f"p6_bbr_reference_comparison=ok case={case} "
    f"base_rtt_ms={ts['base_rtt_ms']} rate_mbit={ts['rate_mbit']} "
    f"bdp_bytes={ts['bdp_bytes']} queue_pkts={ts['queue_pkts']} "
    f"tcp_shift_goodput_mbps={ts_goodput:.6f} "
    f"linux_bbr_goodput_mbps={linux_goodput:.6f} "
    f"goodput_ratio={ratio:.6f} "
    f"tcp_shift_final_cwnd_bytes={require(ts, 'cwnd_bytes')} "
    f"tcp_shift_max_rate_Bps={ts_max_rate} "
    f"linux_delivery_rate_median_Bps={linux_delivery_median} "
    f"linux_pacing_rate_median_Bps={linux_pacing_median} "
    f"linux_pacing_rate_final_Bps={linux_pacing_final} "
    f"linux_total_retrans={linux_retrans}"
)

print("tcp_shift_summary=" + tcp_shift_text)
print("linux_bbr_summary=" + linux_text)
