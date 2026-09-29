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


def parse_trace(path):
    losses = []
    exits = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        marker = "tcp-shift-cubic-trace: "
        if marker not in raw:
            continue
        fields = kv_fields(raw.split(marker, 1)[1])
        event = fields.get("event")
        if event == "loss":
            losses.append(fields)
        elif event == "recovery-exit":
            exits.append(fields)
    return losses, exits


def read_linux_rows(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing Linux TCP_INFO samples")
    header = lines[0].split("\t")
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]
    transitions = []
    prior_retrans = 0
    for row in rows:
        current = int(row["total_retrans"])
        if current > prior_retrans:
            transitions.append((current - prior_retrans, row))
        prior_retrans = current
    return rows, transitions


def median(values):
    return statistics.median(values) if values else 0.0


def ratio(num, den):
    return float(num) / float(den) if den else 0.0


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-cubic-loss-timeline-evaluate.py "
        "<tcp-shift-runtime.stderr> <tcp-shift-summary> "
        "<linux-tcp-info.tsv> <linux-summary>"
    )

trace_path, ts_summary_path, linux_tsv_path, linux_summary_path = sys.argv[1:]
ts_text, ts = read_summary(ts_summary_path)
linux_text, linux = read_summary(linux_summary_path)
losses, exits = parse_trace(trace_path)
linux_rows, linux_transitions = read_linux_rows(linux_tsv_path)

expected_losses = int(ts.get("fault_marker_count", "0"))
ts_loss_events = int(ts.get("loss_events", "0"))
linux_retrans = int(linux.get("total_retrans", "0"))
if expected_losses <= 0:
    raise SystemExit("missing deterministic marker count")
if ts_loss_events != expected_losses:
    raise SystemExit(
        f"tcp-shift loss count mismatch: events={ts_loss_events} "
        f"markers={expected_losses}"
    )
if linux_retrans != expected_losses:
    raise SystemExit(
        f"Linux retransmission count mismatch: retrans={linux_retrans} "
        f"markers={expected_losses}"
    )
if len(losses) != ts_loss_events:
    raise SystemExit(
        f"trace loss count mismatch: trace={len(losses)} stats={ts_loss_events}"
    )
if not linux_transitions:
    raise SystemExit("Linux TCP_INFO observed no retransmission transition")

flight_over_cwnd = []
post_over_pre_cwnd = []
post_over_flight = []
pre_cwnd = []
flight = []
post_cwnd = []
trace_pacing = []
active_before = 0
for event in losses:
    pre = int(event["pre_cwnd"])
    inflight = int(event["inflight_bytes"])
    post = int(event["post_cwnd"])
    if pre <= 0 or inflight <= 0 or post <= 0:
        raise SystemExit(f"invalid CUBIC trace event: {event}")
    pre_cwnd.append(pre)
    flight.append(inflight)
    post_cwnd.append(post)
    flight_over_cwnd.append(ratio(inflight, pre))
    post_over_pre_cwnd.append(ratio(post, pre))
    post_over_flight.append(ratio(post, inflight))
    trace_pacing.append(int(event["post_pacing_Bps"]))
    active_before += int(event.get("recovery_active_before", "0"))

beta_flight_median = median(post_over_flight)
if not (0.68 <= beta_flight_median <= 0.72):
    raise SystemExit(
        f"tcp-shift CUBIC no longer reflects beta over flight_size: "
        f"median={beta_flight_median:.6f}"
    )

linux_event_cwnd_bytes = []
linux_event_ssthresh_bytes = []
linux_event_pacing = []
linux_transition_packets = 0
for delta, row in linux_transitions:
    linux_transition_packets += delta
    linux_event_cwnd_bytes.append(int(row["snd_cwnd"]) * 1460)
    linux_event_ssthresh_bytes.append(int(row["snd_ssthresh"]) * 1460)
    linux_event_pacing.append(int(row["pacing_rate_Bps"]))

if linux_transition_packets != expected_losses:
    raise SystemExit(
        f"Linux transition accounting mismatch: transitions={linux_transition_packets} "
        f"retrans={expected_losses}"
    )

flight_ratio_med = median(flight_over_cwnd)
cwnd_reduction_med = median(post_over_pre_cwnd)
materially_below = 1 if flight_ratio_med < 0.90 else 0

print(
    "p6_cubic_loss_timeline=ok "
    f"tcp_shift_loss_events={len(losses)} "
    f"tcp_shift_recovery_exits={len(exits)} "
    f"tcp_shift_loss_while_recovery_active={active_before} "
    f"tcp_shift_pre_cwnd_median_bytes={int(median(pre_cwnd))} "
    f"tcp_shift_flight_median_bytes={int(median(flight))} "
    f"tcp_shift_post_cwnd_median_bytes={int(median(post_cwnd))} "
    f"tcp_shift_flight_over_cwnd_median={flight_ratio_med:.6f} "
    f"tcp_shift_post_over_pre_cwnd_median={cwnd_reduction_med:.6f} "
    f"tcp_shift_post_over_flight_median={beta_flight_median:.6f} "
    f"tcp_shift_post_pacing_median_Bps={int(median(trace_pacing))} "
    f"flight_size_materially_below_cwnd={materially_below} "
    f"linux_tcp_info_samples={len(linux_rows)} "
    f"linux_retrans_transition_samples={len(linux_transitions)} "
    f"linux_retrans_transition_packets={linux_transition_packets} "
    f"linux_event_cwnd_median_bytes={int(median(linux_event_cwnd_bytes))} "
    f"linux_event_ssthresh_median_bytes={int(median(linux_event_ssthresh_bytes))} "
    f"linux_event_pacing_median_Bps={int(median(linux_event_pacing))}"
)
print("tcp_shift_summary=" + ts_text)
print("linux_summary=" + linux_text)
