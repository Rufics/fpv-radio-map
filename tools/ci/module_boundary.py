#!/usr/bin/env python3
"""Checks that no module talks to the database on its own.

    python3 -I tools/ci/module_boundary.py [--root <repository>]

Modules (modules/, examples/modules/) reach stored data only through host calls of the rfmod SDK
(docs/modules/developer-guide.md, section 6); the server alone holds database sessions, in its
bounded pool. This check reads every C/C++ source and CMake file under the module folders and
fails when one includes a database client header (libpqxx, libpq, SQLite, MySQL, the server's
own rf-db layer) or links a database client library. ctest runs it with the C++ tests.

Prints one line per finding. The exit code is 1 when anything was found, 2 on a usage error and
0 otherwise.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Rule:
    """One kind of forbidden reference: the files it applies to and the pattern that finds it.

    The pattern runs over the whole file with its comments blanked out, so a reference split over
    several lines (a CMake command spans lines freely) is found and one in a comment is not.
    """

    def __init__(self, name, suffixes, pattern, lexemes):
        """`lexemes` finds the file's literals and comments, in the order the language reads them:
        a quoted or bracket argument is kept (a '#' or '//' inside it starts no comment), a comment
        or raw string literal is blanked."""
        self.name = name
        self.suffixes = tuple(suffixes)
        self.pattern = re.compile(pattern, re.MULTILINE)
        self.lexemes = re.compile(lexemes, re.DOTALL)

    def applies_to(self, path):
        return path.name in self.suffixes or path.suffix in self.suffixes

    @staticmethod
    def blank(match):
        text = match.group()
        if text[0] in "\"'[":
            return text
        return re.sub(r"[^\n]", " ", text)

    def line_numbers(self, text):
        """The 1-based lines with a match, each once; blanking keeps every offset and line break."""
        code = self.lexemes.sub(self.blank, text)
        lines = {code.count("\n", 0, match.start()) + 1 for match in self.pattern.finditer(code)}
        yield from sorted(lines)


class ModuleBoundary:
    """Finds database access in the module sources below a repository root."""

    MODULE_FOLDERS = ("modules", "examples/modules")
    SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")
    CMAKE_FILES = ("CMakeLists.txt", ".cmake")
    # Raw strings (blanked: they may hold any text), strings and characters, block and line comments.
    C_LEXEMES = (r'\b(?:u8|[uUL])?R"([^ ()\\\t\v\f\n]{0,16})\(.*?\)\1"'
                 r'|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*')
    # Quoted and bracket arguments, then bracket and line comments.
    CMAKE_LEXEMES = r'"(?:\\.|[^"\\])*"|\[(=*)\[.*?\]\1\]|#\[(=*)\[.*?\]\2\]|#[^\n]*'
    RULES = (
        Rule("database client header", SOURCE_SUFFIXES,
             r'^[ \t]*#[ \t]*include[ \t]*[<"](pqxx/|libpq-fe\.h|libpq/|postgresql/|sqlite3\.h|mysql/|mysql\.h|'
             r'rfdb/|database\.hpp)', C_LEXEMES),
        Rule("database client library", CMAKE_FILES,
             r'(?i)\b(pq|pqxx|libpqxx|libpq|PkgConfig::PQ|PostgreSQL::\w+|find_package\s*\(\s*PostgreSQL|'
             r'rf_db|rf_db_pg|rfmap_pqxx|rfmap_server_lib|(lib)?sqlite3|SQLite::\w+|(lib)?mysqlclient)\b',
             CMAKE_LEXEMES),
    )

    def __init__(self, root):
        self.root = Path(root)

    def files(self):
        for folder in self.MODULE_FOLDERS:
            base = self.root / folder
            if base.is_dir():
                yield from sorted(p for p in base.rglob("*") if p.is_file())

    def findings(self):
        for path in self.files():
            rules = [rule for rule in self.RULES if rule.applies_to(path)]
            if rules:
                yield from self.check_file(path, rules)

    def check_file(self, path, rules):
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.split("\n")
        for rule in rules:
            for number in rule.line_numbers(text):
                yield f"{path.relative_to(self.root)}:{number}: {rule.name}: {lines[number - 1].strip()}"


class Command:
    """The command line: parses the arguments and reports the findings."""

    def __init__(self, argv, out=sys.stdout):
        self.argv = argv
        self.out = out

    def run(self):
        parser = argparse.ArgumentParser(description="Checks that no module talks to the database on its own.")
        parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
        args = parser.parse_args(self.argv)
        if not args.root.is_dir():
            parser.error(f"{args.root} is not a directory")
        found = list(ModuleBoundary(args.root).findings())
        for finding in found:
            print(finding, file=self.out)
        if found:
            print("Modules reach stored data through host calls only (docs/modules/developer-guide.md, "
                  "section 6).", file=self.out)
            return 1
        print("No module talks to the database on its own.", file=self.out)
        return 0


if __name__ == "__main__":
    sys.exit(Command(sys.argv[1:]).run())
