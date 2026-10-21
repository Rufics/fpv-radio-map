"""The perf.py command line; see perf.py for the usage."""
import argparse
import sys
from pathlib import Path
from typing import TextIO

from perfkit.catalog import Catalog, CatalogLoader, KindRegistry
from perfkit.commands import CommandRunner
from perfkit.environment import BuildProbe, Environment, GitProbe, HostProbe
from perfkit.errors import PerfError, Problem
from perfkit.lock import LockComparer, LockFile, Mode
from perfkit.micro import MicroBuild, MicroOptions, MicroSuite
from perfkit.session import RunSession, SessionSettings

# perfkit lives in tools/perf, which is also the default catalog.
TOOL_DIR = Path(__file__).resolve().parent.parent
SCHEMAS_DIR = TOOL_DIR / "schemas"
REPO_DIR = TOOL_DIR.parent.parent
SUITES = ("micro",)


class PerfCli:
    """Parses the arguments, runs one command and turns problems into an exit code."""

    def __init__(self, output: TextIO = sys.stdout, errors: TextIO = sys.stderr, schemas_dir: Path = SCHEMAS_DIR):
        self._output = output
        self._errors = errors
        self._schemas_dir = schemas_dir

    def parser(self) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser(prog="perf.py", description="Perf toolchain for the C++ code.")
        parser.add_argument("--catalog-dir", type=Path, default=TOOL_DIR,
                            help="folder with fixtures/, personas/, scenarios/ and extensions/ (default: %(default)s)")
        commands = parser.add_subparsers(dest="command", required=True)
        listing = commands.add_parser("list", help="show the descriptions and extensions found")
        listing.add_argument("--kind", help="only this kind, for example persona")
        catalog = commands.add_parser("catalog", help="check or lock the catalog")
        catalog.add_argument("action", choices=[mode.value for mode in Mode])
        run = commands.add_parser("run", help="run a suite, record it and compare it with the baseline")
        run.add_argument("--suite", choices=SUITES, required=True)
        run.add_argument("--only", action="append", default=[], metavar="ID",
                         help="only this benchmark; repeat for more")
        run.add_argument("--repetitions", type=int, help="override the repetitions of every benchmark")
        run.add_argument("--build-dir", type=Path, default=REPO_DIR / "build-perf",
                         help="CMake build folder for the benchmarks (default: %(default)s)")
        run.add_argument("--results-dir", type=Path, default=TOOL_DIR / "results",
                         help="run folders and history.sqlite (default: %(default)s)")
        run.add_argument("--window", type=int, default=10, help="earlier runs in the baseline (default: 10)")
        run.add_argument("--no-gate", action="store_true", help="exit 0 even when a measurement regressed")
        return parser

    def main(self, argv: list[str] | None = None) -> int:
        args = self.parser().parse_args(argv)
        try:
            registry = KindRegistry.load(self._schemas_dir)
            catalog, problems = CatalogLoader(registry).load(args.catalog_dir.resolve())
            if args.command == "list":
                return self._list(catalog, problems, args.kind, registry)
            if args.command == "run":
                return self._run(catalog, problems, args)
            return self._catalog(catalog, problems, Mode(args.action))
        except PerfError as error:
            print(f"error: {error}", file=self._errors)
            return 1

    def _list(self, catalog: Catalog, problems: list[Problem], kind: str | None, registry: KindRegistry) -> int:
        if kind is not None and registry.get(kind) is None:
            known = ", ".join(sorted(spec.name for spec in registry.kinds()))
            print(f"error: unknown kind '{kind}'; known kinds: {known}", file=self._errors)
            return 2
        rows = [("KIND", "ID", "VERSION", "GROUP", "FOLDER")]
        rows += [(item.kind.name, item.id, str(item.version), item.kind.group,
                  item.folder.relative_to(catalog.root).as_posix()) for item in catalog.items(kind)]
        widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]) - 1)]
        for row in rows:
            cells = [cell.ljust(width) for cell, width in zip(row[:-1], widths, strict=True)] + [row[-1]]
            print("  ".join(cells), file=self._output)
        return self._report(catalog, problems)

    def _catalog(self, catalog: Catalog, problems: list[Problem], mode: Mode) -> int:
        lock_file = LockFile(catalog.root)
        comparer = LockComparer(catalog, lock_file.read(), lock_file.path)
        problems = problems + comparer.problems(mode)
        if problems:
            return self._report(catalog, problems)
        if mode is Mode.LOCK:
            lock_file.write(comparer.current())
            print(f"locked {len(catalog.items())} item(s) in {lock_file.path.name}", file=self._output)
        else:
            print(f"catalog is consistent: {len(catalog.items())} item(s)", file=self._output)
        return 0

    def _run(self, catalog: Catalog, problems: list[Problem], args: argparse.Namespace) -> int:
        if problems:
            return self._report(catalog, problems)
        if args.repetitions is not None and args.repetitions < 1:
            print("error: --repetitions must be at least 1", file=self._errors)
            return 2
        build = MicroBuild(REPO_DIR, args.build_dir.resolve(), catalog.root)
        suite = MicroSuite(catalog, build, CommandRunner(self._errors),
                           MicroOptions(tuple(args.only), args.repetitions))
        environment = Environment([GitProbe(REPO_DIR), HostProbe(), BuildProbe(build.build_dir)])
        settings = SessionSettings(args.results_dir.resolve(), gate=not args.no_gate, window=args.window)
        return RunSession(settings, environment, self._output).run(suite)

    def _report(self, catalog: Catalog, problems: list[Problem]) -> int:
        for problem in problems:
            print(problem.describe(catalog.root), file=self._errors)
        if problems:
            print(f"{len(problems)} problem(s) in the catalog", file=self._errors)
        return 1 if problems else 0
