"""Runs the external tools (cmake, the benchmark binaries) with fixed argument lists, no shell."""
import subprocess  # nosec B404: argument lists only, never a shell
import sys
from pathlib import Path
from typing import TextIO

from perfkit.errors import PerfError


class CommandRunner:
    """Prints each command, runs it and turns a failure into a PerfError.

    What the command prints goes to stderr, so stdout carries only the result table.
    """

    def __init__(self, log: TextIO = sys.stderr):
        self._log = log

    def run(self, args: list[str], cwd: Path | None = None) -> None:
        print("+ " + " ".join(args), file=self._log, flush=True)
        try:
            subprocess.run(args, cwd=cwd, check=True, stdout=sys.stderr)  # nosec B603
        except FileNotFoundError as error:
            raise PerfError(f"{args[0]} is not installed") from error
        except subprocess.CalledProcessError as error:
            raise PerfError(f"{Path(args[0]).name} failed with exit code {error.returncode}") from error
