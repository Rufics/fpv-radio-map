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

    def test_finds_a_header_in_an_included_fragment(self):
        self.write("modules/fpv/core/src/tables.inc", "#include <pqxx/pqxx>\n")
        self.assertEqual(len(self.findings()), 1)

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

    def test_a_hash_inside_a_quoted_cmake_argument_starts_no_comment(self):
        self.write("modules/fpv/CMakeLists.txt",
                   'target_link_libraries(x PRIVATE "/opt/lib#debug.a" PostgreSQL::PostgreSQL)\n')
        self.assertEqual(len(self.findings()), 1)

    def test_ignores_cmake_bracket_comments_and_cpp_block_comments(self):
        self.write("modules/fpv/CMakeLists.txt", "project(fpv)\n#[==[\nPostgreSQL::PostgreSQL\n]==]\n")
        self.write("modules/fpv/core/src/module.cpp", "/* the old way:\n#include <pqxx/pqxx>\n*/\nint x = 0;\n")
        self.assertEqual(self.findings(), [])

    def test_a_hash_inside_a_cmake_bracket_argument_starts_no_comment(self):
        self.write("modules/fpv/CMakeLists.txt",
                   "target_link_libraries(x PRIVATE [=[/opt/lib#debug.a]=] PostgreSQL::PostgreSQL)\n")
        self.assertEqual(len(self.findings()), 1)

    def test_a_raw_string_neither_includes_nor_hides_an_include(self):
        self.write("modules/fpv/core/src/doc.cpp",
                   'const char* t = R"x(\n#include <pqxx/pqxx>\n" /* )x";\n#include <libpq-fe.h>\n')
        found = self.findings()
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("modules/fpv/core/src/doc.cpp:4:"), found)

    def test_finds_the_plain_libpq_link_name(self):
        self.write("modules/fpv/CMakeLists.txt", "target_link_libraries(x PRIVATE pq)\n")
        self.assertEqual(len(self.findings()), 1)

    def test_finds_libraries_linked_as_linker_flags(self):
        self.write("modules/fpv/CMakeLists.txt", "target_link_libraries(x PRIVATE -lpq)\nlink_libraries(-lsqlite3)\n"
                                                 "target_link_libraries(y -lmysqlclient)\n")
        self.assertEqual(len(self.findings()), 3)

    def test_finds_libpq_looked_up_by_find_library(self):
        self.write("modules/fpv/CMakeLists.txt", "find_library(DB_DRIVER NAMES pq)\n"
                                                 "target_link_libraries(core PRIVATE ${DB_DRIVER})\n")
        self.assertEqual(len(self.findings()), 1)

    def test_finds_a_quoted_postgresql_package(self):
        self.write("modules/fpv/CMakeLists.txt", 'find_package("PostgreSQL" REQUIRED)\nfind_package([[PostgreSQL]])\n')
        self.assertEqual(len(self.findings()), 2)

    def test_a_pq_name_outside_a_link_is_not_a_finding(self):
        self.write("modules/fpv/CMakeLists.txt", "project(pq LANGUAGES CXX)\nadd_library(pq_maps STATIC a.cpp)\n"
                                                 "target_link_libraries(pq_maps PRIVATE fpv_common)\n")
        self.assertEqual(self.findings(), [])

    def test_finds_full_client_library_file_names(self):
        self.write("modules/fpv/CMakeLists.txt",
                   "target_link_libraries(x PRIVATE /usr/lib/libmysqlclient.so)\n"
                   "target_link_libraries(x PRIVATE /usr/lib/libsqlite3.so)\n"
                   "target_link_libraries(y PRIVATE libpqxx.a)\n")
        self.assertEqual(len(self.findings()), 3)

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
