"""Tests of the Sonar scope planner: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sonar_scope
from sonar_scope import (Areas, ChangeClassifier, CommitHash, Command, GitChanges, Scope, ScopePlanner,
                         ScopeSettings, StepOutputs)

BASE = "a" * 40
HEAD = "b" * 40


class ChangeClassifierTest(unittest.TestCase):
    def setUp(self):
        self.classifier = ChangeClassifier()

    def test_sorts_paths_into_areas(self):
        cases = {
            "core/rf-core/src/geodesy.cpp": {Areas.CPP},
            "server/sql/schema.sql": {Areas.CPP},
            "CMakeLists.txt": {Areas.CPP},
            "cmake/tools.cmake": {Areas.CPP},
            "web/projects/fpv-app/src/main.ts": {Areas.WEB},
            "schemas/mission.schema.json": {Areas.CPP, Areas.WEB},
            "tools/ci/report.py": {Areas.PYTHON},
            "tools/tiles-builder/build.py": {Areas.PYTHON},
            "deploy/nginx.conf": set(),
            ".github/workflows/ci.yml": set(),
        }
        for path, areas in cases.items():
            with self.subTest(path=path):
                self.assertEqual(self.classifier.areas(path), areas)

    def test_analysis_files_need_everything(self):
        self.assertTrue(self.classifier.needs_everything("sonar-project.properties"))
        self.assertTrue(self.classifier.needs_everything(".github/workflows/sonar.yml"))
        self.assertFalse(self.classifier.needs_everything("web/package.json"))


class ScopePlannerTest(unittest.TestCase):
    def test_web_only_change_skips_cpp_and_python(self):
        scope = ScopePlanner().plan(["web/a.ts", "web/b.ts", "web/a.ts"])
        self.assertFalse(scope.is_full)
        self.assertEqual(scope.areas, {Areas.WEB})
        self.assertEqual(scope.files, ("web/a.ts", "web/b.ts"))

    def test_mixed_change_takes_every_touched_area(self):
        scope = ScopePlanner().plan(["core/a.cpp", "tools/ci/x.py", "deploy/nginx.conf"])
        self.assertEqual(scope.areas, {Areas.CPP, Areas.PYTHON})

    def test_changed_header_scans_every_cpp_file(self):
        scope = ScopePlanner().plan(["core/a.hpp", "web/a.ts"])
        self.assertFalse(scope.is_full)
        self.assertIn("core/a.hpp", scope.files)
        self.assertIn("**/*.cpp", scope.files)
        self.assertIn("**/*.hpp", scope.files)
        self.assertIn("**/tests/**/*.cpp", scope.tests)

    def test_changed_tests_are_collected(self):
        scope = ScopePlanner().plan(["core/rf-core/tests/a_test.cpp", "tools/ci/x_test.py", "web/a.spec.ts",
                                     "web/a.ts"])
        self.assertEqual(scope.tests, ("core/rf-core/tests/a_test.cpp", "tools/ci/x_test.py", "web/a.spec.ts"))

    def test_coverage_merge_script_needs_everything(self):
        self.assertTrue(ScopePlanner().plan(["tools/ci/sonar_lcov.py"]).is_full)

    def test_changed_source_alone_scans_only_itself(self):
        self.assertEqual(ScopePlanner().plan(["core/a.cpp"]).files, ("core/a.cpp",))

    def test_falls_back_to_the_full_analysis(self):
        cases = {
            "nothing": [],
            "analysis": ["web/a.ts", "sonar-project.properties"],
            "unsafe name": ["web/a b.ts"],
            "comma": ["web/a,b.ts"],
            "wildcard": ["web/*.ts"],
            "not utf-8": ["web/a\udcff.ts"],
        }
        for name, files in cases.items():
            with self.subTest(name):
                scope = ScopePlanner().plan(files)
                self.assertTrue(scope.is_full)
                self.assertEqual(scope.areas, set(Areas.ALL))

    def test_too_many_files_is_a_full_analysis(self):
        scope = ScopePlanner(max_files=2).plan(["a.ts", "b.ts", "c.ts"])
        self.assertTrue(scope.is_full)
        self.assertIn("more than 2", scope.reason)


class ScopeSettingsTest(unittest.TestCase):
    def test_full_scope_only_appends_a_comment(self):
        text = ScopeSettings("sonar.projectKey=x\n").render(Scope.full("no base commit given"))
        self.assertTrue(text.startswith("sonar.projectKey=x\n"))
        self.assertNotIn("sonar.inclusions", text)
        self.assertNotIn("sonar.test.inclusions", text)
        self.assertNotIn("suffixes", text)

    def test_web_scope_limits_files_and_switches_off_other_parts(self):
        text = ScopeSettings("sonar.projectKey=x").render(Scope({Areas.WEB}, ["web/a.ts", "web/b.ts"]))
        self.assertIn("\nsonar.inclusions=web/a.ts,web/b.ts\n", text)
        self.assertIn("\nsonar.cpp.file.suffixes=-\n", text)
        self.assertIn("\nsonar.coverageReportPaths=\n", text)
        self.assertIn("\nsonar.python.coverage.reportPaths=\n", text)
        self.assertNotIn("sonar.javascript.lcov.reportPaths=", text)
        self.assertIn(f"\nsonar.test.inclusions={sonar_scope.NO_TEST_PATTERN}\n", text)

    def test_changed_tests_limit_the_test_files(self):
        scope = Scope({Areas.WEB}, ["web/a.ts", "web/a.spec.ts"], tests=["web/a.spec.ts"])
        self.assertIn("\nsonar.test.inclusions=web/a.spec.ts\n", ScopeSettings("").render(scope))

    def test_cpp_scope_keeps_the_cpp_analyser(self):
        text = ScopeSettings("").render(Scope({Areas.CPP}, ["core/a.cpp"]))
        self.assertNotIn("file.suffixes", text)
        self.assertIn("sonar.javascript.lcov.reportPaths=", text)


class StepOutputsTest(unittest.TestCase):
    def test_appends_outputs_to_the_github_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "out"
            path.write_text("before=1\n", encoding="utf-8")
            StepOutputs({"GITHUB_OUTPUT": str(path)}).write(Scope({Areas.WEB}, ["web/a.ts"]))
            self.assertEqual(path.read_text(encoding="utf-8"),
                             "before=1\ncpp=false\npython=false\nweb=true\nfull=false\n")

    def test_without_github_only_returns_the_text(self):
        self.assertIn("full=true", StepOutputs({}).write(Scope.full("x")))


class GitChangesTest(unittest.TestCase):
    def test_reads_added_and_modified_files_since_the_merge_base(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, stdout="web/a.ts\0core/b.cpp\0", stderr="")

        with mock.patch.object(sonar_scope.subprocess, "run", run), \
                mock.patch.object(sonar_scope.shutil, "which", return_value="/usr/bin/git"):
            files = GitChanges(Path("/repo")).between(BASE, HEAD)
        self.assertEqual(files, ["web/a.ts", "core/b.cpp"])
        self.assertIn(f"{BASE}...{HEAD}", calls[0])
        self.assertIn("--diff-filter=d", calls[0])

    def test_keeps_names_that_are_not_utf8(self):
        def run(command, **kwargs):
            raw = b"web/a\xff.ts\0"
            return subprocess.CompletedProcess(command, 0, stdout=raw.decode(kwargs["encoding"], kwargs["errors"]))

        with mock.patch.object(sonar_scope.subprocess, "run", run), \
                mock.patch.object(sonar_scope.shutil, "which", return_value="/usr/bin/git"):
            files = GitChanges(Path("/repo")).between(BASE, HEAD)
        self.assertTrue(ScopePlanner().plan(files).is_full)

    def test_refuses_anything_but_commit_hashes(self):
        changes = GitChanges()
        for revision in ("--output=x", "abc1234", "A" * 40):
            with self.subTest(revision), self.assertRaises(ValueError):
                changes.between(revision, HEAD)

    def test_commit_hash_is_written_back_unchanged(self):
        for text in ("0" * 39 + "1", "f" * 64):
            self.assertEqual(str(CommitHash(text)), text)

    def test_needs_git(self):
        changes = GitChanges()
        with mock.patch.object(sonar_scope.shutil, "which", return_value=None), self.assertRaises(FileNotFoundError):
            changes.between(BASE, HEAD)


class FakeChanges:
    def __init__(self, files):
        self.files = files

    def between(self, base, head):
        return self.files


class CommandTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (self.root / "sonar-project.properties").write_text("sonar.projectKey=x\n", encoding="utf-8")
        self.outputs = self.root / "github-output"

    def run_command(self, files, *argv):
        command = Command(self.root, FakeChanges(files), {"GITHUB_OUTPUT": str(self.outputs)})
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return command.run(["--settings", "sonar-project.properties", "--output", "out/s.properties", *argv])

    def written(self):
        return (self.root / "out/s.properties").read_text(encoding="utf-8")

    def test_pull_request_scope(self):
        self.assertEqual(self.run_command(["web/a.ts"], "--base", BASE, "--head", HEAD), 0)
        self.assertIn("sonar.inclusions=web/a.ts", self.written())
        self.assertIn("web=true\n", self.outputs.read_text(encoding="utf-8"))

    def test_without_base_everything_is_analysed(self):
        self.assertEqual(self.run_command(["web/a.ts"], "--base", "", "--head", ""), 0)
        self.assertNotIn("sonar.inclusions", self.written())
        self.assertIn("full=true\n", self.outputs.read_text(encoding="utf-8"))

    def test_base_without_head_is_a_usage_error(self):
        self.assertEqual(self.run_command([], "--base", BASE), 2)

    def test_settings_can_be_rewritten_in_place(self):
        command = Command(self.root, FakeChanges(["web/a.ts"]), {})
        with contextlib.redirect_stdout(io.StringIO()):
            code = command.run(["--settings", "sonar-project.properties", "--output", "sonar-project.properties",
                                "--base", BASE, "--head", HEAD])
        self.assertEqual(code, 0)
        text = (self.root / "sonar-project.properties").read_text(encoding="utf-8")
        self.assertTrue(text.startswith("sonar.projectKey=x\n"))
        self.assertIn("sonar.inclusions=web/a.ts", text)

    def test_output_outside_the_repository_is_a_usage_error(self):
        command = Command(self.root, FakeChanges([]), {})
        with contextlib.redirect_stderr(io.StringIO()):
            code = command.run(["--settings", "sonar-project.properties", "--output", "../s.properties"])
        self.assertEqual(code, 2)

    def test_git_failure_is_reported(self):
        class Failing:
            def between(self, base, head):
                raise subprocess.CalledProcessError(128, ["git"])

        command = Command(self.root, Failing(), {})
        with contextlib.redirect_stderr(io.StringIO()):
            code = command.run(["--settings", "sonar-project.properties", "--output", "s.properties",
                                "--base", BASE, "--head", HEAD])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
