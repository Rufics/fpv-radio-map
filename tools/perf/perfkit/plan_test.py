import unittest

from perfkit.catalog_test import CatalogTestCase
from perfkit.errors import PerfError
from perfkit.plan import FixtureReader, PlanBuilder


class PlanTestCase(CatalogTestCase):
    def setUp(self):
        super().setUp()
        self.sample.write("fixtures/link-setups", {"kind": "fixture", "id": "link-setups", "version": "1.0.0",
                                                   "files": {"control": "control.json", "many": "many.json"}},
                          {"control.json": '{"radio": "x"}', "many.json": '[{"n": 1}, {"n": 2}]'})
        manifest = self.sample.manifest("personas/link-editor")
        manifest["setup"] = {"missions": "link-setups:many.json", "count": 2}
        self.sample.write("personas/link-editor", manifest)
        self.sample.write("personas/viewer", {"kind": "persona", "id": "viewer", "version": "1.0.0",
                                              "auth": {"type": "none"},
                                              "steps": [{"name": "health", "weight": 1,
                                                         "method": "GET", "path": "/health"}]})
        scenario = self.sample.manifest("scenarios/peak")
        scenario["personas"] = [{"id": "link-editor", "weight": 3}, {"id": "viewer", "weight": 1}]
        scenario["thresholds"] = {"http_req_failed": "rate<0.01"}
        self.sample.write("scenarios/peak", scenario)
        self.catalog, problems = self.loader.load(self.sample.root)
        self.assertEqual(problems, [])
        self.builder = PlanBuilder(self.catalog, FixtureReader(self.catalog))


class PlanBuilderTest(PlanTestCase):
    def test_plan(self):
        asked = []

        def credentials(persona, missions):
            asked.append((persona.id, missions))
            return [{"missionId": "m1"}]

        plan = self.builder.build(self.catalog.get("scenario", "peak"), "http://s/api/v1", "tok", credentials)
        editor, viewer = plan["personas"]
        self.assertEqual((editor["share"], viewer["share"]), (0.75, 0.25))
        self.assertEqual(asked, [("link-editor", [{"n": 1}, {"n": 2}]), ("viewer", [])])
        path, coverage = editor["steps"]
        self.assertEqual(path["bodies"], [{"radio": "x"}])
        self.assertEqual(coverage["bodies"], [{"inline": True}])
        self.assertNotIn("body", path)
        self.assertIsNone(viewer["steps"][0]["bodies"])
        self.assertEqual(plan["thresholds"], {"http_req_failed": ["rate<0.01"]})
        self.assertEqual((plan["baseUrl"], plan["adminToken"], plan["scenario"]), ("http://s/api/v1", "tok", "peak"))

    def test_constant_rate_has_one_stage(self):
        scenario = self.catalog.get("scenario", "peak")
        scenario.manifest["load"] = {"type": "constant-arrival-rate",
                                     "stages": [{"durationS": 1, "targetRps": 1}, {"durationS": 1, "targetRps": 2}]}
        with self.assertRaisesRegex(PerfError, "exactly one stage"):
            self.builder.build(scenario, "u", "t", lambda persona, missions: [])

    def test_missing_items(self):
        scenario = self.catalog.get("scenario", "peak")
        scenario.manifest["personas"] = [{"id": "ghost", "weight": 1}]
        with self.assertRaisesRegex(PerfError, "no persona 'ghost'"):
            self.builder.personas(scenario)
        reader = FixtureReader(self.catalog)
        with self.assertRaisesRegex(PerfError, "no fixture 'nope'"):
            reader.read("nope:x.json")
        with self.assertRaisesRegex(PerfError, "cannot read link-setups:gone.json"):
            reader.read("link-setups:gone.json")


if __name__ == "__main__":
    unittest.main()
