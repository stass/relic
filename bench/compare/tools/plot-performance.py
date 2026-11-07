#!/usr/bin/env python3
"""Plot selected scale-1 parallel medians from the published comparison."""

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch


REPO = Path(__file__).resolve().parents[3]
ENGINES = (
    ("relic", "relic", "#087f8c"),
    ("souffle-magic", "Souffle Magic", "#e69f00"),
    ("souffle", "Souffle compiled", "#4477aa"),
    ("souffle-interp", "Souffle interpreter", "#aa3377"),
    ("duckdb", "DuckDB", "#666666"),
)
# Each chart lists workloads that cover its theme and include cases where a
# rival is at or ahead of relic; revisit the lists when a new cohort changes
# which workloads those are.
CHARTS = (
    ("recursive", "Recursive queries", (
        "chain-closure", "graph-left", "tc-sparse-acyclic",
        "tree-sg-all", "tc-acyclic-bf", "tc-cyclic-bf",
    )),
    ("analysis", "Analysis and table processing", (
        "andersen", "cspa", "cspa-httpd",
        "cspa-postgresql", "cspa-linux", "files-grouped",
    )),
)


def read_results(path):
    """Read the fixed cohort; unavailable timings remain explicit statuses."""
    results = {}
    with path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        required = {"scale", "threads", "workload", "engine", "status", "median"}
        if not required.issubset(reader.fieldnames or ()):
            raise ValueError("results file is missing required columns")
        rows = list(reader)
        parallel = {row["threads"] for row in rows
                    if row["scale"] == "1" and int(row["threads"]) > 1}
        if len(parallel) != 1:
            raise ValueError("expected exactly one parallel thread count")
        threads = parallel.pop()
        for row in rows:
            if row["scale"] != "1" or row["threads"] != threads:
                continue
            key = (row["workload"], row["engine"])
            if key in results:
                raise ValueError(f"duplicate result: {key}")
            status = row["status"]
            if status == "ok":
                median = float(row["median"])
                if not math.isfinite(median) or median <= 0:
                    raise ValueError(f"invalid median: {key}")
                results[key] = median
            elif status in {"timeout", "n/a"}:
                results[key] = status
            else:
                raise ValueError(f"unsupported result status {status}: {key}")
    return results


def relative_speeds(results, workloads):
    """Speed is inverse wall time: relic time / engine time, with relic at 1."""
    speeds = {}
    for workload in workloads:
        reference = results[workload, "relic"]
        if not isinstance(reference, float):
            raise ValueError(f"relic has no completed time: {workload}")
        for engine, _, _ in ENGINES:
            median = results[workload, engine]
            speeds[workload, engine] = (
                reference / median if isinstance(median, float) else median
            )
    return speeds


def plot_chart(results, title, workloads, context):
    """Return a grouped bar chart; missing results have labels, never bars."""
    speeds = relative_speeds(results, workloads)
    figure, axes = plt.subplots(figsize=(12, 5.4), layout="constrained")
    figure.set_facecolor("#ffffff")
    axes.set_facecolor("#ffffff")
    width = 0.15
    for index, (engine, label, color) in enumerate(ENGINES):
        for group, workload in enumerate(workloads):
            x = group + (index - (len(ENGINES) - 1) / 2) * width
            value = speeds[workload, engine]
            if isinstance(value, float):
                axes.bar(
                    x, value, width=width * 0.9, color=color,
                    hatch="//" if engine == "relic" else None,
                    edgecolor="white", linewidth=0.4, zorder=3,
                )
            else:
                axes.text(
                    x, 0.03, "T/O" if value == "timeout" else "n/a",
                    ha="center", va="bottom", rotation=90,
                    fontsize=8, color=color,
                    transform=axes.get_xaxis_transform(),
                )
    axes.axhline(1, color="#333333", linewidth=0.8, linestyle="--")
    axes.set_ylim(bottom=0)
    axes.set_xticks(range(len(workloads)), workloads, rotation=18, ha="right")
    axes.set_ylabel("Relative speed (relic = 1x; higher is faster)")
    axes.set_title(title, loc="left", fontsize=17, weight="bold", pad=46)
    axes.legend(
        handles=[Patch(facecolor=color, label=label,
                       hatch="//" if engine == "relic" else None)
                 for engine, label, color in ENGINES],
        loc="lower left", bbox_to_anchor=(0, 1.01), ncol=len(ENGINES),
        frameon=False, fontsize=9,
    )
    axes.grid(axis="y", color="#dddddd", linewidth=0.6, zorder=0)
    axes.spines[["top", "right"]].set_visible(False)
    figure.supxlabel(
        f"Scale 1, {context['parallel_threads']} threads | {context['processor']} | "
        "Median wall time including loading and output\n"
        "T/O: verification timeout; n/a: unavailable | Source: bench/compare/results.tsv",
        fontsize=9, color="#444444",
    )
    return figure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results", type=Path, default=REPO / "bench/compare/results.tsv",
    )
    parser.add_argument("--output-dir", type=Path, default=REPO / "docs/images")
    parser.add_argument("--context", type=Path,
                        default=REPO / "bench/compare/environment.json")
    args = parser.parse_args()
    try:
        results = read_results(args.results)
        context = json.loads(args.context.read_text())
        with args.results.open(newline="") as source:
            counts = {int(row["threads"]) for row in csv.DictReader(source, delimiter="\t")
                      if row["scale"] == "1" and int(row["threads"]) > 1}
        if counts != {context["parallel_threads"]}:
            raise ValueError("chart context does not match recorded thread count")
        # Validate every selected cell before publishing either chart.
        for _, _, workloads in CHARTS:
            relative_speeds(results, workloads)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        with plt.rc_context({"svg.hashsalt": "relic-performance"}):
            for name, title, workloads in CHARTS:
                figure = plot_chart(results, title, workloads, context)
                figure.savefig(
                    args.output_dir / f"performance-{name}.svg",
                    metadata={"Date": None},
                )
                plt.close(figure)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
