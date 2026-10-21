"""The HTTP suite: one scenario against a fresh stand, loaded by k6.

Phases: the stand comes up, the set-up creates the missions and links the personas sign in
with, k6 runs the load (its stages become events of their own, timed from k6's start), and the
collectors read what k6 and the stand reported. The stand is removed at the end, also after a
failure, unless it is kept for debugging.
"""
import hashlib
import json
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from perfkit.api import ApiClient, Provisioner
from perfkit.catalog import Catalog, Item
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.measure import CollectContext, Collector
from perfkit.plan import FixtureReader, PlanBuilder
from perfkit.session import Collected
from perfkit.stand import Stand, StandProbe
from perfkit.timeline import Timeline

PLAN_NAME = "plan.json"
SUMMARY_NAME = "k6-summary.json"
DEFAULT_METRICS = ("p95", "error_rate")
DEFAULT_LIMIT_PCT = 5.0
DEFAULT_CREDENTIALS = 8


@dataclass(frozen=True)
class Dataset:
    """The terrain a run used: its key in the history and what goes into environment.json."""

    key: str
    manifest: dict

    @classmethod
    def of(cls, manifest: dict) -> "Dataset":
        files = json.dumps(manifest.get("files", {}), sort_keys=True)
        return cls("terrain-" + hashlib.sha256(files.encode("utf-8")).hexdigest()[:12], manifest)


class ScenarioSuite:
    def __init__(self, catalog: Catalog, scenario_id: str, stand: Stand, dataset: Dataset,
                 loader: ExtensionLoader | None = None):
        scenario = catalog.get("scenario", scenario_id)
        if scenario is None:
            raise PerfError(f"no scenario {scenario_id}; perf.py list --kind scenario shows them")
        self.scenario = scenario
        self.name = f"scenario/{scenario.id}"
        self.dataset = dataset.key
        self._dataset = dataset
        self._catalog = catalog
        self._stand = stand
        self._loader = loader or ExtensionLoader()
        self._fixtures = FixtureReader(catalog)
        self._plans = PlanBuilder(catalog, self._fixtures)
        self._environment: dict[str, Any] = {"dataset": dataset.manifest}
        self._problems: list[str] = []

    def items(self) -> list[Item]:
        personas = [persona for persona, _ in self._plans.personas(self.scenario)]
        fixture_ids = sorted({fixture for persona in personas for fixture in persona.manifest.get("fixtures", [])})
        fixtures = [self._require("fixture", fixture_id) for fixture_id in fixture_ids]
        return [self.scenario, *personas, *fixtures, *self.collectors()]

    def collectors(self) -> list[Item]:
        return [self._require("collector", item_id) for item_id in self.scenario.manifest.get("collectors", [])]

    def environment(self) -> dict[str, Any]:
        return self._environment

    def problems(self) -> list[str]:
        return self._problems

    def execute(self, timeline: Timeline, run_dir: Path) -> list[Collected]:
        self._stand.run_dir = run_dir
        client = ApiClient(self._stand.base_url)
        probe = StandProbe(self._stand, client)
        try:
            with timeline.phase("stand up", "stand"):
                self._stand.up(client)
            self._environment["server"] = probe.server()
            self._environment["database"] = probe.database()
            with timeline.phase("set-up", "setup"):
                self._write_plan(run_dir, client)
            with timeline.phase("load", "load", scenario=self.scenario.id) as load:
                if not self._stand.load():
                    self._problems.append(f"{self.scenario.key}: k6 thresholds crossed (see {SUMMARY_NAME})")
            self._environment["database"]["afterLoad"] = probe.sizes()
            summary = run_dir / SUMMARY_NAME
            self._stages(timeline, summary)
            return [self._collect(item, summary, load.started, load.finished) for item in self.collectors()]
        finally:
            self._save_logs(run_dir)
            self._stand.down()

    def _write_plan(self, run_dir: Path, client: ApiClient) -> None:
        provisioner = Provisioner(client)

        def credentials(persona: Item, missions: list[dict]) -> list[dict]:
            count = persona.manifest.get("setup", {}).get("count", DEFAULT_CREDENTIALS)
            return provisioner.credentials(persona.manifest["auth"], missions, count)

        # k6 runs next to the server in the stand's network.
        plan = self._plans.build(self.scenario, "http://rfmap-server:8080/api/v1", self._stand.admin_token, credentials)
        (run_dir / PLAN_NAME).write_text(json.dumps(plan), encoding="utf-8")

    def _stages(self, timeline: Timeline, summary: Path) -> None:
        """The load stages as events, timed from the moment k6 finished its set-up."""
        try:
            started_ms = json.loads(summary.read_text(encoding="utf-8"))["setup_data"]["startedMs"]
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise PerfError(f"{summary} has no start time from k6: {error}") from error
        at = datetime.fromtimestamp(started_ms / 1000, UTC)
        for index, stage in enumerate(self.scenario.manifest["load"]["stages"]):
            end = at + timedelta(seconds=stage["durationS"])
            timeline.record(stage.get("name", f"stage {index + 1}"), "stage", self._iso(at), self._iso(end),
                            targetRps=stage["targetRps"])
            at = end

    @staticmethod
    def _iso(moment: datetime) -> str:
        return moment.isoformat(timespec="milliseconds").replace("+00:00", "Z")

    def _collect(self, item: Item, summary: Path, started: str, finished: str) -> Collected:
        collector = self._loader.load(item, Collector)(item)
        collection = collector.collect(CollectContext(self.scenario, (summary,), started, finished))
        compare = self.scenario.manifest.get("compare", {})
        # Personas and fixtures shape the load too: a new major of any of them starts a new baseline.
        related = tuple(entry for entry in self.items() if entry.kind.name in ("persona", "fixture"))
        return Collected(self.scenario, item, collection, frozenset(compare.get("metrics", DEFAULT_METRICS)),
                         float(compare.get("maxRegressionPct", DEFAULT_LIMIT_PCT)), related)

    def _save_logs(self, run_dir: Path) -> None:
        try:
            (run_dir / "raw").mkdir(parents=True, exist_ok=True)
            self._stand.logs(run_dir / "raw" / "rfmap-server.log")
        except (PerfError, OSError) as error:
            self._problems.append(f"could not save the server log: {error}")

    def _require(self, kind: str, item_id: str) -> Item:
        item = self._catalog.get(kind, item_id)
        if item is None:
            raise PerfError(f"{self.scenario.key}: no {kind} '{item_id}'")
        return item
