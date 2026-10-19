"""Reads LCOV coverage for the CI report (report.py): the web coverage written by Vitest.

The report may come from the code of a fork's pull request, so it is read as plain text with
bounded line formats and nothing in it is trusted beyond being numbers and paths to show. The
paths are made relative to the repository root as tools/ci/sonar_lcov.py does: the Angular test
runner writes them relative to the web/ folder, and the CI merges them with that script.
"""
import re
from pathlib import PurePosixPath

MAX_LINE_NUMBER = 10_000_000


class LcovFile:
    """Line and function hits of one source file, merged over every record of the file."""

    def __init__(self):
        self.hits = {}
        self.functions = {}

    def add_line(self, number, count):
        if 1 <= number <= MAX_LINE_NUMBER:
            self.hits[number] = max(self.hits.get(number, 0), count)

    def add_function(self, name, count):
        self.functions[name] = max(self.functions.get(name, 0), count)

    def summary(self):
        """The per-file coverage shape report.py uses for gcovr too."""
        return {
            "lines": len(self.hits),
            "linesCovered": sum(1 for c in self.hits.values() if c > 0),
            "functions": len(self.functions),
            "functionsCovered": sum(1 for c in self.functions.values() if c > 0),
            "hits": self.hits,
        }


class LcovParser:
    """Per-file coverage of LCOV text: {repository path: summary}."""

    LINE = re.compile(r"DA:(\d{1,9}),(\d{1,15})(?:,.*)?")
    FUNCTION = re.compile(r"FN:\d{1,9}(?:,\d{1,9})?,(.{1,500})")
    FUNCTION_HITS = re.compile(r"FNDA:(\d{1,15}),(.{1,500})")

    def __init__(self, base="web"):
        self.base = PurePosixPath(base)

    def parse(self, text):
        files, current = {}, None
        for raw in text.splitlines():
            line = raw.strip()
            if line.startswith("SF:"):
                path = self.repository_path(line[3:])
                current = files.setdefault(path, LcovFile()) if path else None
            elif line == "end_of_record":
                current = None
            elif current is not None:
                self._record(current, line)
        return {path: f.summary() for path, f in files.items()}

    def _record(self, current, line):
        if m := self.LINE.fullmatch(line):
            current.add_line(int(m.group(1)), int(m.group(2)))
        elif m := self.FUNCTION_HITS.fullmatch(line):
            current.add_function(m.group(2), int(m.group(1)))
        elif m := self.FUNCTION.fullmatch(line):
            current.add_function(m.group(1), 0)

    def repository_path(self, source):
        """The path from the repository root, or None for a path outside the base folder."""
        source = source.strip()
        if source.startswith("/"):
            marker = f"/{self.base}/"
            if marker not in source:
                return None
            source = source.rsplit(marker, 1)[1]
        path = PurePosixPath(source)
        if ".." in path.parts or not path.parts:
            return None
        # tools/ci/sonar_lcov.py has already put the base in front of the merged report's paths.
        return str(path if path.parts[0] == self.base.name else self.base / path)


class WebComponents:
    """The project and folder of a web source file, such as ("ui-kit", "ui-kit/charts")."""

    PATH = re.compile(r"web/projects/([\w.-]+)/src/(?:lib|app)/(?:([\w.-]+)/)?[^/]+")

    @classmethod
    def of(cls, path):
        """(project, component), or None for files outside web/projects/<name>/src/."""
        m = cls.PATH.fullmatch(path)
        if m:
            project, folder = m.group(1), m.group(2)
            return project, f"{project}/{folder}" if folder else project
        project = re.match(r"web/projects/([\w.-]+)/", path)
        return (project.group(1), project.group(1)) if project else None
