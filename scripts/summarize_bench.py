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

CASE_ORDER = ("native-cubic", "native-bbr", "gvisor-cubic", "gvisor-bbr")


def ratio_percent(value: float, baseline: float) -> float:
    return ((value / baseline) - 1.0) * 100.0 if baseline else 0.0


def tcp_stats(log_path: Path) -> dict[str, float]:
    """Read gVisor TCP counters from the relay's periodic stats line."""
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
    """Return router data-path and ACK-path netem drops."""
    if not state_path.exists():
        return 0.0, 0.0
    text = state_path.read_text(errors="replace")
    sections = {
        "data": re.search(
            r"--- router data qdisc ---\n(.*?)(?=\n--- router ACK qdisc ---)",
            text,
            re.S,
        ),
        "ack": re.search(
            r"--- router ACK qdisc ---\n(.*?)(?=\n--- client routes ---)",
            text,
            re.S,
        ),
    }
    values: dict[str, float] = {}
    for name, section in sections.items():
        match = re.search(r"dropped (\d+)", section.group(1) if section else "")
        values[name] = float(match.group(1)) if match else 0.0
    return values["data"], values["ack"]


def mean(values: list[float]) -> float:
    return statistics.mean(values) if values else 0.0


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def stdev(values: list[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def metric(summary: dict[str, object], case: str, key: str) -> float:
    entry = summary.get(case, {})
    if not isinstance(entry, dict):
        return 0.0
    return float(entry.get(key, 0.0))


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
        data_drops, ack_drops = qdisc_drops(
            bench_dir / f"network-{name}-{trial}-post.txt"
        )
        values["data_qdisc_drops"] = data_drops
        values["ack_qdisc_drops"] = ack_drops
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
            "data_qdisc_drops_mean": mean([v["data_qdisc_drops"] for v in vals]),
            "ack_qdisc_drops_mean": mean([v["ack_qdisc_drops"] for v in vals]),
        }

    nc_tp = metric(summary, "native-cubic", "throughput_mbps_median")
    nb_tp = metric(summary, "native-bbr", "throughput_mbps_median")
    gc_tp = metric(summary, "gvisor-cubic", "throughput_mbps_median")
    gb_tp = metric(summary, "gvisor-bbr", "throughput_mbps_median")

    nc_rss = metric(summary, "native-cubic", "peak_rss_mib_mean")
    nb_rss = metric(summary, "native-bbr", "peak_rss_mib_mean")
    gc_rss = metric(summary, "gvisor-cubic", "peak_rss_mib_mean")
    gb_rss = metric(summary, "gvisor-bbr", "peak_rss_mib_mean")

    nc_cpu = metric(summary, "native-cubic", "cpu_seconds_mean")
    nb_cpu = metric(summary, "native-bbr", "cpu_seconds_mean")
    gc_cpu = metric(summary, "gvisor-cubic", "cpu_seconds_mean")
    gb_cpu = metric(summary, "gvisor-bbr", "cpu_seconds_mean")

    comparison = {
        "throughput_basis": "median",
        "native_bbr_vs_native_cubic_throughput_percent": ratio_percent(nb_tp, nc_tp),
        "gvisor_bbr_vs_gvisor_cubic_throughput_percent": ratio_percent(gb_tp, gc_tp),
        "gvisor_cubic_vs_native_cubic_throughput_percent": ratio_percent(gc_tp, nc_tp),
        "gvisor_bbr_vs_native_bbr_throughput_percent": ratio_percent(gb_tp, nb_tp),
        "gvisor_cubic_vs_native_cubic_peak_rss_mib": gc_rss - nc_rss,
        "gvisor_bbr_vs_native_bbr_peak_rss_mib": gb_rss - nb_rss,
        "gvisor_cubic_vs_native_cubic_cpu_percent": ratio_percent(gc_cpu, nc_cpu),
        "gvisor_bbr_vs_native_bbr_cpu_percent": ratio_percent(gb_cpu, nb_cpu),
        "gvisor_cubic_vs_native_cubic_cpu_seconds": gc_cpu - nc_cpu,
        "gvisor_bbr_vs_native_bbr_cpu_seconds": gb_cpu - nb_cpu,
    }
    summary["comparison"] = comparison

    Path(sys.argv[3]).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "## tcp-shift long-fat network benchmark",
        "",
        "| case | trials | mean Mbit/s | median Mbit/s | stddev | mean RSS MiB | mean CPU s | retrans/trial | RTO/trial | DSACK/trial |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name in CASE_ORDER:
        if name not in summary:
            continue
        s = summary[name]
        assert isinstance(s, dict)
        if name.startswith("native-"):
            tcp_cols = "n/a | n/a | n/a"
        else:
            tcp_cols = (
                f"{s['tcp_retransmits_mean']:.1f} | {s['tcp_timeouts_mean']:.1f} | "
                f"{s['tcp_dsack_mean']:.1f}"
            )
        lines.append(
            f"| {name} | {s['trials']} | {s['throughput_mbps_mean']:.3f} | "
            f"{s['throughput_mbps_median']:.3f} | {s['throughput_mbps_stdev']:.3f} | "
            f"{s['peak_rss_mib_mean']:.2f} | {s['cpu_seconds_mean']:.3f} | {tcp_cols} |"
        )

    lines += [
        "",
        "### Comparisons (median throughput)",
        "",
        f"- Native Linux BBR vs native CUBIC: **{comparison['native_bbr_vs_native_cubic_throughput_percent']:+.2f}%**.",
        f"- gVisor BBR vs gVisor CUBIC: **{comparison['gvisor_bbr_vs_gvisor_cubic_throughput_percent']:+.2f}%**.",
        f"- gVisor CUBIC vs native CUBIC: **{comparison['gvisor_cubic_vs_native_cubic_throughput_percent']:+.2f}%**.",
        f"- gVisor BBR vs native Linux BBR: **{comparison['gvisor_bbr_vs_native_bbr_throughput_percent']:+.2f}%**.",
        "",
        "### Relay overhead",
        "",
        f"- gVisor CUBIC incremental peak RSS vs native CUBIC: **{comparison['gvisor_cubic_vs_native_cubic_peak_rss_mib']:+.2f} MiB**.",
        f"- gVisor BBR incremental peak RSS vs native BBR: **{comparison['gvisor_bbr_vs_native_bbr_peak_rss_mib']:+.2f} MiB**.",
        f"- gVisor CUBIC incremental relay CPU time: **{comparison['gvisor_cubic_vs_native_cubic_cpu_seconds']:+.3f} s** "
        f"(**{comparison['gvisor_cubic_vs_native_cubic_cpu_percent']:+.2f}%**).",
        f"- gVisor BBR incremental relay CPU time: **{comparison['gvisor_bbr_vs_native_bbr_cpu_seconds']:+.3f} s** "
        f"(**{comparison['gvisor_bbr_vs_native_bbr_cpu_percent']:+.2f}%**).",
        "",
        "### Emulation counters",
        "",
    ]
    for name in CASE_ORDER:
        if name not in summary:
            continue
        s = summary[name]
        assert isinstance(s, dict)
        lines.append(
            f"- {name}: mean router data-path netem drops {s['data_qdisc_drops_mean']:.1f}; "
            f"mean router ACK-path netem drops {s['ack_qdisc_drops_mean']:.1f}."
        )

    lines += [
        "",
        "Native Linux retransmission/RTO/DSACK counters are not yet sampled from TCP_INFO, so those table cells are reported as n/a rather than misleading zeros.",
        "",
        "The gVisor BBR implementation is an experimental BBRv1-inspired model. The current version uses ACK-rate sampling rather than Linux's full per-packet delivery-rate sampler; CI results are measurements, not a compatibility claim.",
    ]
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
