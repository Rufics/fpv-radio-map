import unittest

from perfkit.compare import Baseline, Comparer, Verdict
from perfkit.history_test import HistoryTestCase, measurement, record
from perfkit.measure import HIGHER, LOWER, Measurement

INVOLVED = {"benchmark/rf-core": "1.2.0", "collector/google-benchmark": "1.0.0"}


class ComparerTest(HistoryTestCase):
    def add_runs(self, values, version="1.0.0"):
        for day, value in enumerate(values, start=1):
            self.history.record(record(f"{version}-{day}", f"2026-01-{day:02d}T00:00:00Z", [measurement(value)],
                                       version=version))

    def compare(self, value, window=10, limit=5.0):
        comparer = Comparer(Baseline(self.history, "h1", "none", "2026-02-01T00:00:00Z"), window=window)
        return comparer.compare(measurement(value), INVOLVED, limit, True)

    def test_no_baseline_below_three_runs(self):
        self.add_runs([100, 100])
        found = self.compare(200)
        self.assertEqual((found.verdict, found.runs, found.baseline, found.worse_pct),
                         (Verdict.NO_BASELINE, 2, None, None))

    def test_verdicts_against_the_median(self):
        self.add_runs([100, 90, 110, 1000])
        self.assertEqual(self.compare(111).verdict, Verdict.REGRESSION)
        self.assertEqual(self.compare(110).verdict, Verdict.OK)
        found = self.compare(90)
        self.assertEqual((found.verdict, found.baseline, found.runs), (Verdict.IMPROVEMENT, 105.0, 4))
        self.assertAlmostEqual(found.worse_pct, -14.285714, places=5)

    def test_window_takes_the_newest_runs(self):
        self.add_runs([500, 500, 500, 100, 100, 100])
        self.assertEqual(self.compare(100, window=3).baseline, 100)

    def test_other_major_versions_are_not_a_baseline(self):
        self.add_runs([100, 100, 100], version="2.0.0")
        self.assertEqual(self.compare(100).verdict, Verdict.NO_BASELINE)
        self.add_runs([100, 100, 100], version="1.1.0")
        self.assertEqual(self.compare(100).verdict, Verdict.OK)

    def test_worse_pct(self):
        self.assertEqual(Comparer.worse_pct(110, 100, LOWER), 10.0)
        self.assertEqual(Comparer.worse_pct(110, 100, HIGHER), -10.0)
        self.assertEqual(Comparer.worse_pct(0, 0, LOWER), 0.0)
        self.assertEqual(Comparer.worse_pct(5, 0, LOWER), 100.0)
        self.assertEqual(Comparer.worse_pct(5, 0, HIGHER), -100.0)

    def test_higher_is_better(self):
        comparer = Comparer(Baseline(self.history, "h1", "none", "2026-02-01T00:00:00Z"))
        for day in range(1, 4):
            rate = Measurement("benchmark/rf-core", "BM_A", "items_per_second", "1/s", HIGHER, 1000.0)
            self.history.record(record(f"r{day}", f"2026-01-{day:02d}T00:00:00Z", [rate]))
        slower = Measurement("benchmark/rf-core", "BM_A", "items_per_second", "1/s", HIGHER, 900.0)
        self.assertEqual(comparer.compare(slower, INVOLVED, 5.0, False).verdict, Verdict.REGRESSION)


if __name__ == "__main__":
    unittest.main()
