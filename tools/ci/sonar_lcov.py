#!/usr/bin/env python3
"""Merges LCOV reports into one whose file paths are relative to the repository root.

    python3 -I tools/ci/sonar_lcov.py --base web --output sonar-reports/web.lcov \\
        web/coverage/ui-kit/lcov.info web/coverage/fpv-app/lcov.info

The Angular test runner writes source paths relative to the web/ folder (SF:projects/...), while
SonarQube resolves them against the repository root, so it would find none of the files. Every
relative SF: line gets the --base folder in front of it; absolute paths are left as they are. All
paths given must lie inside the repository. The exit code is 2 on a usage error, 0 otherwise.

With --skip-missing, reports that do not exist are left out (Vitest writes none for a project
whose tests failed), and nothing is written when none exists.
"""
import argparse
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
SOURCE_PREFIX = "SF:"


class RepositoryPaths:
    """Resolves command-line paths and refuses any that leave the repository."""

    def __init__(self, root):
        self._root = Path(root).resolve()

    def resolve(self, text):
        path = (self._root / text).resolve()
        if not path.is_relative_to(self._root):
            raise ValueError(f"{text} is outside the repository")
        return path

    def relative(self, text):
        """The folder as a POSIX path relative to the repository root."""
        return PurePosixPath(self.resolve(text).relative_to(self._root).as_posix())


class LcovRebaser:
    """Rewrites the SF: lines of LCOV text so the paths start at the repository root."""

    def __init__(self, base):
        self._base = PurePosixPath(base)

    def rebase_line(self, line):
        if not line.startswith(SOURCE_PREFIX):
            return line
        source = line[len(SOURCE_PREFIX):].strip()
        if PurePosixPath(source).is_absolute() or self._base == PurePosixPath("."):
            return line
        return f"{SOURCE_PREFIX}{self._base / source}\n"

    def rebase(self, text):
        lines = (self.rebase_line(line) for line in text.splitlines(keepends=True))
        rebased = "".join(lines)
        return rebased if rebased.endswith("\n") or not rebased else rebased + "\n"


class LcovMerger:
    """Reads the input reports, rebases them and writes one merged report."""

    def __init__(self, paths, rebaser):
        self._paths = paths
        self._rebaser = rebaser

    def merge(self, inputs, output):
        parts = [self._rebaser.rebase(self._paths.resolve(name).read_text(encoding="utf-8")) for name in inputs]
        target = self._paths.resolve(output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("".join(parts), encoding="utf-8")
        return target


class Command:
    """The command line: parses the arguments and runs the merge."""

    def __init__(self, root=ROOT):
        self._paths = RepositoryPaths(root)

    @staticmethod
    def _parser():
        parser = argparse.ArgumentParser(description="Merge LCOV reports with repository-relative paths.")
        parser.add_argument("--base", required=True, help="folder the report paths are relative to")
        parser.add_argument("--output", required=True, help="merged report to write")
        parser.add_argument("--skip-missing", action="store_true", help="leave out reports that do not exist")
        parser.add_argument("reports", nargs="+", help="LCOV reports to merge")
        return parser

    def run(self, argv):
        args = self._parser().parse_args(argv)
        try:
            reports = self._existing(args.reports) if args.skip_missing else args.reports
            if not reports:
                print("sonar_lcov: none of the reports exists; nothing written")
                return 0
            rebaser = LcovRebaser(self._paths.relative(args.base))
            target = LcovMerger(self._paths, rebaser).merge(reports, args.output)
        except (ValueError, OSError) as error:
            print(f"sonar_lcov: {error}", file=sys.stderr)
            return 2
        print(f"sonar_lcov: {len(reports)} report(s) -> {target.relative_to(self._paths.resolve('.'))}")
        return 0

    def _existing(self, reports):
        return [name for name in reports if self._paths.resolve(name).is_file()]


if __name__ == "__main__":
    sys.exit(Command().run(sys.argv[1:]))
