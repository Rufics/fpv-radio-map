#!/usr/bin/env python3
"""Builds the Markdown CI report that is posted to a pull request and to the job summary.

    cd <data folder> && python3 -I path/to/tools/ci/report.py > report.md

The script takes no arguments and reads only fixed names under the current directory, so no
input can point it at another file:

    tests/*.xml             JUnit results (ctest --output-junit, vitest junit reporter)
    coverage/coverage.json  gcovr JSON report of the instrumented C++ build
    coverage/changes.diff   `git diff --unified=0` of the pull request
    coverage/commit.txt     the commit the coverage was measured on; uncovered lines link to it
    jobs.json               the run's jobs as listed by the Actions API: [{name, result, url}]

Environment, all optional:

    REPORT_JOBS          JSON `needs` context of the calling job, used when jobs.json is absent
    REPORT_SHA           the pull request's head commit, shown in the headline
    REPORT_RUN_URL       link to the workflow run
    REPORT_COVERAGE_URL  link to the HTML coverage artifact
    GITHUB_REPOSITORY    owner/name, for links to the uncovered lines

Every input may come from a pull request of a fork, so it is treated as untrusted data: sizes are
capped, nothing is executed, and every string that reaches the Markdown is escaped.
"""
import json
import os
import re
import sys
from pathlib import Path

MARKER = "<!-- rfmap-ci-report -->"
MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_LISTED = 20

# Display names of the CI jobs, keyed by their id in ci.yml.
JOB_NAMES = {"clang-tidy": "C++ static analysis", "cpp": "C++ build and tests", "coverage": "C++ coverage",
             "fuzz": "Fuzz C++", "web": "Web build and tests"}
# Jobs that only orchestrate the run; they are not results worth a row.
HIDDEN_JOBS = {"changes", "report", "Changed parts", "Test report"}
SUITES = {"cpp.xml": "C++", "web-ui-kit.xml": "Web: ui-kit", "web-fpv-app.xml": "Web: fpv-app"}
ICONS = {"success": "✅", "skipped": "⏭️", "cancelled": "🚫", "failure": "❌"}
DETAILS_END = "</details>"
FPV = "FPV modules"

# Coverage areas, most specific prefix first. Files that match none count in "All C++" only.
COMPONENTS = [
    ("core/rf-core/", "Core", "rf-core"),
    ("core/rf-ext/", "Core", "rf-ext"),
    ("platform/rfmod/", "Platform", "rfmod"),
    ("modules/fpv/common/", FPV, "fpv common"),
    ("modules/fpv/", FPV, None),  # fpv.<module folder>
    ("server/", "Server", "server"),
]
AREAS = [
    ("Core", "Core: rf-core, rf-ext"),
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

TESTCASE = re.compile(r"<testcase\b([^>]*?)(/>|>(.*?)</testcase>)", re.S)
ROOT = re.compile(r"<testsuites?\b[^>]*>")


def _attr(tag, name):
    m = re.search(rf'\s{name}="([^"]*)"', tag)
    return m.group(1) if m else None


def _unescape_xml(s):
    for a, b in (("&quot;", '"'), ("&apos;", "'"), ("&lt;", "<"), ("&gt;", ">"), ("&amp;", "&")):
        s = s.replace(a, b)
    return s


FAILED_BODY = re.compile(r"<(failure|error)\b")
FAILED_STATUS = re.compile(r'\sstatus="fail(ed)?"')
SKIPPED_BODY = re.compile(r"<skipped\b")
SKIPPED_STATUS = re.compile(r'\sstatus="(notrun|disabled|skipped)"')


def _case_name(attrs):
    name = _unescape_xml(_attr(attrs, "name") or "?")
    cls = _unescape_xml(_attr(attrs, "classname") or "")
    return f"{cls} › {name}" if cls and cls != name else name


def _outcome(attrs, body):
    """"failed", "skipped" or "passed" for one test case."""
    if FAILED_BODY.search(body) or FAILED_STATUS.search(attrs):
        return "failed"
    if SKIPPED_BODY.search(body) or SKIPPED_STATUS.search(attrs):
        return "skipped"
    return "passed"


def _declared_total(xml):
    """The test count the runner wrote on the root element, or 0."""
    root = ROOT.search(xml)
    return max(as_int(_attr(root.group(0), "tests")), 0) if root else 0


def parse_junit(xml):
    """Totals and failed test names of one JUnit file."""
    cases = TESTCASE.findall(xml)
    outcomes = [(_outcome(attrs, body), attrs) for attrs, _, body in cases]
    failed = [_case_name(attrs) for outcome, attrs in outcomes if outcome == "failed"]
    skipped = sum(1 for outcome, _ in outcomes if outcome == "skipped")
    # Prefer the count the runner wrote on the root element; fall back to counting cases.
    total = max(_declared_total(xml) or len(cases), len(failed) + skipped)
    return {"total": total, "failed": failed, "skipped": skipped, "passed": total - len(failed) - skipped}


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
# Report


def build_report(jobs=None, suites=(), coverage=None, diff=None, sha=None, commit=None, repo=None,
                 run_url=None, coverage_url=None):
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
    ctx["failed_tests"] = [(s["label"], n) for s in ctx["suites"] for n in s["failed"]]
    ctx["ran"] = any(j["result"] != "skipped" for j in ctx["jobs"]) or bool(ctx["suites"])

    lines = [MARKER, *_headline(ctx), *_failed_tests(ctx["failed_tests"])]
    if ctx["ran"]:
        lines += _jobs_table(ctx)
    if ctx["cov"]:
        lines += _coverage_section(ctx)
    lines += _footer(checked(SAFE_URL, run_url), checked(SAFE_URL, coverage_url))
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
    if ctx["changed"] and ctx["changed"]["total"]:
        c = ctx["changed"]
        facts.append(f"changed lines **{fmt_pct(pct(c['covered'], c['total']))}** covered")
    return facts


def _failed_tests(failed):
    if not failed:
        return []
    out = ["<details open><summary><b>Failed tests</b></summary>", ""]
    out += [f"- {md(suite, 40)}: {md(name, 300)}" for suite, name in failed[:MAX_LISTED]]
    if len(failed) > MAX_LISTED:
        out.append(f"- … and {len(failed) - MAX_LISTED} more")
    return out + ["", DETAILS_END, ""]


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
    out = ["#### C++ coverage", "", "| Area | Lines | Functions |", "|---|---:|---:|"]
    out += [_coverage_row(label, cov["areas"][name]) for name, label in AREAS if name in cov["areas"]]
    out += [_coverage_row("All C++", cov["total"], strong=True), ""]
    if changed and changed["total"]:
        out += _changed_lines(changed, ctx["repo"], ctx["commit"])
    elif ctx["diff"] is not None:
        out += ["**Changed lines:** this pull request changes no executable C++ line.", ""]
    out += _components(cov["components"])
    out += ["<sub>🟢 80% or more · 🟡 60–80% · 🔴 below 60%. Coverage is reported, not enforced. "
            "Test code is not counted.</sub>", ""]
    return out


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


def _components(components):
    if not components:
        return []
    order = [name for name, _ in AREAS]
    out = ["<details><summary>Coverage per component</summary>", "", "| Component | Lines | Functions |", "|---|---:|---:|"]
    for c in sorted(components, key=lambda c: (order.index(c["area"]), c["component"])):
        lp = pct(c["linesCovered"], c["lines"])
        out.append(f"| {md(c['component'], 60)} | {dot(lp)} {fmt_pct(lp)} <sub>{c['linesCovered']}/{c['lines']}</sub> "
                   f"| {fmt_pct(pct(c['functionsCovered'], c['functions']))} |")
    return out + ["", DETAILS_END, ""]


def _footer(run_url, coverage_url):
    links = []
    if run_url:
        links.append(f"[Workflow run]({run_url})")
    if coverage_url:
        links.append(f"[HTML coverage report]({coverage_url})")
    return [" · ".join(links)] if links else []


# --------------------------------------------------------------------------------------------
# CLI


def _json(text, default):
    try:
        return json.loads(text) if text is not None else default
    except ValueError:
        return default


def main():
    coverage = read_capped("coverage/coverage.json")
    diff = read_capped("coverage/changes.diff")
    commit = read_capped("coverage/commit.txt")
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
    ))


if __name__ == "__main__":
    main()
