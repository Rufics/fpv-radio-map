"""plan.json for k6/runner.js: a scenario with its personas resolved into plain data.

Fixture references (FIXTURE:FILE) become the file's content: a list of bodies sent in turn, or
one body. Each persona gets its share of the scenario's rate from its weight, and the
credentials the provisioner made for it.
"""
import json
from collections.abc import Callable
from typing import Any

from perfkit.catalog import Catalog, Item
from perfkit.errors import PerfError

# k6 starts this many VUs per persona up front and adds more, up to the maximum, when requests
# take longer than the arrival rate allows.
DEFAULT_VUS = {"preAllocated": 5, "max": 200}

Credentials = Callable[[Item, list[dict]], list[dict]]


class FixtureReader:
    """Reads FIXTURE:FILE references of one catalog, once each."""

    def __init__(self, catalog: Catalog):
        self._catalog = catalog
        self._cache: dict[str, Any] = {}

    def read(self, reference: str) -> Any:
        if reference not in self._cache:
            fixture_id, _, file_name = reference.partition(":")
            fixture = self._catalog.get("fixture", fixture_id)
            if fixture is None:
                raise PerfError(f"no fixture '{fixture_id}'")
            try:
                self._cache[reference] = json.loads((fixture.folder / file_name).read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                raise PerfError(f"cannot read {reference}: {error}") from error
        return self._cache[reference]

    def bodies(self, body: Any) -> list[Any] | None:
        if body is None:
            return None
        if isinstance(body, dict):
            return [body]
        content = self.read(body)
        return content if isinstance(content, list) else [content]


class PlanBuilder:
    def __init__(self, catalog: Catalog, fixtures: FixtureReader):
        self._catalog = catalog
        self._fixtures = fixtures

    def personas(self, scenario: Item) -> list[tuple[Item, float]]:
        """The scenario's personas with their share of the load."""
        entries = scenario.manifest["personas"]
        total = sum(entry["weight"] for entry in entries)
        found = []
        for entry in entries:
            persona = self._catalog.get("persona", entry["id"])
            if persona is None:
                raise PerfError(f"{scenario.key}: no persona '{entry['id']}'")
            found.append((persona, entry["weight"] / total))
        return found

    def setup_missions(self, persona: Item) -> list[dict]:
        setup = persona.manifest.get("setup")
        if not setup:
            return []
        return self._fixtures.bodies(setup["missions"]) or []

    def build(self, scenario: Item, base_url: str, admin_token: str, credentials: Credentials) -> dict:
        load = scenario.manifest["load"]
        if load["type"] == "constant-arrival-rate" and len(load["stages"]) != 1:
            raise PerfError(f"{scenario.key}: a constant-arrival-rate load has exactly one stage")
        personas = []
        for persona, share in self.personas(scenario):
            manifest = persona.manifest
            personas.append({
                "id": persona.id, "share": share, "auth": manifest["auth"],
                "thinkTimeS": manifest.get("thinkTimeS"),
                "credentials": credentials(persona, self.setup_missions(persona)),
                "steps": [self._step(step) for step in manifest["steps"]],
            })
        return {"scenario": scenario.id, "baseUrl": base_url, "adminToken": admin_token, "load": load,
                "thresholds": {name: [rule] for name, rule in scenario.manifest.get("thresholds", {}).items()},
                "vus": DEFAULT_VUS, "personas": personas}

    def _step(self, step: dict) -> dict:
        resolved = {key: value for key, value in step.items() if key != "body"}
        resolved["bodies"] = self._fixtures.bodies(step.get("body"))
        return resolved
