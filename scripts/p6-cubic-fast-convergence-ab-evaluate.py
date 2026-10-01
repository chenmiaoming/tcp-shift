#!/usr/bin/env python3
import statistics
import sys
from pathlib import Path


def kv_fields(text):
    fields = {}
    for token in text.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def read_summary(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"empty summary: {path}")
    return text, kv_fields(text)


def parse_losses(path):
    marker = "tcp-shift-cubic-trace: "
    rows = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        fields = kv_fields(raw.split(marker, 1)[1])
        if fields.get("event") == "loss":
            rows.append(fields)
    return rows


def median_int(values):
    return int(statistics.median(values)) if values else 0


def goodput(summary):
    return float(summary.get("goodput_mbps", "0"))


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-cubic-fast-convergence-ab-evaluate.py "
        "<default-runtime.stderr> <default-summary> "
        "<single-flow-runtime.stderr> <single-flow-summary>"
    )

default_trace, default_summary_path, single_trace, single_summary_path = sys.argv[1:]
default_text, default = read_summary(default_summary_path)
single_text, single = read_summary(single_summary_path)
default_losses = parse_losses(default_trace)
single_losses = parse_losses(single_trace)

expected = int(default.get("fault_marker_count", "0"))
if expected <= 0 or int(single.get("fault_marker_count", "0")) != expected:
    raise SystemExit("deterministic fault count mismatch")

for name, summary, losses in (
    ("default", default, default_losses),
    ("single-flow", single, single_losses),
):
    if int(summary.get("loss_events", "0")) != expected:
        raise SystemExit(f"{name} loss count mismatch")
    if int(summary.get("retransmit_events", "0")) != expected:
        raise SystemExit(f"{name} retransmit count mismatch")
    if int(summary.get("timeout_events", "0")) != 0:
        raise SystemExit(f"{name} unexpectedly used RTO")
    if summary.get("qdisc_drops") != "0/0":
        raise SystemExit(f"{name} has unrelated qdisc drops")
    if summary.get("payload_integrity") != "ok":
        raise SystemExit(f"{name} payload integrity failed")
    if len(losses) != expected:
        raise SystemExit(
            f"{name} loss trace mismatch: trace={len(losses)} expected={expected}"
        )

required = {
    "pre_cwnd", "post_cwnd", "inflight_bytes",
    "fast_convergence", "fast_convergence_applied",
    "post_w_max_q16",
}
for name, losses in (("default", default_losses), ("single-flow", single_losses)):
    for row in losses:
        missing = sorted(required.difference(row))
        if missing:
            raise SystemExit(f"{name} loss trace missing: {','.join(missing)}")

default_fast = sum(int(row["fast_convergence_applied"]) for row in default_losses)
single_fast = sum(int(row["fast_convergence_applied"]) for row in single_losses)
if default_fast == 0:
    raise SystemExit("default path did not exercise fast convergence")
if any(int(row["fast_convergence"]) != 0 for row in single_losses):
    raise SystemExit("single-flow qualification did not disable fast convergence")
if single_fast != 0:
    raise SystemExit("single-flow qualification still applied fast convergence")

default_steady = default_losses[8:]
single_steady = single_losses[8:]

default_pre = [int(row["pre_cwnd"]) for row in default_steady]
single_pre = [int(row["pre_cwnd"]) for row in single_steady]
default_post = [int(row["post_cwnd"]) for row in default_steady]
single_post = [int(row["post_cwnd"]) for row in single_steady]
default_wmax = [int(row["post_w_max_q16"]) for row in default_steady]
single_wmax = [int(row["post_w_max_q16"]) for row in single_steady]

default_goodput = goodput(default)
single_goodput = goodput(single)
ratio = single_goodput / default_goodput if default_goodput else 0.0

print(
    "p6_cubic_fast_convergence_ab=ok "
    f"episodes={expected} "
    f"default_fast_convergence_events={default_fast} "
    f"single_flow_fast_convergence_events={single_fast} "
    f"default_goodput_mbps={default_goodput:.6f} "
    f"single_flow_goodput_mbps={single_goodput:.6f} "
    f"single_over_default_goodput={ratio:.6f} "
    f"default_steady_pre_cwnd_median_bytes={median_int(default_pre)} "
    f"single_flow_steady_pre_cwnd_median_bytes={median_int(single_pre)} "
    f"default_steady_post_cwnd_median_bytes={median_int(default_post)} "
    f"single_flow_steady_post_cwnd_median_bytes={median_int(single_post)} "
    f"default_steady_wmax_q16_median={median_int(default_wmax)} "
    f"single_flow_steady_wmax_q16_median={median_int(single_wmax)}"
)

for index, (base, single_row) in enumerate(
    zip(default_losses[:12], single_losses[:12]), start=1
):
    print(
        f"episode={index} "
        f"default_pre={base['pre_cwnd']} "
        f"default_post={base['post_cwnd']} "
        f"default_fast={base['fast_convergence_applied']} "
        f"default_wmax_q16={base['post_w_max_q16']} "
        f"single_pre={single_row['pre_cwnd']} "
        f"single_post={single_row['post_cwnd']} "
        f"single_fast={single_row['fast_convergence_applied']} "
        f"single_wmax_q16={single_row['post_w_max_q16']}"
    )

print("default_summary=" + default_text)
print("single_flow_summary=" + single_text)
