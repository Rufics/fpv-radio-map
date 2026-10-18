"""Tests of the clang-tidy runner: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import io
import json
import os
import runpy
import subprocess
import tempfile
import threading
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import clang_tidy
from clang_tidy import (ROOT, TIMEOUT_SECONDS, annotations, changed_files, escape_annotation, main, parse_finding,
                        repo_relative, run_one, select, shard, translation_units)

BUILD = ROOT / "build-tidy"
UNITS = ["core/rf-core/src/geodesy.cpp", "platform/rfmod/src/crypto.cpp"]


class TranslationUnitsTest(unittest.TestCase):
    def test_keeps_repository_sources_only(self):
        commands = [
            {"directory": str(BUILD), "file": str(ROOT / "core/rf-core/src/geodesy.cpp")},
            {"directory": str(BUILD), "file": "../platform/rfmod/src/crypto.cpp"},
            {"directory": str(BUILD), "file": str(BUILD / "_deps/json-src/src/json-validator.cpp")},
            {"directory": str(BUILD), "file": str(BUILD / "platform/rfmod/generated/schema.cpp")},
            {"directory": str(BUILD), "file": "/usr/src/googletest/src/gtest-all.cc"},
            {"directory": str(BUILD), "file": str(ROOT / "core/rf-core/src/geodesy.cpp")},
            {"directory": str(BUILD), "file": str(ROOT / "server/src/schema.c")},
        ]
        self.assertEqual(translation_units(commands, BUILD), UNITS)


class SelectTest(unittest.TestCase):
    def test_no_change_set_checks_everything(self):
        self.assertEqual(select(UNITS, None), UNITS)

    def test_changed_sources_only(self):
        self.assertEqual(select(UNITS, ["platform/rfmod/src/crypto.cpp", "docs/README.md"]),
                         ["platform/rfmod/src/crypto.cpp"])

    def test_changes_outside_cpp_check_nothing(self):
        self.assertEqual(select(UNITS, ["web/src/main.ts"]), [])

    def test_changes_that_reach_other_files_check_everything(self):
        for path in ("core/rf-core/include/rfcore/geodesy.hpp", "platform/rfmod/include/rfmod/abi.h",
                     ".clang-tidy", "server/tests/.clang-tidy", "core/rf-core/CMakeLists.txt",
                     "platform/rfmod/cmake/embed_schema.cmake", "tools/ci/clang_tidy.py"):
            with self.subTest(path=path):
                self.assertEqual(select(UNITS, [path]), UNITS)


class ShardTest(unittest.TestCase):
    def test_shards_split_the_units_without_overlap(self):
        units = [f"f{i}.cpp" for i in range(7)]
        shards = [shard(units, f"{k}/3") for k in (1, 2, 3)]
        self.assertEqual(shards[0], ["f0.cpp", "f3.cpp", "f6.cpp"])
        self.assertEqual(sorted(unit for part in shards for unit in part), units)
        self.assertEqual(shard(units, "1/1"), units)

    def test_shard_must_be_k_of_n(self):
        for spec in ("0/3", "4/3", "1/0", "1", "a/b", "1/3/4", "-1/3", "100/100"):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                shard(["a.cpp"], spec)


class ChangedFilesTest(unittest.TestCase):
    def test_base_must_be_a_commit_or_head_relative(self):
        for base in ("--output=/tmp/x", "HEAD^1;rm", "HEAD@{1}", "main", ""):
            with self.subTest(base=base), self.assertRaises(ValueError):
                changed_files(base)


class ArgumentsTest(unittest.TestCase):
    def test_only_clang_tidy_executables_run(self):
        for name in ("sh", "/usr/bin/clang-tidy", "clang-tidy-18;rm", "../clang-tidy"):
            with self.subTest(name=name):
                self.assertEqual(main(["--build-dir", "build", "--clang-tidy", name]), 2)


class ParseTest(unittest.TestCase):
    def test_findings_and_other_lines(self):
        self.assertEqual(parse_finding("/r/a b:c.cpp:12:5: warning: x: y [z]"), ("/r/a b:c.cpp", "12", "5", "x: y [z]"))
        for line in ("   12 | int x;", "2 warnings generated.", "a.cpp:1:2: error: relative", "/a.cpp:x:2: error: m",
                     "/a.cpp: error: m", "/" + "a" * 5000 + ".cpp:1:1: error: long"):
            with self.subTest(line=line[:40]):
                self.assertIsNone(parse_finding(line))

    def test_paths_outside_the_repository(self):
        self.assertEqual(repo_relative(ROOT / "core/x.cpp"), "core/x.cpp")
        for path in (ROOT, ROOT.parent, ROOT / "../other/x.cpp", "/usr/include/vector"):
            with self.subTest(path=str(path)):
                self.assertIsNone(repo_relative(path))


class BuildDirTest(unittest.TestCase):
    def test_only_folders_under_the_root(self):
        for name in ("/tmp/build", "../build", "a/b", "..", "build..x", ".hidden"):
            with self.subTest(name=name):
                self.assertEqual(main(["--build-dir", name, "--clang-tidy", "clang-tidy"]), 2)


class AnnotationTest(unittest.TestCase):
    def test_one_annotation_per_distinct_finding(self):
        path = ROOT / "core/rf-core/include/rfcore/geodesy.hpp"
        message = "field 'x' is not initialized [cppcoreguidelines-pro-type-member-init,-warnings-as-errors]"
        finding = f"{path}:15:8: error: {message}"
        output = "\n".join([finding, "   15 | struct X {", finding,
                            "/usr/include/c++/13/vector:1:1: warning: outside [misc-x]", "2 warnings generated."])
        self.assertEqual(list(annotations(output)), [
            f"::error file=core/rf-core/include/rfcore/geodesy.hpp,line=15,col=8::{message}"])

    def test_workflow_command_characters_are_escaped(self):
        self.assertEqual(escape_annotation("50%\r\n::warning::x"), "50%25%0D%0A::warning::x")
        path = ROOT / "core/a,b:c.cpp"
        self.assertEqual(list(annotations(f"{path}:1:2: error: m")),
                         ["::error file=core/a%2Cb%3Ac.cpp,line=1,col=2::m"])


class FakeProcesses:
    """Stands in for subprocess.run: answers git diff and clang-tidy, and records every command line."""

    def __init__(self, diff="", findings=None):
        self.diff = diff
        self.findings = findings or {}
        self.commands = []
        self._lock = threading.Lock()

    def run(self, command, **kwargs):
        with self._lock:
            self.commands.append((command, kwargs))
        if Path(command[0]).name == "git":
            return subprocess.CompletedProcess(command, 0, stdout=self.diff, stderr="")
        output = self.findings.get(command[-1])
        return subprocess.CompletedProcess(command, 1 if output else 0, stdout=output or "", stderr="")

    def checked(self):
        """The files clang-tidy was run on, sorted."""
        return sorted(command[-1] for command, _ in self.commands if Path(command[0]).name != "git")


def installed(name):
    """shutil.which() for a machine where git and every clang-tidy are installed in /usr/bin."""
    return f"/usr/bin/{name}"


class GitTestCase(unittest.TestCase):
    """Replaces subprocess.run and shutil.which in the runner for one test."""

    def fake(self, processes, which=installed):
        self.enterContext(mock.patch.object(clang_tidy.subprocess, "run", processes.run))
        self.enterContext(mock.patch.object(clang_tidy.shutil, "which", which))
        return processes


class ChangedFilesFromGitTest(GitTestCase):
    def test_lists_the_changed_paths_from_git(self):
        processes = self.fake(FakeProcesses(diff="core/a.cpp\n\ndocs/README.md\n"))
        self.assertEqual(changed_files("HEAD^1"), ["core/a.cpp", "docs/README.md"])
        command, kwargs = processes.commands[0]
        self.assertEqual(command, ["/usr/bin/git", "diff", "--name-only", "--no-renames", "HEAD^1", "HEAD", "--"])
        self.assertEqual((kwargs["cwd"], kwargs["check"]), (ROOT, True))

    def test_git_must_be_installed(self):
        processes = self.fake(FakeProcesses(), which=lambda name: None)
        with self.assertRaisesRegex(FileNotFoundError, "git is not installed"):
            changed_files("0123abc")
        self.assertEqual(processes.commands, [])


class RunOneTest(GitTestCase):
    def test_returns_the_exit_code_and_both_streams(self):
        def run(command, **kwargs):
            return subprocess.CompletedProcess(command, 1, stdout="out\n", stderr="err\n")

        self.enterContext(mock.patch.object(clang_tidy.subprocess, "run", run))
        self.assertEqual(run_one("/usr/bin/clang-tidy", BUILD, UNITS[0]), (1, "out\nerr\n"))

    def test_a_run_that_takes_too_long_counts_as_a_finding(self):
        def run(command, **kwargs):
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])

        self.enterContext(mock.patch.object(clang_tidy.subprocess, "run", run))
        code, output = run_one("/usr/bin/clang-tidy", BUILD, UNITS[0])
        self.assertEqual(code, 1)
        self.assertEqual(output, f"{UNITS[0]}: clang-tidy took longer than {TIMEOUT_SECONDS} s\n")


class MainRunTest(GitTestCase):
    """main() on a fake repository in a temporary folder, with git and clang-tidy faked."""

    UNITS = ["core/a.cpp", "core/b.cpp", "server/c.cpp"]

    def setUp(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory())).resolve()
        build = root / "build-tidy"
        build.mkdir()
        commands = [{"directory": str(build), "file": str(root / unit)} for unit in self.UNITS]
        (build / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
        self.root = root
        self.enterContext(mock.patch.object(clang_tidy, "ROOT", root))
        self.enterContext(mock.patch.object(clang_tidy, "ROOT_TEXT", str(root)))
        self.enterContext(mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}))

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = main(["--build-dir", "build-tidy", "--jobs", "2", *args])
        return code, out.getvalue(), err.getvalue()

    def test_clang_tidy_must_be_installed(self):
        processes = self.fake(FakeProcesses(), which=lambda name: None)
        code, _, err = self.run_main("--clang-tidy", "clang-tidy-18")
        self.assertEqual((code, err), (2, "clang-tidy-18 is not installed\n"))
        self.assertEqual(processes.commands, [])

    def test_clean_files_pass(self):
        processes = self.fake(FakeProcesses())
        code, out, _ = self.run_main()
        self.assertEqual(code, 0)
        self.assertEqual(out, "Checking 3 file(s) with 2 job(s).\nclang-tidy: 0 of 3 file(s) with findings.\n")
        self.assertEqual(processes.checked(), [str(self.root / unit) for unit in self.UNITS])
        command, kwargs = processes.commands[0]
        self.assertEqual(command[:4], ["/usr/bin/clang-tidy", "-p", str(self.root / "build-tidy"), "--quiet"])
        self.assertEqual(kwargs["timeout"], TIMEOUT_SECONDS)

    def test_findings_fail_the_run_and_are_annotated_on_actions(self):
        finding = f"{self.root}/core/b.cpp:3:1: warning: bad [misc-x]"
        self.fake(FakeProcesses(findings={str(self.root / "core/b.cpp"): finding + "\n"}))
        os.environ["GITHUB_ACTIONS"] = "true"
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn(f"--- core/b.cpp\n{finding}\n", out)
        self.assertIn("::error file=core/b.cpp,line=3,col=1::bad [misc-x]\n", out)
        self.assertTrue(out.endswith("clang-tidy: 1 of 3 file(s) with findings.\n"))

    def test_findings_are_not_annotated_outside_actions(self):
        finding = f"{self.root}/core/a.cpp:1:1: error: bad [misc-x]"
        self.fake(FakeProcesses(findings={str(self.root / "core/a.cpp"): finding}))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertNotIn("::error", out)

    def test_base_checks_only_the_changed_sources(self):
        processes = self.fake(FakeProcesses(diff="server/c.cpp\ndocs/x.md\n"))
        code, out, _ = self.run_main("--base", "HEAD~1")
        self.assertEqual(code, 0)
        self.assertIn("Checking 1 file(s)", out)
        self.assertEqual(processes.checked(), [str(self.root / "server/c.cpp")])

    def test_a_change_without_sources_checks_nothing(self):
        processes = self.fake(FakeProcesses(diff="docs/x.md\n"))
        code, out, _ = self.run_main("--base", "HEAD~1")
        self.assertEqual((code, out), (0, "No C++ sources to check.\n"))
        self.assertEqual(processes.checked(), [])

    def test_shards_share_the_files(self):
        processes = self.fake(FakeProcesses())
        self.assertEqual(self.run_main("--shard", "2/2")[0], 0)
        self.assertEqual(processes.checked(), [str(self.root / "core/b.cpp")])

    def test_an_invalid_shard_is_a_usage_error(self):
        processes = self.fake(FakeProcesses())
        code, _, err = self.run_main("--shard", "3/2")
        self.assertEqual(code, 2)
        self.assertIn("--shard must be K/N", err)
        self.assertEqual(processes.checked(), [])


class ScriptTest(unittest.TestCase):
    def test_runs_as_a_script(self):
        with mock.patch.object(clang_tidy.sys, "argv", ["clang_tidy.py", "--build-dir", "../x"]), \
                redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit_:
            runpy.run_path(clang_tidy.__file__, run_name="__main__")
        self.assertEqual(exit_.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
