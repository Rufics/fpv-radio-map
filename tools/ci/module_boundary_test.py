"""Tests of the module database boundary check: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import io
import tempfile
import unittest
from pathlib import Path

from module_boundary import Command, ModuleBoundary


class ModuleBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def findings(self):
        return list(ModuleBoundary(self.root).findings())

    def test_a_module_using_host_calls_only_passes(self):
        self.write("modules/fpv/core/src/module.cpp", '#include <rfmod/module.hpp>\nint x = 0;\n')
        self.write("modules/fpv/CMakeLists.txt", "target_link_libraries(core PRIVATE fpv_common)\n")
        self.assertEqual(self.findings(), [])

    def test_finds_database_client_headers(self):
        self.write("modules/fpv/core/src/module.cpp", '#include <rfmod/module.hpp>\n#include <pqxx/pqxx>\n')
        self.write("examples/modules/x/src/x.cpp", '#  include "libpq-fe.h"\n')
        self.write("modules/fpv/core/src/store.hpp", '#include <rfdb/connection_pool.hpp>\n')
        found = self.findings()
        self.assertEqual(len(found), 3, found)
        self.assertTrue(any(f.startswith("modules/fpv/core/src/module.cpp:2: database client header") for f in found),
                        found)

    def test_finds_database_client_libraries(self):
        self.write("modules/fpv/CMakeLists.txt", "target_link_libraries(core PRIVATE fpv_common rfmap_pqxx)\n")
        self.write("examples/modules/x/CMakeLists.txt", "pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)\n")
        self.write("modules/fpv/sub/CMakeLists.txt",
                   "find_package(PostgreSQL REQUIRED)\ntarget_link_libraries(x PRIVATE PostgreSQL::PostgreSQL)\n")
        self.assertEqual(len(self.findings()), 4)

    def test_finds_a_reference_split_over_several_lines(self):
        self.write("modules/fpv/CMakeLists.txt", "project(fpv)\nfind_package(\n  PostgreSQL\n  REQUIRED\n)\n")
        found = self.findings()
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("modules/fpv/CMakeLists.txt:2: database client library"), found)

    def test_ignores_a_trailing_cmake_comment(self):
        self.write("modules/fpv/CMakeLists.txt",
                   "target_link_libraries(x PRIVATE fpv_common)  # not PostgreSQL::PostgreSQL\n")
        self.assertEqual(self.findings(), [])

    def test_ignores_comments_and_code_outside_the_module_folders(self):
        self.write("modules/fpv/CMakeLists.txt", "# Modules never link libpq: see the guide.\n")
        self.write("modules/fpv/core/src/module.cpp", "// #include <pqxx/pqxx> is what we do not do\n")
        self.write("server/src/database.hpp", "#include <pqxx/pqxx>\n")
        self.assertEqual(self.findings(), [])

    def test_the_command_fails_on_a_finding(self):
        self.write("modules/fpv/core/src/module.cpp", "#include <sqlite3.h>\n")
        out = io.StringIO()
        self.assertEqual(Command(["--root", str(self.root)], out).run(), 1)
        self.assertIn("sqlite3.h", out.getvalue())

    def test_the_command_passes_a_clean_tree(self):
        out = io.StringIO()
        self.assertEqual(Command(["--root", str(self.root)], out).run(), 0)

    def test_the_repository_itself_is_clean(self):
        out = io.StringIO()
        self.assertEqual(Command([], out).run(), 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
