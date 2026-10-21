import io
import sys
import tempfile
import unittest
from pathlib import Path

from perfkit.commands import CommandRunner
from perfkit.errors import PerfError


class CommandRunnerTest(unittest.TestCase):
    def setUp(self):
        self.log = io.StringIO()
        self.runner = CommandRunner(self.log)

    def test_run_and_capture(self):
        self.assertEqual(self.runner.run([sys.executable, "-c", "raise SystemExit(3)"], allowed=(0, 3)), 3)
        self.assertIn("+ ", self.log.getvalue())
        self.assertEqual(self.runner.capture([sys.executable, "-c", "print('hi')"]), "hi\n")

    def test_output_into_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out.txt"
            self.runner.run([sys.executable, "-c", "print('to file')"], stdout_path=out)
            self.assertEqual(out.read_text(encoding="utf-8").strip(), "to file")
            self.assertIn(f"> {out}", self.log.getvalue())

    def test_failures(self):
        failing = [sys.executable, "-c", "import sys; sys.stderr.write('bad'); raise SystemExit(2)"]
        with self.assertRaisesRegex(PerfError, "failed with exit code 2$"):
            self.runner.run(failing)
        with self.assertRaisesRegex(PerfError, "failed with exit code 2: bad"):
            self.runner.capture(failing)
        with self.assertRaisesRegex(PerfError, "is not installed"):
            self.runner.capture(["/nonexistent/tool"])

    def test_a_hung_command_ends(self):
        sleeping = [sys.executable, "-c", "import time; time.sleep(30)"]
        with self.assertRaisesRegex(PerfError, "did not finish in 0.2 s"):
            self.runner.run(sleeping, timeout=0.2)
        with self.assertRaisesRegex(PerfError, "did not finish in 0.2 s"):
            self.runner.capture(sleeping, timeout=0.2)


if __name__ == "__main__":
    unittest.main()
