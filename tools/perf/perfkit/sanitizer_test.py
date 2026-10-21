import shutil

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.google_benchmark_test import TOOL_DIR, CollectorTestCase
from perfkit.measure import CollectContext, Collection, Collector, Measurement
from perfkit.session import Collected, LimitCheck

ASAN = """=================================================================
==17==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010 at pc 0x55 bp 0x7f sp 0x7f
READ of size 4 at 0x602000000010 thread T3
    #0 0x55d in rfmap::ResultCache::get(std::string const&) /src/server/src/result_cache.cpp:88
"""
LSAN = """=================================================================
==17==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 64 byte(s) in 1 object(s) allocated from:
    #0 0x7f in operator new(unsigned long)

SUMMARY: AddressSanitizer: 96 byte(s) leaked in 2 allocation(s).
"""
UBSAN = "/src/core/src/maths.cpp:41:17: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented\n"


class SanitizerCollectorTest(CollectorTestCase):
    def setUp(self):
        super().setUp()
        shutil.copytree(TOOL_DIR / "extensions" / "collectors" / "sanitizer",
                        self.root / "extensions" / "collectors" / "sanitizer",
                        ignore=shutil.ignore_patterns("__pycache__"))
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root)
        self.assertEqual(problems, [])
        self.item = self.catalog.get("collector", "sanitizer")
        self.folder = self.root / "sanitizer"
        self.folder.mkdir()

    def collect(self, endpoints=None) -> Collection:
        collector = ExtensionLoader().load(self.item, Collector)(self.item)
        endpoints = {"sanitizer": str(self.folder)} if endpoints is None else endpoints
        return collector.collect(CollectContext(self.catalog.get("scenario", "peak"), (), "t0", "t1", endpoints))

    def values(self, collection: Collection) -> dict:
        return {(m.name, m.metric): m.value for m in collection.measurements}

    def test_a_clean_run(self):
        collection = self.collect()
        self.assertEqual(set(self.values(collection).values()), {0.0})
        entry = Collected(self.catalog.get("scenario", "peak"), self.item, collection, frozenset(), 5.0)
        self.assertEqual(LimitCheck.problems([entry]), [])

    def test_findings_fail_the_run(self):
        (self.folder / "asan.17").write_text(ASAN + LSAN, encoding="utf-8")
        (self.folder / "ubsan.21").write_text(UBSAN * 2, encoding="utf-8")
        (self.folder / "notes.txt").write_text(ASAN, encoding="utf-8")
        collection = self.collect()
        self.assertEqual(self.values(collection), {("asan", "errors"): 1.0, ("lsan", "leaked_bytes"): 96.0,
                                                   ("lsan", "leaked_allocations"): 2.0, ("ubsan", "errors"): 2.0})
        self.assertEqual(collection.context["sanitizer"]["files"], ["asan.17", "ubsan.21"])
        self.assertIn("heap-use-after-free", collection.context["sanitizer"]["kinds"])
        entry = Collected(self.catalog.get("scenario", "peak"), self.item, collection, frozenset(), 5.0)
        problems = LimitCheck.problems([entry])
        self.assertEqual(len(problems), 4)
        self.assertIn("scenario/peak lsan leaked_bytes is 96 B, above its limit 0 B", problems)

    def test_needs_a_sanitized_server(self):
        with self.assertRaisesRegex(PerfError, "--sanitize"):
            self.collect(endpoints={})


class ScenarioLimitTest(CollectorTestCase):
    def test_a_scenario_limit_wins_over_the_collector(self):
        peak = self.catalog.get("scenario", "peak")
        collector = self.catalog.get("collector", "google-benchmark")
        rss = Measurement("scenario/peak", "server_rss", "tail_slope", "MiB/h", "lower", 40.0)
        p95 = Measurement("scenario/peak", "all", "p95", "ms", "lower", 800.0)
        entry = Collected(peak, collector, Collection([rss, p95]), frozenset(), 5.0,
                          extra_limits={"server_rss:tail_slope": 32.0, "p95": 900.0})
        self.assertEqual(entry.over_limit(rss), 32.0)
        self.assertIsNone(entry.over_limit(p95))
        self.assertEqual(LimitCheck.problems([entry]),
                         ["scenario/peak server_rss tail_slope is 40 MiB/h, above its limit 32 MiB/h"])
