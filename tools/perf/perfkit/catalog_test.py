import json
import tempfile
import unittest
from pathlib import Path

from perfkit.catalog import CatalogLoader, ContentHasher, FieldPath, KindRegistry
from perfkit.errors import PerfError, SchemaError

SCHEMAS_DIR = Path(__file__).resolve().parent.parent / "schemas"


class SampleCatalog:
    """A small valid catalog in a temporary folder, one item of every kind."""

    def __init__(self, root: Path):
        self.root = root
        self.write("fixtures/warsaw-points", {"kind": "fixture", "id": "warsaw-points", "version": "1.0.0",
                                              "files": {"points": "points.json"}},
                   {"points.json": "[[52.23, 21.01]]"})
        self.write("fixtures/link-setups", {"kind": "fixture", "id": "link-setups", "version": "1.0.0",
                                            "files": {"control": "control.json"}},
                   {"control.json": "{}"})
        self.write("personas/link-editor", {
            "kind": "persona", "id": "link-editor", "version": "1.0.0",
            "fixtures": ["warsaw-points", "link-setups"],
            "auth": {"type": "link-pin", "role": "edit"},
            "thinkTimeS": {"min": 2, "max": 8},
            "steps": [
                {"name": "path", "weight": 6, "method": "POST", "path": "/calc/path",
                 "body": "link-setups:control.json"},
                {"name": "coverage", "weight": 1, "method": "POST", "path": "/calc/coverage",
                 "body": {"inline": True},
                 "poll": {"path": "/jobs/{{response.jobId}}", "untilField": "state", "pending": ["queued", "running"],
                          "timeoutS": 120}},
            ]})
        self.write("scenarios/peak", {
            "kind": "scenario", "id": "peak", "version": "1.0.0",
            "personas": [{"id": "link-editor", "weight": 100}],
            "load": {"type": "ramping-arrival-rate", "stages": [{"durationS": 60, "targetRps": 20}]},
            "collectors": ["k6-summary"], "reporters": ["html"],
            "compare": {"metrics": ["p95"], "window": 10, "maxRegressionPct": 5}})
        self.write("extensions/collectors/k6-summary", {
            "kind": "collector", "id": "k6-summary", "version": "1.0.0", "entry": "collector.py:K6SummaryCollector",
            "metrics": [{"name": "p95", "unit": "ms", "better": "lower"}]},
                   {"collector.py": "class K6SummaryCollector:\n    pass\n"})
        self.write("extensions/reporters/html", {"kind": "reporter", "id": "html", "version": "1.0.0",
                                                 "entry": "reporter.py:HtmlReporter"},
                   {"reporter.py": "class HtmlReporter:\n    pass\n"})
        self.write("extensions/benchmarks/rf-core", {"kind": "benchmark", "id": "rf-core", "version": "1.0.0",
                                                     "target": "bench_rf_core", "links": ["rfcore"]},
                   {"CMakeLists.txt": "# bench\n", "path_loss.cpp": "// bench\n"})

    def write(self, folder: str, manifest: dict, files: dict[str, str] | None = None) -> Path:
        path = self.root / folder
        path.mkdir(parents=True, exist_ok=True)
        (path / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        for name, content in (files or {}).items():
            (path / name).write_text(content, encoding="utf-8")
        return path

    def manifest(self, folder: str) -> dict:
        return json.loads((self.root / folder / "manifest.json").read_text(encoding="utf-8"))

    def change(self, folder: str, **fields) -> None:
        manifest = self.manifest(folder)
        manifest.update(fields)
        self.write(folder, manifest)


class CatalogTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.sample = SampleCatalog(Path(tmp.name))
        self.registry = KindRegistry.load(SCHEMAS_DIR)

        self.loader = CatalogLoader(self.registry)


class KindRegistryTest(unittest.TestCase):
    def test_every_schema_with_a_block_is_a_kind(self):
        registry = KindRegistry.load(SCHEMAS_DIR)
        self.assertEqual({kind.name for kind in registry.kinds()},
                         {"fixture", "persona", "scenario", "collector", "reporter", "benchmark", "dashboard",
                          "profiler"})
        self.assertEqual(registry.get("benchmark").required_files, ("CMakeLists.txt",))
        self.assertIsNone(registry.get("nope"))

    def test_bad_blocks(self):
        for block in ({"folder": "x"}, {"folder": "x", "group": "other"}, {"folder": "../x", "group": "extension"}):
            with self.subTest(block=block), tempfile.TemporaryDirectory() as tmp:
                Path(tmp, "x.schema.json").write_text(json.dumps({"x-perfkit": block}), encoding="utf-8")
                with self.assertRaises(SchemaError):
                    KindRegistry.load(Path(tmp))

    def test_two_kinds_in_one_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("a", "b"):
                Path(tmp, f"{name}.schema.json").write_text(
                    json.dumps({"x-perfkit": {"folder": "same", "group": "description"}}), encoding="utf-8")
            with self.assertRaisesRegex(SchemaError, "share"):
                KindRegistry.load(Path(tmp))


class CatalogLoaderTest(CatalogTestCase):
    def messages(self):
        return [problem.describe(self.sample.root) for problem in self.loader.load(self.sample.root)[1]]

    def test_sample_is_valid(self):
        catalog, problems = self.loader.load(self.sample.root)
        self.assertEqual(problems, [])
        self.assertEqual(len(catalog.items()), 7)
        self.assertEqual([item.id for item in catalog.items("fixture")], ["link-setups", "warsaw-points"])
        self.assertEqual(str(catalog.get("persona", "link-editor").version), "1.0.0")

    def test_missing_root(self):
        missing = self.sample.root / "nope"
        with self.assertRaises(PerfError):
            self.loader.load(missing)

    def test_folder_without_manifest_and_bad_json(self):
        (self.sample.root / "personas" / "empty").mkdir()
        (self.sample.root / "personas" / "broken").mkdir()
        (self.sample.root / "personas" / "broken" / "manifest.json").write_text("{", encoding="utf-8")
        messages = self.messages()
        self.assertIn("personas/empty: no manifest.json", messages)
        self.assertTrue(any(m.startswith("personas/broken/manifest.json: not valid JSON") for m in messages))

    def test_schema_errors_are_all_listed(self):
        self.sample.change("scenarios/peak", load={"type": "spike", "stages": []}, extra=1)
        messages = self.messages()
        self.assertIn("scenarios/peak/manifest.json: $: unknown field 'extra'", messages)
        self.assertTrue(any("$.load.type: must be one of" in m for m in messages))
        self.assertTrue(any("$.load.stages: needs at least 1" in m for m in messages))

    def test_limit_keys_and_expected_statuses(self):
        self.sample.change("scenarios/peak", limits={"server_rss:tail_slope": 32, "p95": 900, "bad key!": 1})
        messages = self.messages()
        self.assertEqual([m for m in messages if "limits" in m],
                         [("scenarios/peak/manifest.json: $.limits.bad key!: 'bad key!' does not match "
                           "^([A-Za-z0-9_./-]+:)?[a-z0-9_]+$")])

    def test_id_must_match_folder(self):
        self.sample.change("personas/link-editor", id="someone")
        self.assertTrue(any("must equal the folder name" in m for m in self.messages()))

    def test_missing_references(self):
        self.sample.change("scenarios/peak", personas=[{"id": "ghost", "weight": 1}], collectors=["nope"])
        messages = self.messages()
        self.assertIn("scenarios/peak/manifest.json: personas[].id: no persona 'ghost'", messages)
        self.assertIn("scenarios/peak/manifest.json: collectors[]: no collector 'nope'", messages)

    def test_reference_with_missing_file(self):
        manifest = self.sample.manifest("personas/link-editor")
        manifest["steps"][0]["body"] = "link-setups:video.json"
        self.sample.write("personas/link-editor", manifest)
        self.assertIn("personas/link-editor/manifest.json: steps[].body: file 'video.json' does not exist in "
                      "link-setups/", self.messages())

    def test_missing_entry_and_required_files(self):
        (self.sample.root / "extensions/collectors/k6-summary/collector.py").unlink()
        (self.sample.root / "extensions/benchmarks/rf-core/CMakeLists.txt").unlink()
        messages = self.messages()
        self.assertIn("extensions/collectors/k6-summary/manifest.json: file 'collector.py' does not exist in "
                      "k6-summary/", messages)
        self.assertIn("extensions/benchmarks/rf-core/manifest.json: file 'CMakeLists.txt' does not exist in "
                      "rf-core/", messages)

    def test_fixture_file_must_stay_inside(self):
        self.sample.change("fixtures/warsaw-points", files={"points": "data/../../secret.json"})
        self.assertTrue(any("must be a relative path inside warsaw-points/" in m for m in self.messages()))

    def test_missing_file_rules(self):
        folder = self.sample.root / "fixtures" / "warsaw-points"
        self.assertEqual(CatalogLoader.missing_file(folder, "points.json"), "")
        self.assertIn("relative path", CatalogLoader.missing_file(folder, ""))
        self.assertIn("relative path", CatalogLoader.missing_file(folder, "/etc/passwd"))
        (folder / "link.json").symlink_to(self.sample.root / "personas" / "link-editor" / "manifest.json")
        self.assertIn("leads outside", CatalogLoader.missing_file(folder, "link.json"))


class ContentHasherTest(CatalogTestCase):
    def test_hash_follows_content_and_names_but_not_caches(self):
        folder = self.sample.root / "extensions/collectors/k6-summary"
        before = ContentHasher.folder(folder)
        (folder / "__pycache__").mkdir()
        (folder / "__pycache__" / "collector.cpython-313.pyc").write_bytes(b"x")
        (folder / ".DS_Store").write_bytes(b"x")
        self.assertEqual(ContentHasher.folder(folder), before)
        (folder / "collector.py").write_text("changed", encoding="utf-8")
        changed = ContentHasher.folder(folder)
        self.assertNotEqual(changed, before)
        (folder / "collector.py").rename(folder / "other.py")
        self.assertNotEqual(ContentHasher.folder(folder), changed)

    def test_files_are_ordered_by_posix_path_on_every_os(self):
        folder = self.sample.root / "fixtures/warsaw-points"
        (folder / "B.json").write_text("{}", encoding="utf-8")
        (folder / "a.json").write_text("{}", encoding="utf-8")
        names = [path.name for path in ContentHasher.files(folder)]
        self.assertEqual(names, ["B.json", "a.json", "manifest.json", "points.json"])


class FieldPathTest(unittest.TestCase):
    def test_paths(self):
        data = {"a": [{"b": 1}, {"b": 2}, {"c": 3}], "d": {"x": "1", "y": "2"}, "e": "s"}
        self.assertEqual(list(FieldPath.values(data, "a[].b")), [1, 2])
        self.assertEqual(list(FieldPath.values(data, "d.*")), ["1", "2"])
        self.assertEqual(list(FieldPath.values(data, "e")), ["s"])
        self.assertEqual(list(FieldPath.values(data, "missing[].b")), [])
        self.assertEqual(list(FieldPath.values(data, "e[]")), [])
        self.assertEqual(list(FieldPath.values(data, "e.*")), [])


if __name__ == "__main__":
    unittest.main()
