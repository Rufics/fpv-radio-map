"""Tests of the LCOV reader: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import unittest

from lcov import LcovParser, WebComponents

LCOV = """TN:
SF:projects/ui-kit/src/lib/charts/link-view.ts
FN:18,groundToAir
FN:25,(anonymous_1)
FNDA:2,groundToAir
FNDA:0,(anonymous_1)
DA:19,2
DA:20,0
DA:0,5
LF:2
LH:1
end_of_record
SF:web/projects/ui-kit/src/lib/charts/link-view.ts
DA:20,3
FN:30,40,later
end_of_record
SF:/home/runner/work/r/r/web/projects/fpv-app/src/app/main.ts
DA:1,1
end_of_record
SF:../../etc/passwd
DA:1,1
end_of_record
SF:/usr/lib/node_modules/x.js
DA:1,1
end_of_record
"""


class ParserTest(unittest.TestCase):
    def test_records_merge_per_repository_path(self):
        files = LcovParser("web").parse(LCOV)
        self.assertEqual(sorted(files), ["web/projects/fpv-app/src/app/main.ts",
                                         "web/projects/ui-kit/src/lib/charts/link-view.ts"])
        view = files["web/projects/ui-kit/src/lib/charts/link-view.ts"]
        self.assertEqual(view["hits"], {19: 2, 20: 3})
        self.assertEqual((view["lines"], view["linesCovered"], view["functions"], view["functionsCovered"]),
                         (2, 2, 3, 1))

    def test_lines_outside_a_record_are_ignored(self):
        self.assertEqual(LcovParser("web").parse("DA:1,1\nend_of_record\n"), {})


class ComponentsTest(unittest.TestCase):
    def test_project_and_folder(self):
        self.assertEqual(WebComponents.of("web/projects/ui-kit/src/lib/charts/link-view.ts"), ("ui-kit", "ui-kit/charts"))
        self.assertEqual(WebComponents.of("web/projects/fpv-app/src/app/main.ts"), ("fpv-app", "fpv-app"))
        self.assertEqual(WebComponents.of("web/projects/fpv-app/other/x.ts"), ("fpv-app", "fpv-app"))
        self.assertIsNone(WebComponents.of("web/scripts/sync.mjs"))


if __name__ == "__main__":
    unittest.main()
