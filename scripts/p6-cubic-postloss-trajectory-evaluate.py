#!/usr/bin/env python3
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

MSS = 1460
APP_LIMITED = 0x02
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
        "unacked",
        "sacked",
        "lost",
        "retrans",
        "rtt_us",
        "snd_cwnd",
        "snd_ssthresh",
        "pacing_rate_Bps",
        "bytes_acked",
        "delivered",
        "bytes_sent",
        "bytes_retrans",
        "total_retrans",
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


def pct(part, whole):
    return float(part) / float(whole) if whole else 0.0


def episode_num(row, key="episode"):
    return int(row.get(key, "0"))


if len(sys.argv) != 5:
    raise SystemExit(
        "usage: p6-cubic-postloss-trajectory-evaluate.py "
        "<tcp-shift-runtime.stderr> <tcp-shift-summary> "
        "<linux-tcp-info-trace.tsv> <linux-summary>"
    )

trace_path, ts_summary_path, linux_trace_path, linux_summary_path = sys.argv[1:]
ts_text, ts = read_summary(ts_summary_path)
linux_text, linux = read_summary(linux_summary_path)
events = parse_tcp_shift(trace_path)
linux_rows = read_linux_trace(linux_trace_path)

losses = events["loss"]
prr_acks = events["prr-ack"]
prr_txs = events["prr-tx"]
prr_exits = events["prr-exit"]
ca_acks = events["ca-ack"]

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
    raise SystemExit("Linux retransmit count no longer matches deterministic faults")
if len(losses) != expected:
    raise SystemExit(f"loss trace mismatch: trace={len(losses)} expected={expected}")
if len(prr_exits) != expected:
    raise SystemExit(
        f"PRR exit trace mismatch: trace={len(prr_exits)} expected={expected}"
    )

acks_by_episode = defaultdict(list)
txs_by_episode = defaultdict(list)
exits_by_episode = {}
ca_by_episode = defaultdict(list)
for row in prr_acks:
    acks_by_episode[episode_num(row)].append(row)
for row in prr_txs:
    txs_by_episode[episode_num(row)].append(row)
for row in prr_exits:
    exits_by_episode[episode_num(row)] = row
for row in ca_acks:
    ca_by_episode[episode_num(row)].append(row)

recovery_ns = []
post_exit_to_first_ca_ns = []
ca_total = 0
ca_app_limited = 0
ca_no_growth = 0
ca_app_limited_no_growth = 0
episodes_with_ca = 0
episode_rows = []

for episode in range(1, expected + 1):
    loss = losses[episode - 1]
    exit_row = exits_by_episode.get(episode)
    if exit_row is None:
        raise SystemExit(f"missing PRR exit for episode {episode}")

    loss_time = int(loss["time_ns"])
    exit_time = int(exit_row["time_ns"])
    if exit_time < loss_time:
        raise SystemExit(f"episode {episode} PRR exit precedes loss")
    recovery_ns.append(exit_time - loss_time)

    rows = ca_by_episode.get(episode, [])
    ca_total += len(rows)
    app_count = sum(
        1 for row in rows if int(row.get("rate_flags", "0")) & APP_LIMITED
    )
    no_growth = sum(
        1 for row in rows if int(row["post_cwnd"]) == int(row["pre_cwnd"])
    )
    app_no_growth = sum(
        1
        for row in rows
        if (int(row.get("rate_flags", "0")) & APP_LIMITED)
        and int(row["post_cwnd"]) == int(row["pre_cwnd"])
    )
    ca_app_limited += app_count
    ca_no_growth += no_growth
    ca_app_limited_no_growth += app_no_growth

    first_ca = rows[0] if rows else None
    last_ca = rows[-1] if rows else None
    if first_ca is not None:
        episodes_with_ca += 1
        first_time = int(first_ca["time_ns"])
        if first_time >= exit_time:
            post_exit_to_first_ca_ns.append(first_time - exit_time)

    episode_rows.append(
        {
            "episode": episode,
            "loss_pre": int(loss["pre_cwnd"]),
            "loss_post": int(loss["post_cwnd"]),
            "loss_inflight": int(loss["inflight_bytes"]),
            "exit_cwnd": int(exit_row["cwnd"]),
            "exit_inflight": int(exit_row.get("inflight_bytes", "0")),
            "recovery_ns": exit_time - loss_time,
            "prr_acks": len(acks_by_episode.get(episode, [])),
            "prr_txs": len(txs_by_episode.get(episode, [])),
            "ca_acks": len(rows),
            "ca_app": app_count,
            "ca_no_growth": no_growth,
            "first_ca_cwnd": int(first_ca["post_cwnd"]) if first_ca else 0,
            "first_ca_flags": int(first_ca["rate_flags"]) if first_ca else 0,
            "first_ca_epoch": int(first_ca["epoch_active"]) if first_ca else 0,
            "first_ca_paused": int(first_ca["app_limited_paused"]) if first_ca else 0,
            "last_ca_cwnd": int(last_ca["post_cwnd"]) if last_ca else 0,
        }
    )

linux_state_counts = Counter(int(row["ca_state"]) for row in linux_rows)
linux_transitions = []
prior_retrans = int(linux_rows[0]["total_retrans"])
for index, row in enumerate(linux_rows[1:], start=1):
    current = int(row["total_retrans"])
    if current > prior_retrans:
        for episode in range(prior_retrans + 1, current + 1):
            linux_transitions.append((episode, index))
    prior_retrans = current

linux_by_episode = {}
for position, (episode, index) in enumerate(linux_transitions):
    next_index = (
        linux_transitions[position + 1][1]
        if position + 1 < len(linux_transitions)
        else len(linux_rows)
    )
    before = linux_rows[index - 1] if index > 0 else linux_rows[index]
    transition = linux_rows[index]
    window = linux_rows[index:next_index]
    recovery_rows = [row for row in window if int(row["ca_state"]) != TCP_CA_OPEN]
    open_after = next(
        (row for row in window[1:] if int(row["ca_state"]) == TCP_CA_OPEN),
        None,
    )
    last = window[-1] if window else transition
    linux_by_episode[episode] = {
        "before_cwnd": int(before["snd_cwnd"]) * MSS,
        "transition_cwnd": int(transition["snd_cwnd"]) * MSS,
        "transition_state": int(transition["ca_state"]),
        "transition_inflight": max(
            0,
            int(transition["unacked"])
            - int(transition["sacked"])
            - int(transition["lost"])
            + int(transition["retrans"]),
        )
        * MSS,
        "transition_pacing": int(transition["pacing_rate_Bps"]),
        "open_cwnd": int(open_after["snd_cwnd"]) * MSS if open_after else 0,
        "open_elapsed_ns": int(open_after["elapsed_ns"]) if open_after else 0,
        "transition_elapsed_ns": int(transition["elapsed_ns"]),
        "last_cwnd": int(last["snd_cwnd"]) * MSS,
        "recovery_samples": len(recovery_rows),
        "window_samples": len(window),
    }

linux_observed_episodes = len(linux_by_episode)
linux_recovery_episodes = sum(
    1 for row in linux_by_episode.values() if row["recovery_samples"] > 0
)
linux_open_after_episodes = sum(
    1 for row in linux_by_episode.values() if row["open_cwnd"] > 0
)

first = episode_rows[0]
first_linux = linux_by_episode.get(1, {})

print(
    "p6_cubic_postloss_trajectory=ok "
    f"tcp_shift_episodes={expected} "
    f"tcp_shift_ca_episodes={episodes_with_ca} "
    f"tcp_shift_ca_ack_events={ca_total} "
    f"tcp_shift_ca_app_limited_events={ca_app_limited} "
    f"tcp_shift_ca_app_limited_fraction={pct(ca_app_limited, ca_total):.6f} "
    f"tcp_shift_ca_no_growth_events={ca_no_growth} "
    f"tcp_shift_ca_app_limited_no_growth_events={ca_app_limited_no_growth} "
    f"tcp_shift_recovery_median_ms={median_int(recovery_ns) / 1_000_000:.3f} "
    f"tcp_shift_exit_to_first_ca_median_ms="
    f"{median_int(post_exit_to_first_ca_ns) / 1_000_000:.3f} "
    f"tcp_shift_first_loss_pre_cwnd_bytes={first['loss_pre']} "
    f"tcp_shift_first_loss_post_cwnd_bytes={first['loss_post']} "
    f"tcp_shift_first_exit_cwnd_bytes={first['exit_cwnd']} "
    f"tcp_shift_first_ca_cwnd_bytes={first['first_ca_cwnd']} "
    f"tcp_shift_first_ca_flags={first['first_ca_flags']} "
    f"linux_trace_samples={len(linux_rows)} "
    f"linux_retrans_transition_episodes={linux_observed_episodes} "
    f"linux_recovery_state_episodes={linux_recovery_episodes} "
    f"linux_open_after_recovery_episodes={linux_open_after_episodes} "
    f"linux_ca_open_samples={linux_state_counts[TCP_CA_OPEN]} "
    f"linux_ca_nonopen_samples={len(linux_rows) - linux_state_counts[TCP_CA_OPEN]} "
    f"linux_first_pre_retrans_cwnd_bytes={first_linux.get('before_cwnd', 0)} "
    f"linux_first_transition_cwnd_bytes={first_linux.get('transition_cwnd', 0)} "
    f"linux_first_open_cwnd_bytes={first_linux.get('open_cwnd', 0)}"
)

for row in episode_rows[:8]:
    linux_row = linux_by_episode.get(row["episode"], {})
    print(
        "episode="
        f"{row['episode']} "
        f"ts_loss_pre={row['loss_pre']} "
        f"ts_loss_post={row['loss_post']} "
        f"ts_loss_inflight={row['loss_inflight']} "
        f"ts_exit_cwnd={row['exit_cwnd']} "
        f"ts_exit_inflight={row['exit_inflight']} "
        f"ts_recovery_ms={row['recovery_ns'] / 1_000_000:.3f} "
        f"ts_prr_acks={row['prr_acks']} "
        f"ts_prr_txs={row['prr_txs']} "
        f"ts_ca_acks={row['ca_acks']} "
        f"ts_ca_app={row['ca_app']} "
        f"ts_ca_no_growth={row['ca_no_growth']} "
        f"ts_first_ca_cwnd={row['first_ca_cwnd']} "
        f"ts_first_ca_flags={row['first_ca_flags']} "
        f"ts_first_ca_epoch={row['first_ca_epoch']} "
        f"ts_first_ca_paused={row['first_ca_paused']} "
        f"ts_last_ca_cwnd={row['last_ca_cwnd']} "
        f"linux_pre={linux_row.get('before_cwnd', 0)} "
        f"linux_transition={linux_row.get('transition_cwnd', 0)} "
        f"linux_state={linux_row.get('transition_state', -1)} "
        f"linux_inflight={linux_row.get('transition_inflight', 0)} "
        f"linux_pacing={linux_row.get('transition_pacing', 0)} "
        f"linux_recovery_samples={linux_row.get('recovery_samples', 0)} "
        f"linux_open={linux_row.get('open_cwnd', 0)} "
        f"linux_last={linux_row.get('last_cwnd', 0)}"
    )

print(
    "linux_ca_state_counts="
    + ",".join(f"{key}:{linux_state_counts[key]}" for key in sorted(linux_state_counts))
)
print("tcp_shift_summary=" + ts_text)
print("linux_summary=" + linux_text)
