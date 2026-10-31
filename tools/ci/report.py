#!/usr/bin/env python3
"""Builds the Markdown CI report that is posted to a pull request and to the job summary.

    cd <data folder> && python3 -I path/to/tools/ci/report.py > report.md
    cd <data folder> && python3 -I path/to/tools/ci/report.py --all-tests > all-tests.md

The first form writes the report; `--all-tests` writes the list of every test, one folded table
per module, for the run summary (the pull request comment would be too long with it). The
script takes no other arguments and reads only fixed names under the current directory, so no
input can point it at another file:

    tests/*.xml             JUnit results (ctest --output-junit, vitest junit reporter,
                            unittest_junit.py); see junit.py
    coverage/coverage.json  gcovr JSON report of the instrumented C++ build
    coverage/changes.diff   `git diff --unified=0` of the pull request
    coverage/commit.txt     the commit the coverage was measured on; uncovered lines link to it
    coverage-web/web.lcov   LCOV of the web unit tests, paths from the repository root
    coverage-web/changes.diff, coverage-web/commit.txt
                            the same as above, from the web job; used when the C++ ones are absent
    jobs.json               the run's jobs as listed by the Actions API: [{name, result, url}]

Environment, all optional:

    REPORT_JOBS          JSON `needs` context of the calling job, used when jobs.json is absent
    REPORT_SHA           the pull request's head commit, shown in the headline
    REPORT_RUN_URL       link to the workflow run
    REPORT_COVERAGE_URL  link to the HTML coverage artifact
    REPORT_WEB_COVERAGE_URL  link to the HTML web coverage artifact
    GITHUB_REPOSITORY    owner/name, for links to the uncovered lines

Every input may come from a pull request of a fork, so it is treated as untrusted data: sizes are
capped, nothing is executed, and every string that reaches the Markdown is escaped.
"""
import json
import os
import re
import sys
from pathlib import Path

# `python3 -I` leaves the script's folder off the path; junit.py sits next to this file in the
# trusted checkout, never in the data folder this script runs in.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from junit import JUnitParser, SourceLocation, TestCase  # noqa: E402
from lcov import LcovParser, WebComponents  # noqa: E402

MARKER = "<!-- rfmap-ci-report -->"
MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_LISTED = 20

# Display names of the CI jobs, keyed by their id in ci.yml.
JOB_NAMES = {"clang-tidy": "C++ static analysis", "cpp": "C++ build and tests", "coverage": "C++ coverage",
             "fuzz": "Fuzz C++", "web": "Web build and tests"}
# Jobs that only orchestrate the run; they are not results worth a row.
HIDDEN_JOBS = {"changes", "report", "Changed parts", "Test report"}
SUITES = {"cpp.xml": "C++", "web-ui-kit.xml": "Web: ui-kit", "web-fpv-app.xml": "Web: fpv-app",
          "python-ci.xml": "Python: tools/ci", "python-perf.xml": "Python: tools/perf",
          "python-blackbox.xml": "Python: tools/blackbox"}
# CTest labels (the module of a C++ test) in the order of the coverage areas.
TEST_MODULE_ORDER = ("rf-core", "rf-ext", "rf-flightlog", "rf-db", "rfmod", "fpv", "server", "example")
# The failure excerpts of one report, so the comment stays far below GitHub's 65536 characters.
MAX_REASONS_CHARS = 30000
# The list of all tests goes to the run summary, which takes at most 1 MiB per step.
MAX_ALL_TESTS_CHARS = 900_000
OUTCOME_ICONS = {"passed": "✅", "failed": "❌", "skipped": "⏭️"}
LEGEND = ("<sub>🟢 80% or more · 🟡 60–80% · 🔴 below 60%. Coverage is reported, not enforced. "
          "Test code is not counted.</sub>")
ICONS = {"success": "✅", "skipped": "⏭️", "cancelled": "🚫", "failure": "❌"}
DETAILS_END = "</details>"
COVERAGE_ALIGN = "|---|---:|---:|"
FPV = "FPV modules"

# Coverage areas, most specific prefix first. Files that match none count in "All C++" only.
COMPONENTS = [
    ("core/rf-core/", "Core", "rf-core"),
    ("core/rf-ext/", "Core", "rf-ext"),
    ("core/rf-flightlog/", "Core", "rf-flightlog"),
    ("core/rf-db/", "Core", "rf-db"),
    ("platform/rfmod/", "Platform", "rfmod"),
    ("modules/fpv/common/", FPV, "fpv common"),
    ("modules/fpv/", FPV, None),  # fpv.<module folder>
    ("server/", "Server", "server"),
]
AREAS = [
    ("Core", "Core: rf-core, rf-ext, rf-flightlog, rf-db"),
    (FPV, FPV),
    ("Platform", "Platform: rfmod"),
    ("Server", "Server"),
]

SAFE_PATH = re.compile(r"\w[\w.+/-]{0,300}")
SAFE_SHA = re.compile(r"[0-9a-f]{7,64}")
SAFE_REPO = re.compile(r"[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}")
SAFE_URL = re.compile(r"https://github\.com/[A-Za-z0-9_./?=&%#-]{1,500}")
MD_SPECIAL = re.compile(r"([\\`*_{}\[\]()|~])")

# --------------------------------------------------------------------------------------------
# Escaping and small helpers


def md(text, limit=200):
    """Plain text for a Markdown table cell or list item: no HTML, links, mentions or table breaks."""
    s = re.sub(r"[\x00-\x1f\x7f]+", " ", str(text if text is not None else "")).strip()
    if len(s) > limit:
        s = s[: limit - 1] + "…"
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    s = MD_SPECIAL.sub(r"\\\1", s)
    return s.replace("@", "@​").replace("://", ":​//")


def safe_path(p):
    """Repository paths that are safe to show and to put in a URL."""
    return isinstance(p, str) and SAFE_PATH.fullmatch(p) is not None and ".." not in p.split("/")


def checked(pattern, value):
    return value if isinstance(value, str) and pattern.fullmatch(value) else None


def read_capped(rel):
    """Text of a file under the current directory, or None when missing, odd or too large."""
    base = Path.cwd().resolve()
    path = (base / rel).resolve()
    if not path.is_relative_to(base) or not path.is_file() or path.stat().st_size > MAX_INPUT_BYTES:
        return None
    return path.read_text(encoding="utf-8", errors="replace")


def pct(covered, total):
    return 100 * covered / total if total else None


def fmt_pct(p):
    return "–" if p is None else f"{p:.1f}%"


def dot(p):
    if p is None:
        return "⚪"
    if p >= 80:
        return "🟢"
    return "🟡" if p >= 60 else "🔴"


def plural(n, one, many=None):
    return f"{n} {one if n == 1 else (many or one + 's')}"


def as_list(v):
    return v if isinstance(v, list) else []


def as_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


# --------------------------------------------------------------------------------------------
# Tests (JUnit)

def parse_junit(xml):
    """Totals, failed test names and the test cases of one JUnit file."""
    parsed = JUnitParser().parse(xml)
    failed = [c.name for c in parsed.cases if c.failed]
    skipped = sum(1 for c in parsed.cases if c.skipped)
    # Prefer the count the runner wrote on the root element; fall back to counting cases.
    total = max(parsed.declared_total or len(parsed.cases), len(failed) + skipped)
    return {"total": total, "failed": failed, "skipped": skipped, "passed": total - len(failed) - skipped,
            "cases": list(parsed.cases)}


def read_suites():
    folder = Path.cwd() / "tests"
    if not folder.is_dir():
        return []
    suites = []
    for f in sorted(p.name for p in folder.iterdir() if p.suffix == ".xml"):
        xml = read_capped(Path("tests") / f)
        if xml is not None:
            suites.append({"file": f, "label": SUITES.get(f, f.removesuffix(".xml")), **parse_junit(xml)})
    return suites


# --------------------------------------------------------------------------------------------
# Coverage (gcovr JSON)


def component_of(path):
    """(area, component) of a repository path, or None for files outside the known areas."""
    for prefix, area, name in COMPONENTS:
        if path.startswith(prefix):
            return area, name or "fpv." + path[len(prefix):].split("/")[0]
    return None


def _empty():
    return {"lines": 0, "linesCovered": 0, "functions": 0, "functionsCovered": 0}


def _add(t, f):
    for k in _empty():
        t[k] += f[k]


def _max_into(d, key, count):
    d[key] = max(d.get(key, 0), count)


def _line_hits(hits, lines):
    for line in lines:
        if not isinstance(line, dict) or line.get("gcovr/noncode") or line.get("gcovr/excluded"):
            continue
        n = line.get("line_number")
        if isinstance(n, int) and n >= 1:
            _max_into(hits, n, as_int(line.get("count")))


def _function_hits(fns, functions):
    for fn in functions:
        if isinstance(fn, dict) and not fn.get("gcovr/excluded"):
            count = fn.get("execution_count", fn.get("count"))
            _max_into(fns, f"{fn.get('lineno')}:{fn.get('name')}", as_int(count))


def parse_coverage(text):
    """Per-file coverage: {path: {lines, linesCovered, functions, functionsCovered, hits}}."""
    try:
        data = json.loads(text)
    except ValueError:
        return {}
    raw = {}
    for f in as_list(data.get("files") if isinstance(data, dict) else None):
        path = f.get("file") if isinstance(f, dict) else None
        path = path.removeprefix("./") if isinstance(path, str) else None
        if not safe_path(path):
            continue
        hits, fns = raw.setdefault(path, ({}, {}))
        _line_hits(hits, as_list(f.get("lines")))
        _function_hits(fns, as_list(f.get("functions")))
    files = {}
    for path, (hits, fns) in raw.items():
        files[path] = {
            "lines": len(hits),
            "linesCovered": sum(1 for c in hits.values() if c > 0),
            "functions": len(fns),
            "functionsCovered": sum(1 for c in fns.values() if c > 0),
            "hits": hits,
        }
    return files


def summarize_coverage(files):
    total, areas, components = _empty(), {}, {}
    for path, f in files.items():
        _add(total, f)
        c = component_of(path)
        if not c:
            continue
        _add(areas.setdefault(c[0], _empty()), f)
        _add(components.setdefault(c, {"area": c[0], "component": c[1], **_empty()}), f)
    return {"total": total, "areas": areas, "components": list(components.values())}


# --------------------------------------------------------------------------------------------
# Changed lines (git diff --unified=0)

TARGET = re.compile(r"\+\+\+ b/(.+)")
HUNK = re.compile(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def parse_diff(diff):
    """Added or modified line numbers per file of a unified diff: {path: set(lines)}."""
    changed, lines = {}, None
    for line in diff.split("\n"):
        if line.startswith("+++ "):
            m = TARGET.fullmatch(line)
            lines = changed.setdefault(m.group(1), set()) if m and safe_path(m.group(1)) else None
        elif lines is not None and (h := HUNK.match(line)):
            start = int(h.group(1))
            count = min(1 if h.group(2) is None else int(h.group(2)), 100000)
            lines.update(range(start, start + count))
    return changed


def _ranges(numbers):
    out = []
    for n in numbers:
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return out


def changed_line_coverage(files, changed):
    """Coverage of the changed executable lines, with the uncovered ones as ranges per file."""
    total = covered = 0
    uncovered = []
    for path, lines in changed.items():
        hits = files[path]["hits"] if path in files else {}
        executable = sorted(n for n in lines if n in hits)
        total += len(executable)
        covered += sum(1 for n in executable if hits[n] > 0)
        missing = [n for n in executable if hits[n] == 0]
        uncovered += [{"path": path, "from": a, "to": b} for a, b in _ranges(missing)]
    return {"total": total, "covered": covered, "uncovered": uncovered}


# --------------------------------------------------------------------------------------------
# Jobs


def normalize_jobs(value):
    if isinstance(value, list):
        jobs = [
            {
                "name": str(j.get("name", "?")),
                "result": str(j.get("result") or j.get("conclusion") or "unknown"),
                "url": checked(SAFE_URL, j.get("url")),
            }
            for j in value
            if isinstance(j, dict)
        ]
    elif isinstance(value, dict):
        jobs = [
            {"name": JOB_NAMES.get(k, k), "result": str((v or {}).get("result", "unknown")), "url": None}
            for k, v in value.items()
        ]
    else:
        jobs = []
    return [j for j in jobs if j["name"] not in HIDDEN_JOBS]


# --------------------------------------------------------------------------------------------
# Test details


def html_text(text, limit=200):
    """Plain text for an HTML element such as <summary>: no tags, links or mentions."""
    s = re.sub(r"[\x00-\x1f\x7f]+", " ", str(text)).strip()
    if len(s) > limit:
        s = s[: limit - 1] + "…"
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")
    return s.replace("@", "@&#8203;").replace("://", ":&#8203;//")


def code_block(text):
    """A fenced block that no backtick run in the text can close."""
    longest = max((len(run) for run in re.findall(r"`+", text)), default=0)
    fence = "`" * max(3, longest + 1)
    return [f"{fence}text", text, fence]


class ModuleTally:
    """Test counts of one module of a suite."""

    def __init__(self, label):
        self.label = label
        self.passed = self.failed = self.skipped = 0
        self.seconds = 0.0

    def add(self, case):
        if case.failed:
            self.failed += 1
        elif case.skipped:
            self.skipped += 1
        else:
            self.passed += 1
        self.seconds += case.seconds

    def row(self):
        icon = ICONS["failure"] if self.failed else ICONS["success"]
        failed = f"**{self.failed}**" if self.failed else "0"
        return (f"| {md(self.label, 80)} | {icon} | {self.passed} | {failed} | {self.skipped} "
                f"| {self.seconds:.1f} s |")


class TestDetails:
    """The failed tests with their reasons, and the test counts per module."""

    def __init__(self, suites, repo=None, commit=None):
        self.suites = suites
        self.repo = repo
        self.commit = commit
        self.failed = [(s["label"], c) for s in suites for c in self.cases_of(s) if c.failed]

    @staticmethod
    def cases_of(suite):
        """The test cases of a suite; a summary without them still lists its failed names."""
        if suite.get("cases"):
            return suite["cases"]
        return [TestCase(name=n, outcome="failed") for n in suite.get("failed", [])]

    def failures_section(self):
        if not self.failed:
            return []
        out = [f"<details open><summary><b>Failed tests</b> ({len(self.failed)})</summary>", ""]
        budget = MAX_REASONS_CHARS
        for suite, case in self.failed[:MAX_LISTED]:
            lines, used = self._failure(suite, case, budget)
            out += lines
            budget -= used
        if len(self.failed) > MAX_LISTED:
            out += [f"… and {len(self.failed) - MAX_LISTED} more failed tests; see the workflow run.", ""]
        return out + [DETAILS_END, ""]

    def _failure(self, suite, case, budget):
        """The lines of one failed test, and how much of the reason budget they use."""
        where = " · ".join(p for p in (suite, case.module, case.name) if p)
        if not case.reason:
            return [f"- {md(where, 400)}", ""], 0
        out = [f"<details><summary>{html_text(where, 400)}</summary>", ""]
        location = SourceLocation.find(case.reason)
        if location and safe_path(location[0]):
            out += [self._location_link(*location), ""]
        if len(case.reason) <= budget:
            out += [*code_block(case.reason), ""]
            used = len(case.reason)
        else:
            out += ["The reason is in the log of the workflow run.", ""]
            used = 0
        return out + [DETAILS_END, ""], used

    def _location_link(self, path, line):
        text = f"`{path}:{line}`"
        if not (self.repo and self.commit):
            return text
        return f"[{text}](https://github.com/{self.repo}/blob/{self.commit}/{path}#L{line})"

    def modules_section(self):
        tallies = self._tallies()
        if not tallies:
            return []
        out = ["<details><summary>Tests per module</summary>", "",
               "| Module | Result | Passed | Failed | Skipped | Time |", "|---|:---:|---:|---:|---:|---:|"]
        out += [t.row() for t in tallies]
        return out + ["", DETAILS_END, ""]

    def _tallies(self):
        tallies = {}
        for suite in self.suites:
            for case in suite.get("cases") or []:
                label = self._module_label(suite["label"], case.module)
                tallies.setdefault(label, ModuleTally(label)).add(case)
        return sorted(tallies.values(), key=lambda t: self._order(t.label))

    @staticmethod
    def _module_label(suite, module):
        return f"{suite}: {module}" if module else suite

    @staticmethod
    def _order(label):
        """C++ modules in the order of the coverage areas, then the other suites by name."""
        suite, _, module = label.partition(": ")
        if suite != "C++":
            return (1, len(TEST_MODULE_ORDER), label)
        rank = next((i for i, p in enumerate(TEST_MODULE_ORDER) if module.startswith(p)), len(TEST_MODULE_ORDER))
        return (0, rank, label)


class AllTestsList:
    """Every test of the run, one folded table per module, for the run summary."""

    def __init__(self, suites, limit=MAX_ALL_TESTS_CHARS):
        self.suites = suites
        self.limit = limit

    def render(self):
        groups = self._groups()
        total = sum(len(cases) for cases in groups.values())
        out = [f"### All tests ({total})", ""]
        if not groups:
            return "\n".join(out + ["No test results were found.", ""])
        used = 0
        for label in sorted(groups, key=TestDetails._order):
            block = self._block(label, groups[label])
            used += sum(len(line) + 1 for line in block)
            if used > self.limit:
                out += ["… the list is cut here; the JUnit files are in the artifacts of the run.", ""]
                break
            out += block
        return "\n".join(out)

    def _groups(self):
        groups = {}
        for suite in self.suites:
            for case in suite.get("cases") or []:
                groups.setdefault(TestDetails._module_label(suite["label"], case.module), []).append(case)
        return groups

    @staticmethod
    def _block(label, cases):
        failed = sum(1 for c in cases if c.failed)
        icon = ICONS["failure"] if failed else ICONS["success"]
        counts = plural(len(cases), "test") + (f", {failed} failed" if failed else "")
        out = [f"<details><summary>{icon} {html_text(label, 80)}: {counts}</summary>", "",
               "| Test | Result | Time |", "|---|:---:|---:|"]
        ordered = sorted(cases, key=lambda c: (not c.failed, c.name))
        out += [f"| {md(c.name, 300)} | {OUTCOME_ICONS[c.outcome]} | {c.seconds:.2f} s |" for c in ordered]
        return out + ["", DETAILS_END, ""]


# --------------------------------------------------------------------------------------------
# Web coverage (LCOV)


def summarize_web_coverage(files):
    """Totals, per project and per folder of the web sources."""
    total, areas, components = _empty(), {}, {}
    for path, f in files.items():
        _add(total, f)
        c = WebComponents.of(path)
        if not c:
            continue
        _add(areas.setdefault(c[0], _empty()), f)
        _add(components.setdefault(c, {"area": c[0], "component": c[1], **_empty()}), f)
    return {"total": total, "areas": areas, "components": list(components.values())}


def parse_web_coverage(text):
    """Per-file web coverage of an LCOV report: product code with safe repository paths only."""
    files = LcovParser("web").parse(text).items()
    return {p: f for p, f in files if safe_path(p) and "/tests/" not in p and ".spec." not in p}


class WebCoverageSection:
    """The web coverage: per project, the changed lines and a breakdown per folder."""

    def __init__(self, files, diff, repo, commit):
        self.summary = summarize_web_coverage(files)
        self.changed = changed_line_coverage(files, diff) if diff is not None else None
        self.diff = diff
        self.repo = repo
        self.commit = commit

    def line_pct(self):
        t = self.summary["total"]
        return pct(t["linesCovered"], t["lines"])

    def lines(self):
        areas = self.summary["areas"]
        out = ["#### Web coverage", "", "| Project | Lines | Functions |", COVERAGE_ALIGN]
        out += [_coverage_row(md(name, 60), areas[name]) for name in sorted(areas)]
        out += [_coverage_row("All web", self.summary["total"], strong=True), ""]
        if self.changed and self.changed["total"]:
            out += _changed_lines(self.changed, self.repo, self.commit)
        elif self.diff is not None:
            out += ["**Changed lines:** this pull request changes no executable web line.", ""]
        return out + _components(self.summary["components"], sorted(areas))


# --------------------------------------------------------------------------------------------
# Report


def build_report(jobs=None, suites=(), coverage=None, diff=None, sha=None, commit=None, repo=None,
                 run_url=None, coverage_url=None, *, web_coverage=None, web_coverage_url=None):
    ctx = {
        "jobs": normalize_jobs(jobs if jobs is not None else {}),
        "suites": list(suites),
        "sha": checked(SAFE_SHA, sha),
        "repo": checked(SAFE_REPO, repo),
        "diff": diff,
    }
    ctx["commit"] = checked(SAFE_SHA, commit) or ctx["sha"]
    ctx["cov"] = summarize_coverage(coverage) if coverage else None
    ctx["changed"] = changed_line_coverage(coverage, diff) if ctx["cov"] and diff is not None else None
    ctx["web"] = WebCoverageSection(web_coverage, diff, ctx["repo"], ctx["commit"]) if web_coverage else None
    details = TestDetails(ctx["suites"], ctx["repo"], ctx["commit"])
    ctx["failed_tests"] = details.failed
    ctx["ran"] = any(j["result"] != "skipped" for j in ctx["jobs"]) or bool(ctx["suites"])

    lines = [MARKER, *_headline(ctx), *details.failures_section()]
    if ctx["ran"]:
        lines += _jobs_table(ctx)
    lines += details.modules_section()
    if ctx["cov"]:
        lines += _coverage_section(ctx)
    if ctx["web"]:
        lines += ctx["web"].lines()
    if ctx["cov"] or ctx["web"]:
        lines += [LEGEND, ""]
    lines += _footer(checked(SAFE_URL, run_url), checked(SAFE_URL, coverage_url), checked(SAFE_URL, web_coverage_url))
    return re.sub(r"\n{3,}", "\n\n", "\n".join(lines)).rstrip() + "\n"


def _headline(ctx):
    """The one line a developer needs to read, and the key numbers under it."""
    failed_jobs = [j for j in ctx["jobs"] if j["result"] in ("failure", "cancelled")]
    failed_tests = ctx["failed_tests"]
    at = f" for {ctx['sha'][:7]}" if ctx["sha"] else ""
    out = []
    if failed_jobs or failed_tests:
        what = plural(len(failed_tests), "test") if failed_tests else ", ".join(j["name"] for j in failed_jobs)
        what += " failed"
        out.append(f"### ❌ CI failed{at}: {md(what, 300)}")
    else:
        out.append(f"### ✅ CI passed{at}")
        if not ctx["ran"]:
            out += ["", "Nothing to build: this change touches only files the tests do not cover, such as docs."]
    facts = _key_facts(ctx)
    if facts:
        out += ["", " · ".join(facts)]
    out.append("")
    return out


def _key_facts(ctx):
    facts = []
    if ctx["suites"]:
        facts.append(f"**{sum(s['passed'] for s in ctx['suites'])}** tests passed")
    if ctx["cov"]:
        t = ctx["cov"]["total"]
        facts.append(f"C++ line coverage **{fmt_pct(pct(t['linesCovered'], t['lines']))}**")
    if ctx["web"]:
        facts.append(f"web line coverage **{fmt_pct(ctx['web'].line_pct())}**")
    changed = [c for c in (ctx["changed"], ctx["web"] and ctx["web"].changed) if c]
    total = sum(c["total"] for c in changed)
    if total:
        covered = sum(c["covered"] for c in changed)
        facts.append(f"changed lines **{fmt_pct(pct(covered, total))}** covered")
    return facts


def _suites_of(job, suites):
    """The JUnit suites a job produced."""
    if job["name"].startswith("C++ build"):
        return [s for s in suites if s["file"] == "cpp.xml"]
    if job["name"].startswith("Web"):
        return [s for s in suites if s["file"].startswith("web-")]
    return []


def _suite_counts(s, prefixed):
    counts = [f"{s['passed']} passed"]
    if s["failed"]:
        counts.append(f"**{len(s['failed'])} failed**")
    if s["skipped"]:
        counts.append(f"{s['skipped']} skipped")
    prefix = f"{md(s['label'], 40)}: " if prefixed else ""
    return prefix + ", ".join(counts)


def _job_details(job, ctx):
    own = _suites_of(job, ctx["suites"])
    if own:
        return "<br>".join(_suite_counts(s, len(own) > 1 or s["label"] != "C++") for s in own)
    if job["name"] == JOB_NAMES["coverage"] and ctx["cov"]:
        t = ctx["cov"]["total"]
        return f"{fmt_pct(pct(t['linesCovered'], t['lines']))} of C++ lines"
    if job["result"] == "failure":
        return "failed before any test result was written; open the log"
    if job["result"] == "skipped":
        return "not affected by this change"
    return ""


def _jobs_table(ctx):
    """One row per job, with the test counts of the suites it produced."""
    out = ["| Check | Result | Details |", "|---|---|---|"]
    for j in ctx["jobs"]:
        name = f"[{md(j['name'], 80)}]({j['url']})" if j["url"] else md(j["name"], 80)
        out.append(f"| {name} | {ICONS.get(j['result'], '❔')} {md(j['result'], 20)} | {_job_details(j, ctx)} |")
    return out + [""]


def _coverage_row(label, t, strong=False):
    lp, fp = pct(t["linesCovered"], t["lines"]), pct(t["functionsCovered"], t["functions"])

    def b(s):
        return f"**{s}**" if strong else s

    return (f"| {b(label)} | {dot(lp)} {b(fmt_pct(lp))} <sub>{t['linesCovered']}/{t['lines']}</sub> "
            f"| {b(fmt_pct(fp))} <sub>{t['functionsCovered']}/{t['functions']}</sub> |")


def _coverage_section(ctx):
    cov, changed = ctx["cov"], ctx["changed"]
    out = ["#### C++ coverage", "", "| Area | Lines | Functions |", COVERAGE_ALIGN]
    out += [_coverage_row(label, cov["areas"][name]) for name, label in AREAS if name in cov["areas"]]
    out += [_coverage_row("All C++", cov["total"], strong=True), ""]
    if changed and changed["total"]:
        out += _changed_lines(changed, ctx["repo"], ctx["commit"])
    elif ctx["diff"] is not None:
        out += ["**Changed lines:** this pull request changes no executable C++ line.", ""]
    return out + _components(cov["components"], [name for name, _ in AREAS])


def _changed_lines(changed, repo, commit):
    p = pct(changed["covered"], changed["total"])
    missing = changed["total"] - changed["covered"]
    out = [f"**Changed lines:** {dot(p)} {fmt_pct(p)} covered ({changed['covered']} of "
           f"{plural(changed['total'], 'executable line')} this pull request adds or changes)."]
    if missing:
        out += ["", f"<details><summary>{plural(missing, 'changed line')} not run by any test</summary>", ""]
        out += _uncovered_list(changed["uncovered"], repo, commit)
        out += ["", DETAILS_END]
    return out + [""]


def _line_link(repo, commit, path, u):
    """A line range of a file, linked to the commit when the repository and commit are known."""
    if u["from"] == u["to"]:
        text, anchor = f"{u['from']}", f"L{u['from']}"
    else:
        text, anchor = f"{u['from']}-{u['to']}", f"L{u['from']}-L{u['to']}"
    if not (repo and commit):
        return text
    return f"[{text}](https://github.com/{repo}/blob/{commit}/{path}#{anchor})"


def _uncovered_list(uncovered, repo, commit):
    """Uncovered changed lines, one item per file, each range linked to the commit."""
    by_file = {}
    for u in uncovered:
        by_file.setdefault(u["path"], []).append(u)
    out = []
    for path, ranges in list(by_file.items())[:MAX_LISTED]:
        shown = [_line_link(repo, commit, path, u) for u in ranges[:10]]
        if len(ranges) > 10:
            shown.append(f"… {len(ranges) - 10} more")
        # safe_path() admits no backtick, so the path is shown as code without escaping.
        out.append(f"- `{path}`: {', '.join(shown)}")
    if len(by_file) > MAX_LISTED:
        out.append(f"- … and {plural(len(by_file) - MAX_LISTED, 'more file')}")
    return out


def _components(components, order):
    """The coverage of each component, folded away, grouped by area in the given order."""
    if not components:
        return []
    out = ["<details><summary>Coverage per component</summary>", "", "| Component | Lines | Functions |", COVERAGE_ALIGN]
    for c in sorted(components, key=lambda c: (order.index(c["area"]), c["component"])):
        lp = pct(c["linesCovered"], c["lines"])
        out.append(f"| {md(c['component'], 60)} | {dot(lp)} {fmt_pct(lp)} <sub>{c['linesCovered']}/{c['lines']}</sub> "
                   f"| {fmt_pct(pct(c['functionsCovered'], c['functions']))} |")
    return out + ["", DETAILS_END, ""]


def _footer(run_url, coverage_url, web_coverage_url=None):
    links = []
    if run_url:
        links.append(f"[Workflow run and the list of all tests]({run_url})")
    if coverage_url:
        links.append(f"[HTML coverage report]({coverage_url})")
    if web_coverage_url:
        links.append(f"[HTML web coverage report]({web_coverage_url})")
    return [" · ".join(links)] if links else []


# --------------------------------------------------------------------------------------------
# CLI


def _json(text, default):
    try:
        return json.loads(text) if text is not None else default
    except ValueError:
        return default


def _first_of(*names):
    """The text of the first of the files that exists, or None."""
    return next((t for t in map(read_capped, names) if t is not None), None)


def main():
    args = sys.argv[1:]
    if args not in ([], ["--all-tests"]):
        sys.exit("usage: report.py [--all-tests]")
    if args:
        sys.stdout.write(AllTestsList(read_suites()).render())
        return
    coverage = read_capped("coverage/coverage.json")
    web_coverage = read_capped("coverage-web/web.lcov")
    diff = _first_of("coverage/changes.diff", "coverage-web/changes.diff")
    commit = _first_of("coverage/commit.txt", "coverage-web/commit.txt")
    jobs = _json(read_capped("jobs.json"), None)
    if jobs is None:
        jobs = _json(os.environ.get("REPORT_JOBS"), {})
    sys.stdout.write(build_report(
        jobs=jobs,
        suites=read_suites(),
        coverage=parse_coverage(coverage) if coverage is not None else None,
        diff=parse_diff(diff) if diff is not None else None,
        sha=os.environ.get("REPORT_SHA"),
        commit=commit.strip() if commit else None,
        repo=os.environ.get("GITHUB_REPOSITORY"),
        run_url=os.environ.get("REPORT_RUN_URL"),
        coverage_url=os.environ.get("REPORT_COVERAGE_URL"),
        web_coverage=parse_web_coverage(web_coverage) if web_coverage is not None else None,
        web_coverage_url=os.environ.get("REPORT_WEB_COVERAGE_URL"),
    ))


if __name__ == "__main__":
    main()
