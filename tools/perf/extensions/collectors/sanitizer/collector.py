"""Counts what AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer reported.

A sanitized server writes one file per process (asan.<pid>, ubsan.<pid>) into raw/sanitizer/ of
the run; the server is stopped before this collector runs, so the leak report at exit is there
too. The manifest's failAbove of 0 fails the run on any finding; the files stay in the run
folder with the stacks.
"""
import re
from pathlib import Path

from perfkit.errors import PerfError
from perfkit.measure import LOWER, CollectContext, Collection, Collector, Measurement

_ASAN_ERROR = re.compile(r"^==\d+==ERROR: AddressSanitizer: (?P<kind>[\w-]+)", re.MULTILINE)
_LEAKS = re.compile(r"^SUMMARY: AddressSanitizer: (?P<bytes>\d+) byte\(s\) leaked in (?P<count>\d+) allocation",
                    re.MULTILINE)
_UB = re.compile(r": runtime error: (?P<what>.+)$", re.MULTILINE)


class SanitizerCollector(Collector):
    def collect(self, context: CollectContext) -> Collection:
        folder = context.endpoints.get("sanitizer")
        if not folder:
            raise PerfError(f"{self.item.key} needs a sanitized server (perf.py run --sanitize)")
        texts = {path.name: self._read(path) for path in sorted(Path(folder).glob("*san.*")) if path.is_file()}
        asan = [match["kind"] for text in texts.values() for match in _ASAN_ERROR.finditer(text)]
        leaks = [match for text in texts.values() for match in _LEAKS.finditer(text)]
        undefined = [match["what"] for text in texts.values() for match in _UB.finditer(text)]
        source = context.source.key
        found = [Measurement(source, "asan", "errors", "reports", LOWER, float(len(asan))),
                 Measurement(source, "lsan", "leaked_bytes", "B", LOWER, float(sum(int(m["bytes"]) for m in leaks))),
                 Measurement(source, "lsan", "leaked_allocations", "allocations", LOWER,
                             float(sum(int(m["count"]) for m in leaks))),
                 Measurement(source, "ubsan", "errors", "reports", LOWER, float(len(undefined)))]
        kinds = sorted(set(asan) | {what[:80] for what in undefined})
        return Collection(found, {"sanitizer": {"files": sorted(texts), "kinds": kinds}})

    @staticmethod
    def _read(path: Path) -> str:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise PerfError(f"cannot read the sanitizer report {path}: {error}") from error
