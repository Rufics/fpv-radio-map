"""What collectors hand back: measurements, and the base class every collector extends.

A collector is an extension (extensions/collectors/<id>/) whose entry class derives from
Collector. The run gives it a CollectContext: the catalog item whose numbers it reads, the files
the tool wrote and the time window of the phase. It returns a Collection.
"""
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from perfkit.catalog import Item

LOWER = "lower"
HIGHER = "higher"


@dataclass(frozen=True)
class Measurement:
    """One number of a run: `source` is the item it belongs to (benchmark/rf-core)."""

    source: str
    name: str
    metric: str
    unit: str
    better: str
    value: float

    @property
    def key(self) -> tuple[str, str, str]:
        return self.source, self.name, self.metric


@dataclass(frozen=True)
class CollectContext:
    source: Item
    files: tuple[Path, ...]
    started: str
    finished: str


@dataclass
class Collection:
    """The measurements, plus what the tool said about the machine (goes into the environment)."""

    measurements: list[Measurement]
    context: dict[str, Any] = field(default_factory=dict)


class Collector:
    """Base of collector extensions. `item` is the collector's own catalog item."""

    def __init__(self, item: Item):
        self.item = item
        self.options: dict[str, Any] = item.manifest.get("options", {})

    def collect(self, context: CollectContext) -> Collection:
        raise NotImplementedError
