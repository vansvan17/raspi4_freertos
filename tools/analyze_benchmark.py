#!/usr/bin/env python3
"""Summarize UART logs produced by the Raspberry Pi scheduling benchmark."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


FIELDS = (
    "mode",
    "ctrl_min_us",
    "ctrl_max_us",
    "ctrl_avg_us",
    "urgent_max_us",
    "interference",
    "switches",
    "missed",
)
FIELD_PATTERN = re.compile(r"([a-z_]+)=0x([0-9a-fA-F]+)")


def read_samples(path: Path) -> list[dict[str, int]]:
    samples: list[dict[str, int]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        values = {name: int(raw, 16) for name, raw in FIELD_PATTERN.findall(line)}
        if all(field in values for field in FIELDS):
            samples.append(values)
    return samples


def summarize(path: Path, warmup: int) -> dict[str, str | int]:
    samples = read_samples(path)
    if len(samples) <= warmup:
        raise ValueError(
            f"{path}: found {len(samples)} complete samples, need more than {warmup}"
        )

    samples = samples[warmup:]
    modes = {sample["mode"] for sample in samples}
    if len(modes) != 1:
        raise ValueError(f"{path}: mixed benchmark modes: {sorted(modes)}")

    first = samples[0]
    last = samples[-1]
    return {
        "log": str(path),
        "mode": next(iter(modes)),
        "samples": len(samples),
        "controller_min_us": min(s["ctrl_min_us"] for s in samples),
        "controller_max_us": max(s["ctrl_max_us"] for s in samples),
        "controller_final_avg_us": last["ctrl_avg_us"],
        "urgent_max_us": max(s["urgent_max_us"] for s in samples),
        "interference_jobs": last["interference"] - first["interference"],
        "context_switches": last["switches"] - first["switches"],
        "missed_deadlines": last["missed"] - first["missed"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--warmup-samples",
        type=int,
        default=10,
        help="discard this many one-second UART reports (default: 10)",
    )
    parser.add_argument("--output", type=Path, help="write CSV to this path")
    args = parser.parse_args()

    try:
        rows = [summarize(path, args.warmup_samples) for path in args.logs]
    except (OSError, ValueError) as error:
        parser.error(str(error))

    stream = args.output.open("w", newline="", encoding="utf-8") if args.output else sys.stdout
    try:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    finally:
        if args.output:
            stream.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
