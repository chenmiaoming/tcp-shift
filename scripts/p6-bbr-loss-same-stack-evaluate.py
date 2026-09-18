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
        "usage: p6-bbr-loss-same-stack-evaluate.py "
        "<bbr-summary> <cubic-summary>"
    )

bbr_text, bbr = parse(sys.argv[1])
cubic_text, cubic = parse(sys.argv[2])

for fields, expected_cc in ((bbr, "bbr-internal"), (cubic, "cubic")):
    if need(fields, "p6_bbr_long_flow") != "ok":
        raise SystemExit(f"{expected_cc} long-flow summary is not successful")
    if need(fields, "cc") != expected_cc:
        raise SystemExit(
            f"expected cc={expected_cc}, got {fields.get('cc')}"
        )
    if need(fields, "loss_mode") != "random":
        raise SystemExit(f"{expected_cc} did not run random loss")

for key in (
    "base_rtt_ms",
    "rate_mbit",
    "loss_pct",
    "loss_mode",
    "bdp_bytes",
    "queue_pkts",
    "payload_bytes",
):
    if need(bbr, key) != need(cubic, key):
        raise SystemExit(
            f"path mismatch for {key}: bbr={bbr[key]} cubic={cubic[key]}"
        )

for name, fields in (("bbr", bbr), ("cubic", cubic)):
    drops = need(fields, "qdisc_drops").split("/")
    if len(drops) != 2:
        raise SystemExit(f"{name} invalid qdisc_drops={fields['qdisc_drops']}")
    data_drops, ack_drops = map(int, drops)
    if data_drops < 1 or ack_drops != 0:
        raise SystemExit(
            f"{name} loss direction invalid: data={data_drops} ack={ack_drops}"
        )
    if int(need(fields, "retransmit_events")) < 1:
        raise SystemExit(f"{name} observed no retransmission")
    if int(need(fields, "loss_events")) + int(need(fields, "timeout_events")) < 1:
        raise SystemExit(f"{name} observed no recovery event")

bbr_goodput = float(need(bbr, "goodput_mbps"))
cubic_goodput = float(need(cubic, "goodput_mbps"))
if bbr_goodput <= 0.0 or cubic_goodput <= 0.0:
    raise SystemExit("non-positive same-stack goodput")

print(
    "p6_bbr_loss_same_stack=ok "
    f"base_rtt_ms={bbr['base_rtt_ms']} rate_mbit={bbr['rate_mbit']} "
    f"loss_pct={bbr['loss_pct']} "
    f"bbr_goodput_mbps={bbr_goodput:.6f} "
    f"cubic_goodput_mbps={cubic_goodput:.6f} "
    f"bbr_to_cubic_goodput_ratio={bbr_goodput / cubic_goodput:.6f} "
    f"bbr_qdisc_drops={bbr['qdisc_drops']} "
    f"cubic_qdisc_drops={cubic['qdisc_drops']} "
    f"bbr_retransmit_events={bbr['retransmit_events']} "
    f"cubic_retransmit_events={cubic['retransmit_events']} "
    f"bbr_loss_events={bbr['loss_events']} "
    f"cubic_loss_events={cubic['loss_events']} "
    f"bbr_timeout_events={bbr['timeout_events']} "
    f"cubic_timeout_events={cubic['timeout_events']} "
    f"bbr_final_cwnd_bytes={bbr['cwnd_bytes']} "
    f"cubic_final_cwnd_bytes={cubic['cwnd_bytes']}"
)
print("bbr_summary=" + bbr_text)
print("cubic_summary=" + cubic_text)
