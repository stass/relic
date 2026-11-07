"""Checks for the README charts' cohort coverage and speed normalization."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "plot_performance", Path(__file__).with_name("plot-performance.py"),
)
charts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(charts)


class PerformanceChartTests(unittest.TestCase):
    def read_fixture(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.tsv"
            path.write_text(
                "scale\tthreads\tworkload\tengine\tstatus\tmedian\n" + rows,
            )
            return charts.read_results(path)

    def test_speed_is_inverse_time_and_statuses_are_preserved(self):
        results = {
            ("sample", "relic"): 2.0,
            ("sample", "souffle"): 4.0,
            ("sample", "souffle-magic"): 1.0,
            ("sample", "souffle-interp"): "timeout",
            ("sample", "duckdb"): "n/a",
        }
        speeds = charts.relative_speeds(results, ["sample"])
        self.assertEqual(speeds["sample", "relic"], 1)
        self.assertEqual(speeds["sample", "souffle"], 0.5)
        self.assertEqual(speeds["sample", "souffle-magic"], 2)
        self.assertEqual(speeds["sample", "souffle-interp"], "timeout")
        self.assertEqual(speeds["sample", "duckdb"], "n/a")

    def test_other_scales_and_thread_counts_are_excluded(self):
        results = self.read_fixture(
            "1\t1\tsample\trelic\tok\t99\n"
            "2\t16\tsample\trelic\tok\t88\n"
            "1\t16\tsample\trelic\tok\t2\n",
        )
        self.assertEqual(results, {("sample", "relic"): 2.0})

    def test_duplicate_cells_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.read_fixture("1\t16\tsample\trelic\tok\t2\n" * 2)

    def test_parallel_count_comes_from_the_recorded_cohort(self):
        results = self.read_fixture(
            "1\t1\tsample\trelic\tok\t99\n"
            "1\t64\tsample\trelic\tok\t2\n")
        self.assertEqual(results, {("sample", "relic"): 2.0})

    def test_mixed_parallel_cohorts_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "parallel"):
            self.read_fixture(
                "1\t16\tsample\trelic\tok\t2\n"
                "1\t64\tsample\trelic\tok\t3\n")

    def test_invalid_medians_are_rejected(self):
        for value in ("0", "-1", "nan", "inf", "bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.read_fixture(f"1\t16\tsample\trelic\tok\t{value}\n")

    def test_missing_selected_cell_is_rejected(self):
        with self.assertRaises(KeyError):
            charts.relative_speeds({("sample", "relic"): 2.0}, ["sample"])

    def test_every_recorded_workload_is_charted_once(self):
        results = charts.read_results(charts.REPO / "bench/compare/results.tsv")
        charts.check_coverage(results)
        extra = dict(results)
        extra["new-workload", "relic"] = 1.0
        with self.assertRaises(ValueError):
            charts.check_coverage(extra)
        missing = {k: v for k, v in results.items() if k[0] != "cspa"}
        with self.assertRaises(ValueError):
            charts.check_coverage(missing)

    def test_recorded_selection_has_all_five_engines(self):
        results = charts.read_results(charts.REPO / "bench/compare/results.tsv")
        for _, _, workloads in charts.CHARTS:
            speeds = charts.relative_speeds(results, workloads)
            self.assertEqual(len(speeds), len(workloads) * len(charts.ENGINES))
            context = {"parallel_threads": 16, "processor": "Test processor"}
            figure = charts.plot_chart(results, "Test", workloads, context)
            axes = figure.axes[0]
            self.assertEqual(len(axes.get_legend().get_texts()), 5)
            # Unavailable cells must not become zero-height bars.
            self.assertTrue(all(bar.get_height() > 0 for bar in axes.patches))
            self.assertTrue(any("16 threads | Test processor" in text.get_text()
                                for text in figure.texts))
            charts.plt.close(figure)


if __name__ == "__main__":
    unittest.main()
