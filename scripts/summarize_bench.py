#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


TCP_FIELDS = {
    "retransmits": "Retransmits",
    "timeouts": "Timeouts",
    "dsack": "SegmentsAckedWithDSACK",
    "spurious_recovery": "SpuriousRecovery",
}


def ratio_percent(value: float, baseline: float) -> float:
    return ((value / baseline) - 1.0) * 100.0 if baseline else 0.0


def tcp_stats(log_path: Path) -> dict[str, float]:
    if not log_path.exists():
        return {name: 0.0 for name in TCP_FIELDS}
    last = ""
    for line in log_path.read_text(errors="replace").splitlines():
        if "tcp={" in line:
            last = line
    out: dict[str, float] = {}
    for name, field in TCP_FIELDS.items():
        match = re.search(rf"\b{re.escape(field)}:(\d+)", last)
        out[name] = float(match.group(1)) if match else 0.0
    return out


def qdisc_drops(state_path: Path) -> tuple[float, float]:
    """Return root-veth and client-veth netem drops from one post-case dump."""
    if not state_path.exists():
        return 0.0, 0.0
    text = state_path.read_text(errors="replace")
    sections = {
        "root": re.search(
            r"--- root veth qdisc ---\n(.*?)(?=\n--- TUN ---)", text, re.S
        ),
        "client": re.search(
            r"--- client veth qdisc ---\n(.*?)(?=\n--- FORWARD chain ---)",
            text,
            re.S,
        ),
    }
    values: dict[str, float] = {}
    for name, section in sections.items():
        match = re.search(r"dropped (\d+)", section.group(1) if section else "")
        values[name] = float(match.group(1)) if match else 0.0
    return values["root"], values["client"]


def mean(values: list[float]) -> float:
    return statistics.mean(values) if values else 0.0


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def stdev(values: list[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: summarize_bench.py results.csv summary.md summary.json")

    csv_path = Path(sys.argv[1])
    bench_dir = csv_path.parent
    rows = list(csv.DictReader(csv_path.open(newline="")))
    groups: dict[str, list[dict[str, float]]] = defaultdict(list)

    for row in rows:
        name = row["case"]
        trial = row["trial"]
        values = {
            "mbps": float(row["mbps"]),
            "peak_rss_kib": float(row["peak_rss_kib"]),
            "cpu_seconds": float(row["cpu_seconds"]),
        }
        values.update(tcp_stats(bench_dir / f"{name}-{trial}-proxy.log"))
        root_drops, client_drops = qdisc_drops(
            bench_dir / f"network-{name}-{trial}-post.txt"
        )
        values["root_qdisc_drops"] = root_drops
        values["client_qdisc_drops"] = client_drops
        groups[name].append(values)

    summary: dict[str, object] = {}
    for name, vals in groups.items():
        throughputs = [v["mbps"] for v in vals]
        summary[name] = {
            "trials": len(vals),
            "throughput_mbps_mean": mean(throughputs),
            "throughput_mbps_median": median(throughputs),
            "throughput_mbps_stdev": stdev(throughputs),
            "throughput_mbps_min": min(throughputs),
            "throughput_mbps_max": max(throughputs),
            "peak_rss_mib_mean": mean([v["peak_rss_kib"] for v in vals]) / 1024.0,
            "cpu_seconds_mean": mean([v["cpu_seconds"] for v in vals]),
            "tcp_retransmits_mean": mean([v["retransmits"] for v in vals]),
            "tcp_timeouts_mean": mean([v["timeouts"] for v in vals]),
            "tcp_dsack_mean": mean([v["dsack"] for v in vals]),
            "tcp_spurious_recovery_mean": mean([v["spurious_recovery"] for v in vals]),
            "root_qdisc_drops_mean": mean([v["root_qdisc_drops"] for v in vals]),
            "client_qdisc_drops_mean": mean([v["client_qdisc_drops"] for v in vals]),
        }

    native = summary.get("native-cubic", {})
    cubic = summary.get("gvisor-cubic", {})
    bbr = summary.get("gvisor-bbr", {})

    # Use medians for throughput comparisons once repeated trials are enabled;
    # one-trial control jobs naturally have median == mean.
    native_tp = float(native.get("throughput_mbps_median", 0.0))
    native_rss = float(native.get("peak_rss_mib_mean", 0.0))
    native_cpu = float(native.get("cpu_seconds_mean", 0.0))
    cubic_tp = float(cubic.get("throughput_mbps_median", 0.0))
    cubic_rss = float(cubic.get("peak_rss_mib_mean", 0.0))
    cubic_cpu = float(cubic.get("cpu_seconds_mean", 0.0))
    bbr_tp = float(bbr.get("throughput_mbps_median", 0.0))

    comparison = {
        "throughput_basis": "median",
        "bbr_vs_gvisor_cubic_throughput_percent": ratio_percent(bbr_tp, cubic_tp),
        "gvisor_cubic_vs_native_throughput_percent": ratio_percent(cubic_tp, native_tp),
        "gvisor_cubic_vs_native_peak_rss_percent": ratio_percent(cubic_rss, native_rss),
        "gvisor_cubic_vs_native_peak_rss_mib": cubic_rss - native_rss,
        "gvisor_cubic_vs_native_cpu_percent": ratio_percent(cubic_cpu, native_cpu),
    }
    summary["comparison"] = comparison

    Path(sys.argv[3]).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "## tcp-shift long-fat network benchmark",
        "",
        "| case | trials | mean Mbit/s | median Mbit/s | stddev | mean RSS MiB | mean CPU s | retrans/trial | RTO/trial | DSACK/trial |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name in ("native-cubic", "gvisor-cubic", "gvisor-bbr"):
        if name not in summary:
            continue
        s = summary[name]
        lines.append(
            f"| {name} | {s['trials']} | {s['throughput_mbps_mean']:.3f} | "
            f"{s['throughput_mbps_median']:.3f} | {s['throughput_mbps_stdev']:.3f} | "
            f"{s['peak_rss_mib_mean']:.2f} | {s['cpu_seconds_mean']:.3f} | "
            f"{s['tcp_retransmits_mean']:.1f} | {s['tcp_timeouts_mean']:.1f} | "
            f"{s['tcp_dsack_mean']:.1f} |"
        )

    lines += [
        "",
        "### Comparisons (median throughput)",
        "",
        f"- BBR vs gVisor CUBIC throughput: **{comparison['bbr_vs_gvisor_cubic_throughput_percent']:+.2f}%**.",
        f"- gVisor CUBIC vs native throughput: **{comparison['gvisor_cubic_vs_native_throughput_percent']:+.2f}%**.",
        f"- gVisor CUBIC incremental peak RSS: **{comparison['gvisor_cubic_vs_native_peak_rss_mib']:+.2f} MiB** "
        f"(**{comparison['gvisor_cubic_vs_native_peak_rss_percent']:+.2f}%** vs native).",
        f"- gVisor CUBIC vs native relay CPU time: **{comparison['gvisor_cubic_vs_native_cpu_percent']:+.2f}%**.",
        "",
        "### Emulation counters",
        "",
    ]
    for name in ("native-cubic", "gvisor-cubic", "gvisor-bbr"):
        if name not in summary:
            continue
        s = summary[name]
        lines.append(
            f"- {name}: mean root/data-path qdisc drops {s['root_qdisc_drops_mean']:.1f}; "
            f"mean client/ACK-path qdisc drops {s['client_qdisc_drops_mean']:.1f}."
        )

    lines += [
        "",
        "The BBR implementation is an experimental BBRv1-inspired model. The current version uses ACK-rate sampling rather than Linux's full per-packet delivery-rate sampler; CI results are measurements, not a compatibility claim.",
    ]
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
