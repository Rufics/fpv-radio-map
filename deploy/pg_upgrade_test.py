"""Tests of pg_upgrade.py against a recorded fake of the docker CLI."""
import io
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import pg_upgrade
from pg_upgrade import Command, Compose, RowCounts, Upgrade, UpgradeError


class FakeDocker(pg_upgrade.Docker):
    """Records every docker call and answers from a table of canned outputs and failures."""

    def __init__(self, volumes=(), counts_before="missions|2\nsessions|1\n", counts_after=None):
        super().__init__()
        self.calls = []
        self.volumes = set(volumes)
        self.counts_before = counts_before
        self.counts_after = counts_before if counts_after is None else counts_after
        self.fail_on = None

    def run(self, args, stdin=None, stdout=None):
        self.calls.append(list(args))
        if self.fail_on and self.fail_on in args:
            raise UpgradeError(f"{self.fail_on} failed")
        if args[:2] == ["volume", "inspect"]:
            if args[2] not in self.volumes:
                raise UpgradeError("no such volume")
            return "[]"
        if "pg_dump" in args and stdout is not None:
            stdout.write(b"PGDMP")
        if "psql" in args:
            return self.counts_after if "compose" in args else self.counts_before
        return ""

    def commands(self):
        """The docker calls without the compose prefix, joined for easy matching."""
        return [" ".join(call) for call in self.calls]


class RowCountsTest(unittest.TestCase):
    def test_parses_psql_lines(self):
        counts = RowCounts.parse("missions|3\n\nsessions|0\n")
        self.assertEqual(counts.counts, {"missions": 3, "sessions": 0})
        self.assertEqual(counts.total, 3)

    def test_names_changed_and_missing_tables(self):
        before = RowCounts({"missions": 3, "library": 5})
        after = RowCounts({"missions": 2, "sessions": 1})
        self.assertEqual(before.differences(after), [
            "library: 5 before, missing after",
            "missions: 3 before, 2 after",
            "sessions: missing before, 1 after",
        ])

    def test_equal_counts_have_no_differences(self):
        self.assertEqual(RowCounts({"a": 1}).differences(RowCounts({"a": 1})), [])


class UpgradeTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.backup = Path(self._tmp.name) / "backup"
        self.messages = []

    def tearDown(self):
        self._tmp.cleanup()

    def upgrade(self, docker):
        compose = Compose(docker, Path("/srv/deploy"))
        return Upgrade(docker, compose, self.backup, out=self.messages.append)

    def test_refuses_without_the_old_volume(self):
        with self.assertRaisesRegex(UpgradeError, "nothing to move"):
            self.upgrade(FakeDocker()).preflight()

    def test_refuses_when_the_new_volume_exists(self):
        docker = FakeDocker(volumes={pg_upgrade.OLD_VOLUME, pg_upgrade.NEW_VOLUME})
        with self.assertRaisesRegex(UpgradeError, "has run before"):
            self.upgrade(docker).preflight()

    def test_moves_the_data_in_order(self):
        docker = FakeDocker(volumes={pg_upgrade.OLD_VOLUME})
        upgrade = self.upgrade(docker)
        upgrade.preflight()
        upgrade.run()
        commands = docker.commands()
        steps = ["compose --project-directory /srv/deploy -p rfmap stop rfmap-server postgis",
                 f"run -d --rm --name {pg_upgrade.OLD_CONTAINER} --network none",
                 "pg_dump -U rfmap -Fc rfmap",
                 f"stop {pg_upgrade.OLD_CONTAINER}",
                 "up -d --wait",
                 "drop database rfmap with (force) -c create database rfmap template template0 owner rfmap",
                 "pg_restore -U rfmap -d rfmap --no-owner --single-transaction --exit-on-error"]
        positions = [next(i for i, c in enumerate(commands) if step in c) for step in steps]
        self.assertEqual(positions, sorted(positions))
        mount = f"-v {pg_upgrade.OLD_VOLUME}:/var/lib/postgresql/data {pg_upgrade.OLD_IMAGE}"
        self.assertIn(mount, commands[positions[1]])
        self.assertEqual(commands[-1], "compose --project-directory /srv/deploy -p rfmap up -d")
        dumps = list(self.backup.glob("rfmap-pg16-*.dump"))
        self.assertEqual(len(dumps), 1)
        self.assertEqual(dumps[0].read_bytes(), b"PGDMP")
        self.assertIn("Row counts match", self.messages)

    def test_stops_when_the_counts_differ(self):
        docker = FakeDocker(volumes={pg_upgrade.OLD_VOLUME}, counts_after="missions|1\nsessions|1\n")
        with self.assertRaisesRegex(UpgradeError, "missions: 2 before, 1 after"):
            self.upgrade(docker).run()
        self.assertNotIn("compose --project-directory /srv/deploy -p rfmap up -d", docker.commands())

    def test_stops_the_temporary_container_when_the_dump_fails(self):
        docker = FakeDocker(volumes={pg_upgrade.OLD_VOLUME})
        docker.fail_on = "pg_dump"
        with self.assertRaises(UpgradeError):
            self.upgrade(docker).run()
        self.assertEqual(docker.commands()[-1], f"stop {pg_upgrade.OLD_CONTAINER}")


class CommandTest(unittest.TestCase):
    def setUp(self):
        self._docker_class = pg_upgrade.Docker
        self.fake = FakeDocker(volumes={pg_upgrade.OLD_VOLUME})
        pg_upgrade.Docker = lambda: self.fake
        self._tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        pg_upgrade.Docker = self._docker_class
        self._tmp.cleanup()

    def run_command(self, *argv, answer="y"):
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr), mock.patch("sys.stdout", io.StringIO()):
            return Command().run(["--backup-dir", self._tmp.name, *argv], ask=lambda _prompt: answer)

    def test_no_answer_changes_nothing(self):
        self.assertEqual(self.run_command(answer="n"), 2)
        self.assertFalse(any("stop" in call for call in self.fake.calls))

    def test_refused_preflight_is_a_usage_error(self):
        self.fake.volumes = set()
        self.assertEqual(self.run_command("--yes"), 2)

    def test_failed_step_returns_one(self):
        self.fake.fail_on = "pg_restore"
        self.assertEqual(self.run_command("--yes"), 1)

    def test_success_returns_zero(self):
        self.assertEqual(self.run_command("--yes"), 0)


if __name__ == "__main__":
    unittest.main()
