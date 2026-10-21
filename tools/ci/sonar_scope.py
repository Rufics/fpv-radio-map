#!/usr/bin/env python3
"""Decides what the SonarQube Cloud analysis of a pull request has to build, test and scan.

    python3 -I tools/ci/sonar_scope.py --settings sonar-project.properties \\
        --output sonar-reports/sonar-project.properties [--base <commit> --head <commit>]

Without --base every part is built and the whole repository is scanned (pushes to main and manual
runs). With --base the files changed between the merge base of the two commits and --head are
sorted into parts:

- cpp: C and C++ sources, CMake files and every folder the C++ build or its tests read;
- python: Python files;
- web: the Angular workspace and the JSON schemas it copies in.

Only the changed parts are built and tested, and only the changed files are scanned. A change to
the analysis itself (the Sonar settings, the workflow, this script), a file name that a Sonar
pattern or a properties file cannot carry, too many files, or no file left to scan falls back
to the full analysis.

The settings file given is copied to --output with the scope appended; CI writes it over
sonar-project.properties in its checkout, so the scanner reads it as usual. On GitHub Actions the
decision is also written as the step outputs cpp, python, web and full (true or false). The exit
code is 2 on a usage error, 0 otherwise.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REVISION = re.compile(r"[0-9a-f]{7,64}")
# Characters a Sonar inclusion pattern and a properties value can carry without escaping.
SAFE_PATH = re.compile(r"[A-Za-z0-9_./@+-]{1,512}")
MAX_FILES = 500
GIT_TIMEOUT_SECONDS = 120


class Areas:
    """The parts of the repository the analysis can build and test on their own."""

    CPP = "cpp"
    PYTHON = "python"
    WEB = "web"
    ALL = (CPP, PYTHON, WEB)


class ChangeClassifier:
    """Sorts changed paths into the parts they affect, and spots changes that need everything."""

    CPP_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".cmake")
    CPP_NAMES = ("CMakeLists.txt", ".clang-tidy")
    CPP_FOLDERS = ("core/", "platform/", "modules/", "server/", "examples/", "fuzz/", "schemas/", "data/",
                   ".github/actions/cpp-deps/", ".github/actions/fuzz/")
    WEB_FOLDERS = ("web/", "schemas/")
    PYTHON_SUFFIXES = (".py",)
    ANALYSIS_FILES = ("sonar-project.properties", ".github/workflows/sonar.yml", "tools/ci/sonar_scope.py")

    def needs_everything(self, path):
        return path in self.ANALYSIS_FILES

    def areas(self, path):
        found = set()
        name = path.rsplit("/", 1)[-1]
        if path.endswith(self.CPP_SUFFIXES) or name in self.CPP_NAMES or path.startswith(self.CPP_FOLDERS):
            found.add(Areas.CPP)
        if path.endswith(self.PYTHON_SUFFIXES):
            found.add(Areas.PYTHON)
        if path.startswith(self.WEB_FOLDERS):
            found.add(Areas.WEB)
        return found


class Scope:
    """What one analysis covers: the parts to build and the files to scan (None for all of them)."""

    def __init__(self, areas, files=None, reason=""):
        self.areas = frozenset(areas)
        self.files = None if files is None else tuple(files)
        self.reason = reason

    @classmethod
    def full(cls, reason):
        return cls(Areas.ALL, None, reason)

    @property
    def is_full(self):
        return self.files is None

    def includes(self, area):
        return area in self.areas


class ScopePlanner:
    """Turns the list of changed files into a scope."""

    def __init__(self, classifier=None, max_files=MAX_FILES):
        self._classifier = classifier or ChangeClassifier()
        self._max_files = max_files

    def plan(self, changed):
        files = sorted(set(changed))
        reason = self._full_reason(files)
        if reason:
            return Scope.full(reason)
        areas = set()
        for path in files:
            areas |= self._classifier.areas(path)
        return Scope(areas, files, f"{len(files)} changed file(s)")

    def _full_reason(self, files):
        if not files:
            return "no changed file is left to scan"
        if len(files) > self._max_files:
            return f"more than {self._max_files} files changed"
        unsafe = next((path for path in files if not SAFE_PATH.fullmatch(path)), None)
        if unsafe is not None:
            return f"the file name {unsafe!r} cannot be passed to the scanner"
        analysis = next((path for path in files if self._classifier.needs_everything(path)), None)
        if analysis is not None:
            return f"{analysis} changed"
        return ""


class GitChanges:
    """The files that a pull request adds or modifies, read from git."""

    def __init__(self, root=ROOT):
        self._root = root

    def between(self, base, head):
        for revision in (base, head):
            if not REVISION.fullmatch(revision):
                raise ValueError(f"a revision must be a commit hash, got {revision!r}")
        git = shutil.which("git")
        if git is None:
            message = "git is not installed"
            raise FileNotFoundError(message)
        # Deleted files are left out: there is nothing of them to scan.
        command = [git, "diff", "--name-only", "--diff-filter=d", "-z", f"{base}...{head}", "--"]
        done = subprocess.run(command, cwd=self._root, check=True, capture_output=True, text=True,
                              timeout=GIT_TIMEOUT_SECONDS)
        return [path for path in done.stdout.split("\0") if path]


class ScopeSettings:
    """The scanner settings: the project's settings file followed by the overrides of the scope."""

    def __init__(self, base_text):
        self._base_text = base_text

    def render(self, scope):
        lines = [self._base_text.rstrip("\n"), "", "# Scope of this analysis, written by tools/ci/sonar_scope.py."]
        lines.append(f"# {scope.reason}")
        if not scope.is_full:
            lines.append("sonar.inclusions=" + ",".join(scope.files))
        lines.extend(self._skipped_area_lines(scope))
        return "\n".join(lines) + "\n"

    @staticmethod
    def _skipped_area_lines(scope):
        lines = []
        if not scope.includes(Areas.CPP):
            # Without a compile database the C and C++ analyser must not run at all.
            lines += ["sonar.c.file.suffixes=-", "sonar.cpp.file.suffixes=-", "sonar.objc.file.suffixes=-",
                      "sonar.coverageReportPaths="]
        if not scope.includes(Areas.PYTHON):
            lines.append("sonar.python.coverage.reportPaths=")
        if not scope.includes(Areas.WEB):
            lines.append("sonar.javascript.lcov.reportPaths=")
        return lines


class StepOutputs:
    """Writes the decision as GitHub Actions step outputs, when the step runs there."""

    def __init__(self, environ=None):
        self._path = (os.environ if environ is None else environ).get("GITHUB_OUTPUT")

    def write(self, scope):
        values = {area: scope.includes(area) for area in Areas.ALL}
        values["full"] = scope.is_full
        text = "".join(f"{key}={str(value).lower()}\n" for key, value in values.items())
        if self._path:
            with open(self._path, "a", encoding="utf-8") as output:
                output.write(text)
        return text


class RepositoryPaths:
    """Resolves command-line paths and refuses any that leave the repository."""

    def __init__(self, root):
        self._root = Path(root).resolve()

    def resolve(self, text):
        path = (self._root / text).resolve()
        if not path.is_relative_to(self._root):
            raise ValueError(f"{text} is outside the repository")
        return path


class Command:
    """The command line: reads the changes, plans the scope and writes the settings and outputs."""

    def __init__(self, root=ROOT, changes=None, environ=None):
        self._paths = RepositoryPaths(root)
        self._changes = changes or GitChanges(Path(root))
        self._outputs = StepOutputs(environ)

    @staticmethod
    def _parser():
        parser = argparse.ArgumentParser(description="Decide the scope of a SonarQube Cloud analysis.")
        parser.add_argument("--settings", required=True, help="the project's Sonar settings file")
        parser.add_argument("--output", required=True, help="settings file to write for the scanner")
        parser.add_argument("--base", default="", help="commit the pull request is compared with; empty for all")
        parser.add_argument("--head", default="", help="last commit of the pull request")
        return parser

    def run(self, argv):
        args = self._parser().parse_args(argv)
        try:
            scope = self._scope(args)
            settings = ScopeSettings(self._paths.resolve(args.settings).read_text(encoding="utf-8"))
            target = self._paths.resolve(args.output)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(settings.render(scope), encoding="utf-8")
            outputs = self._outputs.write(scope)
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            print(f"sonar_scope: {error}", file=sys.stderr)
            return 2
        kind = "full analysis" if scope.is_full else "changed files only"
        print(f"sonar_scope: {kind} ({scope.reason})")
        print(outputs, end="")
        return 0

    def _scope(self, args):
        if not args.base:
            return Scope.full("no base commit given")
        if not args.head:
            raise ValueError("--head is required with --base")
        return ScopePlanner().plan(self._changes.between(args.base, args.head))


if __name__ == "__main__":
    sys.exit(Command().run(sys.argv[1:]))
