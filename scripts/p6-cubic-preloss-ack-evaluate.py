#!/usr/bin/env python3
import sys
from pathlib import Path

MSS = 1460


def kv_fields(text):
    fields = {}
    for token in text.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def parse_tcp_shift(path):
    acks = []
    losses = []
    marker = "tcp-shift-cubic-trace: "
    cumulative_acked = 0
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if marker not in raw:
            continue
        fields = kv_fields(raw.split(marker, 1)[1])
        event = fields.get("event")
        if event == "preloss-ack":
            cumulative_acked += int(fields["acked_bytes"])
            fields["cumulative_acked_bytes"] = str(cumulative_acked)
            acks.append(fields)
        elif event == "loss":
            losses.append(fields)
    if not acks:
        raise SystemExit("missing tcp-shift pre-loss ACK trace")
    if not losses:
        raise SystemExit("missing tcp-shift CUBIC loss trace")
    return acks, losses


def read_linux(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        raise SystemExit("missing Linux TCP_INFO samples")
    header = lines[0].split("\t")
    required = {
        "source",
        "bytes_acked",
        "delivered",
        "sacked",
        "snd_cwnd",
        "snd_ssthresh",
        "rtt_us",
        "total_retrans",
    }
    missing = sorted(required.difference(header))
    if missing:
        raise SystemExit("Linux TCP_INFO missing fields: " + ",".join(missing))
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]
    if not rows:
        raise SystemExit("empty Linux TCP_INFO sample set")
    baseline = rows[0]
    base_acked = int(baseline["bytes_acked"])
    base_delivered = int(baseline["delivered"])
    for row in rows:
        row["acked_progress_bytes"] = str(max(0, int(row["bytes_acked"]) - base_acked))
        row["delivered_progress_packets"] = str(
            max(0, int(row["delivered"]) - base_delivered)
        )
    pre_retrans = [row for row in rows if int(row["total_retrans"]) == 0]
    if not pre_retrans:
        raise SystemExit("Linux has no pre-retransmission TCP_INFO sample")
    return rows, pre_retrans


def nearest_ack(acks, target):
    return min(
        acks,
        key=lambda row: abs(int(row["cumulative_acked_bytes"]) - target),
    )


def first_counter_event(acks, key):
    for row in acks:
        if int(row.get(key, "0")) > 0:
            return row
    return None


def compact_rounds(acks):
    result = []
    previous = None
    round_index = 0
    for row in acks:
        current = int(row["hystart_next_round_delivered"])
        if current == previous:
            continue
        previous = current
        round_index += 1
        result.append(
            "r%d:ack=%s,deliv=%s,cwnd=%s,next=%s,css=%s"
            % (
                round_index,
                row["cumulative_acked_bytes"],
                row["rate_delivered_total_bytes"],
                row["post_cwnd"],
                row["hystart_next_round_delivered"],
                row["hystart_css"],
            )
        )
    return ";".join(result)


def compact_acks(rows):
    return ";".join(
        "i%s:ack=%s,cum=%s,flags=%s,cwnd=%s>%s,css=%s,en=%s,next=%s"
        % (
            row["index"],
            row["acked_bytes"],
            row["cumulative_acked_bytes"],
            row["rate_flags"],
            row["pre_cwnd"],
            row["post_cwnd"],
            row["hystart_css"],
            row["hystart_enabled"],
            row["hystart_next_round_delivered"],
        )
        for row in rows
    )


if len(sys.argv) != 3:
    raise SystemExit(
        "usage: p6-cubic-preloss-ack-evaluate.py "
        "<tcp-shift-runtime.stderr> <linux-tcp-info.tsv>"
    )

acks, losses = parse_tcp_shift(sys.argv[1])
linux_rows, linux_pre = read_linux(sys.argv[2])

first_loss = losses[0]
linux_last = linux_pre[-1]
linux_ack_progress = int(linux_last["acked_progress_bytes"])
linux_delivered_packets = int(linux_last["delivered_progress_packets"])
match = nearest_ack(acks, linux_ack_progress)

ts_last = acks[-1]
ts_first = acks[0]
ts_match_ack = int(match["cumulative_acked_bytes"])
ts_match_cwnd = int(match["post_cwnd"])
linux_cwnd = int(linux_last["snd_cwnd"]) * MSS
match_gap = linux_cwnd - ts_match_cwnd
match_ack_delta = linux_ack_progress - ts_match_ack

max_ack_credit = max(int(row["acked_bytes"]) for row in acks)
aggregated_acks = sum(1 for row in acks if int(row["acked_bytes"]) > MSS)
round_changes = 0
prior_round = None
for row in acks:
    current = row["hystart_next_round_delivered"]
    if current != prior_round:
        round_changes += 1
        prior_round = current

css_enter = first_counter_event(acks, "hystart_css_enters")
hystart_exit = first_counter_event(acks, "hystart_exits")
linux_initial_cwnd = int(linux_rows[0]["snd_cwnd"]) * MSS
linux_sacked_bytes = int(linux_last["sacked"]) * MSS
linux_delivered_bytes_approx = linux_delivered_packets * MSS

print(
    "p6_cubic_pre_loss_ack=ok "
    f"tcp_shift_pre_loss_ack_events={len(acks)} "
    f"tcp_shift_first_post_cwnd_bytes={int(ts_first['post_cwnd'])} "
    f"tcp_shift_last_post_cwnd_bytes={int(ts_last['post_cwnd'])} "
    f"tcp_shift_cumulative_acked_bytes={int(ts_last['cumulative_acked_bytes'])} "
    f"tcp_shift_last_delivered_total_bytes={int(ts_last['rate_delivered_total_bytes'])} "
    f"tcp_shift_max_ack_credit_bytes={max_ack_credit} "
    f"tcp_shift_aggregated_ack_events={aggregated_acks} "
    f"tcp_shift_hystart_round_transitions={round_changes} "
    f"tcp_shift_first_loss_pre_cwnd_bytes={int(first_loss['pre_cwnd'])} "
    f"tcp_shift_first_loss_sacked_ahead_bytes={int(first_loss['sacked_ahead_bytes'])} "
    f"linux_pre_retrans_samples={len(linux_pre)} "
    f"linux_initial_cwnd_bytes={linux_initial_cwnd} "
    f"linux_pre_retrans_cwnd_bytes={linux_cwnd} "
    f"linux_pre_retrans_acked_progress_bytes={linux_ack_progress} "
    f"linux_pre_retrans_sacked_bytes={linux_sacked_bytes} "
    f"linux_pre_retrans_delivered_packets={linux_delivered_packets} "
    f"linux_pre_retrans_delivered_bytes_approx={linux_delivered_bytes_approx} "
    f"matched_tcp_shift_ack_progress_bytes={ts_match_ack} "
    f"matched_tcp_shift_cwnd_bytes={ts_match_cwnd} "
    f"matched_ack_progress_delta_bytes={match_ack_delta} "
    f"linux_minus_matched_tcp_shift_cwnd_bytes={match_gap}"
)

if css_enter is None:
    print("tcp_shift_hystart_css_enter=none")
else:
    print(
        "tcp_shift_hystart_css_enter="
        f"ack_index:{css_enter['index']},"
        f"acked:{css_enter['cumulative_acked_bytes']},"
        f"delivered:{css_enter['rate_delivered_total_bytes']},"
        f"cwnd:{css_enter['post_cwnd']},"
        f"sample_rtt_ns:{css_enter['sample_rtt_ns']}"
    )

if hystart_exit is None:
    print("tcp_shift_hystart_exit=none")
else:
    print(
        "tcp_shift_hystart_exit="
        f"ack_index:{hystart_exit['index']},"
        f"acked:{hystart_exit['cumulative_acked_bytes']},"
        f"delivered:{hystart_exit['rate_delivered_total_bytes']},"
        f"cwnd:{hystart_exit['post_cwnd']}"
    )

print("tcp_shift_hystart_rounds=" + compact_rounds(acks))
print("tcp_shift_first_acks=" + compact_acks(acks[:15]))
print("tcp_shift_last_acks=" + compact_acks(acks[-10:]))
print(
    "linux_pre_first_retrans="
    f"sample:{linux_last['sample']},"
    f"source:{linux_last['source']},"
    f"acked_progress:{linux_ack_progress},"
    f"delivered_packets:{linux_delivered_packets},"
    f"sacked_packets:{linux_last['sacked']},"
    f"cwnd_packets:{linux_last['snd_cwnd']},"
    f"ssthresh_packets:{linux_last['snd_ssthresh']},"
    f"rtt_us:{linux_last['rtt_us']}"
)
print(
    "matched_tcp_shift_ack="
    f"index:{match['index']},"
    f"acked_progress:{ts_match_ack},"
    f"delivered_total:{match['rate_delivered_total_bytes']},"
    f"sacked_ahead:{match['sacked_ahead_bytes']},"
    f"cwnd:{ts_match_cwnd},"
    f"css:{match['hystart_css']},"
    f"initial_complete:{match['hystart_initial_complete']}"
)
