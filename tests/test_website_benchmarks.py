"""Check benchmark chart provenance, missing results and comparison eligibility."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("export_benchmarks", ROOT / "bench/compare/tools/export-website.py")
benchmarks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmarks)


class WebsiteBenchmarkTests(unittest.TestCase):
    def read(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.tsv"
            path.write_text("scale\tthreads\tworkload\tengine\tstatus\tmedian\n" + rows)
            return benchmarks.read_measurements(path)

    def test_statuses_do_not_become_timings(self):
        for status in benchmarks.STATUSES - {"ok"}:
            with self.subTest(status=status):
                result = self.read(f"1\t16\twork\tsouffle\t{status}\t-\n")
                self.assertEqual(result[(1, 16, "work", "souffle")],
                                 {"status": status, "seconds": None})

    def test_invalid_completed_times_and_duplicate_cells_fail(self):
        for value in ("nan", "inf", "-1", "0", "bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.read(f"1\t16\twork\trelic\tok\t{value}\n")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.read("1\t16\twork\trelic\tok\t1\n" * 2)
        with self.assertRaisesRegex(ValueError, "unsuccessful"):
            self.read("1\t16\twork\tsouffle\ttimeout\t300\n")

    def test_profiles_do_not_mix_scales_or_thread_counts(self):
        cells = self.read(
            "1\t1\tserial\trelic\tok\t1\n"
            "1\t16\tparallel\trelic\tok\t2\n"
            "8\t16\tlarge\trelic\tok\t3\n")
        self.assertEqual(set(benchmarks.profile(cells, 1, 1)), {"serial"})
        self.assertEqual(set(benchmarks.profile(cells, 1, 16)), {"parallel"})

    def test_recorded_report_matches_exported_profiles(self):
        data = benchmarks.export(ROOT / "bench/compare/results.tsv")
        self.assertEqual(set(data["profiles"]), {"1", "16"})
        for threads, expected_wins in (("1", 22), ("16", 26)):
            workloads = data["profiles"][threads]
            self.assertEqual(len(workloads), 31)
            wins = 0
            for engines in workloads.values():
                rivals = [cell["seconds"] for name, cell in engines.items()
                          if name != "relic" and cell["status"] == "ok"]
                wins += engines["relic"]["seconds"] <= min(rivals)
            self.assertEqual(wins, expected_wins)
        self.assertEqual(data["profiles"]["16"]["cspa"]["souffle-magic"]["status"], "ok")
        self.assertEqual(data["profiles"]["16"]["andersen"]["souffle-magic"]["status"], "timeout")


if __name__ == "__main__":
    unittest.main()
