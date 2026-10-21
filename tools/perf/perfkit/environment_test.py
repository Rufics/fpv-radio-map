import subprocess
import tempfile
import unittest
from pathlib import Path

from perfkit.environment import BuildProbe, Environment, GitProbe, HostProbe, Probe


class FolderTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


class HostProbeTest(FolderTestCase):
    def test_reads_hardware_and_limits(self):
        self.write("proc/cpuinfo", "processor\t: 0\nmodel name\t: AMD EPYC 7B13\n")
        self.write("proc/meminfo", "MemTotal:       16477352 kB\nMemFree: 1 kB\n")
        self.write("etc/os-release", 'NAME="Ubuntu"\nPRETTY_NAME="Ubuntu 24.04.5 LTS"\n')
        self.write("sys/fs/cgroup/cpu.max", "400000 100000\n")
        self.write("sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance\n")
        host = HostProbe(self.root / "proc", self.root / "sys", self.root / "etc").collect()
        self.assertEqual(host["cpuModel"], "AMD EPYC 7B13")
        self.assertEqual((host["memoryKiB"], host["memoryGiB"]), (16477352, 16))
        self.assertEqual(host["os"], "Ubuntu 24.04.5 LTS")
        self.assertEqual((host["cgroupCpuMax"], host["cgroupMemoryMax"]), ("400000 100000", ""))
        self.assertEqual(host["cpuGovernor"], "performance")

    def test_missing_files_leave_fields_empty(self):
        host = HostProbe(self.root / "proc", self.root / "sys", self.root / "etc").collect()
        self.assertEqual((host["cpuModel"], host["memoryKiB"], host["memoryGiB"]), ("", None, None))


class BuildProbeTest(FolderTestCase):
    def test_reads_cache_and_compiler(self):
        self.write("CMakeCache.txt", "//comment\nCMAKE_BUILD_TYPE:STRING=Release\n"
                   "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\nUNRELATED:BOOL=ON\n")
        self.write("CMakeFiles/3.28.3/CMakeCXXCompiler.cmake", 'set(CMAKE_CXX_COMPILER_VERSION "13.3.0")\n')
        build = BuildProbe(self.root).collect()
        self.assertEqual(build, {"CMAKE_BUILD_TYPE": "Release", "CMAKE_CXX_COMPILER": "/usr/bin/c++",
                                 "compilerVersion": "13.3.0"})

    def test_no_build_folder(self):
        self.assertEqual(BuildProbe(self.root / "nope").collect(), {"compilerVersion": ""})


class GitProbeTest(FolderTestCase):
    def test_reads_a_repository(self):
        def git(*args):
            subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)
        git("init", "-q", "-b", "main")
        self.write("a.txt", "a")
        git("add", "a.txt")
        git("-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "-m", "a")
        found = GitProbe(self.root).collect()
        self.assertEqual((len(found["commit"]), found["branch"], found["dirty"]), (40, "main", False))
        self.write("a.txt", "b")
        self.assertTrue(GitProbe(self.root).collect()["dirty"])

    def test_not_a_repository(self):
        self.assertEqual(GitProbe(self.root).collect(), {"commit": "", "branch": "", "dirty": False})


class StaticProbe(Probe):
    def __init__(self, name, data):
        self.name = name
        self._data = data

    def collect(self):
        return self._data


class EnvironmentTest(unittest.TestCase):
    def test_snapshot_and_fingerprint(self):
        host = {"cpuModel": "X", "logicalCpus": 4, "memoryGiB": 16, "kernel": "6.1"}
        snapshot = Environment([StaticProbe("host", host), StaticProbe("git", {"commit": "abc"})]).snapshot()
        self.assertEqual(snapshot["git"], {"commit": "abc"})
        fingerprint = Environment.host_fingerprint(snapshot)
        self.assertRegex(fingerprint, r"^[0-9a-f]{12}$")
        # The kernel and other non-hardware fields do not split the history.
        self.assertEqual(Environment.host_fingerprint({"host": {**host, "kernel": "6.2"}}), fingerprint)
        self.assertNotEqual(Environment.host_fingerprint({"host": {**host, "logicalCpus": 8}}), fingerprint)

    def test_base_probe_is_abstract(self):
        probe = Probe()
        with self.assertRaises(NotImplementedError):
            probe.collect()


if __name__ == "__main__":
    unittest.main()
