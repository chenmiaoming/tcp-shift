#!/usr/bin/env python3
import argparse
import json
import math
import re
from pathlib import Path


def load_json(path):
    with Path(path).open(encoding="utf-8") as handle:
        return json.load(handle)


def parse_size(path):
    text = Path(path).read_text(encoding="utf-8")
    match = re.search(
        r"rack_resource_size=ok rack_enabled=(\d+) "
        r"adapter_bytes=(\d+) pcb_bytes=(\d+) loop_bytes=(\d+) "
        r"rack_state_bytes=(\d+) pacer_event_bytes=(\d+)",
        text,
    )
    if not match:
        raise SystemExit(f"missing rack resource size record: {path}")
    keys = (
        "rack_enabled",
        "adapter_bytes",
        "pcb_bytes",
        "loop_bytes",
        "rack_state_bytes",
        "pacer_event_bytes",
    )
    return {key: int(value) for key, value in zip(keys, match.groups())}


def parse_recovery_timer(path):
    text = Path(path).read_text(encoding="utf-8")
    match = re.search(r"tcp-shift-p2-recovery-timer: ([^\n]+)", text)
    if not match:
        raise SystemExit(f"missing RACK recovery timer telemetry: {path}")
    fields = {}
    for key, value in re.findall(r"([a-z_]+)=([0-9]+)", match.group(1)):
        fields[key] = int(value)
    required = {
        "loop_wakeups",
        "release_callbacks",
        "callback_errors",
        "timerfd_creates",
        "timer_expirations",
        "scheduled_events",
        "released_events",
        "cancelled_events",
        "heap_current",
        "heap_peak",
        "heap_capacity",
    }
    missing = sorted(required - fields.keys())
    if missing:
        raise SystemExit(f"missing recovery timer fields {missing}: {path}")
    return fields


def need(condition, message):
    if not condition:
        raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--default-size", required=True)
    parser.add_argument("--rack-size", required=True)
    parser.add_argument("--default-memory", required=True)
    parser.add_argument("--rack-memory", required=True)
    parser.add_argument("--default-cpu", required=True)
    parser.add_argument("--rack-cpu", required=True)
    parser.add_argument("--rack-runtime-stderr", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    default_size = parse_size(args.default_size)
    rack_size = parse_size(args.rack_size)
    default_mem = load_json(args.default_memory)
    rack_mem = load_json(args.rack_memory)
    default_cpu = load_json(args.default_cpu)
    rack_cpu = load_json(args.rack_cpu)
    recovery = parse_recovery_timer(args.rack_runtime_stderr)

    need(default_size["rack_enabled"] == 0, "default size binary unexpectedly has RACK")
    need(rack_size["rack_enabled"] == 1, "RACK size binary does not have RACK")
    need(
        default_size["pacer_event_bytes"] == rack_size["pacer_event_bytes"],
        "RACK changed the process-wide timer event ABI",
    )

    adapter_delta = rack_size["adapter_bytes"] - default_size["adapter_bytes"]
    pcb_delta = rack_size["pcb_bytes"] - default_size["pcb_bytes"]
    loop_delta = rack_size["loop_bytes"] - default_size["loop_bytes"]
    static_flow_delta = adapter_delta + pcb_delta

    need(0 <= adapter_delta <= 256,
         f"unexpected RACK adapter growth: {adapter_delta} bytes")
    need(0 <= pcb_delta <= 128,
         f"unexpected SACK/RACK PCB growth: {pcb_delta} bytes")
    need(0 <= static_flow_delta <= 384,
         f"unexpected per-flow static transport growth: {static_flow_delta} bytes")
    need(0 <= loop_delta <= 512,
         f"unexpected process-wide loop growth: {loop_delta} bytes")

    for stage in ("ready", "max_idle", "drained"):
        default_fd = int(default_mem[stage]["fd_count"])
        rack_fd = int(rack_mem[stage]["fd_count"])
        need(
            rack_fd - default_fd == 1,
            f"RACK must add exactly one process-wide timer fd at {stage}: "
            f"default={default_fd} rack={rack_fd}",
        )

    flows = int(rack_mem["max_idle"]["flows"])
    need(flows == int(default_mem["max_idle"]["flows"]) and flows > 0,
         "memory A/B flow counts differ")

    default_flow_pss = (
        int(default_mem["max_idle"]["pss_kb"]) - int(default_mem["ready"]["pss_kb"])
    )
    rack_flow_pss = (
        int(rack_mem["max_idle"]["pss_kb"]) - int(rack_mem["ready"]["pss_kb"])
    )
    incremental_flow_pss = rack_flow_pss - default_flow_pss
    structural_kb = math.ceil(static_flow_delta * flows / 1024.0)
    live_flow_cap_kb = structural_kb + 64
    need(
        incremental_flow_pss <= live_flow_cap_kb,
        "RACK staged-idle PSS growth exceeds structural+64KiB bound: "
        f"incremental={incremental_flow_pss}KiB cap={live_flow_cap_kb}KiB",
    )

    fixed_pss_delta = int(rack_mem["ready"]["pss_kb"]) - int(default_mem["ready"]["pss_kb"])
    need(
        fixed_pss_delta <= 128,
        f"RACK fixed process PSS overhead exceeds 128KiB: {fixed_pss_delta}KiB",
    )
    default_post_drain_pss = int(default_mem["post_drain_pss_kb_delta"])
    rack_post_drain_pss = int(rack_mem["post_drain_pss_kb_delta"])
    incremental_post_drain_pss = rack_post_drain_pss - default_post_drain_pss
    need(
        incremental_post_drain_pss <= 64,
        "RACK incremental post-drain PSS retention exceeds 64KiB: "
        f"default={default_post_drain_pss}KiB rack={rack_post_drain_pss}KiB "
        f"incremental={incremental_post_drain_pss}KiB",
    )

    need(int(rack_cpu["idle_cpu_ticks"]) == 0,
         f"RACK idle CPU must stay at zero ticks: {rack_cpu['idle_cpu_ticks']}")
    default_work = float(default_cpu["work_cpu_us_per_operation"])
    rack_work = float(rack_cpu["work_cpu_us_per_operation"])
    need(
        rack_work <= default_work + 10.0,
        "RACK small-operation CPU overhead exceeds +10 us/op: "
        f"default={default_work:.6f} rack={rack_work:.6f}",
    )

    need(recovery["timerfd_creates"] == 1,
         f"expected one process-wide RACK timerfd, got {recovery['timerfd_creates']}")
    need(recovery["callback_errors"] == 0,
         f"RACK recovery callback errors: {recovery['callback_errors']}")
    need(recovery["heap_current"] == 0,
         f"RACK recovery heap did not drain: {recovery['heap_current']}")
    need(recovery["loop_wakeups"] == 0,
         f"lossless resource workload caused RACK timer wakeups: {recovery['loop_wakeups']}")
    need(recovery["release_callbacks"] == 0,
         f"lossless resource workload released RACK recovery callbacks: {recovery['release_callbacks']}")
    need(recovery["timer_expirations"] == 0,
         f"lossless resource workload expired RACK timers: {recovery['timer_expirations']}")

    summary = {
        "adapter_delta_bytes": adapter_delta,
        "pcb_delta_bytes": pcb_delta,
        "static_per_flow_delta_bytes": static_flow_delta,
        "loop_delta_bytes": loop_delta,
        "fixed_pss_delta_kb": fixed_pss_delta,
        "flows": flows,
        "default_flow_pss_delta_kb": default_flow_pss,
        "rack_flow_pss_delta_kb": rack_flow_pss,
        "incremental_flow_pss_delta_kb": incremental_flow_pss,
        "live_flow_pss_cap_kb": live_flow_cap_kb,
        "default_post_drain_pss_delta_kb": default_post_drain_pss,
        "rack_post_drain_pss_delta_kb": rack_post_drain_pss,
        "incremental_post_drain_pss_delta_kb": incremental_post_drain_pss,
        "default_work_cpu_us_per_operation": default_work,
        "rack_work_cpu_us_per_operation": rack_work,
        "rack_idle_cpu_ticks": int(rack_cpu["idle_cpu_ticks"]),
        "recovery_timer": recovery,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))
    print("rack_resource_cost=ok")


if __name__ == "__main__":
    main()
