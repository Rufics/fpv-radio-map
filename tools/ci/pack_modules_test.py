"""Tests of the module packer: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import contextlib
import hashlib
import io
import json
import os
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import pack_modules
from pack_modules import CHECKSUMS, Command, ModuleCatalog, PackError, StagedModule

# Stands in for the rfmod tool: `pack` writes the stage folder name into the package, `test`
# appends its arguments to calls.log and passes unless FAKE_RFMOD_FAIL names the command.
FAKE_RFMOD = f"""#!{sys.executable}
import os, sys
from pathlib import Path
args = sys.argv[1:]
log = Path(__file__).with_name("calls.log")
with log.open("a") as stream:
    stream.write(" ".join(Path(a).name if a.startswith("/") else a for a in args) + "\\n")
if os.environ.get("FAKE_RFMOD_FAIL") == args[0]:
    print("fake failure")
    sys.exit(3)
if args[0] == "pack":
    Path(args[args.index("-o") + 1]).write_text(Path(args[1]).parent.name)
elif os.environ.get("FAKE_RFMOD_QUIET") != "1":
    print("All checks passed")
"""


class FakeBuild:
    """A build folder with staged modules and a fake rfmod tool."""

    def __init__(self, root):
        self.root = Path(root)
        self.build = self.root / "build"
        self.out = self.root / "dist"
        self.key = self._file("owner.secret", "secret")
        self.trust = self._file("owner.pub", "public")
        tool = self.build / "platform" / "rfmod" / "rfmod"
        tool.parent.mkdir(parents=True)
        tool.write_text(FAKE_RFMOD, encoding="utf-8")
        tool.chmod(0o755)
        self.log = tool.with_name("calls.log")

    def _file(self, name, text):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    def stage(self, folder, manifest):
        stage = self.build / "modules" / "fpv" / folder / "stage"
        stage.mkdir(parents=True)
        (stage / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return stage

    def argv(self):
        return [str(self.build), str(self.key), str(self.trust), str(self.out)]

    def calls(self):
        return self.log.read_text(encoding="utf-8").splitlines()


def module(module_id, *requires, version="1.0.0"):
    return {"id": module_id, "version": version, "requires": [{"id": r, "version": "^1"} for r in requires]}


class StagedModuleTest(unittest.TestCase):
    def test_reads_id_version_and_requires(self):
        staged = StagedModule(Path("s"), module("fpv.relay", "fpv.core", version="2.1.0"))
        self.assertEqual((staged.id, staged.version, staged.requires), ("fpv.relay", "2.1.0", ["fpv.core"]))
        self.assertEqual(staged.package_name, "fpv.relay-2.1.0.rfmod")

    def test_refuses_a_manifest_without_version(self):
        with self.assertRaisesRegex(PackError, "cannot read version"):
            StagedModule(Path("s"), {"id": "fpv.core"})

    def test_refuses_a_requirement_without_id(self):
        with self.assertRaisesRegex(PackError, "cannot read id"):
            StagedModule(Path("s"), {"id": "fpv.relay", "version": "1.0.0", "requires": [{"version": "^1"}]})


class ModuleCatalogTest(unittest.TestCase):
    def catalog(self, *manifests):
        return ModuleCatalog(StagedModule(Path(m["id"]), m) for m in manifests)

    def test_dependencies_come_before_their_dependents(self):
        catalog = self.catalog(module("core"), module("world", "core"), module("relay", "world", "core"))
        order = catalog.dependencies_of(catalog.get("relay"))
        self.assertEqual([m.id for m in order], ["core", "world"])

    def test_each_dependency_is_listed_once(self):
        catalog = self.catalog(module("core"), module("a", "core"), module("b", "core"), module("top", "a", "b"))
        self.assertEqual([m.id for m in catalog.dependencies_of(catalog.get("top"))], ["core", "a", "b"])

    def test_unknown_requirement_is_an_error(self):
        catalog = self.catalog(module("relay", "missing"))
        with self.assertRaisesRegex(PackError, "missing is not staged"):
            catalog.dependencies_of(catalog.get("relay"))


class CommandTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.fake = FakeBuild(self._tmp.name)
        self.fake.stage("core", module("fpv.core"))
        self.fake.stage("relay", module("fpv.relay", "fpv.world"))
        self.fake.stage("world", module("fpv.world", "fpv.core"))

    def run_command(self, argv=None, env=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, env or {}), contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            status = Command().run(self.fake.argv() if argv is None else argv)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_packs_checks_and_lists_checksums(self):
        status, stdout, _ = self.run_command()
        self.assertEqual(status, 0)
        self.assertIn("All checks passed", stdout)
        names = ["fpv.core-1.0.0.rfmod", "fpv.relay-1.0.0.rfmod", "fpv.world-1.0.0.rfmod"]
        self.assertEqual(sorted(p.name for p in self.fake.out.iterdir()), [CHECKSUMS, *names])
        expected = "".join(f"{hashlib.sha256((self.fake.out / n).read_bytes()).hexdigest()}  {n}\n" for n in names)
        self.assertEqual((self.fake.out / CHECKSUMS).read_text(encoding="utf-8"), expected)
        self.assertIn("test fpv.relay-1.0.0.rfmod --trust owner.pub --with fpv.core-1.0.0.rfmod "
                      "--with fpv.world-1.0.0.rfmod", self.fake.calls())
        self.assertIn("pack stage --key owner.secret -o fpv.core-1.0.0.rfmod", self.fake.calls())

    def test_failed_pack_stops_the_run(self):
        status, stdout, stderr = self.run_command(env={"FAKE_RFMOD_FAIL": "pack"})
        self.assertEqual(status, 1)
        self.assertIn("fake failure", stdout)
        self.assertIn("rfmod pack failed", stderr)
        self.assertFalse((self.fake.out / CHECKSUMS).exists())

    def test_failed_check_stops_the_run(self):
        status, _, stderr = self.run_command(env={"FAKE_RFMOD_FAIL": "test"})
        self.assertEqual(status, 1)
        self.assertIn("upload checks failed for", stderr)
        self.assertIn("(exit 3)", stderr)

    def test_check_without_the_pass_line_fails(self):
        status, _, stderr = self.run_command(env={"FAKE_RFMOD_QUIET": "1"})
        self.assertEqual(status, 1)
        self.assertIn("(exit 0)", stderr)

    def test_broken_manifest_fails(self):
        self.fake.stage("broken", {"version": "1.0.0"})
        status, _, stderr = self.run_command()
        self.assertEqual(status, 1)
        self.assertIn("cannot read id", stderr)

    def test_missing_key_is_a_usage_error(self):
        argv = self.fake.argv()
        argv[1] = str(self.fake.root / "absent.secret")
        status, _, stderr = self.run_command(argv)
        self.assertEqual(status, 2)
        self.assertIn("is not an existing file", stderr)

    def test_missing_build_is_a_usage_error(self):
        argv = self.fake.argv()
        argv[0] = str(self.fake.root / "absent")
        status, _, stderr = self.run_command(argv)
        self.assertEqual(status, 2)
        self.assertIn("is not an existing folder", stderr)

    def test_main_exits_with_the_status(self):
        argv = ["pack_modules.py", str(self.fake.root / "absent"), "k", "p", "o"]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as raised:
            runpy.run_path(pack_modules.__file__, run_name="__main__")
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
