"""report.html: one static page per run with every measurement and every chart.

Sections: the verdict, the environment the run had, the timeline of its phases, a chart per
Prometheus query (raw/prometheus.json), the trend of every gated measurement over earlier runs,
and the table of all measurements. Every chart with a time axis shades the run's phases and load
stages, so a spike can be matched with what the test was doing. Trend charts mark the runs where
the version of an involved item changed. Plotly comes from a CDN, pinned with its integrity hash.
"""
import html
import json
from datetime import UTC, datetime
from pathlib import Path
from string import Template
from typing import Any

from perfkit.history import PastValue
from perfkit.report import ReportContext, Reporter

REPORT_NAME = "report.html"
TEMPLATE = Path(__file__).with_name("template.html")
PLOTLY = {"url": "https://cdn.jsdelivr.net/npm/plotly.js-dist-min@2.35.2/plotly.min.js",
          "integrity": "sha384-cCVCZkAjYNxaYKbM8lsArLznDF/SvMFr1jcZrvOpSTCa0W40ZAdLzHCEulnUa5i7"}
BAND_COLORS = {"stand": "#9e9e9e", "setup": "#8d6e63", "load": "#1e88e5", "stage": "#43a047",
               "build": "#9e9e9e", "benchmark": "#fb8c00"}
LABELLED = ("stage", "benchmark", "setup")
VERDICT_ORDER = {"regression": 0, "improvement": 1, "ok": 2, "no baseline": 3}
TIME_STEPS = ((1e9, "s"), (1e6, "ms"), (1e3, "µs"))
_PLOTLY_TIME = "%Y-%m-%d %H:%M:%S.%f"


class Figures:
    """The Plotly figures of the page, by the id of the element each one goes into."""

    def __init__(self):
        self.charts: dict[str, dict] = {}

    def add(self, data: list[dict], layout: dict, css: str = "chart") -> str:
        element = f"chart-{len(self.charts) + 1}"
        base = {"margin": {"l": 56, "r": 16, "t": 36, "b": 40}, "hovermode": "x unified",
                "legend": {"orientation": "h", "y": -0.18}, "font": {"size": 11}}
        self.charts[element] = {"data": data, "layout": {**base, **layout}}
        height = f' style="height: {layout["height"]}px"' if "height" in layout else ""
        return f'<div id="{element}" class="{css}"{height}></div>'

    def to_script(self) -> str:
        # Inside <script>: no "</" may close it early.
        return json.dumps(self.charts).replace("</", "<\\/")


class Bands:
    """The run's phases and stages as shaded rectangles behind a time axis."""

    def __init__(self, events: list[dict[str, Any]], kinds: tuple[str, ...]):
        self._events = [event for event in events
                        if event.get("kind") in kinds and event.get("started") and event.get("finished")]

    def shapes(self) -> list[dict]:
        return [{"type": "rect", "xref": "x", "yref": "paper", "x0": Moment.text(event["started"]),
                 "x1": Moment.text(event["finished"]),
                 "y0": 0, "y1": 1, "line": {"width": 0}, "layer": "below", "opacity": 0.12,
                 "fillcolor": BAND_COLORS.get(event["kind"], "#7e57c2")} for event in self._events]

    def labels(self) -> list[dict]:
        return [{"x": Moment.text(event["started"]), "y": 1, "xref": "x", "yref": "paper", "yanchor": "bottom",
                 "xanchor": "left", "showarrow": False, "text": html.escape(event["name"]), "font": {"size": 9},
                 "hovertext": html.escape(f"{event['kind']}: {event['name']} {event['started']} – "
                                          f"{event['finished']}")}
                for event in self._events if event["kind"] in LABELLED]

    def layout(self) -> dict:
        return {"shapes": self.shapes(), "annotations": self.labels()}


class Scale:
    """Nanoseconds read better as µs, ms or s; other units stay as they are."""

    @staticmethod
    def of(unit: str, values: list[float]) -> tuple[float, str]:
        if unit != "ns" or not values:
            return 1.0, unit
        top = max(abs(value) for value in values)
        for factor, name in TIME_STEPS:
            if top >= factor:
                return factor, name
        return 1.0, unit


class Text:
    @staticmethod
    def number(value: float | None, unit: str = "") -> str:
        if value is None:
            return "–"
        factor, name = Scale.of(unit, [value])
        return f"{value / factor:.4g} {html.escape(name)}".strip()

    @staticmethod
    def verdict(verdict: str) -> str:
        css = verdict.replace(" ", "-")
        return f'<span class="verdict {css}">{html.escape(verdict)}</span>'


class HtmlReporter(Reporter):
    def render(self, context: ReportContext) -> list[Path]:
        files = context.files
        figures = Figures()
        kinds = tuple(self.options.get("bandKinds", list(BAND_COLORS)))
        bands = Bands(files.events, kinds)
        sections = [self._summary(context), self._environment(files.environment),
                    self._timeline(files.events, figures), self._series(files.raw("prometheus.json"), bands, figures),
                    self._trends(context, figures), self._table(files.results["results"])]
        plotly = {**PLOTLY, **self.options.get("plotly", {})}
        page = Template(TEMPLATE.read_text(encoding="utf-8")).substitute(
            title=html.escape(f"{files.results['suite']} · {files.results['runId']}"),
            plotly_url=html.escape(plotly["url"]), plotly_integrity=html.escape(plotly["integrity"]),
            body="\n".join(section for section in sections if section), figures=figures.to_script())
        path = files.run_dir / REPORT_NAME
        path.write_text(page, encoding="utf-8", newline="\n")
        return [path]

    @staticmethod
    def _summary(context: ReportContext) -> str:
        results = context.files.results
        run = context.files.run
        rows = results["results"]
        gated = [row for row in rows if row["gated"]]
        counts = {verdict: sum(row["verdict"] == verdict for row in gated) for verdict in VERDICT_ORDER}
        badges = " ".join(f"{Text.verdict(verdict)} {count}" for verdict, count in counts.items() if count)
        git = context.files.environment.get("git", {})
        return (f"<h1>{html.escape(results['suite'])}</h1>"
                f'<p class="muted">run {html.escape(results["runId"])} · {html.escape(run["started"])} – '
                f'{html.escape(run["finished"])} · outcome {html.escape(run.get("outcome", ""))} · commit '
                f'{html.escape(str(git.get("commit", ""))[:12])}{" (dirty)" if git.get("dirty") else ""} · host '
                f'{html.escape(results["host"])} · dataset {html.escape(results["dataset"])}</p>'
                f"<p>{len(gated)} gated measurement(s): {badges or 'none'}; {len(rows)} in all.</p>"
                + Problems.html(results.get("problems", [])))

    @staticmethod
    def _environment(environment: dict[str, Any]) -> str:
        facts = EnvironmentFacts.of(environment)
        cells = "".join(f"<div><b>{html.escape(name)}</b> {html.escape(value)}</div>" for name, value in facts)
        full = html.escape(json.dumps(environment, indent=2, sort_keys=True))
        return (f'<h2>Environment</h2><div class="facts">{cells}</div>'
                f"<details><summary>everything recorded (environment.json)</summary><pre>{full}</pre></details>")

    @staticmethod
    def _timeline(events: list[dict[str, Any]], figures: Figures) -> str:
        done = [event for event in events if event.get("finished")]
        if not done:
            return ""
        names = [f"{event['kind']}: {event['name']}" for event in done]
        starts = [Moment.text(event["started"]) for event in done]
        data = [{"type": "bar", "orientation": "h", "y": names, "base": starts,
                 "x": [Duration.ms(event["started"], event["finished"]) for event in done],
                 "marker": {"color": [BAND_COLORS.get(event["kind"], "#7e57c2") for event in done]},
                 "hovertemplate": "%{y}<br>%{base} + %{x} ms<extra></extra>"}]
        layout = {"xaxis": {"type": "date"}, "yaxis": {"autorange": "reversed", "automargin": True},
                  "height": max(200, 28 * len(done) + 80), "hovermode": "closest", "showlegend": False}
        return "<h2>Timeline</h2>" + figures.add(data, layout, "chart timeline")

    @staticmethod
    def _series(raw: dict[str, Any] | None, bands: Bands, figures: Figures) -> str:
        if not raw:
            return ""
        charts = []
        empty = [query["name"] for query in raw.get("queries", []) if not any(s["values"] for s in query["series"])]
        for query in raw.get("queries", []):
            if query["name"] in empty:
                continue
            data = [{"type": "scatter", "mode": "lines", "name": SeriesName.of(series["labels"], query["name"]),
                     "x": [Moment.iso(point[0]) for point in series["values"]],
                     "y": [point[1] for point in series["values"]]} for series in query["series"]]
            layout = {"title": {"text": html.escape(query.get("title", query["name"])), "font": {"size": 13}},
                      "yaxis": {"title": {"text": html.escape(query["unit"])}, "rangemode": "tozero"},
                      "xaxis": {"type": "date"}, **bands.layout()}
            charts.append(figures.add(data, layout))
        return ('<h2>Server, database and load generator</h2><p class="muted">Shaded: the run\'s phases and '
                f'load stages. Every {raw.get("stepS", "?")} s from Prometheus.'
                + (f" No data for: {html.escape(', '.join(empty))}." if empty else "")
                + '</p><div class="grid">' + "".join(charts) + "</div>")

    def _trends(self, context: ReportContext, figures: Figures) -> str:
        rows = [row for row in context.files.results["results"] if row["gated"] or self.options.get("trendAll")]
        charts = [TrendChart(row, context.trends.of(row)).add_to(figures) for row in rows]
        charts = [chart for chart in charts if chart]
        if not charts:
            return ""
        return ('<h2>Trends</h2><p class="muted">This run and the earlier ones on the same host and dataset. '
                "Dashed line: the baseline; shaded: the allowed change; vertical lines: a catalog item changed its "
                'version.</p><div class="grid">' + "".join(charts) + "</div>")

    @staticmethod
    def _table(rows: list[dict[str, Any]]) -> str:
        ordered = sorted(rows, key=lambda row: (not row["gated"], VERDICT_ORDER.get(row["verdict"], 9),
                                                -(row["worsePct"] or 0.0)))
        body = "".join(ResultRowHtml.of(row) for row in ordered)
        return ('<h2>All measurements</h2><div class="table-wrap"><table><thead><tr><th>source</th><th>name</th>'
                '<th>metric</th><th>value</th><th>baseline</th><th>runs</th><th>worse by</th><th>limit</th>'
                f"<th>verdict</th></tr></thead><tbody>{body}</tbody></table></div>")


class Problems:
    @staticmethod
    def html(problems: list[str]) -> str:
        if not problems:
            return ""
        items = "".join(f"<li>{html.escape(problem)}</li>" for problem in problems)
        return f'<div class="problems"><b>{len(problems)} problem(s) failed the run:</b><ul>{items}</ul></div>'


class ResultRowHtml:
    @staticmethod
    def of(row: dict[str, Any]) -> str:
        worse = "–" if row["worsePct"] is None else f"{row['worsePct']:+.1f}%"
        limit = f"{row['limitPct']:g}%" if row["gated"] else "not gated"
        css = "" if row["gated"] else ' class="ungated"'
        cells = [html.escape(row["source"]), html.escape(row["name"]), html.escape(row["metric"])]
        numbers = [Text.number(row["value"], row["unit"]), Text.number(row["baseline"], row["unit"]),
                   str(row["baselineRuns"]), worse, limit]
        return (f"<tr{css}>" + "".join(f"<td>{cell}</td>" for cell in cells)
                + "".join(f'<td class="num">{cell}</td>' for cell in numbers)
                + f"<td>{Text.verdict(row['verdict'])}</td></tr>")


class TrendChart:
    """One gated measurement over the runs: its values, baseline, limit and version changes."""

    def __init__(self, row: dict[str, Any], points: list[PastValue]):
        self._row = row
        self._points = points

    def add_to(self, figures: Figures) -> str:
        if not self._points:
            return ""
        row = self._row
        factor, unit = Scale.of(row["unit"], [point.value for point in self._points])
        x = [Moment.text(point.started) for point in self._points]
        y = [point.value / factor for point in self._points]
        data = [{"type": "scatter", "mode": "lines+markers", "name": "runs", "x": x, "y": y,
                 "text": [point.run_id for point in self._points], "line": {"color": "#607d8b"},
                 "hovertemplate": "%{text}: %{y:.4g}<extra></extra>"},
                {"type": "scatter", "mode": "markers", "name": "this run", "x": [x[-1]], "y": [y[-1]],
                 "marker": {"size": 12, "color": self._color()}, "hovertemplate": "this run: %{y:.4g}<extra></extra>"}]
        layout = {"title": {"text": html.escape(f"{row['name']} · {row['metric']}"), "font": {"size": 13}},
                  "yaxis": {"title": {"text": html.escape(unit)}}, "xaxis": {"type": "date"},
                  "shapes": self._baseline(factor) + self._versions(), "annotations": self._version_labels(),
                  "hovermode": "closest", "height": 280}
        return figures.add(data, layout)

    def _color(self) -> str:
        return {"regression": "#c62828", "improvement": "#2e7d32"}.get(self._row["verdict"], "#1e88e5")

    def _baseline(self, factor: float) -> list[dict]:
        row = self._row
        if row["baseline"] is None:
            return []
        base = row["baseline"] / factor
        edge = base * (1 + row["limitPct"] / 100) if row["better"] == "lower" else base * (1 - row["limitPct"] / 100)
        return [{"type": "line", "xref": "paper", "x0": 0, "x1": 1, "y0": base, "y1": base,
                 "line": {"dash": "dash", "color": "#455a64", "width": 1}},
                {"type": "rect", "xref": "paper", "x0": 0, "x1": 1, "y0": min(base, edge), "y1": max(base, edge),
                 "fillcolor": "#90a4ae", "opacity": 0.15, "line": {"width": 0}, "layer": "below"}]

    def _changes(self) -> list[tuple[str, str]]:
        """(time, what changed) for every run whose items differ from the run before it."""
        found = []
        for before, after in zip(self._points, self._points[1:], strict=False):
            changed = [f"{key} {before.versions.get(key, '–')} → {version}"
                       for key, version in sorted(after.versions.items()) if before.versions.get(key) != version]
            if changed:
                found.append((Moment.text(after.started), "<br>".join(html.escape(text) for text in changed)))
        return found

    def _versions(self) -> list[dict]:
        return [{"type": "line", "xref": "x", "yref": "paper", "x0": at, "x1": at, "y0": 0, "y1": 1,
                 "line": {"dash": "dot", "color": "#8e24aa", "width": 1}} for at, _ in self._changes()]

    def _version_labels(self) -> list[dict]:
        return [{"x": at, "y": 1, "xref": "x", "yref": "paper", "yanchor": "bottom", "showarrow": False,
                 "text": "v", "font": {"color": "#8e24aa", "size": 10}, "hovertext": text}
                for at, text in self._changes()]


class EnvironmentFacts:
    """The few facts that decide whether two runs compare, from environment.json."""

    @classmethod
    def of(cls, environment: dict[str, Any]) -> list[tuple[str, str]]:
        host = environment.get("host", {})
        facts = [("CPU", f"{host.get('cpuModel', '?')} × {host.get('logicalCpus', '?')}"),
                 ("memory", f"{host.get('memoryGiB', '?')} GiB"), ("kernel", str(host.get("kernel", "?"))),
                 ("governor", host.get("cpuGovernor") or "unknown"),
                 ("cgroup limits", (f"cpu {host.get('cgroupCpuMax') or 'none'}, "
                                    f"memory {host.get('cgroupMemoryMax') or 'none'}"))]
        return facts + cls._build(environment.get("build", {})) + cls._stand(environment)

    @staticmethod
    def _build(build: dict[str, Any]) -> list[tuple[str, str]]:
        if not build:
            return []
        compiler = f"{Path(build.get('CMAKE_CXX_COMPILER', '?')).name} {build.get('compilerVersion', '')}".strip()
        flags = f"{build.get('CMAKE_CXX_FLAGS', '')} {build.get('CMAKE_CXX_FLAGS_RELEASE', '')}".strip()
        return [("build", f"{build.get('CMAKE_BUILD_TYPE', '?')}, {compiler}, {flags or 'no flags'}")]

    @staticmethod
    def _stand(environment: dict[str, Any]) -> list[tuple[str, str]]:
        found = []
        server = environment.get("server", {})
        if server:
            config = server.get("config", {}).get("server", {})
            version = server.get("health", {}).get("version", "?")
            found.append(("server", f"{version}, {config.get('threads', '?')} threads"))
            for name, container in sorted(server.get("containers", {}).items()):
                limits = f"{container.get('nanoCpus', 0) / 1e9:g} CPU, {container.get('memoryBytes', 0) / 2**30:g} GiB"
                found.append((name, limits))
        database = environment.get("database", {})
        if database:
            size = database.get("afterLoad", database).get("databaseBytes", 0)
            found.append(("PostgreSQL", f"{database.get('version', '?')}, {size / 2**20:.1f} MiB after the load"))
            found += [(f"pg {name}", value) for name, value in sorted(database.get("settings", {}).items())]
        return found


class SeriesName:
    @staticmethod
    def of(labels: dict[str, str], fallback: str) -> str:
        shown = [value for name, value in sorted(labels.items()) if name not in ("__name__", "instance", "job")]
        return html.escape(", ".join(shown) or fallback)


class Moment:
    """Times for Plotly, which ignores time zones: plain UTC without an offset."""

    @staticmethod
    def iso(seconds: float) -> str:
        return datetime.fromtimestamp(seconds, UTC).strftime(_PLOTLY_TIME)[:-3]

    @staticmethod
    def text(moment: str) -> str:
        return datetime.fromisoformat(moment).astimezone(UTC).strftime(_PLOTLY_TIME)[:-3]


class Duration:
    @staticmethod
    def ms(started: str, finished: str) -> float:
        return (datetime.fromisoformat(finished) - datetime.fromisoformat(started)).total_seconds() * 1000
