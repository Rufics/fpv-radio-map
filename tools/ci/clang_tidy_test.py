"""Tests of the clang-tidy runner: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import unittest

from clang_tidy import (ROOT, annotations, changed_files, escape_annotation, main, parse_finding, repo_relative,
                        select, translation_units)

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


if __name__ == "__main__":
    unittest.main()
