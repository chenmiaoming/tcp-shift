#!/usr/bin/env python3
import statistics
import sys
from collections import defaultdict
from pathlib import Path

MSS = 1460


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
    events = defaultdict(list)
    marker = "tcp-shift-cubic-trace: "
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        fields = kv_fields(raw.split(marker, 1)[1])
        event = fields.get("event")
        if event:
            events[event].append(fields)
    return events


def read_linux(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing Linux TCP_INFO samples")
    header = lines[0].split("\t")
    required = {
        "unacked", "sacked", "lost", "retrans", "rtt_us",
        "snd_cwnd", "snd_ssthresh", "pacing_rate_Bps", "total_retrans",
    }
    if not required.issubset(set(header)):
        missing = sorted(required.difference(set(header)))
        raise SystemExit("Linux TCP_INFO missing fields: " + ",".join(missing))
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]
    transitions = []
    prior_retrans = 0
    for row in rows:
        current = int(row["total_retrans"])
        if current > prior_retrans:
            transitions.append((current - prior_retrans, row))
        prior_retrans = current
    return rows, transitions


def median_int(values):
    return int(statistics.median(values)) if values else 0


def ratio(num, den):
    return float(num) / float(den) if den else 0.0


def linux_shaped_rate_bytes(cwnd, outstanding, ssthresh, rtt_ns):
    if rtt_ns <= 0:
        return 0
    percent = 200 if cwnd < ssthresh // 2 else 120
    return int(max(cwnd, outstanding) * 1_000_000_000 * percent / (100 * rtt_ns))


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-prr-recovery-differential-evaluate.py "
        "<tcp-shift-runtime.stderr> <tcp-shift-summary> "
        "<linux-tcp-info.tsv> <linux-summary>"
    )

trace_path, ts_summary_path, linux_tsv_path, linux_summary_path = sys.argv[1:]
ts_text, ts = read_summary(ts_summary_path)
linux_text, linux = read_summary(linux_summary_path)
events = parse_trace(trace_path)
linux_rows, linux_transitions = read_linux(linux_tsv_path)

acks = events["prr-ack"]
txs = events["prr-tx"]
exits = events["prr-exit"]
losses = events["loss"]

expected_losses = int(ts.get("fault_marker_count", "0"))
ts_loss_events = int(ts.get("loss_events", "0"))
stats_prr_acks = int(ts.get("prr_ack_events", "0"))
stats_prr_txs = int(ts.get("prr_tx_events", "0"))
stats_prr_enters = int(ts.get("prr_recovery_enters", "0"))
stats_prr_exits = int(ts.get("prr_recovery_exits", "0"))
linux_retrans = int(linux.get("total_retrans", "0"))

if expected_losses <= 0 or ts_loss_events != expected_losses:
    raise SystemExit(
        f"invalid tcp-shift deterministic loss evidence: "
        f"markers={expected_losses} loss_events={ts_loss_events}"
    )
if linux_retrans != expected_losses:
    raise SystemExit(
        f"Linux retransmission mismatch: retrans={linux_retrans} "
        f"markers={expected_losses}"
    )
if len(losses) != ts_loss_events:
    raise SystemExit(
        f"CUBIC loss trace mismatch: trace={len(losses)} stats={ts_loss_events}"
    )
if len(acks) != stats_prr_acks:
    raise SystemExit(
        f"PRR ACK trace mismatch: trace={len(acks)} stats={stats_prr_acks}"
    )
if len(txs) != stats_prr_txs:
    raise SystemExit(
        f"PRR TX trace mismatch: trace={len(txs)} stats={stats_prr_txs}"
    )
if len(exits) != stats_prr_exits:
    raise SystemExit(
        f"PRR exit trace mismatch: trace={len(exits)} stats={stats_prr_exits}"
    )
if stats_prr_enters != expected_losses or stats_prr_exits != expected_losses:
    raise SystemExit(
        f"expected one PRR episode per deterministic loss: "
        f"enters={stats_prr_enters} exits={stats_prr_exits} "
        f"losses={expected_losses}"
    )
if not linux_transitions:
    raise SystemExit("Linux TCP_INFO observed no retransmission transitions")

mode_counts = defaultdict(int)
sndcnt = []
inflight = []
raw_outstanding = []
delivered = []
actual_pacing = []
dynamic_expected_pacing = []
actual_over_dynamic = []
episodes = defaultdict(list)

for event in acks:
    episode = int(event["episode"])
    mode = event["mode"]
    mode_counts[mode] += 1
    episodes[episode].append(event)
    sndcnt.append(int(event["sndcnt"]))
    inflight.append(int(event["inflight_bytes"]))
    raw_outstanding.append(int(event["raw_outstanding_bytes"]))
    delivered.append(int(event["delivered_data"]))
    actual = int(event["pacing_Bps"])
    actual_pacing.append(actual)
    expected = linux_shaped_rate_bytes(
        int(event["cwnd"]),
        int(event["raw_outstanding_bytes"]),
        int(event["ssthresh"]),
        int(event["srtt_ns"]),
    )
    if expected > 0:
        dynamic_expected_pacing.append(expected)
        actual_over_dynamic.append(ratio(actual, expected))

first_ack_sndcnt = []
first_ack_inflight = []
first_ack_pacing = []
episode_constant_pacing = 0
for episode in sorted(episodes):
    rows = episodes[episode]
    first_ack_sndcnt.append(int(rows[0]["sndcnt"]))
    first_ack_inflight.append(int(rows[0]["inflight_bytes"]))
    first_ack_pacing.append(int(rows[0]["pacing_Bps"]))
    if len({int(row["pacing_Bps"]) for row in rows}) == 1:
        episode_constant_pacing += 1

linux_credit = []
linux_inflight = []
linux_actual_pacing = []
linux_expected_pacing = []
linux_actual_over_expected = []
linux_transition_packets = 0
for delta, row in linux_transitions:
    linux_transition_packets += delta
    unacked = int(row["unacked"])
    sacked = int(row["sacked"])
    lost = int(row["lost"])
    retrans = int(row["retrans"])
    in_flight_packets = max(0, unacked - sacked - lost + retrans)
    cwnd_packets = int(row["snd_cwnd"])
    ssthresh_packets = int(row["snd_ssthresh"])
    rtt_us = int(row["rtt_us"])
    pacing = int(row["pacing_rate_Bps"])
    linux_inflight.append(in_flight_packets * MSS)
    linux_credit.append(max(0, cwnd_packets - in_flight_packets) * MSS)
    linux_actual_pacing.append(pacing)
    expected = linux_shaped_rate_bytes(
        cwnd_packets * MSS,
        unacked * MSS,
        ssthresh_packets * MSS,
        rtt_us * 1000,
    )
    if expected > 0:
        linux_expected_pacing.append(expected)
        linux_actual_over_expected.append(ratio(pacing, expected))

print(
    "p6_prr_recovery_differential=ok "
    f"tcp_shift_prr_episodes={stats_prr_enters} "
    f"tcp_shift_prr_ack_events={len(acks)} "
    f"tcp_shift_prr_tx_events={len(txs)} "
    f"tcp_shift_prr_proportional_acks={mode_counts['proportional']} "
    f"tcp_shift_prr_crb_acks={mode_counts['crb']} "
    f"tcp_shift_prr_ssrb_acks={mode_counts['ssrb']} "
    f"tcp_shift_prr_sndcnt_median_bytes={median_int(sndcnt)} "
    f"tcp_shift_prr_first_sndcnt_median_bytes={median_int(first_ack_sndcnt)} "
    f"tcp_shift_prr_inflight_median_bytes={median_int(inflight)} "
    f"tcp_shift_prr_first_inflight_median_bytes={median_int(first_ack_inflight)} "
    f"tcp_shift_prr_raw_outstanding_median_bytes={median_int(raw_outstanding)} "
    f"tcp_shift_prr_delivered_median_bytes={median_int(delivered)} "
    f"tcp_shift_prr_pacing_median_Bps={median_int(actual_pacing)} "
    f"tcp_shift_prr_dynamic_expected_pacing_median_Bps={median_int(dynamic_expected_pacing)} "
    f"tcp_shift_prr_actual_over_dynamic_expected_median="
    f"{statistics.median(actual_over_dynamic) if actual_over_dynamic else 0.0:.6f} "
    f"tcp_shift_prr_constant_pacing_episodes={episode_constant_pacing} "
    f"linux_tcp_info_samples={len(linux_rows)} "
    f"linux_retrans_transition_samples={len(linux_transitions)} "
    f"linux_retrans_transition_packets={linux_transition_packets} "
    f"linux_transition_inflight_median_bytes={median_int(linux_inflight)} "
    f"linux_transition_window_headroom_median_bytes={median_int(linux_credit)} "
    f"linux_transition_pacing_median_Bps={median_int(linux_actual_pacing)} "
    f"linux_transition_expected_pacing_median_Bps={median_int(linux_expected_pacing)} "
    f"linux_transition_actual_over_expected_median="
    f"{statistics.median(linux_actual_over_expected) if linux_actual_over_expected else 0.0:.6f}"
)
print("tcp_shift_summary=" + ts_text)
print("linux_summary=" + linux_text)
