"""A flamegraph with Linux perf and inferno.

perf record samples the call stacks (DWARF unwinding by default, so release builds without frame
pointers still give whole stacks), perf script prints them, inferno-collapse-perf folds them and
inferno-flamegraph draws the SVG. The perf.data and the printed stacks are large and are deleted;
the folded stacks stay, so two runs can be compared with inferno-diff-folded.

RFMAP_PERF_PERF names another perf binary, for a kernel whose perf wrapper has no matching tools
(e.g. /usr/lib/linux-tools/<version>/perf).
"""
import os
import shutil
from pathlib import Path

from perfkit.errors import PerfError
from perfkit.profile import Profiler, ProfileTarget

PERF_ENV = "RFMAP_PERF_PERF"
_CALL_GRAPHS = ("dwarf", "fp", "lbr")


class FlamegraphProfiler(Profiler):
    def profile(self, target: ProfileTarget, out_dir: Path) -> list[Path]:
        perf = self._perf()
        collapse, flamegraph = (self._tool(self.options.get(name, default)) for name, default in
                                (("collapse", "inferno-collapse-perf"), ("flamegraph", "inferno-flamegraph")))
        name = target.file_name
        data, stacks = out_dir / f"{name}.perf.data", out_dir / f"{name}.perf.txt"
        folded, svg = out_dir / f"{name}.folded", out_dir / f"flamegraph-{name}.svg"
        try:
            self.runner.run(self._record(perf, target, data))
            self.runner.run([perf, "script", "-i", str(data)], stdout_path=stacks)
            self.runner.run([collapse, str(stacks)], stdout_path=folded)
            self.runner.run([flamegraph, "--title", target.name, str(folded)], stdout_path=svg)
        finally:
            data.unlink(missing_ok=True)
            stacks.unlink(missing_ok=True)
        return [svg, folded]

    def _record(self, perf: str, target: ProfileTarget, data: Path) -> list[str]:
        frequency = int(self.options.get("frequencyHz", 99))
        call_graph = str(self.options.get("callGraph", "dwarf"))
        if not 1 <= frequency <= 10000:
            raise PerfError(f"{self.item.key}: frequencyHz must be from 1 to 10000")
        if call_graph not in _CALL_GRAPHS:
            raise PerfError(f"{self.item.key}: callGraph must be one of {', '.join(_CALL_GRAPHS)}")
        args = [perf, "record", "-F", str(frequency), "--call-graph", call_graph, "-o", str(data)]
        if target.pid:
            return [*args, "-p", str(target.pid), "--", "sleep", f"{target.duration_s:g}"]
        return [*args, "--", *target.command]

    def _perf(self) -> str:
        override = os.environ.get(PERF_ENV, "")
        if not override:
            return self._tool(self.options.get("perf", "perf"))
        path = Path(override)
        if not (path.is_absolute() and path.is_file() and os.access(path, os.X_OK)):
            raise PerfError(f"{PERF_ENV} must be the absolute path of an executable, not {override!r}")
        return str(path)

    @staticmethod
    def _tool(name: str) -> str:
        found = shutil.which(name)
        if found is None:
            raise PerfError(f"{name} is not installed (perf: linux-perf or linux-tools; "
                            f"inferno: cargo install inferno)")
        return found
