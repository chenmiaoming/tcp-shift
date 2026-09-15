#!/usr/bin/env python3
import argparse
import pathlib
import sys


REQUIRED = {
    "linux-unpaced": "linux-cubic-unpaced/summary.txt",
    "reno-unpaced": "iwip-cubic-reno-unpaced/summary.txt",
    "linux-paced": "linux-cubic-paced/summary.txt",
    "reno-paced": "iwip-cubic-reno-paced/summary.txt",
}


def parse_summary(path: pathlib.Path):
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        raise ValueError(f"empty summary: {path}")
    result = {}
    for token in text.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        result[key] = value
    return result


def number(row, key):
    try:
        return float(row[key])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"missing/invalid {key}") from exc


def integer(row, key):
    try:
        return int(row[key])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"missing/invalid {key}") from exc


def relative_drift(left, right):
    denominator = max(abs(left), abs(right))
    return abs(left - right) / denominator if denominator else 0.0


def main():
    parser = argparse.ArgumentParser(
        description="Evaluate 1 MiB tcp-shift/Linux CUBIC parity evidence"
    )
    parser.add_argument("root", type=pathlib.Path,
                        help="one matrix-case output directory")
    parser.add_argument(
        "--cross-harness-drift", type=float, default=0.05,
        help="maximum paced CUBIC goodput drift across the two harnesses "
             "(default: 0.05)",
    )
    parser.add_argument("--linux-ratio-min", type=float, default=0.95)
    parser.add_argument("--linux-ratio-max", type=float, default=1.05)
    parser.add_argument(
        "--strict", action="store_true",
        help="exit nonzero when the evidence is not qualification-ready",
    )
    args = parser.parse_args()

    try:
        rows = {
            name: parse_summary(args.root / relative)
            for name, relative in REQUIRED.items()
        }
    except (OSError, ValueError) as exc:
        print(f"cc_linux_parity_evaluate=error reason={exc}")
        return 2

    issues = []
    for mode in ("unpaced", "paced"):
        linux = rows[f"linux-{mode}"]
        reno = rows[f"reno-{mode}"]
        linux_goodput = number(linux, "tcp_shift_goodput_mbps")
        reno_goodput = number(reno, "cubic_goodput_mbps")
        drift = relative_drift(linux_goodput, reno_goodput)
        linux_loss = integer(linux, "tcp_shift_loss_events")
        reno_loss = integer(reno, "cubic_loss_events")
        linux_rto = integer(linux, "tcp_shift_timeout_events")
        reno_rto = integer(reno, "cubic_timeout_events")

        print(
            f"cross_harness mode={mode} "
            f"linux_harness_goodput_mbps={linux_goodput:.6f} "
            f"reno_harness_goodput_mbps={reno_goodput:.6f} "
            f"relative_drift={drift:.6f} "
            f"linux_harness_loss={linux_loss} reno_harness_loss={reno_loss} "
            f"linux_harness_rto={linux_rto} reno_harness_rto={reno_rto}"
        )

        # The same tcp-shift CUBIC binary/profile is run in both harnesses.
        # Before any Linux parity threshold is trustworthy, paced results need
        # to be reproducible across those independent invocations.
        if mode == "paced" and drift > args.cross_harness_drift:
            issues.append(
                f"paced CUBIC cross-harness goodput drift {drift:.3%} exceeds "
                f"{args.cross_harness_drift:.3%}"
            )
        if mode == "paced" and (linux_loss != reno_loss or linux_rto != reno_rto):
            issues.append(
                "paced CUBIC recovery counters differ across benchmark "
                f"harnesses (loss {linux_loss}/{reno_loss}, "
                f"rto {linux_rto}/{reno_rto})"
            )

    paced = rows["linux-paced"]
    ratio = number(paced, "ratio")
    loss = integer(paced, "tcp_shift_loss_events")
    timeout = integer(paced, "tcp_shift_timeout_events")
    linux_retrans = integer(paced, "linux_total_retrans")

    print(
        f"paced_linux_parity ratio={ratio:.6f} "
        f"tcp_shift_loss_events={loss} "
        f"tcp_shift_timeout_events={timeout} "
        f"linux_total_retrans={linux_retrans}"
    )

    if not (args.linux_ratio_min <= ratio <= args.linux_ratio_max):
        issues.append(
            f"paced CUBIC/Linux goodput ratio {ratio:.6f} outside "
            f"[{args.linux_ratio_min:.3f}, {args.linux_ratio_max:.3f}]"
        )
    if loss != 0:
        issues.append(f"paced tcp-shift CUBIC has {loss} clean-path loss events")
    if timeout != 0:
        issues.append(f"paced tcp-shift CUBIC has {timeout} clean-path RTO events")
    if linux_retrans != 0:
        issues.append(
            f"Linux reference has {linux_retrans} retransmissions; "
            "clean-path reference is not clean"
        )

    ready = not issues
    print(f"qualification_ready={'yes' if ready else 'no'} issue_count={len(issues)}")
    for index, issue in enumerate(issues, 1):
        print(f"issue_{index}={issue}")

    if args.strict and not ready:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
