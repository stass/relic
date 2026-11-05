#!/usr/bin/env python3
"""Publication validates synthetic evidence without external recorded results."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "publish_results", ROOT / "bench/compare/tools/publish-results.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.record = Path(temporary.name)
        self.metadata = {
            "processor": "Test CPU", "parallel_threads": 64,
            "physical_cores": 64, "logical_cpus": 128, "installed_memory_gib": 256,
            "os": "Test OS", "source_commit": "a" * 40,
            "output_policy": "stdout", "verification_timeout_seconds": 300,
            "version_strings": {key: "engine 1.0" for key in
                                ("relic", "swipl", "souffle", "clingo", "nemo",
                                 "duckdb", "sqlite", "hyperfine", "compiler")},
            "compiler": "Test compiler",
            "relic_configure": "--disable-shared --enable-threads", "relic_cflags": "-O3 -g",
        }
        self.workloads = {"generated": False, "external": True}
        self.rows = {}
        for scale, repetitions in publisher.RUNS.items():
            for threads in (1, 64):
                engines = publisher.ENGINES if threads == 1 else publisher.PARALLEL
                rows = []
                for workload in self.workloads:
                    if workload == "external" and scale > 1:
                        continue
                    for engine in engines:
                        row = dict.fromkeys(publisher.COLUMNS, "-")
                        row.update(workload=workload, engine=engine, threads=str(threads),
                                   scale=str(scale), status="ok", rows="2", median="1",
                                   fastest="1", spread="0", user="2", peak_bytes="16")
                        rows.append(row)
                        path = self.sample_path(scale, threads, workload, engine)
                        path.parent.mkdir(parents=True, exist_ok=True)
                        measurement = {"exit_code": 10 if engine == "clingo" else 0,
                                       "time_wall_clock": {"value": 1, "unit": "second"},
                                       "time_user": {"value": 2, "unit": "second"},
                                       "memory_peak_resident": {"value": 16, "unit": "byte"}}
                        path.write_text(json.dumps({"schema_version": 2, "results": [{
                            "command": f"{engine} {workload}",
                            "measurements": [measurement] * repetitions}]}))
                self.rows[scale, threads] = rows
                self.save_scenario(scale, threads)
                prefix = self.record / f"scale-{scale}-threads-{threads}"
                prefix.with_suffix(".exit-status").write_text("0\n")
                prefix.with_suffix(".txt").write_text(
                    f"# machine: Test CPU, Test OS\n"
                    f"# scale {scale}, {repetitions} timed runs, 300s to answer, {threads} threads\n"
                    "# timed answers: complete serialization to discarded stdout\n"
                    "# engine 1.0\n\nResults\n")
                prefix.with_suffix(".stderr.txt").write_text("")
        (self.record / "main-sweep-complete").touch()

    def sample_path(self, scale=1, threads=1, workload="generated", engine="relic"):
        return self.record / f"samples/scale-{scale}/threads-{threads}/{workload}.{engine}.json"

    def save_scenario(self, scale=1, threads=1):
        publisher.write_tsv(self.record / f"scale-{scale}-threads-{threads}.tsv",
                            self.rows[scale, threads], publisher.COLUMNS)

    def audit(self):
        return publisher.audit(self.record, self.metadata, self.workloads)

    def fingerprint(self):
        return {p.relative_to(self.record): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.record.rglob("*") if p.is_file()}

    def test_complete_sweep_is_read_only_and_preserves_scenario_boundaries(self):
        before = self.fingerprint()
        rows = self.audit()
        self.assertEqual(self.fingerprint(), before)
        self.assertEqual(len(rows), 70)
        self.assertEqual({r["threads"] for r in rows}, {"1", "64"})
        self.assertTrue(all(r["scale"] == "1" for r in rows if r["workload"] == "external"))
        self.assertEqual({r["timed_runs"] for r in rows}, {3, 10})

    def test_failed_samples_units_and_repetition_counts_are_rejected(self):
        path = self.sample_path()
        original = json.loads(path.read_text())
        for damage, expected in (("exit", "failed timed invocation"),
                                 ("units", "invalid sample metric"),
                                 ("count", "wrong repetition count")):
            with self.subTest(damage=damage):
                report = copy.deepcopy(original)
                samples = report["results"][0]["measurements"]
                if damage == "exit":
                    samples[0]["exit_code"] = 1
                elif damage == "units":
                    samples[0]["memory_peak_resident"]["unit"] = "kilobyte"
                else:
                    samples.pop()
                path.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, expected):
                    self.audit()

    def test_incorrect_summary_is_rejected(self):
        self.rows[1, 1][0]["median"] = "99"
        self.save_scenario()
        with self.assertRaisesRegex(ValueError, "differs from samples"):
            self.audit()

    def test_engine_versions_are_checked_only_for_participating_engines(self):
        self.metadata["version_strings"]["swipl"] = "serial-engine 2.0"
        for scale in publisher.RUNS:
            path = self.record / f"scale-{scale}-threads-1.txt"
            path.write_text(path.read_text().replace("# engine 1.0", "# engine 1.0\n# serial-engine 2.0"))
        self.assertEqual(len(self.audit()), 70)

    def test_metadata_cannot_relabel_another_output_policy_or_scenario(self):
        path = self.record / "scale-1-threads-64.txt"
        original = path.read_text()
        for old, new in (("discarded stdout", "answer files"), ("64 threads", "16 threads"),
                         ("10 timed runs", "3 timed runs"), ("300s to answer", "30s to answer")):
            with self.subTest(old=old):
                path.write_text(original.replace(old, new))
                with self.assertRaisesRegex(ValueError, "report context"):
                    self.audit()

    def test_duplicate_and_missing_rival_cells_are_rejected(self):
        self.rows[1, 1].append(dict(self.rows[1, 1][0]))
        self.save_scenario()
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.audit()
        self.rows[1, 1].pop()
        self.rows[1, 1].pop(1)
        self.save_scenario()
        with self.assertRaisesRegex(ValueError, "missing rival"):
            self.audit()

    def test_reference_timeout_leaves_explicit_unmeasured_rivals(self):
        reference = self.rows[1, 1][0]
        for metric in ("rows", "median", "fastest", "spread", "user", "peak_bytes"):
            reference[metric] = "-"
        reference["status"] = "timeout"
        self.rows[1, 1] = [r for r in self.rows[1, 1]
                           if r["workload"] != "generated" or r["engine"] == "relic"]
        self.save_scenario()
        rows = self.audit()
        unmeasured = [r for r in rows if r["workload"] == "generated" and
                      r["scale"] == "1" and r["threads"] == "1" and r["engine"] != "relic"]
        self.assertEqual(len(unmeasured), 8)
        self.assertTrue(all(r["status"] == "no-reference" and r["median"] == "-" and
                            r["timed_runs"] == 0 for r in unmeasured))

    def test_interruption_and_incorrect_exit_status_are_rejected(self):
        marker = self.record / "scale-1-threads-64.interrupted"
        marker.touch()
        with self.assertRaisesRegex(ValueError, "interrupted"):
            self.audit()
        marker.unlink()
        (self.record / "scale-1-threads-1.exit-status").write_text("1\n")
        with self.assertRaisesRegex(ValueError, "exit status"):
            self.audit()

    def test_raw_no_reference_cannot_hide_a_missing_timing(self):
        row = self.rows[1, 1][1]
        row["status"] = "no-reference"
        for metric in ("rows", "median", "fastest", "spread", "user", "peak_bytes"):
            row[metric] = "-"
        self.save_scenario()
        with self.assertRaisesRegex(ValueError, "raw no-reference"):
            self.audit()

    def test_capacity_classification_requires_the_exact_cell_diagnostic(self):
        row = next(r for r in self.rows[1, 1] if r["workload"] == "generated" and r["engine"] == "clingo")
        row["status"] = "failed"
        for metric in ("median", "fastest", "spread", "user", "peak_bytes"):
            row[metric] = "-"
        self.save_scenario()
        (self.record / "scale-1-threads-1.exit-status").write_text("1\n")
        diagnosis = {"scale": 1, "threads": 1, "workload": "generated", "engine": "clingo",
                     "exit_code": 65, "classified_status": "engine-limit",
                     "evidence": "scale-1-threads-1.stderr.txt",
                     "diagnostic": "Value too large to be stored in data type: Id out of range"}
        (self.record / "investigated-failures.json").write_text(json.dumps([diagnosis]))
        evidence = self.record / diagnosis["evidence"]
        evidence.write_text("compare: clingo failed on generated with status 65:\n" + diagnosis["diagnostic"])
        classified = [r for r in self.audit() if r["status"] == "engine-limit"]
        self.assertEqual(len(classified), 1)
        evidence.write_text(evidence.read_text().replace("on generated", "on external"))
        with self.assertRaisesRegex(ValueError, "missing cell diagnostic"):
            self.audit()

    def test_publication_preserves_editorial_sections_and_uses_host_context(self):
        root = self.record / "repo"
        (root / "website").mkdir(parents=True)
        readme = "Editorial introduction\n"
        for name in ("benchmark-summary", "benchmark-table", "benchmark-conditions"):
            readme += f"<!-- {name}:start -->\nold\n<!-- {name}:end -->\n"
        readme += "![Curated chart](chart.svg)\nEditorial conclusion\n"
        (root / "README.md").write_text(readme)
        page = '<h2 id="parallel-title">16 threads</h2>\n<caption>16 threads</caption>\n'
        page += '<div aria-label="Serial benchmark timings"></div>\n'
        for name in ("benchmark-summary", "benchmark-setup"):
            page += f"<!-- {name}:start -->\nold\n<!-- {name}:end -->\n"
        (root / "website/performance.html").write_text(page)
        before = self.fingerprint()
        summary, documents = publisher.publication(self.audit(), self.metadata, root)
        self.assertEqual(self.fingerprint(), before)
        self.assertEqual(len(summary), 10)
        self.assertIn("![Curated chart](chart.svg)", documents[root / "README.md"])
        self.assertTrue(documents[root / "README.md"].startswith("Editorial introduction\n"))
        self.assertTrue(documents[root / "README.md"].endswith("Editorial conclusion\n"))
        self.assertIn("64 threads", documents[root / "website/performance.html"])
        self.assertIn('aria-label="Serial benchmark timings"', documents[root / "website/performance.html"])
        self.assertIn("Test CPU", documents[root / "BENCHMARKS.md"])
        self.assertIn("discarded stdout for every engine", documents[root / "README.md"])


if __name__ == "__main__":
    unittest.main()
