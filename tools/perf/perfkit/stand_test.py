import json
import tempfile
import unittest
from pathlib import Path

from perfkit.api import ApiClient
from perfkit.errors import PerfError
from perfkit.fake_server_test import FakeServer
from perfkit.stand import SERVER_CONFIG, ServerConfigFile, Stand, StandLimits, StandProbe, StandSettings


class FakeRunner:
    """Answers docker the way a running stand would."""

    def __init__(self, k6_code: int = 0):
        self.runs: list[list[str]] = []
        self.envs: list[dict] = []
        self.k6_code = k6_code

    def run(self, args, cwd=None, env=None, allowed=(0,)):
        self.runs.append(args)
        self.envs.append(env)
        code = self.k6_code if "k6" in args else 0
        if code not in allowed:
            raise PerfError(f"exit {code}")
        return code

    def capture(self, args, env=None):
        sql = args[-1]
        if args[:2] == ["docker", "inspect"]:
            return json.dumps([{"Name": "/rfmap-perf-rfmap-server-1", "Image": "sha256:abc",
                                "Config": {"Image": "rfmap-server:perf"},
                                "HostConfig": {"NanoCpus": 4_000_000_000, "Memory": 4 << 30}}])
        if "ps" in args:
            return "abc123\n"
        if "logs" in args:
            return "server log\n"
        if "pg_settings" in sql:
            return "shared_buffers\t16384\t8kB\njit\ton\t\n"
        if "server_version" in sql:
            return "16.4\n"
        if "pg_database_size" in sql:
            return "9000000\n"
        if "pg_stat_user_tables" in sql:
            return "missions\t12\t81920\n"
        raise AssertionError(f"unexpected {args}")


class StandTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.server = FakeServer()
        self.addCleanup(self.server.stop)
        self.port = int(self.server.base_url.split(":")[2].split("/")[0])
        self.runner = FakeRunner()

    def stand(self, **settings):
        stand = Stand(StandSettings(self.root / "data", port=self.port, **settings), self.runner, "admin-token")
        stand.run_dir = self.root / "run"
        return stand


class StandTest(StandTestCase):
    def test_up_load_down(self):
        stand = self.stand(limits={"RFMAP_PERF_SERVER_CPUS": "2"})
        stand.up(ApiClient(stand.base_url))
        self.assertTrue(stand.load())
        stand.down()
        up, load, down = self.runner.runs
        self.assertEqual(up[-4:], ["-d", "--build", "postgis", "rfmap-server"])
        self.assertIn("/scripts/runner.js", load)
        self.assertEqual(down[-3:], ["down", "-v", "--remove-orphans"])
        env = self.runner.envs[0]
        self.assertEqual(env["RFMAP_PERF_ADMIN_TOKEN"], "admin-token")
        self.assertEqual(env["RFMAP_PERF_RUN_DIR"], str(self.root / "run"))
        self.assertEqual(env["RFMAP_PERF_SERVER_CPUS"], "2")

    def test_thresholds_kept_and_stand_left_running(self):
        self.runner.k6_code = 99
        stand = self.stand(keep=True, build=False)
        self.assertFalse(stand.load())
        stand.down()
        self.assertEqual(len(self.runner.runs), 1)

    def test_server_that_never_answers(self):
        stand = self.stand()
        times = iter(range(0, 1000, 100))
        dead = ApiClient("http://127.0.0.1:9/api/v1")
        with self.assertRaisesRegex(PerfError, "did not answer /health"):
            stand.up(dead, clock=lambda: next(times), sleep=lambda _s: None)

    def test_needs_a_run_folder(self):
        stand = Stand(StandSettings(self.root), self.runner)
        with self.assertRaisesRegex(PerfError, "no run folder"):
            stand.environment()
        self.assertGreater(len(stand.admin_token), 30)

    def test_logs(self):
        stand = self.stand()
        stand.logs(self.root / "server.log")
        self.assertEqual((self.root / "server.log").read_text(encoding="utf-8"), "server log\n")


class StandProbeTest(StandTestCase):
    def test_server_and_database(self):
        stand = self.stand()
        probe = StandProbe(stand, ApiClient(stand.base_url))
        server = probe.server()
        self.assertTrue(server["config"]["metrics"]["enabled"])
        self.assertEqual(server["health"]["version"], "0.1.0")
        self.assertEqual(server["containers"]["rfmap-perf-rfmap-server-1"]["nanoCpus"], 4_000_000_000)
        database = probe.database()
        self.assertEqual(database["settings"], {"shared_buffers": "16384 (8kB)", "jit": "on"})
        self.assertEqual(database["version"], "16.4")
        self.assertEqual(database["databaseBytes"], 9000000)
        self.assertEqual(database["tables"], {"missions": {"rows": 12, "bytes": 81920}})


class ConfigAndLimitsTest(unittest.TestCase):
    def test_the_stand_config_parses(self):
        config = ServerConfigFile.read(SERVER_CONFIG)
        self.assertTrue(config["metrics"]["enabled"])
        self.assertGreaterEqual(config["rateLimits"]["open"]["perMinute"], 100000)

    def test_limits_from_the_environment(self):
        found = StandLimits.from_environment({"RFMAP_PERF_DB_MEMORY": "1g", "OTHER": "x"})
        self.assertEqual(found, {"RFMAP_PERF_DB_MEMORY": "1g"})


if __name__ == "__main__":
    unittest.main()
