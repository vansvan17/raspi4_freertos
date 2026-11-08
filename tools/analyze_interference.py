#!/usr/bin/env python3
"""Summarize logs from the interference-test image (see INTERFERENCE.md)."""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

FIELD_PATTERN_PREFIX = "=0x"
HOGS = ("idle", "compute", "read", "write", "chase")
LAYOUTS = ("shared", "colored", "uncached")
REQUIRED = (
    "cfg", "hog", "layout", "budget", "hogs_ready", "n", "min", "p50", "p99",
    "p999", "max", "mean", "l2d_refill", "elapsed_cycles", "elapsed_ticks",
    "hog1_units", "hog2_units", "hog3_units", "hog_throttled",
)


def parse_fields(line: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for token in line.split():
        name, sep, raw = token.partition(FIELD_PATTERN_PREFIX)
        if sep:
            values[name] = int(raw, 16)
    return values


def read_log(path: Path) -> tuple[dict[str, int], list[dict[str, int]]]:
    header: dict[str, int] | None = None
    rows: list[dict[str, int]] = []
    complete = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("interference-test start"):
            header = parse_fields(line)
            rows = []
            complete = False
        elif line.startswith("interference "):
            values = parse_fields(line)
            missing = [field for field in REQUIRED if field not in values]
            if missing:
                raise ValueError(f"{path}: truncated row, missing {missing}")
            rows.append(values)
        elif line.startswith("interference-test complete"):
            complete = True
    if header is None:
        raise ValueError(f"{path}: no interference-test start line")
    if not complete:
        raise ValueError(f"{path}: run did not reach the complete line")
    if len(rows) != header.get("configs", -1):
        raise ValueError(f"{path}: {len(rows)} rows, header promised {header.get('configs')}")
    return header, rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--csv", type=Path, help="also write the summary table here")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="accept images built from a dirty worktree")
    args = parser.parse_args()

    groups: dict[tuple[int, int, int], list[dict[str, float]]] = defaultdict(list)
    revs = set()
    for path in args.logs:
        try:
            header, rows = read_log(path)
        except ValueError as error:
            print(f"rejected: {error}", file=sys.stderr)
            return 1
        if header.get("dirty") and not args.allow_dirty:
            print(f"rejected: {path}: built from a dirty worktree", file=sys.stderr)
            return 1
        if header.get("hogs_alive") != 3:
            print(f"rejected: {path}: only {header.get('hogs_alive')} hog cores came up",
                  file=sys.stderr)
            return 1
        revs.add(header.get("rev"))
        freq = header["cntfrq"]
        for row in rows:
            if row["hogs_ready"] != 3:
                print(f"rejected: {path}: cfg {row['cfg']} ran with "
                      f"{row['hogs_ready']} hogs ready", file=sys.stderr)
                return 1
            seconds = row["elapsed_ticks"] / freq
            units = row["hog1_units"] + row["hog2_units"] + row["hog3_units"]
            groups[(row["layout"], row["hog"], row["budget"])].append({
                "min": row["min"], "p50": row["p50"], "p99": row["p99"],
                "p999": row["p999"], "max": row["max"], "mean": row["mean"],
                "l2d_refill": row["l2d_refill"],
                "mhz": row["elapsed_cycles"] / seconds / 1e6,
                "hog_mbps": units * 64 / seconds / 1e6,
                "throttled": row["hog_throttled"],
            })

    if len(revs) > 1:
        print(f"rejected: logs come from different builds: {sorted(revs)}", file=sys.stderr)
        return 1

    baseline = {
        layout: statistics.median(r["p99"] for r in groups[(layout, 0, 0)])
        for layout in range(len(LAYOUTS)) if (layout, 0, 0) in groups
    }

    columns = ("layout", "hog", "budget", "runs", "p50", "p99", "p99_spread",
               "max", "p99_vs_idle", "l2d_refill", "cpu_mhz", "hog_MBps", "throttled")
    table = []
    for (layout, hog, budget), runs in sorted(groups.items()):
        p99s = [r["p99"] for r in runs]
        p99 = statistics.median(p99s)
        table.append({
            "layout": LAYOUTS[layout], "hog": HOGS[hog],
            "budget": budget or "none", "runs": len(runs),
            "p50": round(statistics.median(r["p50"] for r in runs)),
            "p99": round(p99),
            "p99_spread": f"{min(p99s)}-{max(p99s)}",
            "max": max(r["max"] for r in runs),
            "p99_vs_idle": f"{p99 / baseline[layout]:.2f}x" if layout in baseline else "",
            "l2d_refill": round(statistics.median(r["l2d_refill"] for r in runs)),
            "cpu_mhz": round(statistics.median(r["mhz"] for r in runs)),
            "hog_MBps": round(statistics.median(r["hog_mbps"] for r in runs)),
            "throttled": round(statistics.median(r["throttled"] for r in runs)),
        })

    widths = {c: max(len(c), *(len(str(row[c])) for row in table)) for c in columns}
    print("  ".join(c.rjust(widths[c]) for c in columns))
    for row in table:
        print("  ".join(str(row[c]).rjust(widths[c]) for c in columns))
    print("\nVictim times are CPU cycles per sample. Hog compute units are loop "
          "blocks, so its MB/s column is not memory traffic.")

    if args.csv:
        with args.csv.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns)
            writer.writeheader()
            writer.writerows(table)
    return 0


if __name__ == "__main__":
    sys.exit(main())
