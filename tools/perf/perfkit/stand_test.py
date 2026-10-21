import json
import shutil
import tempfile
import unittest
from pathlib import Path

from perfkit.api import ApiClient
from perfkit.errors import PerfError
from perfkit.fake_server_test import FakeServer
from perfkit.stand import SANITIZE_FILE, SERVER_CONFIG, ServerConfigFile, Stand, StandLimits, StandProbe, StandSettings
from perfkit.timeline import Event


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
        # Prometheus and Grafana answer on the fake server's port too.
        settings = {"prometheus_port": self.port, "grafana_port": self.port, **settings}
        stand = Stand(StandSettings(self.root / "data", port=self.port, **settings), self.runner, "admin-token",
                      sleep=lambda _s: None)
        stand.run_dir = self.root / "run"
        return stand


class StandTest(StandTestCase):
    def test_up_load_down(self):
        stand = self.stand(limits={"RFMAP_PERF_SERVER_CPUS": "2"})
        stand.up(ApiClient(stand.base_url))
        self.assertTrue(stand.load())
        stand.down()
        up, load, down = self.runner.runs
        self.assertEqual(up[-7:], ["-d", "--build", "postgis", "rfmap-server", "prometheus", "cadvisor",
                                   "postgres-exporter"])
        self.assertIn("monitoring", up)
        self.assertNotIn("grafana", up)
        self.assertEqual(load[-4:], ["--quiet", "-o", "experimental-prometheus-rw", "/scripts/runner.js"])
        self.assertIn("K6_PROMETHEUS_RW_SERVER_URL=http://prometheus:9090/api/v1/write", load)
        self.assertEqual(down[-3:], ["down", "-v", "--remove-orphans"])
        env = self.runner.envs[0]
        self.assertEqual(env["RFMAP_PERF_ADMIN_TOKEN"], "admin-token")
        self.assertEqual(env["RFMAP_PERF_RUN_DIR"], str(self.root / "run"))
        self.assertEqual(env["RFMAP_PERF_SERVER_CPUS"], "2")
        self.assertEqual(env["RFMAP_PERF_PROMETHEUS_PORT"], str(self.port))
        self.assertFalse(Path(env["RFMAP_PERF_WORK_DIR"]).exists(), "the work folder goes with the stand")
        self.assertEqual(stand.endpoints(), {"prometheus": f"http://127.0.0.1:{self.port}/api/v1"})

    def test_without_monitoring(self):
        stand = self.stand(monitoring=False)
        stand.up(ApiClient(stand.base_url))
        stand.load()
        stand.settle()
        up, load = self.runner.runs
        self.assertEqual(up[-4:], ["-d", "--build", "postgis", "rfmap-server"])
        self.assertNotIn("monitoring", up)
        self.assertNotIn("experimental-prometheus-rw", load)
        self.assertEqual(stand.endpoints(), {})

    def test_a_sanitized_server(self):
        stand = self.stand(sanitize=True, monitoring=False)
        stand.up(ApiClient(stand.base_url))
        stand.stop_server()
        up, stop = self.runner.runs
        self.assertEqual(up[4:8], ["-f", str(SANITIZE_FILE), "-p", "rfmap-perf"])
        self.assertEqual(stop[-4:], ["stop", "-t", "60", "rfmap-server"])
        self.assertEqual(self.runner.envs[0]["RFMAP_PERF_SERVER_IMAGE"], "rfmap-server:perf-sanitize")
        folder = self.root / "run" / "raw" / "sanitizer"
        self.assertTrue(folder.is_dir())
        self.assertEqual(stand.endpoints(), {"sanitizer": str(folder)})
        self.assertEqual(StandSettings(self.root, sanitize=True, server_image="mine").image, "mine")

    def test_work_folder_holds_the_token_for_prometheus(self):
        stand = self.stand()
        token = stand.work_dir / "admin-token"
        self.assertEqual(token.read_text(encoding="utf-8"), "admin-token")
        if hasattr(token, "owner"):
            self.assertEqual(token.stat().st_mode & 0o777, 0o600)
        source = self.root / "board.json"
        source.write_text("{}", encoding="utf-8")
        stand.add_dashboard("rfmap-stand", source)
        self.assertTrue((stand.work_dir / "dashboards" / "rfmap-stand.json").is_file())
        stand.down()
        self.assertFalse(token.exists())

    def test_grafana_with_a_kept_stand(self):
        stand = self.stand(keep=True)
        stand.up(ApiClient(stand.base_url))
        self.assertIn("grafana", self.runner.runs[0])
        stand.annotate([Event("load", "load", "2026-10-21T10:00:00.000Z", "2026-10-21T10:01:00.000Z"),
                        Event("run", "run", "2026-10-21T09:59:00.000Z")])
        (annotation,) = [call for call in self.server.calls if call[1] == "/api/annotations"]
        self.assertEqual(annotation[2], {"time": 1792576800000, "timeEnd": 1792576860000,
                                         "tags": ["rfmap-perf", "load"], "text": "load: load"})
        work_dir = stand.work_dir
        stand.down()
        self.assertTrue(work_dir.exists(), "a kept stand keeps its work folder")
        shutil.rmtree(work_dir)

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
            stand.up(dead, clock=lambda: next(times))

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
