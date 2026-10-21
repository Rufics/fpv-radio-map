"""The micro suite: builds the benchmark extensions and runs each binary with repetitions.

The C++ tree is configured with RFMAP_BUILD_BENCHMARKS=ON in a Release build folder of its own
(default build-perf/), with RFMAP_PERF_CATALOG_DIR pointing at the catalog in use, so the
benchmarks of another catalog folder are built too. Each binary's JSON is read by the collector
the benchmark names (google-benchmark by default).
"""
from dataclasses import dataclass
from pathlib import Path

from perfkit.catalog import Catalog, Item
from perfkit.commands import CommandRunner
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.measure import CollectContext, Collector
from perfkit.session import Collected
from perfkit.timeline import Timeline

DEFAULT_COLLECTOR = "google-benchmark"
DEFAULT_REPETITIONS = 10
DEFAULT_METRIC = "cpu_time"
DEFAULT_LIMIT_PCT = 5.0


@dataclass(frozen=True)
class MicroBuild:
    source_dir: Path
    build_dir: Path
    catalog_dir: Path

    @property
    def bin_dir(self) -> Path:
        return self.build_dir / "perf" / "bin"


@dataclass(frozen=True)
class MicroOptions:
    only: tuple[str, ...] = ()
    repetitions: int | None = None


class MicroSuite:
    name = "micro"
    dataset = "none"

    def __init__(self, catalog: Catalog, build: MicroBuild, runner: CommandRunner,
                 options: MicroOptions | None = None, loader: ExtensionLoader | None = None):
        self._catalog = catalog
        self._build = build
        self._runner = runner
        self._options = options or MicroOptions()
        self._loader = loader or ExtensionLoader()

    def benchmarks(self) -> list[Item]:
        items = self._catalog.items("benchmark")
        if not self._options.only:
            return items
        unknown = sorted(set(self._options.only) - {item.id for item in items})
        if unknown:
            raise PerfError(f"no benchmark {', '.join(unknown)}; perf.py list --kind benchmark shows them")
        return [item for item in items if item.id in self._options.only]

    def collector(self, benchmark: Item) -> Item:
        collector_id = benchmark.manifest.get("collector", DEFAULT_COLLECTOR)
        item = self._catalog.get("collector", collector_id)
        if item is None:
            raise PerfError(f"{benchmark.key}: no collector '{collector_id}'")
        return item

    def items(self) -> list[Item]:
        """Every item this run uses, for the versions recorded with it."""
        benchmarks = self.benchmarks()
        collectors = {self.collector(item).key: self.collector(item) for item in benchmarks}
        return benchmarks + list(collectors.values())

    def execute(self, timeline: Timeline, run_dir: Path) -> list[Collected]:
        benchmarks = self.benchmarks()
        if not benchmarks:
            raise PerfError("the catalog has no benchmarks")
        with timeline.phase("build", "build"):
            self.build(benchmarks)
        return [self._benchmark(timeline, item, run_dir) for item in benchmarks]

    def build(self, items: list[Item]) -> None:
        build = self._build
        self._runner.run(["cmake", "-S", str(build.source_dir), "-B", str(build.build_dir),
                          "-DCMAKE_BUILD_TYPE=Release", "-DRFMAP_BUILD_TESTS=OFF", "-DRFMAP_BUILD_SERVER=ON",
                          "-DRFMAP_BUILD_BENCHMARKS=ON", f"-DRFMAP_PERF_CATALOG_DIR={build.catalog_dir}"])
        self._runner.run(["cmake", "--build", str(build.build_dir), "--target",
                          *(item.manifest["target"] for item in items)])

    def _benchmark(self, timeline: Timeline, item: Item, run_dir: Path) -> Collected:
        collector_item = self.collector(item)
        collector = self._loader.load(collector_item, Collector)(collector_item)
        with timeline.phase(item.key, "benchmark") as event:
            output = self.run(item, run_dir / "raw")
        collection = collector.collect(CollectContext(item, (output,), event.started, event.finished))
        compare = item.manifest.get("compare", {})
        return Collected(item, collector_item, collection, frozenset({compare.get("metric", DEFAULT_METRIC)}),
                         float(compare.get("maxRegressionPct", DEFAULT_LIMIT_PCT)))

    def run(self, item: Item, out_dir: Path) -> Path:
        """Runs one benchmark binary; returns the JSON file it wrote."""
        out_dir.mkdir(parents=True, exist_ok=True)
        output = out_dir / f"{item.id}.json"
        count = self._options.repetitions or item.manifest.get("repetitions", DEFAULT_REPETITIONS)
        self._runner.run([str(self._build.bin_dir / item.manifest["target"]), "--benchmark_format=console",
                          f"--benchmark_out={output}", "--benchmark_out_format=json",
                          f"--benchmark_repetitions={count}", "--benchmark_report_aggregates_only=true"])
        return output
