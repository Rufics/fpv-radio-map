import sqlite3
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

from perfkit.errors import PerfError
from perfkit.history import HISTORY_NAME, History, RunRecord
from perfkit.measure import LOWER, Measurement
from perfkit.timeline import Event


def measurement(value: float, name: str = "BM_A") -> Measurement:
    return Measurement("benchmark/rf-core", name, "cpu_time", "ns", LOWER, value)


def record(run_id: str, started: str, values: list[Measurement], *, host: str = "h1", dataset: str = "none",
           version: str = "1.0.0") -> RunRecord:
    return RunRecord(run_id, "micro", started, started, host, dataset, "abc",
                     {"benchmark/rf-core": version, "collector/google-benchmark": "1.0.0"}, {"host": {}},
                     values, [Event("micro run", "run", started, started, "ok")])


class HistoryTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name) / "results"
        self.history = History.open(self.folder)
        self.addCleanup(self.history.close)


class HistoryTest(HistoryTestCase):
    def test_past_values_filter_host_dataset_and_time(self):
        self.history.record(record("r1", "2026-01-01T00:00:01Z", [measurement(10), measurement(1, "BM_B")]))
        self.history.record(record("r2", "2026-01-02T00:00:01Z", [measurement(11)]))
        self.history.record(record("r3", "2026-01-03T00:00:01Z", [measurement(50)], host="h2"))
        self.history.record(record("r4", "2026-01-04T00:00:01Z", [measurement(60)], dataset="warsaw"))
        past = self.history.past(measurement(0), "h1", "none", "2026-01-05T00:00:00Z")
        self.assertEqual([(p.run_id, p.value) for p in past], [("r2", 11), ("r1", 10)])
        self.assertEqual(past[0].versions["benchmark/rf-core"], "1.0.0")
        self.assertEqual(self.history.past(measurement(0), "h1", "none", "2026-01-02T00:00:00Z")[0].run_id, "r1")

    def test_same_majors(self):
        versions = {"benchmark/rf-core": "1.4.0", "collector/google-benchmark": "2.0.0"}
        self.assertTrue(History.same_majors(versions, {"benchmark/rf-core": "1.0.3"}))
        self.assertFalse(History.same_majors(versions, {"benchmark/rf-core": "2.0.0"}))
        self.assertFalse(History.same_majors(versions, {"benchmark/other": "1.0.0"}))

    def test_events_are_stored(self):
        self.history.record(record("r1", "2026-01-01T00:00:01Z", [measurement(10)]))
        with closing(sqlite3.connect(self.folder / HISTORY_NAME)) as connection:
            rows = connection.execute("select name, kind, outcome from events").fetchall()
        self.assertEqual(rows, [("micro run", "run", "ok")])

    def test_reopen_keeps_runs_and_unknown_schema_fails(self):
        self.history.record(record("r1", "2026-01-01T00:00:01Z", [measurement(10)]))
        self.history.close()
        again = History.open(self.folder)
        self.assertEqual(len(again.past(measurement(0), "h1", "none", "2027")), 1)
        again.close()
        connection = sqlite3.connect(self.folder / HISTORY_NAME)
        connection.execute("pragma user_version = 99")
        connection.close()
        with self.assertRaisesRegex(PerfError, "schema 99"):
            History.open(self.folder)

    def test_results_folder_is_a_file(self):
        (self.folder / "file").write_text("x", encoding="utf-8")
        with self.assertRaises(PerfError):
            History.open(self.folder / "file")


if __name__ == "__main__":
    unittest.main()
