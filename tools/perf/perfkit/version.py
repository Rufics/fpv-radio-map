"""Semantic versions of catalog items."""
import re
from dataclasses import dataclass

from perfkit.errors import PerfError

_PATTERN = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")


@dataclass(frozen=True, order=True)
class Version:
    """MAJOR.MINOR.PATCH. A major change means results are no longer comparable."""

    major: int
    minor: int
    patch: int

    @classmethod
    def parse(cls, text: str) -> "Version":
        match = _PATTERN.match(text)
        if not match:
            raise PerfError(f"'{text}' is not a MAJOR.MINOR.PATCH version")
        return cls(*(int(part) for part in match.groups()))

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"
