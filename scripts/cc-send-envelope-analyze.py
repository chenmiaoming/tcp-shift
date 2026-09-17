#!/usr/bin/env python3
import argparse
import math
import re
from pathlib import Path

LENGTH_RE = re.compile(r"\blength\s+(\d+)\s*$")
METRIC_RE = re.compile(r"\b([A-Za-z0-9_]+)=([^\s]+)")


def percentile(values, p):
    if not values:
        return 0
    ordered = sorted(values)
    rank = max(1, math.ceil((p / 100.0) * len(ordered)))
    return ordered[rank - 1]


def parse_timestamp_ns(token):
    if "." not in token:
        return int(token) * 1_000_000_000
    sec, frac = token.split(".", 1)
    frac = (frac + "000000000")[:9]
    return int(sec) * 1_000_000_000 + int(frac)


def parse_trace(path, interface):
    packets = []
    for line in Path(path).read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) < 4 or fields[1] != interface:
            continue
        match = LENGTH_RE.search(line)
        if match is None:
            continue
        payload = int(match.group(1))
        if payload <= 0:
            continue
        try:
            when_ns = parse_timestamp_ns(fields[0])
        except ValueError:
            continue
        packets.append((when_ns, payload))
    packets.sort()
    return packets


def parse_metrics(path):
    metrics = {}
    if not path:
        return metrics
    for line in Path(path).read_text(errors="replace").splitlines():
        for key, value in METRIC_RE.findall(line):
            metrics[key] = value
    return metrics


def summarize(packets, rtt_ms, rate_mbit):
    if not packets:
        raise SystemExit("no tcp-shift data packets matched the requested interface")

    first_ns = packets[0][0]
    rtt_cutoff_ns = first_ns + rtt_ms * 1_000_000
    first_rtt = [item for item in packets if item[0] < rtt_cutoff_ns]
    gaps = [packets[i][0] - packets[i - 1][0] for i in range(1, len(packets))]

    payloads = sorted(payload for _, payload in packets)
    median_payload = payloads[(len(payloads) - 1) // 2]
    serialization_ns = max(1, math.ceil(median_payload * 8_000 / rate_mbit))
    microburst_threshold_ns = max(1, serialization_ns // 4)

    max_burst_packets = 1
    max_burst_bytes = packets[0][1]
    burst_packets = 1
    burst_bytes = packets[0][1]
    microburst_runs = 0
    microburst_packets = 0
    previous_run_packets = 1

    for index in range(1, len(packets)):
        gap = packets[index][0] - packets[index - 1][0]
        payload = packets[index][1]
        if gap <= microburst_threshold_ns:
            burst_packets += 1
            burst_bytes += payload
        else:
            if burst_packets > 1:
                microburst_runs += 1
                microburst_packets += burst_packets
            max_burst_packets = max(max_burst_packets, burst_packets)
            max_burst_bytes = max(max_burst_bytes, burst_bytes)
            burst_packets = 1
            burst_bytes = payload
        previous_run_packets = burst_packets

    if previous_run_packets > 1:
        microburst_runs += 1
        microburst_packets += burst_packets
    max_burst_packets = max(max_burst_packets, burst_packets)
    max_burst_bytes = max(max_burst_bytes, burst_bytes)

    return {
        "packets": len(packets),
        "bytes": sum(payload for _, payload in packets),
        "first_rtt_packets": len(first_rtt),
        "first_rtt_bytes": sum(payload for _, payload in first_rtt),
        "median_payload_bytes": median_payload,
        "path_serialization_ns": serialization_ns,
        "microburst_threshold_ns": microburst_threshold_ns,
        "microburst_runs": microburst_runs,
        "microburst_packets": microburst_packets,
        "max_microburst_packets": max_burst_packets,
        "max_microburst_bytes": max_burst_bytes,
        "gap_min_ns": min(gaps) if gaps else 0,
        "gap_p50_ns": percentile(gaps, 50),
        "gap_p95_ns": percentile(gaps, 95),
        "gap_p99_ns": percentile(gaps, 99),
        "gap_max_ns": max(gaps) if gaps else 0,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Summarize tcp-shift packet timing for pacing-envelope qualification"
    )
    parser.add_argument("--trace", required=True)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--rtt-ms", type=int, required=True)
    parser.add_argument("--rate-mbit", type=int, required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--runtime-stderr")
    parser.add_argument("--benchmark-summary")
    args = parser.parse_args()

    if args.rtt_ms <= 0 or args.rate_mbit <= 0:
        raise SystemExit("rtt-ms and rate-mbit must be positive")

    packets = parse_trace(args.trace, args.interface)
    stats = summarize(packets, args.rtt_ms, args.rate_mbit)
    runtime = parse_metrics(args.runtime_stderr)
    benchmark = parse_metrics(args.benchmark_summary)

    selected_runtime = {
        "loss_events": runtime.get("cc_loss_events", "na"),
        "timeout_events": runtime.get("cc_timeout_events", "na"),
        "last_cwnd_bytes": runtime.get("cc_last_cwnd", "na"),
        "last_ssthresh_bytes": runtime.get("cc_last_ssthresh", "na"),
        "pacing_rate_bytes_per_sec": runtime.get("last_rate_bytes_per_sec", "na"),
        "pacing_deferrals": runtime.get("deferrals", "na"),
        "pacing_tx_events": runtime.get("tx_events", "na"),
        "pacing_scheduler_errors": runtime.get("scheduler_errors", "na"),
        "pacing_max_lateness_ns": runtime.get("max_lateness_ns", "na"),
    }

    print(
        "send_envelope=ok "
        f"mode={args.mode} interface={args.interface} "
        f"rtt_ms={args.rtt_ms} rate_mbit={args.rate_mbit} "
        + " ".join(f"{key}={value}" for key, value in stats.items())
        + " "
        + " ".join(f"{key}={value}" for key, value in selected_runtime.items())
        + f" goodput_mbps={benchmark.get('tcp_shift_goodput_mbps', 'na')}"
    )


if __name__ == "__main__":
    main()
