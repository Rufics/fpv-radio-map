"""Tests of the LCOV merger: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path, PurePosixPath

from sonar_lcov import Command, LcovRebaser, RepositoryPaths

REPORT = "TN:\nSF:projects/ui-kit/src/lib/a.ts\nDA:1,1\nend_of_record\n"


class LcovRebaserTest(unittest.TestCase):
    def test_prefixes_relative_sources(self):
        rebased = LcovRebaser(PurePosixPath("web")).rebase(REPORT)
        self.assertEqual(rebased, "TN:\nSF:web/projects/ui-kit/src/lib/a.ts\nDA:1,1\nend_of_record\n")

    def test_keeps_absolute_sources(self):
        line = "SF:/home/runner/work/a.ts\n"
        self.assertEqual(LcovRebaser(PurePosixPath("web")).rebase_line(line), line)

    def test_root_base_changes_nothing(self):
        self.assertEqual(LcovRebaser(PurePosixPath(".")).rebase(REPORT), REPORT)

    def test_adds_missing_final_newline(self):
        self.assertEqual(LcovRebaser(PurePosixPath("web")).rebase("end_of_record"), "end_of_record\n")


class RepositoryPathsTest(unittest.TestCase):
    def test_refuses_paths_outside_the_repository(self):
        with tempfile.TemporaryDirectory() as root, self.assertRaises(ValueError):
            RepositoryPaths(root).resolve("../elsewhere/lcov.info")

    def test_relative_folder(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(RepositoryPaths(root).relative("web/./"), PurePosixPath("web"))


class CommandTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for name in ("ui-kit", "fpv-app"):
            report = self.root / "web/coverage" / name / "lcov.info"
            report.parent.mkdir(parents=True)
            report.write_text(REPORT.replace("ui-kit", name), encoding="utf-8")

    def run_command(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return Command(self.root).run(list(argv))

    def test_merges_reports(self):
        code = self.run_command("--base", "web", "--output", "out/web.lcov",
                                "web/coverage/ui-kit/lcov.info", "web/coverage/fpv-app/lcov.info")
        self.assertEqual(code, 0)
        merged = (self.root / "out/web.lcov").read_text(encoding="utf-8")
        self.assertIn("SF:web/projects/ui-kit/src/lib/a.ts\n", merged)
        self.assertIn("SF:web/projects/fpv-app/src/lib/a.ts\n", merged)

    def test_output_outside_the_repository_is_a_usage_error(self):
        self.assertEqual(self.run_command("--base", "web", "--output", "../web.lcov",
                                          "web/coverage/ui-kit/lcov.info"), 2)

    def test_missing_report_is_a_usage_error(self):
        self.assertEqual(self.run_command("--base", "web", "--output", "out/web.lcov", "web/none.info"), 2)


if __name__ == "__main__":
    unittest.main()
