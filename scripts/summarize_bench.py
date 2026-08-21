#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: summarize_bench.py results.csv summary.md summary.json")
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    groups: dict[str, list[dict[str, float]]] = defaultdict(list)
    for row in rows:
        groups[row["case"]].append(
            {
                "mbps": float(row["mbps"]),
                "peak_rss_kib": float(row["peak_rss_kib"]),
                "cpu_seconds": float(row["cpu_seconds"]),
            }
        )

    summary = {}
    for name, vals in groups.items():
        summary[name] = {
            "trials": len(vals),
            "throughput_mbps_mean": statistics.mean(v["mbps"] for v in vals),
            "throughput_mbps_min": min(v["mbps"] for v in vals),
            "peak_rss_mib_mean": statistics.mean(v["peak_rss_kib"] for v in vals) / 1024.0,
            "cpu_seconds_mean": statistics.mean(v["cpu_seconds"] for v in vals),
        }

    cubic = summary.get("gvisor-cubic", {}).get("throughput_mbps_mean", 0.0)
    bbr = summary.get("gvisor-bbr", {}).get("throughput_mbps_mean", 0.0)
    improvement = ((bbr / cubic) - 1.0) * 100.0 if cubic else 0.0
    summary["comparison"] = {"bbr_vs_gvisor_cubic_percent": improvement}

    Path(sys.argv[3]).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "## tcp-shift long-fat network benchmark",
        "",
        "| case | trials | mean throughput (Mbit/s) | min throughput | mean peak RSS (MiB) | mean CPU (s) |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for name in ("native-cubic", "gvisor-cubic", "gvisor-bbr"):
        if name not in summary:
            continue
        s = summary[name]
        lines.append(
            f"| {name} | {s['trials']} | {s['throughput_mbps_mean']:.3f} | "
            f"{s['throughput_mbps_min']:.3f} | {s['peak_rss_mib_mean']:.2f} | {s['cpu_seconds_mean']:.3f} |"
        )
    lines += [
        "",
        f"BBR vs gVisor CUBIC mean throughput: **{improvement:+.2f}%**.",
        "",
        "The BBR implementation is an experimental BBRv1-inspired model. The first version uses ACK-rate sampling rather than Linux's full per-packet delivery-rate sampler; CI results are measurements, not a compatibility claim.",
    ]
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
