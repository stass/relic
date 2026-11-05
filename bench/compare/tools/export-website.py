#!/usr/bin/env python3
"""Export recorded benchmark medians for the browser charts; run no benchmarks."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
STATUSES = {"ok", "timeout", "n/a", "no-reference", "engine-limit", "resource-limit"}
ENGINE_NAMES = {
    "relic": "relic", "souffle": "Souffle compiled",
    "souffle-magic": "Souffle Magic", "souffle-interp": "Souffle interpreter",
    "nemo": "Nemo", "swipl": "SWI-Prolog", "clingo": "clingo",
    "duckdb": "DuckDB", "sqlite": "SQLite",
}


def read_measurements(path):
    measurements = {}
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        required = {"scale", "threads", "workload", "engine", "status", "median"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError("missing required benchmark columns")
        for row in reader:
            scale, threads = int(row["scale"]), int(row["threads"])
            if scale <= 0 or threads <= 0 or not row["workload"]:
                raise ValueError("invalid benchmark profile")
            if row["engine"] not in ENGINE_NAMES or row["status"] not in STATUSES:
                raise ValueError("unknown engine or measurement status")
            key = (scale, threads, row["workload"], row["engine"])
            if key in measurements:
                raise ValueError(f"duplicate measurement: {key}")
            seconds = None
            if row["status"] == "ok":
                seconds = float(row["median"])
                if not math.isfinite(seconds) or seconds <= 0:
                    raise ValueError(f"invalid completed median: {key}")
            elif row["median"] not in ("", "-"):
                raise ValueError(f"unsuccessful measurement has a median: {key}")
            measurements[key] = {"status": row["status"], "seconds": seconds}
    return measurements


def profile(measurements, scale, threads):
    workloads = {}
    for (cell_scale, cell_threads, workload, engine), cell in measurements.items():
        if (cell_scale, cell_threads) == (scale, threads):
            workloads.setdefault(workload, {})[engine] = cell
    return dict(sorted(workloads.items()))


def export(path):
    measurements = read_measurements(path)
    parallel_counts = {threads for scale, threads, _, _ in measurements
                       if scale == 1 and threads > 1}
    if len(parallel_counts) != 1:
        raise ValueError("expected exactly one parallel thread count")
    parallel = parallel_counts.pop()
    profiles = {str(threads): profile(measurements, 1, threads)
                for threads in (1, parallel)}
    if not profiles["1"] or profiles["1"].keys() != profiles[str(parallel)].keys():
        raise ValueError("chart profiles must contain the same workloads")
    # A chart reference must complete in both profiles. Unavailable rivals
    # retain their statuses and never receive invented numeric times.
    for name in profiles[str(parallel)]:
        for workloads in profiles.values():
            if workloads[name].get("relic", {}).get("status") != "ok":
                raise ValueError(f"missing completed chart reference: {name}")
    return {
        "source": {"path": "bench/compare/results.tsv",
                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()},
        "engines": ENGINE_NAMES,
        "parallel_threads": parallel,
        "profiles": profiles,

    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=ROOT / "bench/compare/results.tsv")
    parser.add_argument("--output", type=Path, default=ROOT / "website/assets/benchmark-data.json")
    args = parser.parse_args()
    try:
        data = export(args.results)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
