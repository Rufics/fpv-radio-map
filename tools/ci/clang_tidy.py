#!/usr/bin/env python3
"""Runs clang-tidy over the C++ sources of a configured build, with the checks in .clang-tidy.

    python3 -I tools/ci/clang_tidy.py --build-dir build-tidy [--base <commit>] [--jobs N]
                                      [--clang-tidy clang-tidy-18]

The build directory must hold compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS=ON) and any
generated headers. Every translation unit of this repository in it is checked, fetched
dependencies and generated files excepted. With --base only the sources changed since that commit
are checked, unless a change can affect any file: a header, .clang-tidy, a CMake file or this
script then checks everything.

Each finding is printed as clang-tidy prints it and, on GitHub Actions, also as an error
annotation on its line. The exit code is 1 when clang-tidy reported anything, 2 on a usage
error, 0 otherwise.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
# Paths whose change can alter the result for files that did not change.
CHECK_ALL = re.compile(r"\.(h|hh|hpp|hxx|inl|cmake)$|(^|/)(CMakeLists\.txt|\.clang-tidy)$|^tools/ci/clang_tidy\.py$")
SKIPPED_DIRS = ("_deps", "generated")
REVISION = re.compile(r"[0-9a-f]{7,64}|HEAD(\^\d?|~\d{1,3})?")
CLANG_TIDY_NAME = re.compile(r"clang-tidy(-\d+)?")
FINDING = re.compile(r"^(?P<path>/.+?):(?P<line>\d+):(?P<col>\d+): (?P<level>warning|error): (?P<msg>.*)$")
TIMEOUT_SECONDS = 600
MAX_LINE = 4096


def repo_relative(path):
    """The path relative to the repository, or None for a file outside it."""
    try:
        return Path(path).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None


def translation_units(compile_commands, build_dir):
    """Repository sources listed in a compile_commands.json, as sorted repository-relative paths."""
    build = repo_relative(build_dir)
    units = set()
    for entry in compile_commands:
        path = Path(entry.get("directory", "."), entry["file"])
        rel = repo_relative(path)
        if rel is None or (build and rel.startswith(build + "/")):
            continue
        if any(part in SKIPPED_DIRS for part in Path(rel).parts) or not rel.endswith(SOURCE_SUFFIXES):
            continue
        units.add(rel)
    return sorted(units)


def select(units, changed):
    """The units to check for a change set; None as the change set means everything."""
    if changed is None or any(CHECK_ALL.search(path) for path in changed):
        return list(units)
    wanted = set(changed)
    return [unit for unit in units if unit in wanted]


def changed_files(base):
    """Files changed between base and HEAD, from git."""
    if not REVISION.fullmatch(base):
        raise ValueError(f"--base must be a commit hash or HEAD^<n>/HEAD~<n>, got {base!r}")
    git = shutil.which("git")
    if git is None:
        message = "git is not installed"
        raise FileNotFoundError(message)
    out = subprocess.run([git, "diff", "--name-only", "--no-renames", base, "HEAD", "--"],
                         cwd=ROOT, check=True, capture_output=True, text=True, timeout=60).stdout
    return [line for line in out.splitlines() if line]


def escape_annotation(text):
    """A value for a GitHub workflow command, which ends at a newline."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def annotations(output):
    """GitHub error annotations for the findings in clang-tidy output, one per distinct finding."""
    seen = set()
    for raw in output.splitlines():
        match = FINDING.match(raw) if len(raw) <= MAX_LINE else None
        rel = repo_relative(match["path"]) if match else None
        if rel is None:
            continue
        key = (rel, match["line"], match["col"], match["msg"])
        if key in seen:
            continue
        seen.add(key)
        prop = escape_annotation(rel).replace(":", "%3A").replace(",", "%2C")
        yield f"::error file={prop},line={match['line']},col={match['col']}::{escape_annotation(match['msg'])}"


def run_one(clang_tidy, build_dir, unit):
    """clang-tidy on one file: (exit code, output)."""
    try:
        done = subprocess.run([clang_tidy, "-p", str(build_dir), "--quiet", str(ROOT / unit)],
                              cwd=ROOT, check=False, capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return 1, f"{unit}: clang-tidy took longer than {TIMEOUT_SECONDS} s\n"
    return done.returncode, done.stdout + done.stderr


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path, help="CMake build directory")
    parser.add_argument("--base", help="check only the files changed since this commit (hash, HEAD^1, ...)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel clang-tidy runs")
    parser.add_argument("--clang-tidy", default="clang-tidy", help="clang-tidy or clang-tidy-<version>, from PATH")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if not CLANG_TIDY_NAME.fullmatch(args.clang_tidy):
        print(f"--clang-tidy must be clang-tidy or clang-tidy-<version>, got {args.clang_tidy!r}", file=sys.stderr)
        return 2
    clang_tidy = shutil.which(args.clang_tidy)
    if clang_tidy is None:
        print(f"{args.clang_tidy} is not installed", file=sys.stderr)
        return 2
    build_dir = args.build_dir.resolve()
    with open(build_dir / "compile_commands.json", encoding="utf-8") as f:
        units = translation_units(json.load(f), build_dir)
    units = select(units, changed_files(args.base) if args.base else None)
    if not units:
        print("No C++ sources to check.")
        return 0
    print(f"Checking {len(units)} file(s) with {args.jobs} job(s).", flush=True)

    failed = 0
    on_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = pool.map(lambda unit: run_one(clang_tidy, build_dir, unit), units)
        for unit, (code, output) in zip(units, results, strict=True):
            if code == 0:
                continue
            failed += 1
            print(f"--- {unit}\n{output}", flush=True)
            if on_actions:
                print("\n".join(annotations(output)), flush=True)
    print(f"clang-tidy: {failed} of {len(units)} file(s) with findings.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
