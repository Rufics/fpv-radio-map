#!/usr/bin/env python3
"""Packs and signs every FPV module of a finished build, then runs the upload checks and the
self-test of each package against the given owner public key.

    python3 -I tools/ci/pack_modules.py BUILD_DIR OWNER.secret OWNER.pub OUT_DIR

Every staged module (BUILD_DIR/modules/fpv/*/stage) is packed first, because the checks need the
packages of the modules it requires. Each package is then checked with `rfmod test`, with its
required modules installed first, dependencies before dependents, as an admin would install them.
OUT_DIR ends up with the packages and a SHA256SUMS file. The exit code is 1 when a module cannot
be packed or fails its checks, 2 on a usage error, 0 otherwise.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

PACKAGE_SUFFIX = ".rfmod"
PASSED_MARKER = "All checks passed"
CHECKSUMS = "SHA256SUMS"
RFMOD_TIMEOUT_SECONDS = 600


class PackError(Exception):
    """A module could not be packed or failed its upload checks."""


class StagedModule:
    """One staged module: its folder and the id, version and requires of its manifest."""

    def __init__(self, stage, manifest):
        self.stage = stage
        self.id = self._text(manifest, "id", stage)
        self.version = self._text(manifest, "version", stage)
        self.requires = [self._text(entry, "id", stage) for entry in manifest.get("requires", [])]

    @staticmethod
    def _text(manifest, key, stage):
        value = manifest.get(key)
        if not isinstance(value, str) or not value:
            raise PackError(f"cannot read {key} in {stage / 'manifest.json'}")
        return value

    @classmethod
    def load(cls, stage):
        manifest = json.loads((stage / "manifest.json").read_text(encoding="utf-8"))
        return cls(stage, manifest)

    @property
    def package_name(self):
        return f"{self.id}-{self.version}{PACKAGE_SUFFIX}"


class ModuleCatalog:
    """The staged modules of a build, by id, and the install order of their requirements."""

    def __init__(self, modules):
        self._modules = {module.id: module for module in modules}

    @classmethod
    def from_build(cls, build):
        modules = build / "modules" / "fpv"
        stages = sorted(modules.glob("*/stage"))
        if not stages:
            raise PackError(f"no staged modules in {modules}")
        return cls(StagedModule.load(stage) for stage in stages)

    def __iter__(self):
        return iter(self._modules.values())

    def get(self, module_id):
        module = self._modules.get(module_id)
        if module is None:
            raise PackError(f"required module {module_id} is not staged in this build")
        return module

    def dependencies_of(self, module):
        """The transitive requires of the module, each after the modules it requires itself."""
        order = []
        self._visit(module, order, set())
        return order

    def _visit(self, module, order, seen):
        for required_id in module.requires:
            if required_id in seen:
                continue
            seen.add(required_id)
            required = self.get(required_id)
            self._visit(required, order, seen)
            order.append(required)


class RfmodTool:
    """The rfmod command-line tool of the build. Every path is passed absolute, so none can be
    read as an option."""

    def __init__(self, executable):
        self._executable = executable

    def _run(self, args):
        return subprocess.run([str(self._executable), *map(str, args)], capture_output=True, text=True,
                              check=False, timeout=RFMOD_TIMEOUT_SECONDS)

    def pack(self, stage, key, package):
        result = self._run(["pack", stage, "--key", key, "-o", package])
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            raise PackError(f"rfmod pack failed for {stage} (exit {result.returncode})")

    def test(self, package, trust, dependencies):
        """Runs the upload checks; returns the exit code and the combined output."""
        with_args = [arg for dependency in dependencies for arg in ("--with", dependency)]
        result = self._run(["test", package, "--trust", trust, *with_args])
        return result.returncode, result.stdout + result.stderr


class ModulePublisher:
    """Packs, checks and lists the checksums of every module of the catalog in the output folder."""

    def __init__(self, catalog, rfmod, out):
        self._catalog = catalog
        self._rfmod = rfmod
        self._out = out

    def _package(self, module):
        return self._out / module.package_name

    def pack_all(self, key):
        self._out.mkdir(parents=True, exist_ok=True)
        for module in self._catalog:
            self._rfmod.pack(module.stage, key, self._package(module))

    def check_all(self, trust):
        for module in self._catalog:
            package = self._package(module)
            dependencies = [self._package(required) for required in self._catalog.dependencies_of(module)]
            status, output = self._rfmod.test(package, trust, dependencies)
            sys.stdout.write(output)
            if status != 0 or PASSED_MARKER not in output:
                raise PackError(f"upload checks failed for {package} (exit {status})")

    def write_checksums(self):
        lines = [f"{self._sha256(package)}  {package.name}\n"
                 for package in sorted(self._out.glob(f"*{PACKAGE_SUFFIX}"))]
        (self._out / CHECKSUMS).write_text("".join(lines), encoding="utf-8")

    @staticmethod
    def _sha256(path):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
        return digest.hexdigest()


class Command:
    """The command line: checks the arguments and runs pack, check and checksums in turn."""

    @staticmethod
    def _parser():
        parser = argparse.ArgumentParser(description="Pack, sign and check the FPV modules of a build.")
        parser.add_argument("build", type=Path, help="finished CMake build folder")
        parser.add_argument("key", type=Path, help="owner secret key that signs the packages")
        parser.add_argument("trust", type=Path, help="owner public key the packages are checked against")
        parser.add_argument("out", type=Path, help="folder for the packages and SHA256SUMS")
        return parser

    @staticmethod
    def _existing(path, kind):
        resolved = path.resolve()
        if not (resolved.is_dir() if kind == "folder" else resolved.is_file()):
            raise ValueError(f"{path} is not an existing {kind}")
        return resolved

    def run(self, argv):
        args = self._parser().parse_args(argv)
        try:
            build = self._existing(args.build, "folder")
            key = self._existing(args.key, "file")
            trust = self._existing(args.trust, "file")
            rfmod = RfmodTool(self._existing(build / "platform" / "rfmod" / "rfmod", "file"))
        except ValueError as error:
            print(f"pack_modules: {error}", file=sys.stderr)
            return 2
        try:
            publisher = ModulePublisher(ModuleCatalog.from_build(build), rfmod, args.out.resolve())
            publisher.pack_all(key)
            publisher.check_all(trust)
            publisher.write_checksums()
        except (PackError, OSError, ValueError, subprocess.TimeoutExpired) as error:
            print(f"pack_modules: {error}", file=sys.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(Command().run(sys.argv[1:]))
