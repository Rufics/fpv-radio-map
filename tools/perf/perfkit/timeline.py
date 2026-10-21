"""When each part of a run started and finished.

Every phase (building, one benchmark, one load scenario) becomes an event. The events go into
events.json and the history, and later onto every chart: as bands in the HTML report and as
annotations in Grafana, so a curve can always be matched with the test that made it.
"""
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime


def utc_now() -> str:
    return datetime.now(UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


@dataclass
class Event:
    name: str
    kind: str
    started: str
    finished: str = ""
    outcome: str = ""
    attributes: dict = field(default_factory=dict)


class Timeline:
    """The events of one run, in the order they started."""

    def __init__(self, clock: Callable[[], str] = utc_now):
        self._clock = clock
        self._events: list[Event] = []

    @contextmanager
    def phase(self, name: str, kind: str, **attributes) -> Iterator[Event]:
        event = Event(name, kind, self._clock(), attributes=attributes)
        self._events.append(event)
        try:
            yield event
        except BaseException:
            event.outcome = "failed"
            raise
        finally:
            event.finished = self._clock()
            event.outcome = event.outcome or "ok"

    def events(self) -> list[Event]:
        return list(self._events)

    def to_json(self) -> list[dict]:
        return [asdict(event) for event in self._events]
