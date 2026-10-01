#!/usr/bin/env python3
import statistics
import sys
from collections import defaultdict
from pathlib import Path

TCP_CA_OPEN = 0


def kv_fields(text):
    result = {}
    for token in text.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def parse_prr(path):
    rows = []
    marker = "tcp-shift-cubic-trace: event=prr-ack "
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        rows.append(kv_fields(raw.split(marker, 1)[1]))
    if not rows:
        raise SystemExit("missing PRR ACK trace")
    return rows


def read_linux(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing Linux TCP_INFO trace")
    header = lines[0].split("\t")
    required = {"ca_state", "pacing_rate_Bps", "total_retrans", "elapsed_ns"}
    missing = required.difference(header)
    if missing:
        raise SystemExit("Linux trace missing fields: " + ",".join(sorted(missing)))
    return [
        dict(zip(header, line.split("\t")))
        for line in lines[1:]
        if line.strip()
    ]


def median(values):
    return statistics.median(values) if values else 0.0


def ratio(a, b):
    return float(a) / float(b) if b else 0.0


if len(sys.argv) != 3:
    raise SystemExit(
        "usage: p6-cubic-recovery-pacing-publication-evaluate.py "
        "<tcp-shift-runtime.stderr> <linux-tcp-info-trace.tsv>"
    )

rows = parse_prr(sys.argv[1])
linux = read_linux(sys.argv[2])

required = {
    "episode",
    "pacing_Bps",
    "refreshed_actual_inflight_Bps",
    "refreshed_raw_outstanding_Bps",
    "controller_cwnd_q16",
    "cwnd",
    "inflight_bytes",
    "raw_outstanding_bytes",
    "srtt_ns",
}
for row in rows:
    missing = required.difference(row)
    if missing:
        raise SystemExit("PRR trace missing fields: " + ",".join(sorted(missing)))

episodes = defaultdict(list)
for row in rows:
    episodes[int(row["episode"])].append(row)

actual_over_actual = []
actual_over_raw = []
actual_candidate_over_raw = []
episode_actual_unique = []
episode_actual_candidate_unique = []
episode_raw_candidate_unique = []
controller_unique = []
first_last_raw_ratio = []

for episode in sorted(episodes):
    erows = episodes[episode]
    actual = [int(r["pacing_Bps"]) for r in erows]
    actual_candidate = [int(r["refreshed_actual_inflight_Bps"]) for r in erows]
    raw_candidate = [int(r["refreshed_raw_outstanding_Bps"]) for r in erows]
    controller = [int(r["controller_cwnd_q16"]) for r in erows]

    episode_actual_unique.append(len(set(actual)))
    episode_actual_candidate_unique.append(len(set(actual_candidate)))
    episode_raw_candidate_unique.append(len(set(raw_candidate)))
    controller_unique.append(len(set(controller)))

    for a, c, raw in zip(actual, actual_candidate, raw_candidate):
        if c:
            actual_over_actual.append(ratio(a, c))
        if raw:
            actual_over_raw.append(ratio(a, raw))
            actual_candidate_over_raw.append(ratio(c, raw))
    if raw_candidate[0]:
        first_last_raw_ratio.append(ratio(raw_candidate[-1], raw_candidate[0]))

linux_transitions = []
prior_retrans = int(linux[0]["total_retrans"])
for index, row in enumerate(linux[1:], start=1):
    current = int(row["total_retrans"])
    if current > prior_retrans:
        for episode in range(prior_retrans + 1, current + 1):
            linux_transitions.append((episode, index))
    prior_retrans = current

linux_unique = []
linux_first_last = []
linux_nonopen_samples = 0
for pos, (episode, index) in enumerate(linux_transitions):
    next_index = (
        linux_transitions[pos + 1][1]
        if pos + 1 < len(linux_transitions)
        else len(linux)
    )
    window = linux[index:next_index]
    recovery = [r for r in window if int(r["ca_state"]) != TCP_CA_OPEN]
    if not recovery:
        continue
    rates = [int(r["pacing_rate_Bps"]) for r in recovery]
    linux_nonopen_samples += len(rates)
    linux_unique.append(len(set(rates)))
    if rates[0]:
        linux_first_last.append(ratio(rates[-1], rates[0]))

static_episodes = sum(v == 1 for v in episode_actual_unique)
changing_actual_candidates = sum(v > 1 for v in episode_actual_candidate_unique)
changing_raw_candidates = sum(v > 1 for v in episode_raw_candidate_unique)
frozen_controller_episodes = sum(v == 1 for v in controller_unique)
linux_changing = sum(v > 1 for v in linux_unique)

print(
    "p6_cubic_recovery_pacing_publication=ok "
    f"tcp_shift_prr_ack_events={len(rows)} "
    f"tcp_shift_episodes={len(episodes)} "
    f"tcp_shift_static_pacing_episodes={static_episodes} "
    f"tcp_shift_changing_actual_inflight_candidate_episodes="
    f"{changing_actual_candidates} "
    f"tcp_shift_changing_raw_outstanding_candidate_episodes="
    f"{changing_raw_candidates} "
    f"tcp_shift_frozen_controller_cwnd_episodes={frozen_controller_episodes} "
    f"tcp_shift_actual_over_actual_inflight_candidate_median="
    f"{median(actual_over_actual):.6f} "
    f"tcp_shift_actual_over_raw_outstanding_candidate_median="
    f"{median(actual_over_raw):.6f} "
    f"tcp_shift_actual_candidate_over_raw_candidate_median="
    f"{median(actual_candidate_over_raw):.6f} "
    f"tcp_shift_raw_candidate_last_over_first_median="
    f"{median(first_last_raw_ratio):.6f} "
    f"linux_recovery_episodes={len(linux_unique)} "
    f"linux_recovery_nonopen_samples={linux_nonopen_samples} "
    f"linux_changing_pacing_episodes={linux_changing} "
    f"linux_unique_pacing_rates_median={median(linux_unique):.1f} "
    f"linux_pacing_last_over_first_median={median(linux_first_last):.6f}"
)

for episode in sorted(episodes)[:8]:
    erows = episodes[episode]
    first = erows[0]
    last = erows[-1]
    print(
        f"episode={episode} "
        f"acks={len(erows)} "
        f"actual_unique={len(set(int(r['pacing_Bps']) for r in erows))} "
        f"actual_candidate_unique="
        f"{len(set(int(r['refreshed_actual_inflight_Bps']) for r in erows))} "
        f"raw_candidate_unique="
        f"{len(set(int(r['refreshed_raw_outstanding_Bps']) for r in erows))} "
        f"controller_cwnd_unique="
        f"{len(set(int(r['controller_cwnd_q16']) for r in erows))} "
        f"actual={first['pacing_Bps']}>{last['pacing_Bps']} "
        f"actual_candidate="
        f"{first['refreshed_actual_inflight_Bps']}>"
        f"{last['refreshed_actual_inflight_Bps']} "
        f"raw_candidate="
        f"{first['refreshed_raw_outstanding_Bps']}>"
        f"{last['refreshed_raw_outstanding_Bps']} "
        f"prr_cwnd={first['cwnd']}>{last['cwnd']} "
        f"actual_inflight={first['inflight_bytes']}>{last['inflight_bytes']} "
        f"raw_outstanding="
        f"{first['raw_outstanding_bytes']}>{last['raw_outstanding_bytes']} "
        f"srtt_ns={first['srtt_ns']}>{last['srtt_ns']}"
    )
