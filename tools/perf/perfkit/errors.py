"""Errors and problems reported by the perf toolchain."""
from dataclasses import dataclass
from pathlib import Path


class PerfError(Exception):
    """The perf toolchain cannot go on; the message says why."""


class SchemaError(PerfError):
    """A schema file is broken or refers to a definition that does not exist."""


@dataclass(frozen=True)
class Problem:
    """One thing wrong with the catalog: where it is and what is wrong."""

    where: Path
    message: str

    def describe(self, root: Path) -> str:
        try:
            where = self.where.relative_to(root)
        except ValueError:
            where = self.where
        return f"{where.as_posix()}: {self.message}"
