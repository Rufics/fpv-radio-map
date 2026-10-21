import unittest

from perfkit.timeline import Timeline, utc_now


class FakeClock:
    def __init__(self):
        self.ticks = 0

    def __call__(self) -> str:
        self.ticks += 1
        return f"2026-01-01T00:00:{self.ticks:02d}.000Z"


class TimelineTest(unittest.TestCase):
    def test_phases_nest_and_keep_start_order(self):
        timeline = Timeline(FakeClock())
        with timeline.phase("micro run", "run"), timeline.phase("build", "build", target="bench") as build:
            self.assertEqual(build.finished, "")
        names = [(e.name, e.started, e.finished, e.outcome) for e in timeline.events()]
        self.assertEqual(names, [("micro run", "2026-01-01T00:00:01.000Z", "2026-01-01T00:00:04.000Z", "ok"),
                                 ("build", "2026-01-01T00:00:02.000Z", "2026-01-01T00:00:03.000Z", "ok")])
        self.assertEqual(timeline.to_json()[1]["attributes"], {"target": "bench"})

    def test_failed_phase_is_closed_and_marked(self):
        timeline = Timeline(FakeClock())
        with self.assertRaises(ValueError), timeline.phase("bench", "benchmark"):
            raise ValueError("boom")
        event = timeline.events()[0]
        self.assertEqual((event.outcome, event.finished), ("failed", "2026-01-01T00:00:02.000Z"))

    def test_outcome_set_inside_the_phase_stays(self):
        timeline = Timeline(FakeClock())
        with timeline.phase("load", "scenario") as event:
            event.outcome = "thresholds crossed"
        with self.assertRaises(KeyboardInterrupt), timeline.phase("stop", "stand"):
            raise KeyboardInterrupt
        self.assertEqual([e.outcome for e in timeline.events()], ["thresholds crossed", "failed"])

    def test_utc_now_format(self):
        self.assertRegex(utc_now(), r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")


if __name__ == "__main__":
    unittest.main()
