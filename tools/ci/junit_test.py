"""Tests of the JUnit reader: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import unittest

from junit import MAX_REASON_CHARS, MAX_REASON_LINES, FailureReason, GtestOutput, JUnitParser, SourceLocation

CTEST_FAILED = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="2" failures="1">
  <testcase name="Units.Adds" classname="Units.Adds" time="0.0155" status="fail">
    <failure message="Failed"/>
    <properties>
      <property name="cmake_labels" value="rf-core;slow"/>
    </properties>
    <system-out>Running main() from gtest_main.cc
[ RUN      ] Units.Adds
/home/runner/work/fpv-radio-map/fpv-radio-map/core/rf-core/tests/units_test.cpp:11: Failure
Expected equality of these values:
  p.value()
    Which is: 23
[  FAILED  ] Units.Adds (0 ms)
[  FAILED  ] 1 test, listed below:
</system-out>
  </testcase>
  <testcase name="fpv_relay_pack" classname="fpv_relay_pack" time="x" status="run">
    <properties><property name="cmake_labels" value="fpv.relay"/></properties>
  </testcase>
</testsuite>
"""

VITEST_FAILED = """<testsuites name="vitest tests" tests="1" failures="1">
  <testsuite name="projects/x/a.spec.ts" tests="1">
    <testcase classname="projects/x/a.spec.ts" name="LinkView &gt; flips" time="0.012">
      <failure message="expected 600 to be 601" type="AssertionError">
AssertionError: expected 600 to be 601
 ❯ projects/x/a.spec.ts:3:53
      </failure>
    </testcase>
  </testsuite>
</testsuites>
"""


class ParserTest(unittest.TestCase):
    def test_ctest_failure_reason_comes_from_the_gtest_output(self):
        suite = JUnitParser().parse(CTEST_FAILED)
        failed, packed = suite.cases
        self.assertEqual(suite.declared_total, 2)
        self.assertEqual((failed.name, failed.outcome, failed.module), ("Units.Adds", "failed", "rf-core"))
        self.assertAlmostEqual(failed.seconds, 0.0155)
        self.assertTrue(failed.reason.startswith("core/rf-core/tests/units_test.cpp:11: Failure"))
        self.assertNotIn("Running main()", failed.reason)
        self.assertNotIn("[  FAILED  ]", failed.reason)
        self.assertEqual((packed.outcome, packed.module, packed.seconds, packed.reason),
                         ("passed", "fpv.relay", 0.0, ""))

    def test_vitest_failure_keeps_the_trace_once(self):
        case = JUnitParser().parse(VITEST_FAILED).cases[0]
        self.assertEqual(case.name, "projects/x/a.spec.ts › LinkView > flips")
        self.assertEqual(case.reason.count("expected 600 to be 601"), 1)
        self.assertEqual(SourceLocation.find(case.reason), ("web/projects/x/a.spec.ts", 3))

    def test_specific_message_is_kept_with_the_output(self):
        case = JUnitParser().parse('<testcase name="t" status="fail"><failure message="Timeout"/>'
                                   "<system-out>last words</system-out></testcase>").cases[0]
        self.assertEqual(case.reason, "Timeout\nlast words")

    def test_cdata_and_skips(self):
        cases = JUnitParser().parse('<testsuite tests="bad"><testcase name="a"><error><![CDATA[x < y & z]]></error>'
                                    '</testcase><testcase name="b" status="notrun"/></testsuite>').cases
        self.assertEqual((cases[0].outcome, cases[0].reason), ("failed", "x < y & z"))
        self.assertTrue(cases[1].skipped)
        self.assertEqual(JUnitParser().parse('<testsuite tests="bad"/>').declared_total, 0)


class OutputTest(unittest.TestCase):
    def test_only_failed_gtest_blocks_are_kept(self):
        log = "\n".join(["[ RUN      ] A.ok", "noise", "[       OK ] A.ok", "[ RUN      ] A.bad", "why",
                         "[  FAILED  ] A.bad", "[ RUN      ] A.empty", "[  FAILED  ] A.empty"])
        self.assertEqual(GtestOutput.failed_blocks(log), ["why"])

    def test_other_programs_show_their_last_lines(self):
        body = "<failure/><system-out>" + "\n".join(f"line {i}" for i in range(100)) + "</system-out>"
        reason = FailureReason.of(body)
        self.assertTrue(reason.endswith("line 99"))
        self.assertNotIn("line 59\n", reason)

    def test_excerpt_is_bounded_and_clean(self):
        self.assertEqual(FailureReason.excerpt("\x1b[31mred\x1b[0m\x07 /abs/dir/server/a.cpp"), "red server/a.cpp")
        long_lines = FailureReason.excerpt("\n".join("x" for _ in range(100)))
        self.assertEqual(len(long_lines.splitlines()), MAX_REASON_LINES + 1)
        self.assertEqual(len(FailureReason.excerpt("y" * 10000)), MAX_REASON_CHARS)


class LocationTest(unittest.TestCase):
    def test_earliest_repository_location(self):
        text = 'Traceback:\n  File "tools/ci/report_test.py", line 33, in t\nserver/src/a.cpp:9: x'
        self.assertEqual(SourceLocation.find(text), ("tools/ci/report_test.py", 33))

    def test_unsafe_or_foreign_paths_are_ignored(self):
        self.assertIsNone(SourceLocation.find("core/../etc/passwd.txt:1"))
        self.assertIsNone(SourceLocation.find("/usr/include/gtest/gtest.h:12"))
        self.assertIsNone(SourceLocation.find("no location here"))


if __name__ == "__main__":
    unittest.main()
