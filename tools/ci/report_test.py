"""Tests of the CI report builder: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import io
import json
import os
import re
import runpy
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import report
from report import (MAX_LISTED, MARKER, build_report, changed_line_coverage, component_of, md, normalize_jobs,
                    parse_coverage, parse_diff, parse_junit, read_capped, read_suites)


A_CPP = "core/rf-core/src/a.cpp"


def gcovr(*files):
    return json.dumps({"gcovr/format_version": "0.6", "files": list(files)})


def cov_file(path, hits, fns=()):
    return {
        "file": path,
        "lines": [{"line_number": n, "count": c, "branches": []} for n, c in hits.items()],
        "functions": [{"name": name, "lineno": i + 1, "execution_count": c} for i, (name, c) in enumerate(fns)],
    }


class JunitTest(unittest.TestCase):
    def test_totals_and_failed_names(self):
        r = parse_junit('<testsuites tests="3"><testcase classname="A" name="ok"/>'
                        '<testcase classname="A" name="bad"><failure/></testcase>'
                        '<testcase name="off"><skipped/></testcase></testsuites>')
        self.assertEqual(r, {"total": 3, "failed": ["A › bad"], "skipped": 1, "passed": 1})


class CoverageTest(unittest.TestCase):
    def test_paths_are_grouped_into_areas(self):
        self.assertEqual(component_of(A_CPP), ("Core", "rf-core"))
        self.assertEqual(component_of("core/rf-ext/src/a.cpp"), ("Core", "rf-ext"))
        self.assertEqual(component_of("modules/fpv/relay/src/relay.cpp"), ("FPV modules", "fpv.relay"))
        self.assertEqual(component_of("modules/fpv/common/include/fpv/world.hpp"), ("FPV modules", "fpv common"))
        self.assertEqual(component_of("platform/rfmod/src/registry.cpp"), ("Platform", "rfmod"))
        self.assertEqual(component_of("server/src/app.cpp"), ("Server", "server"))
        self.assertIsNone(component_of("examples/modules/x.cpp"))

    def test_each_line_counts_once_and_unsafe_paths_are_dropped(self):
        files = parse_coverage(gcovr(
            cov_file(A_CPP, {1: 1, 2: 0}, [("f", 1), ("g", 0)]),
            {"file": A_CPP, "lines": [{"line_number": 2, "count": 3}], "functions": []},
            cov_file("../etc/passwd", {1: 1}),
            cov_file("server/src/<b>.cpp", {1: 1}),
        ))
        self.assertEqual(list(files), [A_CPP])
        a = files[A_CPP]
        self.assertEqual((a["lines"], a["linesCovered"], a["functions"], a["functionsCovered"]), (2, 2, 2, 1))
        self.assertEqual(parse_coverage("not json"), {})


class DiffTest(unittest.TestCase):
    def test_changed_lines_come_from_the_new_side(self):
        diff = "\n".join([
            "diff --git a/core/rf-core/src/a.cpp b/core/rf-core/src/a.cpp",
            "--- a/core/rf-core/src/a.cpp",
            "+++ b/core/rf-core/src/a.cpp",
            "@@ -1,0 +2,3 @@",
            "+x",
            "@@ -9 +10 @@",
            "+y",
            "diff --git a/gone.cpp b/gone.cpp",
            "+++ /dev/null",
            "@@ -1,2 +0,0 @@",
        ])
        changed = parse_diff(diff)
        self.assertEqual(changed, {A_CPP: {2, 3, 4, 10}})
        files = parse_coverage(gcovr(cov_file(A_CPP, {2: 1, 3: 0, 4: 0, 10: 5, 11: 0})))
        c = changed_line_coverage(files, changed)
        self.assertEqual((c["total"], c["covered"]), (4, 2))
        self.assertEqual(c["uncovered"], [{"path": A_CPP, "from": 3, "to": 4}])


class JobsTest(unittest.TestCase):
    def test_needs_context_or_actions_api(self):
        self.assertEqual(normalize_jobs({"changes": {"result": "success"}, "cpp": {"result": "failure"}}),
                         [{"name": "C++ build and tests", "result": "failure", "url": None}])
        self.assertEqual(normalize_jobs([
            {"name": "Test report", "conclusion": "success"},
            {"name": "Web build and tests", "conclusion": "success", "url": "https://github.com/o/r/actions/runs/1/job/2"},
            {"name": "Evil", "conclusion": "success", "url": "javascript:alert(1)"},
        ]), [
            {"name": "Web build and tests", "result": "success", "url": "https://github.com/o/r/actions/runs/1/job/2"},
            {"name": "Evil", "result": "success", "url": None},
        ])


class EscapingTest(unittest.TestCase):
    def test_untrusted_text_cannot_inject_markdown_html_or_mentions(self):
        s = md("<img src=x> [link](http://evil) @maintainer | `code`")
        self.assertNotIn("<img", s)
        self.assertNotIn("](http", s)
        self.assertNotIn("@maintainer", s)
        self.assertIsNone(re.search(r"[^\\]\|", s))


class InputTest(unittest.TestCase):
    def test_reads_stay_inside_the_current_directory(self):
        old = Path.cwd()
        with tempfile.TemporaryDirectory() as outer:
            inner = Path(outer) / "data"
            inner.mkdir()
            (Path(outer) / "secret.txt").write_text("secret")
            (inner / "ok.txt").write_text("ok")
            os.chdir(inner)
            try:
                self.assertEqual(read_capped("ok.txt"), "ok")
                self.assertIsNone(read_capped("../secret.txt"))
                self.assertIsNone(read_capped("missing.txt"))
            finally:
                os.chdir(old)


class ReportTest(unittest.TestCase):
    def test_passing_run_with_coverage(self):
        coverage = parse_coverage(gcovr(
            cov_file(A_CPP, {1: 1, 2: 1, 3: 1, 4: 0}),
            cov_file("modules/fpv/relay/src/r.cpp", {1: 1, 2: 0}),
        ))
        out = build_report(
            jobs={"changes": {"result": "success"}, "cpp": {"result": "success"},
                  "coverage": {"result": "success"}, "web": {"result": "skipped"}},
            suites=[{"file": "cpp.xml", "label": "C++", "total": 5, "passed": 5, "skipped": 0, "failed": []}],
            coverage=coverage,
            diff=parse_diff(f"+++ b/{A_CPP}\n@@ -0,0 +3,2 @@\n"),
            sha="0123456789abcdef",
            commit="fedcba9876543210",
            repo="o/r",
        )
        self.assertTrue(out.startswith(f"{MARKER}\n### ✅ CI passed for 0123456"))
        self.assertIn("**5** tests passed · C++ line coverage **66.7%** · changed lines **50.0%** covered", out)
        self.assertIn("| Core: rf-core, rf-ext | 🟡 75.0% <sub>3/4</sub>", out)
        self.assertIn("| FPV modules | 🔴 50.0% <sub>1/2</sub>", out)
        self.assertIn("`core/rf-core/src/a.cpp`: [4](https://github.com/o/r/blob/fedcba9876543210/core/rf-core/src/a.cpp#L4)", out)
        self.assertIn("| Web build and tests | ⏭️ skipped | not affected by this change |", out)

    def test_failing_run_leads_with_the_failed_tests(self):
        out = build_report(
            jobs={"cpp": {"result": "failure"}, "web": {"result": "success"}},
            suites=[{"file": "cpp.xml", "label": "C++", "total": 3, "passed": 2, "skipped": 0,
                     "failed": ["Geo › <b>Breaks</b>"]}],
        )
        self.assertRegex(out, r"(?m)^### ❌ CI failed: 1 test failed$")
        self.assertIn("- C++: Geo › &lt;b&gt;Breaks&lt;/b&gt;", out)

    def test_build_failing_before_its_tests_says_so(self):
        out = build_report(jobs={"cpp": {"result": "failure"}})
        self.assertRegex(out, r"(?m)^### ❌ CI failed: C\+\+ build and tests failed$")
        self.assertIn("failed before any test result was written", out)

    def test_docs_only_change_builds_nothing(self):
        out = build_report(jobs={"changes": {"result": "success"}, "cpp": {"result": "skipped"},
                                 "web": {"result": "skipped"}})
        self.assertIn("Nothing to build", out)
        self.assertNotIn("| Check |", out)


class FolderTestCase(unittest.TestCase):
    """Runs each test in an empty folder, the data folder the report reads its fixed names from."""

    def setUp(self):
        self._cwd = os.getcwd()
        self.folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        os.chdir(self.folder)
        self.addCleanup(os.chdir, self._cwd)

    def write(self, rel, text):
        path = self.folder / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


class JunitStatusTest(unittest.TestCase):
    def test_status_attributes_and_an_unreadable_declared_count(self):
        r = parse_junit('<testsuite tests="many"><testcase name="a" status="failed"/>'
                        '<testcase name="b" status="notrun"/><testcase classname="b" name="b"/></testsuite>')
        self.assertEqual(r, {"total": 3, "failed": ["a"], "skipped": 1, "passed": 1})

    def test_the_count_is_never_below_the_cases_that_did_not_pass(self):
        r = parse_junit('<testsuites tests="1"><testcase name="a"><error/></testcase>'
                        '<testcase name="b"><skipped/></testcase></testsuites>')
        self.assertEqual((r["total"], r["passed"]), (2, 0))


class SuitesTest(FolderTestCase):
    def test_no_tests_folder_means_no_suites(self):
        self.assertEqual(read_suites(), [])

    def test_reads_the_xml_files_of_the_tests_folder(self):
        self.write("tests/cpp.xml", '<testsuites tests="2"><testcase name="a"/><testcase name="b"/></testsuites>')
        self.write("tests/extra.xml", '<testsuite><testcase name="c"><failure/></testcase></testsuite>')
        self.write("tests/notes.txt", "not a result")
        (self.folder / "tests/folder.xml").mkdir()
        suites = read_suites()
        self.assertEqual([(s["file"], s["label"], s["passed"], s["failed"]) for s in suites],
                         [("cpp.xml", "C++", 2, []), ("extra.xml", "extra", 0, ["c"])])


class LimitsTest(unittest.TestCase):
    def test_long_text_is_cut_with_an_ellipsis(self):
        self.assertEqual(md("x" * 50, 10), "x" * 9 + "…")
        self.assertEqual(md(None), "")

    def test_unknown_job_formats_list_nothing(self):
        for value in (None, "cpp", 3):
            with self.subTest(value=value):
                self.assertEqual(normalize_jobs(value), [])


class CoverageFilteringTest(unittest.TestCase):
    def test_non_code_excluded_and_malformed_entries_are_ignored(self):
        files = parse_coverage(gcovr({
            "file": "./" + A_CPP,
            "lines": [{"line_number": 1, "count": 1, "gcovr/noncode": True},
                      {"line_number": 2, "count": 0, "gcovr/excluded": True},
                      {"line_number": 0, "count": 1}, {"line_number": "3", "count": 1}, "junk",
                      {"line_number": 4, "count": "many"}],
            "functions": [{"name": "f", "lineno": 4, "count": 2},
                          {"name": "g", "lineno": 5, "execution_count": 0, "gcovr/excluded": True}, "junk"],
        }, "junk", {"file": 7}))
        self.assertEqual(list(files), [A_CPP])
        a = files[A_CPP]
        self.assertEqual(a["hits"], {4: 0})
        self.assertEqual((a["lines"], a["linesCovered"], a["functions"], a["functionsCovered"]), (1, 0, 1, 1))

    def test_a_document_without_files_has_no_coverage(self):
        for text in ("[]", '{"files": {}}', "null"):
            with self.subTest(text=text):
                self.assertEqual(parse_coverage(text), {})


class CoverageSectionTest(unittest.TestCase):
    def test_files_outside_the_areas_count_in_the_total_only(self):
        out = build_report(coverage=parse_coverage(gcovr(cov_file("examples/x.cpp", {1: 1}))))
        self.assertIn("| **All C++** | 🟢 **100.0%** <sub>1/1</sub>", out)
        self.assertNotIn("Coverage per component", out)
        self.assertNotIn("| Core", out)
        self.assertNotIn("Changed lines", out)

    def test_an_area_without_executable_lines_has_no_percentage(self):
        coverage = parse_coverage(gcovr({"file": "server/src/empty.cpp", "lines": [], "functions": []}))
        out = build_report(coverage=coverage)
        self.assertIn("| Server | ⚪ – <sub>0/0</sub> | – <sub>0/0</sub> |", out)
        self.assertIn("| server | ⚪ – <sub>0/0</sub> | – |", out)

    def test_a_diff_without_executable_lines_says_so(self):
        coverage = parse_coverage(gcovr(cov_file(A_CPP, {1: 1})))
        out = build_report(coverage=coverage, diff=parse_diff(f"+++ b/{A_CPP}\n@@ -0,0 +40,2 @@\n"))
        self.assertIn("**Changed lines:** this pull request changes no executable C++ line.", out)
        self.assertNotIn("changed lines **", out)

    def test_fully_covered_changes_list_no_lines(self):
        coverage = parse_coverage(gcovr(cov_file(A_CPP, {1: 1, 2: 1})))
        out = build_report(coverage=coverage, diff=parse_diff(f"+++ b/{A_CPP}\n@@ -0,0 +1,2 @@\n"))
        self.assertIn("**Changed lines:** 🟢 100.0% covered (2 of 2 executable lines", out)
        self.assertNotIn("not run by any test", out)


class UncoveredLinesTest(unittest.TestCase):
    @staticmethod
    def report(hits_per_file, repo=None, commit=None):
        coverage = parse_coverage(gcovr(*(cov_file(path, hits) for path, hits in hits_per_file.items())))
        diff = "".join(f"+++ b/{path}\n@@ -0,0 +1,100 @@\n" for path in hits_per_file)
        return build_report(coverage=coverage, diff=parse_diff(diff), repo=repo, commit=commit)

    def test_ranges_are_plain_text_without_a_repository(self):
        out = self.report({A_CPP: {1: 1, 3: 0, 4: 0, 6: 0}}, commit="fedcba9876543210")
        self.assertIn(f"- `{A_CPP}`: 3-4, 6", out)
        self.assertIn("3 changed lines not run by any test", out)

    def test_ranges_link_to_the_commit(self):
        out = self.report({A_CPP: {3: 0, 4: 0}}, repo="o/r", commit="fedcba9876543210")
        self.assertIn(f"[3-4](https://github.com/o/r/blob/fedcba9876543210/{A_CPP}#L3-L4)", out)

    def test_long_lists_are_cut(self):
        many_ranges = {n: 0 for n in range(1, 25, 2)}
        files = {A_CPP: many_ranges, **{f"server/src/f{i:02}.cpp": {1: 0} for i in range(MAX_LISTED + 1)}}
        out = self.report(files)
        self.assertIn(f"- `{A_CPP}`: 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, … 2 more", out)
        self.assertIn("- `server/src/f18.cpp`: 1", out)
        self.assertNotIn("f19.cpp", out)
        self.assertIn("- … and 2 more files", out)


class JobsTableTest(unittest.TestCase):
    def test_suite_counts_per_job(self):
        out = build_report(
            jobs=[{"name": "C++ build and tests", "conclusion": "success"},
                  {"name": "Web build and tests", "conclusion": "success"},
                  {"name": "Fuzz C++", "conclusion": "cancelled"},
                  {"name": "Odd", "conclusion": "neutral"}],
            suites=[{"file": "cpp.xml", "label": "C++", "total": 7, "passed": 5, "skipped": 2, "failed": []},
                    {"file": "web-ui-kit.xml", "label": "Web: ui-kit", "total": 1, "passed": 1, "skipped": 0,
                     "failed": []},
                    {"file": "web-fpv-app.xml", "label": "Web: fpv-app", "total": 1, "passed": 1, "skipped": 0,
                     "failed": []}],
        )
        self.assertRegex(out, r"(?m)^### ❌ CI failed: Fuzz C\+\+ failed$")
        self.assertIn("| C++ build and tests | ✅ success | 5 passed, 2 skipped |", out)
        self.assertIn("| Web build and tests | ✅ success | Web: ui-kit: 1 passed<br>Web: fpv-app: 1 passed |", out)
        self.assertIn("| Fuzz C++ | 🚫 cancelled |  |", out)
        self.assertIn("| Odd | ❔ neutral |  |", out)

    def test_many_failed_tests_are_cut(self):
        failed = [f"T › case{i}" for i in range(MAX_LISTED + 3)]
        out = build_report(suites=[{"file": "cpp.xml", "label": "C++", "total": len(failed), "passed": 0,
                                    "skipped": 0, "failed": failed}])
        self.assertRegex(out, r"(?m)^### ❌ CI failed: 23 tests failed$")
        self.assertIn(f"- C++: T › case{MAX_LISTED - 1}\n- … and 3 more\n", out)
        self.assertNotIn(f"case{MAX_LISTED}\n", out)

    def test_footer_links_only_to_github(self):
        out = build_report(run_url="https://github.com/o/r/actions/runs/1",
                           coverage_url="https://github.com/o/r/actions/runs/1#artifacts")
        self.assertTrue(out.endswith("[Workflow run](https://github.com/o/r/actions/runs/1) · "
                                     "[HTML coverage report](https://github.com/o/r/actions/runs/1#artifacts)\n"))
        out = build_report(run_url="https://evil.example/run", coverage_url="javascript:alert(1)")
        self.assertNotIn("evil", out)
        self.assertNotIn("javascript", out)


class MainTest(FolderTestCase):
    """The command line reads the fixed file names and the environment, and writes the report."""

    def run_main(self, env):
        out = io.StringIO()
        with mock.patch.dict(os.environ, env, clear=True), redirect_stdout(out):
            report.main()
        return out.getvalue()

    def write_coverage_inputs(self):
        self.write("coverage/coverage.json", gcovr(cov_file(A_CPP, {1: 1, 2: 0})))
        self.write("coverage/changes.diff", f"+++ b/{A_CPP}\n@@ -0,0 +2 @@\n+x\n")
        self.write("coverage/commit.txt", "fedcba9876543210\n")
        self.write("tests/cpp.xml", '<testsuites tests="4"><testcase name="a"/></testsuites>')

    def test_reads_every_input(self):
        self.write_coverage_inputs()
        self.write("jobs.json", json.dumps([{"name": "C++ build and tests", "conclusion": "success",
                                             "url": "https://github.com/o/r/actions/runs/1/job/2"}]))
        out = self.run_main({"REPORT_SHA": "0123456789abcdef", "GITHUB_REPOSITORY": "o/r",
                             "REPORT_JOBS": json.dumps({"web": {"result": "failure"}}),
                             "REPORT_RUN_URL": "https://github.com/o/r/actions/runs/1"})
        self.assertTrue(out.startswith(f"{MARKER}\n### ✅ CI passed for 0123456\n"))
        self.assertIn("**4** tests passed · C++ line coverage **50.0%** · changed lines **0.0%** covered", out)
        self.assertIn("| [C++ build and tests](https://github.com/o/r/actions/runs/1/job/2) | ✅ success | 4 passed |",
                      out)
        self.assertIn(f"[2](https://github.com/o/r/blob/fedcba9876543210/{A_CPP}#L2)", out)
        self.assertNotIn("Web build", out)
        self.assertTrue(out.endswith("[Workflow run](https://github.com/o/r/actions/runs/1)\n"))

    def test_unreadable_jobs_file_falls_back_to_the_needs_context(self):
        self.write("jobs.json", "{not json")
        out = self.run_main({"REPORT_JOBS": json.dumps({"web": {"result": "failure"}})})
        self.assertRegex(out, r"(?m)^### ❌ CI failed: Web build and tests failed$")

    def test_empty_folder_and_bad_environment_report_nothing_to_build(self):
        out = self.run_main({"REPORT_JOBS": "[oops", "REPORT_SHA": "not-a-sha"})
        self.assertEqual(out, f"{MARKER}\n### ✅ CI passed\n\nNothing to build: this change touches only files "
                              "the tests do not cover, such as docs.\n")

    def test_runs_as_a_script(self):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True), redirect_stdout(out):
            runpy.run_path(report.__file__, run_name="__main__")
        self.assertTrue(out.getvalue().startswith(MARKER))


if __name__ == "__main__":
    unittest.main()
