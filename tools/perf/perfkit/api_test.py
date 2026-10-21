import unittest

from perfkit.api import ApiClient, Provisioner
from perfkit.errors import PerfError
from perfkit.fake_server_test import FakeServer

MISSIONS = [{"name": "a"}, {"name": "b"}]


class ApiTestCase(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.addCleanup(self.server.stop)
        self.client = ApiClient(self.server.base_url)


class ApiClientTest(ApiTestCase):
    def test_calls_and_errors(self):
        self.assertEqual(self.client.call("GET", "/health").body["ok"], True)
        reply = self.client.call("GET", "/nope")
        self.assertEqual((reply.status, reply.body), (404, {"message": "no such route"}))
        with self.assertRaisesRegex(PerfError, "answered 404, expected 200"):
            self.client.expect(200, "GET", "/nope")

    def test_only_local_stands(self):
        with self.assertRaisesRegex(PerfError, "must be local"):
            ApiClient("https://rfmap.example/api/v1")

    def test_unreachable(self):
        gone = FakeServer()
        gone.stop()
        with self.assertRaises(PerfError):
            ApiClient(gone.base_url).call("GET", "/health")

    def test_parse(self):
        self.assertIsNone(ApiClient._parse(b""))
        self.assertEqual(ApiClient._parse(b"plain"), "plain")


class ProvisionerTest(ApiTestCase):
    def test_link_pin_creates_missions_and_links(self):
        found = Provisioner(self.client).credentials({"type": "link-pin", "role": "view"}, MISSIONS, 3)
        self.assertEqual([c["token"] for c in found], ["t-m1", "t-m2", "t-m3"])
        self.assertEqual({c["pin"] for c in found}, {Provisioner.PIN})
        posted = [call[2]["name"] for call in self.server.calls if call[1] == "/api/v1/missions"]
        self.assertEqual(posted, ["a", "b", "a"])
        link = next(call for call in self.server.calls if call[1].endswith("/links"))
        self.assertEqual(link[2]["role"], "view")
        self.assertEqual(link[3]["X-Owner-Key"], "k1")

    def test_owner_and_anonymous(self):
        owner = Provisioner(self.client).credentials({"type": "mission-owner"}, MISSIONS, 1)
        self.assertEqual(owner, [{"missionId": "m1", "ownerKey": "k1", "session": "s1"}])
        self.assertEqual(Provisioner(self.client).credentials({"type": "none"}, [], 5), [])
        with self.assertRaisesRegex(PerfError, "needs setup.missions"):
            Provisioner(self.client).credentials({"type": "mission-owner"}, [], 1)


if __name__ == "__main__":
    unittest.main()
