"""summary.md: the run in a few lines of GitHub Markdown, for a pull request comment.

The verdict first, then the gated measurements worst first (regressions and improvements, the
rest folded away), the machine and the commit, and where the full report lives.
"""
from pathlib import Path
from typing import Any

from perfkit.report import ReportContext, Reporter

SUMMARY_NAME = "summary.md"
ICONS = {"regression": "🔴", "improvement": "🟢", "ok": "⚪", "no baseline": "⚫"}
TIME_STEPS = ((1e9, "s"), (1e6, "ms"), (1e3, "µs"))


class MarkdownReporter(Reporter):
    def render(self, context: ReportContext) -> list[Path]:
        files = context.files
        results = files.results
        gated = sorted((row for row in results["results"] if row["gated"]), key=lambda row: -(row["worsePct"] or 0.0))
        notable = [row for row in gated if row["verdict"] in ("regression", "improvement")]
        rest = [row for row in gated if row not in notable]
        lines = [f"### {self._headline(gated)} `{Cell.text(results['suite'])}`", "",
                 self._about(files.environment, results, files.run), ""]
        lines += Table.of(notable) if notable else ["No gated measurement changed beyond its limit.", ""]
        if rest:
            lines += ["<details><summary>" + f"{len(rest)} other gated measurement(s)" + "</summary>", "",
                      *Table.of(rest), "</details>", ""]
        lines.append(f"Full report: `{files.run_dir.name}/report.html`; all results: "
                     f"`{files.run_dir.name}/results.json`.")
        path = files.run_dir / SUMMARY_NAME
        path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        return [path]

    @staticmethod
    def _headline(gated: list[dict[str, Any]]) -> str:
        regressions = sum(row["verdict"] == "regression" for row in gated)
        if regressions:
            return f"{ICONS['regression']} {regressions} regression(s) in"
        if all(row["verdict"] == "no baseline" for row in gated):
            return f"{ICONS['no baseline']} No baseline yet for"
        return f"{ICONS['ok']} No regression in"

    @staticmethod
    def _about(environment: dict[str, Any], results: dict[str, Any], run: dict[str, Any]) -> str:
        host = environment.get("host", {})
        git = environment.get("git", {})
        commit = str(git.get("commit", ""))[:12] + (" (dirty)" if git.get("dirty") else "")
        return (f"Run `{Cell.text(results['runId'])}` at commit `{Cell.text(commit)}` on "
                f"{Cell.text(str(host.get('cpuModel', '?')))} × {host.get('logicalCpus', '?')}, "
                f"{host.get('memoryGiB', '?')} GiB; dataset `{Cell.text(results['dataset'])}`; "
                f"{Cell.text(run['started'])} – {Cell.text(run['finished'])}.")


class Table:
    @staticmethod
    def of(rows: list[dict[str, Any]]) -> list[str]:
        lines = ["| | measurement | value | baseline | worse by | limit |", "|---|---|--:|--:|--:|--:|"]
        for row in rows:
            worse = "–" if row["worsePct"] is None else f"{row['worsePct']:+.1f}%"
            lines.append(f"| {ICONS.get(row['verdict'], '')} | `{Cell.text(row['name'])}` {Cell.text(row['metric'])} "
                         f"| {Cell.number(row['value'], row['unit'])} | {Cell.number(row['baseline'], row['unit'])} "
                         f"| {worse} | {row['limitPct']:g}% |")
        return [*lines, ""]


class Cell:
    @staticmethod
    def text(value: str) -> str:
        """Safe inside a table cell and inline code: no pipes, backticks or line breaks."""
        return value.replace("|", "/").replace("`", "'").replace("\n", " ")

    @staticmethod
    def number(value: float | None, unit: str) -> str:
        if value is None:
            return "–"
        if unit == "ns":
            for factor, name in TIME_STEPS:
                if abs(value) >= factor:
                    return f"{value / factor:.4g} {name}"
        return f"{value:.4g} {Cell.text(unit)}"
