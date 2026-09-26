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
        "usage: p6-cubic-clean-control-evaluate.py "
        "<tcp-shift-summary> <linux-summary>"
    )

ts_text, ts = parse(sys.argv[1])
linux_text, linux = parse(sys.argv[2])

if need(ts, "p6_bbr_long_flow") != "ok":
    raise SystemExit("tcp-shift summary is not successful")
if need(linux, "linux_cubic_pacing_reference") != "ok":
    raise SystemExit("Linux CUBIC summary is not successful")
if need(ts, "cc") != "cubic" or need(linux, "cc") != "cubic":
    raise SystemExit(
        f"unexpected controllers tcp-shift={ts.get('cc')} linux={linux.get('cc')}"
    )

for key in ("base_rtt_ms", "rate_mbit", "bdp_bytes", "queue_pkts"):
    if need(ts, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch for {key}: tcp-shift={ts[key]} linux={linux[key]}"
        )

if need(ts, "loss_mode") != "none" or need(linux, "loss_mode") != "none":
    raise SystemExit(
        f"expected clean path: tcp-shift={ts.get('loss_mode')} "
        f"linux={linux.get('loss_mode')}"
    )

ts_qdisc = need(ts, "qdisc_drops").split("/")
if len(ts_qdisc) != 2 or any(int(value) != 0 for value in ts_qdisc):
    raise SystemExit(f"tcp-shift clean qdisc is not clean: {ts.get('qdisc_drops')}")
if int(need(linux, "data_qdisc_drops")) != 0 or int(need(linux, "ack_qdisc_drops")) != 0:
    raise SystemExit("Linux clean qdisc is not clean")
if int(need(ts, "retransmit_events")) != 0:
    raise SystemExit(f"tcp-shift clean retransmissions={ts['retransmit_events']}")
if int(need(ts, "loss_events")) != 0 or int(need(ts, "timeout_events")) != 0:
    raise SystemExit(
        f"tcp-shift clean recovery loss={ts['loss_events']} "
        f"timeout={ts['timeout_events']}"
    )
if int(need(linux, "total_retrans")) != 0:
    raise SystemExit(f"Linux clean retransmissions={linux['total_retrans']}")

ts_goodput = float(need(ts, "goodput_mbps"))
linux_goodput = float(need(linux, "goodput_mbps"))
if ts_goodput <= 0.0 or linux_goodput <= 0.0:
    raise SystemExit("non-positive goodput")

print(
    "p6_cubic_clean_control=ok "
    f"base_rtt_ms={ts['base_rtt_ms']} rate_mbit={ts['rate_mbit']} "
    f"tcp_shift_goodput_mbps={ts_goodput:.6f} "
    f"linux_cubic_goodput_mbps={linux_goodput:.6f} "
    f"goodput_ratio={ts_goodput / linux_goodput:.6f}"
)
print("tcp_shift_cubic_clean_summary=" + ts_text)
print("linux_cubic_clean_summary=" + linux_text)
