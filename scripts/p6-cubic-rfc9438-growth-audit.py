#!/usr/bin/env python3
import statistics
import sys
from collections import defaultdict
from pathlib import Path

Q_SHIFT = 16
Q_ONE = 1 << Q_SHIFT
TIME_Q_SHIFT = 10
NSEC_PER_SEC = 1_000_000_000
CUBIC_SCALE = 40960
RENO_ALPHA_NUM = 9
RENO_ALPHA_DEN = 17
FAST_CONV_NUM = 17
FAST_CONV_DEN = 20


def kv_fields(text):
    fields = {}
    for token in text.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


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


def time_q10(ns):
    seconds, remainder = divmod(ns, NSEC_PER_SEC)
    return (seconds << TIME_Q_SHIFT) + (
        (remainder << TIME_Q_SHIFT) // NSEC_PER_SEC
    )


def nonzero_time_q10(ns):
    value = time_q10(ns)
    return 1 if ns != 0 and value == 0 else value


def cubic_window_q16(w_max_q16, k_q10, t_q10):
    if t_q10 >= k_q10:
        distance = t_q10 - k_q10
        return w_max_q16 + (distance ** 3) // CUBIC_SCALE
    distance = k_q10 - t_q10
    term = (distance ** 3) // CUBIC_SCALE
    return max(0, w_max_q16 - term)


def icbrt_floor(value):
    low, high = 0, 1
    while high ** 3 <= value:
        high *= 2
    while low + 1 < high:
        middle = (low + high) // 2
        if middle ** 3 <= value:
            low = middle
        else:
            high = middle
    return low


def median_int(values):
    return int(statistics.median(values)) if values else 0


def ratio(num, den):
    return float(num) / float(den) if den else 0.0


if len(sys.argv) != 2:
    raise SystemExit(
        "usage: p6-cubic-rfc9438-growth-audit.py <tcp-shift-runtime.stderr>"
    )

events = parse_trace(sys.argv[1])
acks = events["recovery-close-ack"] + events["ca-ack"]
acks.sort(key=lambda row: int(row["time_ns"]))
losses = events["loss"]

if not acks or not losses:
    raise SystemExit("missing post-loss CUBIC ACK/loss trace")

required_ack = {
    "episode",
    "time_ns",
    "acked_bytes",
    "pre_cwnd",
    "post_cwnd",
    "srtt_ns",
    "pre_epoch_active",
    "epoch_active",
    "pre_epoch_start_ns",
    "epoch_start_ns",
    "pre_cwnd_q16",
    "cwnd_q16",
    "pre_w_max_q16",
    "w_max_q16",
    "pre_w_est_q16",
    "w_est_q16",
    "pre_cwnd_prior_q16",
    "cwnd_prior_q16",
    "pre_cwnd_epoch_q16",
    "cwnd_epoch_q16",
    "pre_k_q10",
    "k_q10",
    "last_target_q16",
}
for row in acks:
    missing = sorted(required_ack.difference(row))
    if missing:
        raise SystemExit("CUBIC ACK trace missing fields: " + ",".join(missing))

w_est_mismatches = 0
k_mismatches = 0
epoch_start_mismatches = 0
cwnd_mismatches = 0
target_mismatches = 0
reno_events = 0
cubic_events = 0
alpha_cubic_events = 0
alpha_one_events = 0
epoch_starts = 0
episode_rows = defaultdict(lambda: {
    "acks": 0,
    "reno": 0,
    "cubic": 0,
    "alpha_cubic": 0,
    "alpha_one": 0,
    "growth_bytes": 0,
    "first_wmax": 0,
    "first_k": 0,
    "last_cwnd": 0,
})

for row in acks:
    episode = int(row["episode"])
    now_ns = int(row["time_ns"])
    acked_bytes = int(row["acked_bytes"])
    pre_cwnd_bytes = int(row["pre_cwnd"])
    post_cwnd_bytes = int(row["post_cwnd"])
    srtt_ns = int(row["srtt_ns"])
    pre_epoch_active = int(row["pre_epoch_active"])
    epoch_active = int(row["epoch_active"])
    pre_epoch_start_ns = int(row["pre_epoch_start_ns"])
    epoch_start_ns = int(row["epoch_start_ns"])
    pre_cwnd_q16 = int(row["pre_cwnd_q16"])
    post_cwnd_q16 = int(row["cwnd_q16"])
    pre_w_est = int(row["pre_w_est_q16"])
    post_w_est = int(row["w_est_q16"])
    pre_prior = int(row["pre_cwnd_prior_q16"])
    post_prior = int(row["cwnd_prior_q16"])
    post_epoch_cwnd = int(row["cwnd_epoch_q16"])
    post_w_max = int(row["w_max_q16"])
    post_k = int(row["k_q10"])
    last_target = int(row["last_target_q16"])

    if pre_epoch_active == 0:
        epoch_starts += 1
        base_w_est = pre_cwnd_q16
        expected_epoch_cwnd = pre_cwnd_q16
        expected_epoch_start = now_ns
        expected_k = (
            icbrt_floor((post_w_max - pre_cwnd_q16) * CUBIC_SCALE)
            if post_w_max > pre_cwnd_q16
            else 0
        )
        if epoch_active != 1 or post_epoch_cwnd != expected_epoch_cwnd:
            epoch_start_mismatches += 1
        if epoch_start_ns != expected_epoch_start:
            epoch_start_mismatches += 1
        if post_k != expected_k:
            k_mismatches += 1
    else:
        base_w_est = pre_w_est
        if epoch_start_ns != pre_epoch_start_ns:
            epoch_start_mismatches += 1

    increment = (acked_bytes << Q_SHIFT) // pre_cwnd_bytes
    if base_w_est < pre_prior:
        increment = (increment * RENO_ALPHA_NUM) // RENO_ALPHA_DEN
        alpha_cubic_events += 1
        alpha_mode = "9/17"
    else:
        alpha_one_events += 1
        alpha_mode = "1"
    expected_w_est = base_w_est + increment
    if post_w_est != expected_w_est:
        w_est_mismatches += 1

    elapsed_ns = now_ns - epoch_start_ns
    if elapsed_ns < 0:
        raise SystemExit("negative CUBIC epoch elapsed time")
    elapsed_q10 = time_q10(elapsed_ns)
    current_cubic = cubic_window_q16(post_w_max, post_k, elapsed_q10)

    if current_cubic < post_w_est:
        reno_events += 1
        expected_target = post_w_est
        expected_cwnd = max(pre_cwnd_q16, post_w_est)
        branch = "reno"
    else:
        cubic_events += 1
        rtt_q10 = nonzero_time_q10(srtt_ns)
        target = cubic_window_q16(post_w_max, post_k, elapsed_q10 + rtt_q10)
        upper = pre_cwnd_q16 + pre_cwnd_q16 // 2
        target = max(target, pre_cwnd_q16)
        target = min(target, upper)
        expected_target = target
        difference = target - pre_cwnd_q16
        increment_q16 = (
            (difference << Q_SHIFT) // pre_cwnd_q16
            if difference and pre_cwnd_q16
            else 0
        )
        expected_cwnd = pre_cwnd_q16 + increment_q16
        branch = "cubic"

    if last_target != expected_target:
        target_mismatches += 1
    if post_cwnd_q16 != expected_cwnd:
        cwnd_mismatches += 1

    info = episode_rows[episode]
    info["acks"] += 1
    info[branch] += 1
    info["alpha_cubic" if alpha_mode == "9/17" else "alpha_one"] += 1
    info["growth_bytes"] += max(0, post_cwnd_bytes - pre_cwnd_bytes)
    if info["first_wmax"] == 0:
        info["first_wmax"] = post_w_max
        info["first_k"] = post_k
    info["last_cwnd"] = post_cwnd_bytes

fast_convergence_events = 0
fast_convergence_mismatches = 0
for row in losses:
    for required in (
        "pre_cwnd_q16",
        "pre_w_max_q16",
        "post_w_max_q16",
        "post_cwnd_prior_q16",
        "post_k_q10",
        "fast_convergence",
        "fast_convergence_applied",
    ):
        if required not in row:
            raise SystemExit(f"loss trace missing {required}")
    pre_cwnd = int(row["pre_cwnd_q16"])
    pre_wmax = int(row["pre_w_max_q16"])
    post_wmax = int(row["post_w_max_q16"])
    applied = int(row["fast_convergence_applied"])
    enabled = int(row["fast_convergence"])
    expected_applied = 1 if enabled and pre_wmax and pre_cwnd < pre_wmax else 0
    expected_wmax = (
        (pre_cwnd * FAST_CONV_NUM) // FAST_CONV_DEN
        if expected_applied
        else pre_cwnd
    )
    if applied:
        fast_convergence_events += 1
    if (
        applied != expected_applied
        or post_wmax != expected_wmax
        or int(row["post_cwnd_prior_q16"]) != pre_cwnd
        or int(row["post_k_q10"]) != 0
    ):
        fast_convergence_mismatches += 1

if any((
    w_est_mismatches,
    k_mismatches,
    epoch_start_mismatches,
    cwnd_mismatches,
    target_mismatches,
    fast_convergence_mismatches,
)):
    raise SystemExit(
        "RFC 9438 fixed-point audit mismatch: "
        f"w_est={w_est_mismatches} k={k_mismatches} "
        f"epoch={epoch_start_mismatches} cwnd={cwnd_mismatches} "
        f"target={target_mismatches} fast_conv={fast_convergence_mismatches}"
    )

episodes = sorted(episode_rows)
steady = [episode_rows[i] for i in episodes if i >= 9]
total = reno_events + cubic_events

print(
    "p6_cubic_rfc9438_growth_audit=ok "
    f"post_loss_ack_events={len(acks)} "
    f"epoch_starts={epoch_starts} "
    f"reno_friendly_events={reno_events} "
    f"cubic_function_events={cubic_events} "
    f"reno_friendly_fraction={ratio(reno_events, total):.6f} "
    f"alpha_9_17_events={alpha_cubic_events} "
    f"alpha_1_events={alpha_one_events} "
    f"fast_convergence_loss_events={fast_convergence_events} "
    f"w_est_mismatches={w_est_mismatches} "
    f"k_mismatches={k_mismatches} "
    f"epoch_mismatches={epoch_start_mismatches} "
    f"target_mismatches={target_mismatches} "
    f"cwnd_mismatches={cwnd_mismatches} "
    f"fast_convergence_mismatches={fast_convergence_mismatches} "
    f"steady_reno_fraction="
    f"{ratio(sum(x['reno'] for x in steady), sum(x['acks'] for x in steady)):.6f} "
    f"steady_growth_median_bytes="
    f"{median_int([x['growth_bytes'] for x in steady])}"
)

for episode in episodes[:12]:
    row = episode_rows[episode]
    print(
        f"episode={episode} "
        f"acks={row['acks']} reno={row['reno']} cubic={row['cubic']} "
        f"alpha_9_17={row['alpha_cubic']} alpha_1={row['alpha_one']} "
        f"growth_bytes={row['growth_bytes']} "
        f"wmax_q16={row['first_wmax']} k_q10={row['first_k']} "
        f"last_cwnd_bytes={row['last_cwnd']}"
    )

print(
    "steady_state="
    f"episodes={len(steady)} "
    f"acks={sum(x['acks'] for x in steady)} "
    f"reno={sum(x['reno'] for x in steady)} "
    f"cubic={sum(x['cubic'] for x in steady)} "
    f"alpha_9_17={sum(x['alpha_cubic'] for x in steady)} "
    f"alpha_1={sum(x['alpha_one'] for x in steady)} "
    f"growth_median_bytes={median_int([x['growth_bytes'] for x in steady])}"
)
