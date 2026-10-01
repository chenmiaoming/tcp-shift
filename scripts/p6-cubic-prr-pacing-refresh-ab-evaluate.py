#!/usr/bin/env python3
import statistics
import sys
from pathlib import Path


def read_summary(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"empty summary: {path}")
    fields = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return text, fields


def parse_refresh(path):
    marker = "tcp-shift-cubic-trace: event=prr-pacing-refresh "
    rows = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        fields = {}
        for token in raw.split(marker, 1)[1].split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        rows.append(fields)
    return rows


def need(row, key):
    if key not in row:
        raise SystemExit(f"missing {key}")
    return row[key]


def goodput(row):
    return float(need(row, "goodput_mbps"))


def check_tcp_shift(name, row):
    if need(row, "cc") != "cubic":
        raise SystemExit(f"{name}: not CUBIC")
    expected = {
        "fault_drops": "28",
        "loss_events": "28",
        "retransmit_events": "28",
        "timeout_events": "0",
        "qdisc_drops": "0/0",
        "payload_integrity": "ok",
    }
    for key, value in expected.items():
        if need(row, key) != value:
            raise SystemExit(
                f"{name}: {key}={row.get(key)} expected={value}"
            )


def med(values):
    return statistics.median(values) if values else 0.0


if len(sys.argv) != 8:
    raise SystemExit(
        "usage: p6-cubic-prr-pacing-refresh-ab-evaluate.py "
        "<baseline-summary> <actual-summary> <actual-stderr> "
        "<raw-summary> <raw-stderr> <linux-summary> <baseline-stderr>"
    )

baseline_text, baseline = read_summary(sys.argv[1])
actual_text, actual = read_summary(sys.argv[2])
actual_refresh = parse_refresh(sys.argv[3])
raw_text, raw = read_summary(sys.argv[4])
raw_refresh = parse_refresh(sys.argv[5])
linux_text, linux = read_summary(sys.argv[6])
baseline_refresh = parse_refresh(sys.argv[7])

for name, row in (
    ("baseline", baseline),
    ("actual-refresh", actual),
    ("raw-refresh", raw),
):
    check_tcp_shift(name, row)

if baseline_refresh:
    raise SystemExit("baseline unexpectedly contains pacing refresh events")
if not actual_refresh or not raw_refresh:
    raise SystemExit("missing qualification pacing refresh events")

if {row.get("basis") for row in actual_refresh} != {"actual-inflight"}:
    raise SystemExit("actual-refresh trace has unexpected basis")
if {row.get("basis") for row in raw_refresh} != {"raw-outstanding"}:
    raise SystemExit("raw-refresh trace has unexpected basis")

for rows, name in (
    (actual_refresh, "actual-refresh"),
    (raw_refresh, "raw-refresh"),
):
    episodes = {int(need(row, "episode")) for row in rows}
    if episodes != set(range(1, 29)):
        raise SystemExit(f"{name}: refresh episodes mismatch")

if need(linux, "fault_drops") != "28" or need(linux, "total_retrans") != "28":
    raise SystemExit("Linux deterministic loss invariant mismatch")
if (
    need(linux, "data_qdisc_drops") != "0"
    or need(linux, "ack_qdisc_drops") != "0"
):
    raise SystemExit("Linux qdisc drop invariant mismatch")

for key in ("base_rtt_ms", "rate_mbit"):
    values = {need(row, key) for row in (baseline, actual, raw, linux)}
    if len(values) != 1:
        raise SystemExit(f"path mismatch for {key}")

baseline_g = goodput(baseline)
actual_g = goodput(actual)
raw_g = goodput(raw)
linux_g = goodput(linux)

actual_rates = [int(need(row, "new_Bps")) for row in actual_refresh]
raw_rates = [int(need(row, "new_Bps")) for row in raw_refresh]
actual_changes = sum(
    int(need(row, "old_Bps")) != int(need(row, "new_Bps"))
    for row in actual_refresh
)
raw_changes = sum(
    int(need(row, "old_Bps")) != int(need(row, "new_Bps"))
    for row in raw_refresh
)

print(
    "p6_cubic_prr_pacing_refresh_ab=ok "
    f"baseline_goodput_mbps={baseline_g:.6f} "
    f"actual_refresh_goodput_mbps={actual_g:.6f} "
    f"raw_refresh_goodput_mbps={raw_g:.6f} "
    f"linux_goodput_mbps={linux_g:.6f} "
    f"actual_over_baseline={actual_g / baseline_g:.6f} "
    f"raw_over_baseline={raw_g / baseline_g:.6f} "
    f"baseline_over_linux={baseline_g / linux_g:.6f} "
    f"actual_over_linux={actual_g / linux_g:.6f} "
    f"raw_over_linux={raw_g / linux_g:.6f} "
    f"actual_refresh_events={len(actual_refresh)} "
    f"raw_refresh_events={len(raw_refresh)} "
    f"actual_rate_change_events={actual_changes} "
    f"raw_rate_change_events={raw_changes} "
    f"actual_refresh_rate_median_Bps={med(actual_rates):.1f} "
    f"raw_refresh_rate_median_Bps={med(raw_rates):.1f} "
    f"baseline_pacing_deferrals={need(baseline, 'pacing_deferrals')} "
    f"actual_pacing_deferrals={need(actual, 'pacing_deferrals')} "
    f"raw_pacing_deferrals={need(raw, 'pacing_deferrals')}"
)
print("baseline_summary=" + baseline_text)
print("actual_refresh_summary=" + actual_text)
print("raw_refresh_summary=" + raw_text)
print("linux_summary=" + linux_text)
