#!/usr/bin/env python3
import statistics
import sys
from collections import defaultdict
from pathlib import Path

TCP_CA_OPEN = 0


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


def parse_tcp_shift(path):
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


def read_linux_trace(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing high-resolution Linux TCP_INFO trace")
    header = lines[0].split("\t")
    required = {
        "sample",
        "source",
        "elapsed_ns",
        "ca_state",
        "bytes_acked",
        "bytes_sent",
        "bytes_retrans",
        "total_retrans",
        "snd_cwnd",
        "snd_ssthresh",
        "unacked",
        "sacked",
        "lost",
        "retrans",
    }
    missing = sorted(required.difference(header))
    if missing:
        raise SystemExit("Linux trace missing fields: " + ",".join(missing))
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]
    if not rows:
        raise SystemExit("empty Linux TCP_INFO trace")
    return rows


def median_int(values):
    return int(statistics.median(values)) if values else 0


def median_float(values):
    return statistics.median(values) if values else 0.0


def ratio(num, den):
    return float(num) / float(den) if den else 0.0


def first_crossing_ns(rows, field, base, delta, start_ns):
    if delta <= 0:
        return 0
    target = base + delta
    for row in rows:
        if int(row[field]) >= target:
            return max(0, int(row["elapsed_ns"]) - start_ns)
    return 0


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-prr-recovery-byte-trajectory-evaluate.py "
        "<tcp-shift-runtime.stderr> <tcp-shift-summary> "
        "<linux-tcp-info-trace.tsv> <linux-summary>"
    )

ts_trace_path, ts_summary_path, linux_trace_path, linux_summary_path = sys.argv[1:]
ts_text, ts = read_summary(ts_summary_path)
linux_text, linux = read_summary(linux_summary_path)
events = parse_tcp_shift(ts_trace_path)
linux_rows = read_linux_trace(linux_trace_path)

expected = int(ts.get("fault_marker_count", "0"))
if expected <= 0:
    raise SystemExit("missing deterministic fault count")
if int(ts.get("loss_events", "0")) != expected:
    raise SystemExit("tcp-shift loss count no longer matches deterministic faults")
if int(ts.get("retransmit_events", "0")) != expected:
    raise SystemExit("tcp-shift retransmit count no longer matches deterministic faults")
if int(ts.get("timeout_events", "0")) != 0:
    raise SystemExit("tcp-shift unexpectedly fell back to RTO")
if int(linux.get("total_retrans", "0")) != expected:
    raise SystemExit("Linux retransmission count no longer matches deterministic faults")

losses = events["loss"]
acks = events["prr-ack"]
txs = events["prr-tx"]
exits = events["prr-exit"]

if len(losses) != expected or len(exits) != expected:
    raise SystemExit(
        f"tcp-shift recovery trace mismatch: losses={len(losses)} "
        f"exits={len(exits)} expected={expected}"
    )

acks_by_episode = defaultdict(list)
txs_by_episode = defaultdict(list)
exits_by_episode = {}
for row in acks:
    acks_by_episode[int(row["episode"])].append(row)
for row in txs:
    txs_by_episode[int(row["episode"])].append(row)
for row in exits:
    exits_by_episode[int(row["episode"])] = row

ts_episodes = []
prr_delivered_mismatches = 0
for episode in range(1, expected + 1):
    loss = losses[episode - 1]
    exit_row = exits_by_episode.get(episode)
    if exit_row is None:
        raise SystemExit(f"missing tcp-shift PRR exit for episode {episode}")

    loss_ns = int(loss["time_ns"])
    exit_ns = int(exit_row["time_ns"])
    if exit_ns < loss_ns:
        raise SystemExit(f"tcp-shift recovery exit precedes loss for episode {episode}")

    recovery_end_seq = int(loss["recovery_end_seq"])
    ack_rows = sorted(
        acks_by_episode.get(episode, []),
        key=lambda row: int(row["time_ns"]),
    )
    tx_rows = sorted(
        txs_by_episode.get(episode, []),
        key=lambda row: int(row["time_ns"]),
    )

    tx_total = sum(int(row["bytes"]) for row in tx_rows)
    repair_tx = sum(
        int(row["bytes"]) for row in tx_rows
        if int(row["seq"]) < recovery_end_seq
    )
    new_tx = tx_total - repair_tx
    delivered_sum = sum(int(row["delivered_data"]) for row in ack_rows)
    exit_delivered = int(exit_row.get("prr_delivered", "0"))
    exit_prr_out = int(exit_row.get("prr_out", "0"))
    recover_fs = int(exit_row.get("recover_fs", "0"))
    if delivered_sum != exit_delivered:
        prr_delivered_mismatches += 1

    first_tx_ns = int(tx_rows[0]["time_ns"]) if tx_rows else 0
    last_tx_ns = int(tx_rows[-1]["time_ns"]) if tx_rows else 0
    new_rows = [row for row in tx_rows if int(row["seq"]) >= recovery_end_seq]
    first_new_ns = int(new_rows[0]["time_ns"]) if new_rows else 0

    tx_gaps = [
        int(tx_rows[i]["time_ns"]) - int(tx_rows[i - 1]["time_ns"])
        for i in range(1, len(tx_rows))
    ]

    half_tx_delay_ns = 0
    if tx_total > 0:
        target = (tx_total + 1) // 2
        cumulative = 0
        for row in tx_rows:
            cumulative += int(row["bytes"])
            if cumulative >= target:
                half_tx_delay_ns = int(row["time_ns"]) - loss_ns
                break

    ts_episodes.append(
        {
            "episode": episode,
            "recovery_ns": exit_ns - loss_ns,
            "acks": len(ack_rows),
            "tx_events": len(tx_rows),
            "delivered": exit_delivered,
            "recover_fs": recover_fs,
            "prr_out": exit_prr_out,
            "tx_total": tx_total,
            "repair_tx": repair_tx,
            "new_tx": new_tx,
            "first_tx_delay_ns": first_tx_ns - loss_ns if first_tx_ns else 0,
            "first_new_delay_ns": first_new_ns - loss_ns if first_new_ns else 0,
            "half_tx_delay_ns": half_tx_delay_ns,
            "last_tx_to_exit_ns": exit_ns - last_tx_ns if last_tx_ns else 0,
            "tx_gap_median_ns": median_int(tx_gaps),
        }
    )

if prr_delivered_mismatches != 0:
    raise SystemExit(
        f"PRRDelivered accounting mismatch in {prr_delivered_mismatches} episodes"
    )

# Build Linux recovery episodes from retransmission transitions, then expand each
# transition to the surrounding contiguous non-Open CA-state interval.  Deltas
# are measured from the last Open sample before recovery through the first Open
# sample after recovery so the transition retransmission itself is included.
transitions = []
prior_retrans = int(linux_rows[0]["total_retrans"])
for index, row in enumerate(linux_rows[1:], start=1):
    current = int(row["total_retrans"])
    if current > prior_retrans:
        for _ in range(current - prior_retrans):
            transitions.append(index)
    prior_retrans = current

if len(transitions) != expected:
    raise SystemExit(
        f"Linux high-resolution trace recovery count mismatch: "
        f"transitions={len(transitions)} expected={expected}"
    )

linux_episodes = []
for episode, index in enumerate(transitions, start=1):
    start = index
    while start > 0 and int(linux_rows[start - 1]["ca_state"]) != TCP_CA_OPEN:
        start -= 1

    end = index + 1
    while end < len(linux_rows) and int(linux_rows[end]["ca_state"]) != TCP_CA_OPEN:
        end += 1
    if end >= len(linux_rows):
        raise SystemExit(f"Linux recovery episode {episode} has no Open-state exit")

    base_index = start - 1 if start > 0 else start
    base = linux_rows[base_index]
    end_row = linux_rows[end]
    window = linux_rows[start : end + 1]

    base_sent = int(base["bytes_sent"])
    base_retrans = int(base["bytes_retrans"])
    base_acked = int(base["bytes_acked"])
    sent_delta = max(0, int(end_row["bytes_sent"]) - base_sent)
    retrans_delta = max(0, int(end_row["bytes_retrans"]) - base_retrans)
    new_delta = max(0, sent_delta - retrans_delta)
    acked_delta = max(0, int(end_row["bytes_acked"]) - base_acked)

    start_ns = int(linux_rows[start]["elapsed_ns"])
    end_ns = int(end_row["elapsed_ns"])

    send_progress = [
        row for row in window if int(row["bytes_sent"]) > base_sent
    ]
    first_send_delay_ns = (
        int(send_progress[0]["elapsed_ns"]) - start_ns if send_progress else 0
    )
    half_send_delay_ns = first_crossing_ns(
        window, "bytes_sent", base_sent, (sent_delta + 1) // 2, start_ns
    )
    new_target = (new_delta + 1) // 2
    # bytes_sent includes retransmissions, so use an episode-local derived
    # new-data counter when locating the halfway point for new data.
    half_new_delay_ns = 0
    if new_target > 0:
        for row in window:
            sent = max(0, int(row["bytes_sent"]) - base_sent)
            retrans = max(0, int(row["bytes_retrans"]) - base_retrans)
            new_now = max(0, sent - retrans)
            if new_now >= new_target:
                half_new_delay_ns = int(row["elapsed_ns"]) - start_ns
                break

    last_progress_ns = 0
    prior_sent = base_sent
    for row in window:
        current_sent = int(row["bytes_sent"])
        if current_sent > prior_sent:
            last_progress_ns = int(row["elapsed_ns"])
        prior_sent = max(prior_sent, current_sent)

    linux_episodes.append(
        {
            "episode": episode,
            "recovery_ns": max(0, end_ns - start_ns),
            "samples": len(window),
            "acked": acked_delta,
            "tx_total": sent_delta,
            "repair_tx": retrans_delta,
            "new_tx": new_delta,
            "first_tx_delay_ns": first_send_delay_ns,
            "half_tx_delay_ns": half_send_delay_ns,
            "half_new_delay_ns": half_new_delay_ns,
            "last_tx_to_exit_ns": max(0, end_ns - last_progress_ns)
            if last_progress_ns
            else 0,
            "start_cwnd": int(linux_rows[start]["snd_cwnd"]),
            "start_ssthresh": int(linux_rows[start]["snd_ssthresh"]),
        }
    )

steady_ts = ts_episodes[8:]
steady_linux = linux_episodes[8:]

episode_new_ratios = [
    ratio(ts_row["new_tx"], linux_row["new_tx"])
    for ts_row, linux_row in zip(ts_episodes, linux_episodes)
    if linux_row["new_tx"] > 0
]
steady_new_ratios = [
    ratio(ts_row["new_tx"], linux_row["new_tx"])
    for ts_row, linux_row in zip(steady_ts, steady_linux)
    if linux_row["new_tx"] > 0
]
steady_total_ratios = [
    ratio(ts_row["tx_total"], linux_row["tx_total"])
    for ts_row, linux_row in zip(steady_ts, steady_linux)
    if linux_row["tx_total"] > 0
]

print(
    "p6_prr_recovery_byte_trajectory=ok "
    f"episodes={expected} "
    f"tcp_shift_prr_delivered_mismatches={prr_delivered_mismatches} "
    f"tcp_shift_recovery_median_ms={median_int([x['recovery_ns'] for x in ts_episodes]) / 1e6:.3f} "
    f"linux_recovery_median_ms={median_int([x['recovery_ns'] for x in linux_episodes]) / 1e6:.3f} "
    f"tcp_shift_tx_total_median_bytes={median_int([x['tx_total'] for x in ts_episodes])} "
    f"linux_tx_total_median_bytes={median_int([x['tx_total'] for x in linux_episodes])} "
    f"tcp_shift_repair_median_bytes={median_int([x['repair_tx'] for x in ts_episodes])} "
    f"linux_repair_median_bytes={median_int([x['repair_tx'] for x in linux_episodes])} "
    f"tcp_shift_new_median_bytes={median_int([x['new_tx'] for x in ts_episodes])} "
    f"linux_new_median_bytes={median_int([x['new_tx'] for x in linux_episodes])} "
    f"tcp_shift_new_over_linux_episode_median={median_float(episode_new_ratios):.6f} "
    f"tcp_shift_first_tx_delay_median_ms={median_int([x['first_tx_delay_ns'] for x in ts_episodes]) / 1e6:.3f} "
    f"linux_first_tx_delay_median_ms={median_int([x['first_tx_delay_ns'] for x in linux_episodes]) / 1e6:.3f} "
    f"tcp_shift_half_tx_delay_median_ms={median_int([x['half_tx_delay_ns'] for x in ts_episodes]) / 1e6:.3f} "
    f"linux_half_tx_delay_median_ms={median_int([x['half_tx_delay_ns'] for x in linux_episodes]) / 1e6:.3f} "
    f"tcp_shift_last_tx_to_exit_median_ms={median_int([x['last_tx_to_exit_ns'] for x in ts_episodes]) / 1e6:.3f} "
    f"linux_last_tx_to_exit_median_ms={median_int([x['last_tx_to_exit_ns'] for x in linux_episodes]) / 1e6:.3f}"
)

for ts_row, linux_row in list(zip(ts_episodes, linux_episodes))[:10]:
    print(
        f"episode={ts_row['episode']} "
        f"ts_recovery_ms={ts_row['recovery_ns'] / 1e6:.3f} "
        f"ts_acks={ts_row['acks']} ts_tx_events={ts_row['tx_events']} "
        f"ts_recover_fs={ts_row['recover_fs']} "
        f"ts_delivered={ts_row['delivered']} ts_prr_out={ts_row['prr_out']} "
        f"ts_tx={ts_row['tx_total']} ts_repair={ts_row['repair_tx']} "
        f"ts_new={ts_row['new_tx']} "
        f"ts_first_tx_ms={ts_row['first_tx_delay_ns'] / 1e6:.3f} "
        f"ts_first_new_ms={ts_row['first_new_delay_ns'] / 1e6:.3f} "
        f"ts_half_tx_ms={ts_row['half_tx_delay_ns'] / 1e6:.3f} "
        f"ts_last_to_exit_ms={ts_row['last_tx_to_exit_ns'] / 1e6:.3f} "
        f"linux_recovery_ms={linux_row['recovery_ns'] / 1e6:.3f} "
        f"linux_samples={linux_row['samples']} linux_acked={linux_row['acked']} "
        f"linux_tx={linux_row['tx_total']} linux_repair={linux_row['repair_tx']} "
        f"linux_new={linux_row['new_tx']} "
        f"linux_first_tx_ms={linux_row['first_tx_delay_ns'] / 1e6:.3f} "
        f"linux_half_tx_ms={linux_row['half_tx_delay_ns'] / 1e6:.3f} "
        f"linux_half_new_ms={linux_row['half_new_delay_ns'] / 1e6:.3f} "
        f"linux_last_to_exit_ms={linux_row['last_tx_to_exit_ns'] / 1e6:.3f}"
    )

print(
    "steady_state="
    f"episodes={len(steady_ts)} "
    f"ts_recover_fs_median={median_int([x['recover_fs'] for x in steady_ts])} "
    f"ts_delivered_median={median_int([x['delivered'] for x in steady_ts])} "
    f"ts_prr_out_median={median_int([x['prr_out'] for x in steady_ts])} "
    f"ts_tx_median={median_int([x['tx_total'] for x in steady_ts])} "
    f"linux_tx_median={median_int([x['tx_total'] for x in steady_linux])} "
    f"ts_repair_median={median_int([x['repair_tx'] for x in steady_ts])} "
    f"linux_repair_median={median_int([x['repair_tx'] for x in steady_linux])} "
    f"ts_new_median={median_int([x['new_tx'] for x in steady_ts])} "
    f"linux_new_median={median_int([x['new_tx'] for x in steady_linux])} "
    f"ts_new_over_linux_episode_median={median_float(steady_new_ratios):.6f} "
    f"ts_total_over_linux_episode_median={median_float(steady_total_ratios):.6f} "
    f"ts_first_tx_delay_median_ms={median_int([x['first_tx_delay_ns'] for x in steady_ts]) / 1e6:.3f} "
    f"linux_first_tx_delay_median_ms={median_int([x['first_tx_delay_ns'] for x in steady_linux]) / 1e6:.3f} "
    f"ts_half_tx_delay_median_ms={median_int([x['half_tx_delay_ns'] for x in steady_ts]) / 1e6:.3f} "
    f"linux_half_tx_delay_median_ms={median_int([x['half_tx_delay_ns'] for x in steady_linux]) / 1e6:.3f} "
    f"ts_last_tx_to_exit_median_ms={median_int([x['last_tx_to_exit_ns'] for x in steady_ts]) / 1e6:.3f} "
    f"linux_last_tx_to_exit_median_ms={median_int([x['last_tx_to_exit_ns'] for x in steady_linux]) / 1e6:.3f}"
)
print("tcp_shift_summary=" + ts_text)
print("linux_summary=" + linux_text)
