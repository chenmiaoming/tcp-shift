#!/usr/bin/env python3
import statistics
import sys
from pathlib import Path

RFC_BETA_NUM = 7
RFC_BETA_DEN = 10
LINUX_BETA_NUM = 717
LINUX_BETA_DEN = 1024


def kv_fields(text):
    fields = {}
    for token in text.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def parse_losses(path):
    marker = "tcp-shift-cubic-trace: "
    losses = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        fields = kv_fields(raw.split(marker, 1)[1])
        if fields.get("event") == "loss":
            losses.append(fields)
    return losses


def read_linux_trace(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing Linux TCP_INFO trace")
    header = lines[0].split("\t")
    required = {
        "total_retrans", "snd_cwnd", "snd_ssthresh", "unacked",
        "sacked", "lost", "retrans", "ca_state",
    }
    missing = sorted(required.difference(header))
    if missing:
        raise SystemExit("Linux trace missing fields: " + ",".join(missing))
    return [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]


def median_int(values):
    return int(statistics.median(values)) if values else 0


if len(sys.argv) != 3:
    raise SystemExit(
        "usage: p6-cubic-loss-reduction-audit.py "
        "<tcp-shift-runtime.stderr> <linux-tcp-info-trace.tsv>"
    )

losses = parse_losses(sys.argv[1])
linux_rows = read_linux_trace(sys.argv[2])
if not losses:
    raise SystemExit("missing tcp-shift CUBIC loss trace")

linux_transitions = []
prior = int(linux_rows[0]["total_retrans"])
for index, row in enumerate(linux_rows[1:], start=1):
    current = int(row["total_retrans"])
    if current > prior:
        for episode in range(prior + 1, current + 1):
            linux_transitions.append((episode, index, row))
    prior = current

linux_by_episode = {episode: (index, row) for episode, index, row in linux_transitions}

rfc_mismatches = 0
flight_deficit = []
linux_minus_rfc = []
linux_ssthresh = []
tcp_shift_post = []
episode_rows = []

for episode, row in enumerate(losses, start=1):
    for required in ("mss", "inflight_bytes", "pre_cwnd", "post_cwnd"):
        if required not in row:
            raise SystemExit(f"loss trace missing {required}: episode={episode}")

    mss = int(row["mss"])
    inflight = int(row["inflight_bytes"])
    pre_cwnd = int(row["pre_cwnd"])
    post_cwnd = int(row["post_cwnd"])
    floor = 2 * mss

    rfc_flight_post = max((inflight * RFC_BETA_NUM) // RFC_BETA_DEN, floor)
    cwnd_based_post = max((pre_cwnd * RFC_BETA_NUM) // RFC_BETA_DEN, floor)
    linux_style_post = max(
        (pre_cwnd * LINUX_BETA_NUM) // LINUX_BETA_DEN, floor
    )

    if post_cwnd != rfc_flight_post:
        rfc_mismatches += 1

    flight_gap = max(0, pre_cwnd - inflight)
    semantic_gap = cwnd_based_post - rfc_flight_post
    flight_deficit.append(semantic_gap)
    tcp_shift_post.append(post_cwnd)

    linux_index, linux = linux_by_episode.get(episode, (None, None))
    if linux is not None:
        linux_threshold = int(linux["snd_ssthresh"]) * mss
        linux_ssthresh.append(linux_threshold)
        linux_minus_rfc.append(linux_threshold - post_cwnd)
        linux_pre = int(linux["snd_cwnd"]) * mss
        linux_inflight = max(
            0,
            int(linux["unacked"])
            - int(linux["sacked"])
            - int(linux["lost"])
            + int(linux["retrans"]),
        ) * mss
        linux_state = int(linux["ca_state"])
    else:
        linux_threshold = 0
        linux_pre = 0
        linux_inflight = 0
        linux_state = -1

    episode_rows.append({
        "episode": episode,
        "pre": pre_cwnd,
        "inflight": inflight,
        "flight_gap": flight_gap,
        "rfc": rfc_flight_post,
        "cwnd_beta": cwnd_based_post,
        "linux_style": linux_style_post,
        "semantic_gap": semantic_gap,
        "actual": post_cwnd,
        "linux_pre": linux_pre,
        "linux_inflight": linux_inflight,
        "linux_ssthresh": linux_threshold,
        "linux_state": linux_state,
    })

if rfc_mismatches:
    raise SystemExit(
        f"RFC 9438 FlightSize reduction mismatch count={rfc_mismatches}"
    )

steady = episode_rows[8:]
steady_linux = [row for row in steady if row["linux_ssthresh"] > 0]

print(
    "p6_cubic_loss_reduction_audit=ok "
    f"episodes={len(episode_rows)} "
    f"linux_transition_episodes={len(linux_by_episode)} "
    f"rfc_flightsize_mismatches={rfc_mismatches} "
    f"cwnd_minus_flight_median_bytes="
    f"{median_int([row['flight_gap'] for row in episode_rows])} "
    f"cwnd_based_minus_rfc_post_median_bytes={median_int(flight_deficit)} "
    f"linux_ssthresh_minus_tcp_shift_post_median_bytes="
    f"{median_int(linux_minus_rfc)} "
    f"steady_cwnd_minus_flight_median_bytes="
    f"{median_int([row['flight_gap'] for row in steady])} "
    f"steady_cwnd_based_minus_rfc_post_median_bytes="
    f"{median_int([row['semantic_gap'] for row in steady])} "
    f"steady_linux_ssthresh_minus_tcp_shift_post_median_bytes="
    f"{median_int([row['linux_ssthresh'] - row['actual'] for row in steady_linux])}"
)

for row in episode_rows[:12]:
    print(
        f"episode={row['episode']} "
        f"ts_pre_cwnd={row['pre']} "
        f"ts_flight={row['inflight']} "
        f"ts_cwnd_minus_flight={row['flight_gap']} "
        f"ts_rfc_flight_beta={row['rfc']} "
        f"ts_cwnd_beta_7_10={row['cwnd_beta']} "
        f"ts_linux_style_beta_717_1024={row['linux_style']} "
        f"ts_cwnd_based_minus_rfc={row['semantic_gap']} "
        f"ts_actual_post={row['actual']} "
        f"linux_transition_cwnd={row['linux_pre']} "
        f"linux_transition_inflight={row['linux_inflight']} "
        f"linux_ssthresh={row['linux_ssthresh']} "
        f"linux_state={row['linux_state']}"
    )

print(
    "steady_state="
    f"episodes={len(steady)} "
    f"ts_pre_cwnd_median={median_int([row['pre'] for row in steady])} "
    f"ts_flight_median={median_int([row['inflight'] for row in steady])} "
    f"ts_cwnd_minus_flight_median={median_int([row['flight_gap'] for row in steady])} "
    f"ts_rfc_post_median={median_int([row['rfc'] for row in steady])} "
    f"ts_cwnd_beta_post_median={median_int([row['cwnd_beta'] for row in steady])} "
    f"ts_semantic_gap_median={median_int([row['semantic_gap'] for row in steady])} "
    f"linux_ssthresh_median={median_int([row['linux_ssthresh'] for row in steady_linux])}"
)
