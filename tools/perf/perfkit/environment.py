"""environment.json: what the numbers of a run depend on besides the code.

Each probe describes one part: the git checkout, the machine (CPU, memory, container limits,
frequency governor) and the C++ build. The HTTP stand adds probes for the server's config and
the database. A probe that cannot read something says so in its section instead of failing the
run. The host fingerprint groups runs made on the same hardware, because only those are
compared with each other.
"""
import hashlib
import json
import os
import platform
import re
import subprocess  # nosec B404: fixed argument lists, no shell
from pathlib import Path
from typing import Any

_GIT_TIMEOUT_S = 10


class Probe:
    """One section of environment.json."""

    name = ""

    def collect(self) -> dict[str, Any]:
        raise NotImplementedError


class GitProbe(Probe):
    name = "git"

    def __init__(self, repo: Path):
        self._repo = repo

    def collect(self) -> dict[str, Any]:
        return {
            "commit": self._git("rev-parse", "HEAD"),
            "branch": self._git("rev-parse", "--abbrev-ref", "HEAD"),
            "dirty": bool(self._git("status", "--porcelain", "--untracked-files=no")),
        }

    def _git(self, *args: str) -> str:
        try:
            done = subprocess.run(["git", "-C", str(self._repo), *args],  # nosec B603 B607
                                  capture_output=True, text=True, check=True, timeout=_GIT_TIMEOUT_S)
        except (OSError, subprocess.SubprocessError):
            return ""
        return done.stdout.strip()


class HostProbe(Probe):
    """The hardware and OS. Paths are parameters so tests can point them at sample files."""

    name = "host"

    def __init__(self, proc: Path = Path("/proc"), sys_fs: Path = Path("/sys"), etc: Path = Path("/etc")):
        self._proc = proc
        self._sys = sys_fs
        self._etc = etc

    def collect(self) -> dict[str, Any]:
        cpuinfo = self._read(self._proc / "cpuinfo")
        memory = self._kib(self._read(self._proc / "meminfo"), "MemTotal")
        return {
            "cpuModel": self._field(cpuinfo, "model name"),
            "logicalCpus": os.cpu_count(),
            "memoryKiB": memory,
            # Rounded: the kernel keeps a slightly different amount on two VMs of one type.
            "memoryGiB": round(memory / 2**20) if memory else None,
            "kernel": platform.release(),
            "os": self._field(self._read(self._etc / "os-release"), "PRETTY_NAME", "=").strip('"'),
            "cpuGovernor": self._read(self._sys / "devices/system/cpu/cpu0/cpufreq/scaling_governor").strip(),
            "cgroupCpuMax": self._read(self._sys / "fs/cgroup/cpu.max").strip(),
            "cgroupMemoryMax": self._read(self._sys / "fs/cgroup/memory.max").strip(),
            "python": platform.python_version(),
        }

    @staticmethod
    def _read(path: Path) -> str:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    @staticmethod
    def _field(text: str, key: str, separator: str = ":") -> str:
        for line in text.splitlines():
            name, found, value = line.partition(separator)
            if found and name.strip() == key:
                return value.strip()
        return ""

    @classmethod
    def _kib(cls, meminfo: str, key: str) -> int | None:
        match = re.match(r"(\d+)", cls._field(meminfo, key))
        return int(match.group(1)) if match else None


class BuildProbe(Probe):
    """How the C++ code was built, from the CMake cache of the build folder."""

    name = "build"
    _KEYS = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE",
             "CMAKE_GENERATOR", "RFMAP_COVERAGE", "RFMAP_BUILD_FUZZERS")

    def __init__(self, build_dir: Path):
        self._build_dir = build_dir

    def collect(self) -> dict[str, Any]:
        cache = self._entries(self._build_dir / "CMakeCache.txt")
        found = {key: cache[key] for key in self._KEYS if key in cache}
        found["compilerVersion"] = self._compiler_version()
        return found

    @staticmethod
    def _entries(path: Path) -> dict[str, str]:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError:
            return {}
        entries = {}
        for line in lines:
            match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z]+=(.*)$", line)
            if match:
                entries[match.group(1)] = match.group(2)
        return entries

    def _compiler_version(self) -> str:
        for path in sorted(self._build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
            match = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', path.read_text(encoding="utf-8"))
            if match:
                return match.group(1)
        return ""


class Environment:
    """Runs the probes; `snapshot` is what goes into environment.json."""

    # What makes two machines give different numbers; the host name is left out on purpose,
    # since a recreated VM of the same type is the same host for comparing.
    _FINGERPRINT = ("cpuModel", "logicalCpus", "memoryGiB", "cgroupCpuMax", "cgroupMemoryMax")

    def __init__(self, probes: list[Probe]):
        self._probes = probes

    def snapshot(self) -> dict[str, Any]:
        return {probe.name: probe.collect() for probe in self._probes}

    @classmethod
    def host_fingerprint(cls, snapshot: dict[str, Any]) -> str:
        host = snapshot.get("host", {})
        text = json.dumps({key: host.get(key) for key in cls._FINGERPRINT}, sort_keys=True)
        return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]
