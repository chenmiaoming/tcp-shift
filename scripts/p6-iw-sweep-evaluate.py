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


def check_tcp_shift(fields, payload, mode):
    if need(fields, "p6_bbr_long_flow") != "ok":
        raise SystemExit("tcp-shift run not successful")
    if need(fields, "cc") != "bbr-internal":
        raise SystemExit(f"unexpected tcp-shift cc={fields.get('cc')}")
    if int(need(fields, "payload_bytes")) != payload:
        raise SystemExit("tcp-shift payload mismatch")
    if need(fields, "qdisc_drops") != "0/0":
        raise SystemExit(f"tcp-shift qdisc drops: {fields['qdisc_drops']}")
    if int(need(fields, "timeout_events")) != 0:
        raise SystemExit(f"tcp-shift RTO fallback: {fields['timeout_events']}")
    if mode == "clean":
        if need(fields, "loss_mode") != "none":
            raise SystemExit(f"unexpected clean loss_mode={fields['loss_mode']}")
        if int(need(fields, "fault_drops")) != 0:
            raise SystemExit("clean tcp-shift fault drops")
        if int(need(fields, "retransmit_events")) != 0:
            raise SystemExit("clean tcp-shift retransmissions")
    else:
        if need(fields, "loss_mode") != "deterministic-first-send":
            raise SystemExit(f"unexpected loss mode={fields['loss_mode']}")
        if int(need(fields, "fault_drops")) != 28:
            raise SystemExit(f"tcp-shift fault count={fields['fault_drops']}")
        if int(need(fields, "retransmit_events")) != 28:
            raise SystemExit(
                f"tcp-shift retransmissions={fields['retransmit_events']}"
            )


def check_linux(fields, payload, mode):
    if need(fields, "linux_cubic_pacing_reference") != "ok":
        raise SystemExit("Linux reference run not successful")
    if need(fields, "cc") != "bbr":
        raise SystemExit(f"unexpected Linux cc={fields.get('cc')}")
    if need(fields, "data_qdisc_drops") != "0":
        raise SystemExit(f"Linux data qdisc drops={fields['data_qdisc_drops']}")
    if need(fields, "ack_qdisc_drops") != "0":
        raise SystemExit(f"Linux ACK qdisc drops={fields['ack_qdisc_drops']}")
    if mode == "clean":
        if need(fields, "loss_mode") != "none":
            raise SystemExit(f"unexpected Linux clean loss_mode={fields['loss_mode']}")
        if int(need(fields, "fault_drops")) != 0:
            raise SystemExit("clean Linux fault drops")
        if int(need(fields, "total_retrans")) != 0:
            raise SystemExit("clean Linux retransmissions")
    else:
        if need(fields, "loss_mode") != "deterministic-first-send":
            raise SystemExit(f"unexpected Linux loss mode={fields['loss_mode']}")
        if int(need(fields, "fault_drops")) != 28:
            raise SystemExit(f"Linux fault count={fields['fault_drops']}")
        if int(need(fields, "total_retrans")) != 28:
            raise SystemExit(f"Linux retransmissions={fields['total_retrans']}")


if len(sys.argv) != 7:
    raise SystemExit(
        "usage: p6-iw-sweep-evaluate.py "
        "<payload-bytes> <clean|loss> "
        "<iw4-summary> <iw10-summary> <linux-summary> <output>"
    )

payload = int(sys.argv[1])
mode = sys.argv[2]
if mode not in ("clean", "loss"):
    raise SystemExit(f"invalid mode={mode}")

iw4_text, iw4 = parse(sys.argv[3])
iw10_text, iw10 = parse(sys.argv[4])
linux_text, linux = parse(sys.argv[5])
check_tcp_shift(iw4, payload, mode)
check_tcp_shift(iw10, payload, mode)
check_linux(linux, payload, mode)

for key in ("base_rtt_ms", "rate_mbit", "bdp_bytes", "queue_pkts"):
    if need(iw4, key) != need(iw10, key) or need(iw4, key) != need(linux, key):
        raise SystemExit(
            f"path mismatch {key}: iw4={iw4[key]} iw10={iw10[key]} "
            f"linux={linux[key]}"
        )

iw4_goodput = float(need(iw4, "goodput_mbps"))
iw10_goodput = float(need(iw10, "goodput_mbps"))
linux_goodput = float(need(linux, "goodput_mbps"))
if min(iw4_goodput, iw10_goodput, linux_goodput) <= 0.0:
    raise SystemExit("non-positive goodput")

line = (
    "p6_iw_sweep=ok "
    f"payload_bytes={payload} mode={mode} "
    f"iw4_goodput_mbps={iw4_goodput:.6f} "
    f"iw10_goodput_mbps={iw10_goodput:.6f} "
    f"linux_bbr_goodput_mbps={linux_goodput:.6f} "
    f"iw4_linux_ratio={iw4_goodput / linux_goodput:.6f} "
    f"iw10_linux_ratio={iw10_goodput / linux_goodput:.6f} "
    f"iw10_iw4_ratio={iw10_goodput / iw4_goodput:.6f}"
)
print(line)
Path(sys.argv[6]).write_text(
    line + "\n"
    + "iw4_summary=" + iw4_text + "\n"
    + "iw10_summary=" + iw10_text + "\n"
    + "linux_summary=" + linux_text + "\n",
    encoding="utf-8",
)
