"""catalog.lock.json: the version and content hash of every item, so a change cannot hide.

Results of runs are only compared when the items involved have the same major version, so a
change to an item must come with a version bump. The lock file records what each item looked
like when it was last locked. `check` demands that the catalog matches it exactly; `lock`
rewrites it, but refuses while an item changed without a higher version.
"""
import json
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from perfkit.catalog import Catalog, ContentHasher
from perfkit.errors import PerfError, Problem
from perfkit.version import Version

LOCK_NAME = "catalog.lock.json"
LOCK_FORMAT = 1


@dataclass(frozen=True)
class LockEntry:
    version: Version
    sha256: str


class Mode(Enum):
    CHECK = "check"
    LOCK = "lock"


class LockFile:
    """Reads and writes catalog.lock.json at the root of a catalog."""

    def __init__(self, root: Path):
        self.path = root / LOCK_NAME

    def read(self) -> dict[str, LockEntry]:
        if not self.path.is_file():
            return {}
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if data.get("format") != LOCK_FORMAT:
                raise PerfError(f"{self.path}: unsupported format {data.get('format')!r}")
            return {key: LockEntry(Version.parse(entry["version"]), entry["sha256"])
                    for key, entry in data["items"].items()}
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            raise PerfError(f"{self.path} is unreadable ({error}); restore it from git") from error

    def write(self, entries: dict[str, LockEntry]) -> None:
        items = {key: {"version": str(entry.version), "sha256": entry.sha256}
                 for key, entry in sorted(entries.items())}
        text = json.dumps({"format": LOCK_FORMAT, "items": items}, indent=2) + "\n"
        self.path.write_text(text, encoding="utf-8", newline="\n")


class LockComparer:
    """Compares the catalog as it is with the lock file and says what is wrong for a mode."""

    def __init__(self, catalog: Catalog, locked: dict[str, LockEntry], lock_path: Path):
        self._catalog = catalog
        self._locked = locked
        self._lock_path = lock_path

    def current(self) -> dict[str, LockEntry]:
        return {item.key: LockEntry(item.version, ContentHasher.folder(item.folder))
                for item in self._catalog.items()}

    def problems(self, mode: Mode) -> list[Problem]:
        current = self.current()
        problems = []
        for item in self._catalog.items():
            message = self._item_problem(current[item.key], self._locked.get(item.key), mode)
            if message:
                problems.append(Problem(item.folder, message))
        if mode is Mode.CHECK:
            problems.extend(Problem(self._lock_path, f"{key} is locked but no longer exists; run perf.py catalog lock")
                            for key in sorted(set(self._locked) - set(current)))
        return problems

    @staticmethod
    def _item_problem(now: LockEntry, locked: LockEntry | None, mode: Mode) -> str:
        if locked is None:
            return "" if mode is Mode.LOCK else "not in catalog.lock.json yet; run perf.py catalog lock"
        if now == locked:
            return ""
        if now.version < locked.version:
            return f"version went down from {locked.version} to {now.version}"
        if now.version == locked.version:
            return f"content changed but version stayed {now.version}; raise the version in manifest.json"
        return "" if mode is Mode.LOCK else (
            f"version {now.version} is not locked yet (locked {locked.version}); run perf.py catalog lock")
