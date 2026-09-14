#!/usr/bin/env python3
"""Build a conservative P3 userspace-capacity model from retained CI measurements.

This model deliberately covers tcp-shift process PSS only. Backend Linux TCP
kernel memory, the backend application, and public-client kernel memory are not
included. A planning budget therefore reserves most host RAM outside tcp-shift
instead of pretending process PSS is total-host residency.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise SystemExit(f"expected JSON object: {path}")
    return value


def positive_number(mapping: dict, key: str, source: Path) -> float:
    try:
        value = float(mapping[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise SystemExit(f"missing/invalid {key} in {source}") from exc
    if value <= 0:
        raise SystemExit(f"expected positive {key} in {source}; got {value}")
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--idle",
        type=Path,
        default=Path(".build/p3-memory-baseline/summary.json"),
    )
    parser.add_argument(
        "--public-to-backend",
        type=Path,
        default=Path(".build/p3-public-to-backend-residency/summary.json"),
    )
    parser.add_argument(
        "--backend-to-public",
        type=Path,
        default=Path(".build/p3-backend-to-public-residency/summary.json"),
    )
    parser.add_argument(
        "--repeated",
        type=Path,
        default=Path(".build/p3-repeated-drain/summary.json"),
    )
    parser.add_argument(
        "--cpu",
        type=Path,
        default=Path(".build/p3-cpu-baseline/summary.json"),
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path(".build/p3-capacity-model"),
    )
    args = parser.parse_args()

    idle = load_json(args.idle)
    p2b = load_json(args.public_to_backend)
    b2p = load_json(args.backend_to_public)
    repeated = load_json(args.repeated)
    cpu = load_json(args.cpu)

    idle_slope = positive_number(idle, "max_idle_pss_kb_per_flow", args.idle)
    p2b_active_delta = positive_number(
        p2b, "active_pss_delta_kb_per_flow", args.public_to_backend
    )
    b2p_active_delta = positive_number(
        b2p, "active_pss_delta_kb_per_flow", args.backend_to_public
    )

    repeated_details = repeated.get("rounds_detail")
    if not isinstance(repeated_details, list) or not repeated_details:
        raise SystemExit(f"missing repeated rounds in {args.repeated}")
    repeated_ready_pss = positive_number(repeated, "ready_pss_kb", args.repeated)
    repeated_idle_slopes = []
    warm_floor_candidates = []
    for row in repeated_details:
        if not isinstance(row, dict):
            raise SystemExit(f"invalid repeated round in {args.repeated}")
        idle_delta = float(row["idle_pss_delta_from_ready_kb"])
        drained_pss = float(row["drained_pss_kb"])
        flows = positive_number(repeated, "flows_per_round", args.repeated)
        if idle_delta < 0:
            raise SystemExit("repeated idle PSS delta became negative")
        repeated_idle_slopes.append(idle_delta / flows)
        warm_floor_candidates.append(drained_pss)

    idle_slope = max(idle_slope, *repeated_idle_slopes)
    warm_fixed_pss_kb = max(
        float(idle["drained"]["pss_kb"]),
        *warm_floor_candidates,
    )
    active_payload_delta_kb_per_flow = max(p2b_active_delta, b2p_active_delta)
    active_total_kb_per_flow = idle_slope + active_payload_delta_kb_per_flow

    # The current controlled pressure runs deliberately fill the 32-KiB lwIP
    # receive/send window. Treat their larger directional delta as a conservative
    # fully-window-resident userspace slope for planning.
    if active_total_kb_per_flow < 32.0:
        raise SystemExit(
            "active userspace slope is inconsistent with the qualified 32-KiB window"
        )

    targets_mib = [32, 64, 128]
    budget_fractions = [0.25, 0.50]
    projection_flows = [128, 256, 512, 1024]
    projections = []
    for flows in projection_flows:
        idle_pss = warm_fixed_pss_kb + idle_slope * flows
        active_pss = warm_fixed_pss_kb + active_total_kb_per_flow * flows
        projections.append(
            {
                "flows": flows,
                "projected_idle_process_pss_kb": round(idle_pss, 3),
                "projected_fully_window_resident_process_pss_kb": round(
                    active_pss, 3
                ),
            }
        )

    budgets = []
    for target_mib in targets_mib:
        for fraction in budget_fractions:
            process_budget_kb = target_mib * 1024.0 * fraction
            usable_kb = max(0.0, process_budget_kb - warm_fixed_pss_kb)
            max_idle = math.floor(usable_kb / idle_slope)
            max_active = math.floor(usable_kb / active_total_kb_per_flow)
            active_128_pss_kb = warm_fixed_pss_kb + active_total_kb_per_flow * 128
            active_128_headroom_kb = process_budget_kb - active_128_pss_kb
            budgets.append(
                {
                    "host_memory_mib": target_mib,
                    "tcp_shift_process_budget_fraction": fraction,
                    "tcp_shift_process_budget_kb": round(process_budget_kb, 3),
                    "projected_max_idle_flows": max_idle,
                    "projected_max_fully_window_resident_flows": max_active,
                    "projected_128_active_process_pss_kb": round(
                        active_128_pss_kb, 3
                    ),
                    "remaining_process_budget_at_128_active_kb": round(
                        active_128_headroom_kb, 3
                    ),
                    "remaining_process_budget_at_128_active_kb_per_flow": round(
                        active_128_headroom_kb / 128.0, 3
                    ),
                }
            )

    primary = next(
        row
        for row in budgets
        if row["host_memory_mib"] == 32
        and row["tcp_shift_process_budget_fraction"] == 0.25
    )
    if primary["remaining_process_budget_at_128_active_kb"] <= 0:
        raise SystemExit(
            "P3 admission failed: 128 fully-window-resident flows exceed the "
            "25% tcp-shift process budget on a 32-MiB planning target"
        )

    work_cpu_us_per_op = positive_number(
        cpu, "work_cpu_us_per_operation", args.cpu
    )
    summary = {
        "model_scope": "tcp-shift process PSS only",
        "backend_kernel_memory_included": False,
        "backend_application_memory_included": False,
        "public_client_kernel_memory_included": False,
        "planning_note": (
            "25% and 50% host-RAM fractions are sensitivity budgets, not product "
            "limits; the remainder is reserved for kernel/backend/application and "
            "other unmeasured host residency."
        ),
        "warm_fixed_process_pss_kb": round(warm_fixed_pss_kb, 3),
        "conservative_idle_pss_kb_per_flow": round(idle_slope, 6),
        "controlled_active_payload_pss_delta_kb_per_flow": round(
            active_payload_delta_kb_per_flow, 6
        ),
        "conservative_fully_window_resident_pss_kb_per_flow": round(
            active_total_kb_per_flow, 6
        ),
        "qualified_window_bytes_per_flow": 32768,
        "repeated_first_to_last_drain_pss_growth_kb": repeated[
            "first_to_last_drain_pss_growth_kb"
        ],
        "cpu_work_us_per_operation": round(work_cpu_us_per_op, 6),
        "cpu_idle_ms_per_second_observation": cpu["idle_cpu_ms"],
        "projections": projections,
        "budgets": budgets,
        "p4_admission": {
            "planning_host_memory_mib": 32,
            "tcp_shift_process_budget_fraction": 0.25,
            "required_active_flows": 128,
            "passes": True,
            "remaining_process_budget_kb": primary[
                "remaining_process_budget_at_128_active_kb"
            ],
            "remaining_process_budget_kb_per_flow": primary[
                "remaining_process_budget_at_128_active_kb_per_flow"
            ],
        },
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    summary_path = args.out_dir / "summary.json"
    with summary_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")

    with (args.out_dir / "projections.tsv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=list(projections[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(projections)

    with (args.out_dir / "budgets.tsv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=list(budgets[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(budgets)

    print(json.dumps(summary, sort_keys=True))
    print("P3 constrained-host capacity model passed")


if __name__ == "__main__":
    main()
