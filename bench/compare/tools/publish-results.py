#!/usr/bin/env python3
"""Audit a complete stdout-output sweep and publish its tables and context."""

import argparse
import csv
import html
import json
import math
from pathlib import Path
import re
import statistics


ROOT = Path(__file__).resolve().parents[3]
ENGINES = ("relic", "swipl", "souffle-interp", "souffle", "souffle-magic",
           "clingo", "nemo", "duckdb", "sqlite")
PARALLEL = ("relic", "souffle-interp", "souffle", "souffle-magic", "duckdb")
LABELS = {"relic": "relic", "swipl": "SWI-Prolog", "souffle-interp": "SF interp",
          "souffle": "Souffle compiled", "souffle-magic": "Souffle Magic",
          "clingo": "clingo", "nemo": "Nemo", "duckdb": "DuckDB", "sqlite": "SQLite"}
RUNS = {1: 10, 2: 3, 4: 3, 8: 3}
COLUMNS = ("workload", "engine", "threads", "scale", "status", "rows", "median",
           "fastest", "spread", "user", "peak_bytes", "compile_seconds")


def read_tsv(path):
    with path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != list(COLUMNS):
            raise ValueError(f"unexpected result columns: {path.name}")
        return list(reader)


def validate_samples(row, reference, path, requested):
    """Recompute every summary from successful invocations with explicit units."""
    report = json.loads(path.read_text())
    if report.get("schema_version") != 2 or len(report.get("results", [])) != 1:
        raise ValueError(f"invalid sample schema: {path}")
    result = report["results"][0]
    if result["command"] != f"{row['engine']} {row['workload']}":
        raise ValueError(f"wrong sample command: {path}")
    samples = result["measurements"]
    if len(samples) != requested:
        raise ValueError(f"wrong repetition count: {path}")
    for sample in samples:
        successful = {10, 30} if row["engine"] == "clingo" else {0}
        if sample["exit_code"] not in successful:
            raise ValueError(f"failed timed invocation: {path}")
        for metric, unit in (("time_wall_clock", "second"), ("time_user", "second"),
                             ("memory_peak_resident", "byte")):
            value = sample[metric]["value"]
            if sample[metric]["unit"] != unit or not math.isfinite(value) or value < 0:
                raise ValueError(f"invalid sample metric: {path} {metric}")
    if reference["status"] != "ok" or row["rows"] != reference["rows"] or int(row["rows"]) <= 0:
        raise ValueError(f"missing verified reference or differing row count: {path}")
    times = [sample["time_wall_clock"]["value"] for sample in samples]
    summaries = {
        "median": statistics.median(times), "fastest": min(times),
        "spread": statistics.stdev(times),
        "user": statistics.mean(sample["time_user"]["value"] for sample in samples),
        "peak_bytes": max(sample["memory_peak_resident"]["value"] for sample in samples),
    }
    for metric, value in summaries.items():
        reported = float(row[metric])
        if not math.isfinite(reported) or reported < 0 or (metric == "median" and reported == 0):
            raise ValueError(f"invalid summary: {path} {metric}")
        if not math.isclose(reported, value, rel_tol=1e-8, abs_tol=1e-10):
            raise ValueError(f"summary differs from samples: {path} {metric}")


def classify_failure(row, scale, threads, record, resolutions):
    """Accept capacity diagnoses only with matching cell-specific diagnostics."""
    key = (scale, threads, row["workload"], row["engine"])
    resolution = resolutions.pop(key, None)
    if row["status"] != "failed" or resolution is None:
        raise ValueError(f"uninvestigated outcome: {key} {row['status']}")
    status = resolution["classified_status"]
    signatures = {("clingo", "engine-limit", 65):
                  "Value too large to be stored in data type: Id out of range",
                  ("duckdb", "resource-limit", 1): "Out of Memory Error:"}
    signature = signatures.get((row["engine"], status, resolution["exit_code"]))
    if signature is None:
        raise ValueError(f"unsupported capacity diagnosis: {key}")
    evidence = f"scale-{scale}-threads-{threads}.stderr.txt"
    if resolution["evidence"] != evidence or not resolution["diagnostic"]:
        raise ValueError(f"incorrect diagnostic source: {key}")
    marker = f"compare: {row['engine']} failed on {row['workload']} with status {resolution['exit_code']}:"
    diagnostic = (record / evidence).read_text()
    if marker not in diagnostic:
        raise ValueError(f"missing cell diagnostic: {key}")
    block = diagnostic.split(marker, 1)[1].split("\ncompare:", 1)[0]
    if signature not in block or resolution["diagnostic"] not in block:
        raise ValueError(f"unsupported diagnostic: {key}")
    return status


def audit(record, metadata, workloads):
    """Require every scenario and cell before accepting any publication output."""
    parallel = metadata["parallel_threads"]
    if type(parallel) is not int or not 2 <= parallel <= 256:
        raise ValueError("invalid parallel thread count")
    if metadata["output_policy"] != "stdout" or metadata["verification_timeout_seconds"] != 300:
        raise ValueError("unsupported output policy or verification timeout")
    if not re.fullmatch(r"[0-9a-f]{40}", metadata["source_commit"]):
        raise ValueError("invalid source revision")
    if not (record / "main-sweep-complete").exists() or list(record.glob("*.interrupted")):
        raise ValueError("incomplete or interrupted sweep")
    resolution_path = record / "investigated-failures.json"
    entries = json.loads(resolution_path.read_text()) if resolution_path.exists() else []
    resolutions = {(r["scale"], r["threads"], r["workload"], r["engine"]): r for r in entries}
    if len(resolutions) != len(entries):
        raise ValueError("duplicate capacity diagnosis")
    accepted = []
    for scale, requested in RUNS.items():
        for threads in (1, parallel):
            name = f"scale-{scale}-threads-{threads}"
            rows = read_tsv(record / f"{name}.tsv")
            engines = ENGINES if threads == 1 else PARALLEL
            selected = {w for w, external in workloads.items() if scale == 1 or not external}
            expected = {(w, e) for w in selected for e in engines}
            index = {(r["workload"], r["engine"]): r for r in rows}
            if len(index) != len(rows) or not index.keys() <= expected:
                raise ValueError(f"duplicate or unexpected cell: {name}")
            if any(r["status"] == "no-reference" for r in rows):
                raise ValueError(f"raw no-reference status: {name}")
            failed = any(r["status"] in {"failed", "mismatch", "empty"} for r in rows)
            if int((record / f"{name}.exit-status").read_text()) != int(failed):
                raise ValueError(f"unexpected harness exit status: {name}")
            header = (record / f"{name}.txt").read_text().split("\n\n", 1)[0]
            required_header = (
                f"# scale {scale}, {requested} timed runs, 300s to answer, {threads} threads",
                "# timed answers: complete serialization to discarded stdout",
            )
            if any(line not in header.splitlines() for line in required_header):
                raise ValueError(f"report context does not match metadata: {name}")
            machine = header.splitlines()[0]
            if metadata["processor"] not in machine or metadata["os"] not in machine:
                raise ValueError(f"report context has different hardware or OS: {name}")
            version_keys = {"souffle" if e.startswith("souffle") else e for e in engines}
            version_keys.update(("hyperfine", "compiler"))
            for key in sorted(version_keys):
                version = metadata["version_strings"][key]
                if version not in header:
                    raise ValueError(f"version missing from report: {name}: {version}")
            for workload, engine in sorted(expected):
                row = index.get((workload, engine))
                reference = index.get((workload, "relic"))
                if reference is None:
                    raise ValueError(f"missing reference cell: {name} {workload}")
                if row is None:
                    if reference["status"] != "timeout":
                        raise ValueError(f"missing rival cell: {name} {workload} {engine}")
                    row = dict.fromkeys(COLUMNS, "-")
                    row.update(workload=workload, engine=engine, scale=str(scale),
                               threads=str(threads), status="no-reference")
                if int(row["scale"]) != scale or int(row["threads"]) != threads:
                    raise ValueError(f"incorrect scenario coordinates: {name}")
                status = row["status"]
                if status == "ok":
                    sample = record / f"samples/scale-{scale}/threads-{threads}/{workload}.{engine}.json"
                    validate_samples(row, reference, sample, requested)
                elif status in {"failed", "mismatch", "empty", "no-data"}:
                    row["status"] = classify_failure(row, scale, threads, record, resolutions)
                elif status not in {"timeout", "n/a", "no-reference"}:
                    raise ValueError(f"unknown result status: {status}")
                elif any(row[metric] != "-" for metric in ("median", "fastest", "spread", "user", "peak_bytes")):
                    raise ValueError(f"unmeasured cell contains timing values: {name} {workload} {engine}")
                row["timed_runs_requested"] = requested
                row["timed_runs"] = requested if status == "ok" else 0
                accepted.append(row)
    if resolutions:
        raise ValueError("unused capacity diagnosis")
    return accepted


def comparison(rows):
    reference = next((r for r in rows if r["engine"] == "relic" and r["status"] == "ok"), None)
    rivals = [r for r in rows if r["engine"] != "relic" and r["status"] == "ok"]
    rival = min(rivals, key=lambda r: float(r["median"])) if rivals else None
    return reference, rival


def time(seconds):
    value = float(seconds)
    if value < .001:
        return f"{value * 1000000:.3g} us"
    if value < 1:
        return f"{value * 1000:.3g} ms"
    return f"{value:.3g} s"


def metric_table(rows, metric):
    engines = ENGINES if int(rows[0]["threads"]) == 1 else PARALLEL
    lines = ["| Workload | " + " | ".join(LABELS[e] for e in engines) + " |",
             "| --- | " + " | ".join("---:" for _ in engines) + " |"]
    for workload in sorted({r["workload"] for r in rows}):
        index = {r["engine"]: r for r in rows if r["workload"] == workload}
        values = [float(r[metric]) for r in index.values() if r["status"] == "ok"]
        minimum = min(values) if values else None
        cells = []
        for engine in engines:
            row = index[engine]
            if row["status"] != "ok":
                cells.append(row["status"])
                continue
            value = float(row[metric])
            text = (f"{value / 1073741824:.3g} GiB" if value >= 1073741824 else
                    f"{value / 1048576:.3g} MiB") if metric == "peak_bytes" else time(value)
            cells.append(f"**{text}**" if metric == "median" and value == minimum else text)
        lines.append("| `" + workload + "` | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def summaries(rows):
    result = []
    for scale, threads, workload in sorted({(int(r["scale"]), int(r["threads"]), r["workload"]) for r in rows}):
        selected = [r for r in rows if (int(r["scale"]), int(r["threads"]), r["workload"]) == (scale, threads, workload)]
        reference, rival = comparison(selected)
        result.append(dict(scale=scale, threads=threads, workload=workload,
                           relic_median_seconds=reference["median"] if reference else "-",
                           best_other_engine=rival["engine"] if rival else "-",
                           best_other_median_seconds=rival["median"] if rival else "-"))
    return result


def write_tsv(path, rows, columns):
    with path.open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=columns, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def replace_block(text, name, replacement):
    """Bound generated prose so publishing preserves editorial content."""
    pattern = rf"(<!-- {name}:start -->\n).*?(\n<!-- {name}:end -->)"
    result, count = re.subn(pattern, lambda m: m[1] + replacement + m[2], text, flags=re.S)
    if count != 1:
        raise ValueError(f"expected one generated block: {name}")
    return result


def scenario_overview(rows, summary, parallel):
    """Count wins only when both relic and at least one rival completed."""
    overview = ["| Scale | Threads | Repetitions | relic lowest median / comparable workloads | Rival cells without a completed time |",
                "| ---: | ---: | ---: | ---: | ---: |"]
    wins = {}
    for scale in RUNS:
        for threads in (1, parallel):
            selected = [r for r in summary if (r["scale"], r["threads"]) == (scale, threads)]
            comparable = [r for r in selected if r["relic_median_seconds"] != "-" and r["best_other_median_seconds"] != "-"]
            won = sum(float(r["relic_median_seconds"]) <= float(r["best_other_median_seconds"]) for r in comparable)
            wins[scale, threads] = (won, len(comparable))
            incomplete = sum(r["engine"] != "relic" and r["status"] not in {"ok", "n/a"}
                             for r in rows if (int(r["scale"]), int(r["threads"])) == (scale, threads))
            overview.append(f"| {scale} | {threads} | {RUNS[scale]} | {won} / {len(comparable)} | {incomplete} |")
    return overview, wins


def render_report(rows, metadata, overview):
    parallel = metadata["parallel_threads"]
    context = (f"{metadata['processor']}: **{metadata['physical_cores']} physical cores, "
               f"{metadata['logical_cpus']} hardware threads, {metadata['installed_memory_gib']} GiB installed RAM**.\n"
               f"OS: {metadata['os']}. Parallel runs request {parallel} workers; placement follows the OS scheduler.\n"
               f"Source revision: `{metadata['source_commit']}`.\n")
    method = ("Scale 1 uses ten repetitions and all 31 workloads, including three fixed Graspan datasets. "
              "Scales 2, 4 and 8 use three repetitions and the 28 generated workloads. "
              "Scaling follows each generator; tree depth increases with scale, so tree size grows exponentially.\n\n"
              "Every engine's complete answer is checked as an exact set against relic before timing. "
              "Times include process startup, input loading, evaluation and serialization of the complete answer "
              "to discarded stdout for every engine. Verification and compilation are excluded. "
              "User CPU time sums across threads. Peak memory is the maximum observed across timed runs. "
              "Small timing differences should be read alongside the recorded spread.\n\n"
              f"relic is configured with `{metadata['relic_configure']}` and `CFLAGS='{metadata['relic_cflags']}'`. "
              f"Compiler: {metadata['compiler']}. Generated Souffle programs use `-O3` and OpenMP.\n\n"
              "Engine versions: " + "; ".join(metadata["version_strings"].values()) + ".\n\n"
              "`timeout` means verification exceeded 300 seconds; it is not a timed 300-second result. "
              "`n/a` means no supported program; `no-reference` means relic did not finish verification. "
              "`engine-limit` denotes an investigated engine capacity limit; `resource-limit` denotes "
              "investigated memory exhaustion. Unavailable outcomes are excluded from rankings. "
              "Bold marks the lowest observed median without asserting statistical significance. "
              "A comparable workload has a completed relic time and at least one completed rival time.\n\n"
              "The machine is idle at launch apart from the benchmark driver and background OS work. "
              "Build logs, executable hashes, start/end context, exact source revision and raw samples "
              "are retained with the measurement cohort. CPU frequency and thermal control follow the host OS. "
              "These results describe this host; operating systems, compilers, engine versions and output policy "
              "differ from the Apple M3 Max study, so cross-host differences cannot be attributed solely to hardware.\n")
    build_table = ["| Engine | Build settings |", "| --- | --- |"]
    build_table.extend(f"| {engine} | {settings} |"
                       for engine, settings in metadata["engine_builds"].items())
    full = ["# Benchmark results", "", context, "## Comparison overview", "", "\n".join(overview), "",
            "## Method and limits", "", method, "## Engine builds", "",
            metadata["optimization_policy"], "", "\n".join(build_table), "", "## Full comparisons", ""]
    for scale in RUNS:
        for threads in (1, parallel):
            selected = [r for r in rows if (int(r["scale"]), int(r["threads"])) == (scale, threads)]
            full.extend([f"### Scale {scale}, {threads} thread" + ("s" if threads != 1 else ""), "",
                         "**Median wall time**", "", metric_table(selected, "median"), "",
                         "<details>", "<summary>Mean user CPU time and peak resident memory</summary>", "",
                         "**Mean user CPU time**", "", metric_table(selected, "user"), "",
                         "**Maximum peak resident memory across timed runs**", "",
                         metric_table(selected, "peak_bytes"), "", "</details>", ""])
    full.extend(["## Raw results and reproduction", "",
                 "[Canonical results](bench/compare/results.tsv) record every cell, timing spread, memory, answer size, "
                 "compilation time and requested/accepted repetitions. [Comparison summary](bench/compare/suite-summary.tsv) "
                 "names each scenario's fastest completed rival. [Environment](bench/compare/environment.json) records the cohort context.", "",
                 "Use the [FreeBSD build instructions](bench/compare/tools/README-freebsd.md). "
                 "Run the full suite on an idle host with the documented engine paths:", "", "```sh",
                 f"sh bench/compare/sweep.sh -j {parallel} -o /path/to/fresh-cohort", "```", "",
                 "Keep every scenario, raw sample and diagnostic together. Supply `environment.json` beside the cohort, "
                 "then audit and publish with:", "", "```sh",
                 "python3 bench/compare/tools/publish-results.py --record /path/to/cohort --check",
                 "python3 bench/compare/tools/publish-results.py --record /path/to/cohort",
                 "python3.15 bench/compare/tools/plot-performance.py",
                 "python3 bench/compare/tools/export-website.py", "```", "",
                 "The Apple M3 Max file-output study is preserved in git history. "
                 "Its timings are a separate cohort and are not merged into these tables.", ""])
    return "\n".join(full)


def readme_table(rows, parallel):
    """Rank the scale-1 workloads by relic's ratio to the fastest rival."""
    selected = [r for r in rows if (int(r["scale"]), int(r["threads"])) == (1, parallel)]
    rankings = []
    for workload in sorted({r["workload"] for r in selected}):
        cells = [r for r in selected if r["workload"] == workload]
        reference, rival = comparison(cells)
        ratio = float(rival["median"]) / float(reference["median"]) if reference and rival else None
        rankings.append((workload, cells, reference, rival, ratio))
    table = ["| Workload | relic | Souffle Magic | Souffle compiled | Fastest rival | relic speedup |",
             "| --- | ---: | ---: | ---: | --- | ---: |"]
    for workload, cells, reference, rival, ratio in sorted(rankings, key=lambda item: -(item[4] or 0)):
        index = {r["engine"]: r for r in cells}
        values = [time(index[e]["median"]) if index[e]["status"] == "ok" else index[e]["status"]
                  for e in ("relic", "souffle-magic", "souffle")]
        if ratio is not None and ratio >= 1:
            values[0] = f"**{values[0]}**"
        fastest = f"{LABELS[rival['engine']]}: {time(rival['median'])}" if rival else "unavailable"
        speedup = f"{ratio:.1f}x" if ratio is not None else "unavailable"
        table.append(f"| `{workload}` | " + " | ".join(values + [fastest, speedup]) + " |")
    return "\n".join(table)


def render_readme(rows, metadata, introduction, root):
    parallel = metadata["parallel_threads"]
    conditions = (f"These are scale-1 medians with {parallel} threads on {metadata['processor']} with "
                  f"{metadata['installed_memory_gib']} GiB RAM. Times include startup, loading, evaluation and "
                  "complete answer serialization to discarded stdout for every engine; compilation is excluded.")
    readme = (root / "README.md").read_text()
    for name, replacement in (("benchmark-summary", introduction), ("benchmark-table", readme_table(rows, parallel)),
                              ("benchmark-conditions", conditions)):
        readme = replace_block(readme, name, replacement)
    return readme


def render_website(metadata, introduction, root):
    parallel = metadata["parallel_threads"]
    page = (root / "website/performance.html").read_text()
    page = replace_block(page, "benchmark-summary", "<p>" + html.escape(introduction.replace("**", "")) + "</p>")
    setup = (f"Native command-line results: {metadata['processor']}, {metadata['physical_cores']} physical cores, "
             f"{metadata['logical_cpus']} hardware threads, {metadata['installed_memory_gib']} GiB RAM, {metadata['os']}. "
             "Scale 1, ten repetitions per timing. Medians include startup, loading, evaluation and complete answer "
             "serialization to discarded stdout for every engine; compilation is excluded. Serial runs include "
             f"all engines; {parallel}-thread runs compare relic, three Souffle modes and DuckDB. "
             "Unavailable measurements remain statuses. These are native CLI measurements.")
    page = replace_block(page, "benchmark-setup", "<p>" + html.escape(setup) + "</p>")
    page = re.sub(r'(<h2 id="parallel-title">)\d+ threads', rf'\g<1>{parallel} threads', page)
    page = re.sub(r'(<caption>)\d+ threads', rf'\g<1>{parallel} threads', page)
    return page


def publication(rows, metadata, root):
    """Prepare every document in memory before writing the validated cohort."""
    parallel = metadata["parallel_threads"]
    summary = summaries(rows)
    overview, wins = scenario_overview(rows, summary, parallel)
    serial_wins, serial_total = wins[1, 1]
    parallel_wins, parallel_total = wins[1, parallel]
    introduction = (f"On {metadata['processor']} with {metadata['installed_memory_gib']} GiB RAM, relic records the "
                    f"**lowest median wall time on {parallel_wins} of {parallel_total} comparable workloads with {parallel} threads**, "
                    f"and **{serial_wins} of {serial_total} with one thread**, against completed rival runs. "
                    "The suite compares compiled Souffle (with and without Magic Sets), interpreted Souffle, "
                    "Nemo, SWI-Prolog, clingo, DuckDB and SQLite.")
    return summary, {
        root / "README.md": render_readme(rows, metadata, introduction, root),
        root / "BENCHMARKS.md": render_report(rows, metadata, overview),
        root / "website/performance.html": render_website(metadata, introduction, root),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        metadata = json.loads((args.record / "environment.json").read_text())
        workloads = {p.parent.name: p.read_text().strip().startswith("graspan-")
                     for p in (ROOT / "bench/compare").glob("*/dataset")}
        rows = audit(args.record, metadata, workloads)
        summary, documents = publication(rows, metadata, ROOT)
        if not args.check:
            write_tsv(ROOT / "bench/compare/results.tsv", rows,
                      (*COLUMNS, "timed_runs_requested", "timed_runs"))
            write_tsv(ROOT / "bench/compare/suite-summary.tsv", summary, list(summary[0]))
            (ROOT / "bench/compare/environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
            for path, text in documents.items():
                path.write_text(text)
        print(f"Validated {len(rows)} cells and every successful timing sample" +
              ("; publication unchanged" if args.check else "; published results"))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
