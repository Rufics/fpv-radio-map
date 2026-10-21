"""Runs the external tools (cmake, docker, the benchmark binaries) with fixed argument lists,
never through a shell."""
import subprocess  # nosec B404: argument lists only, never a shell
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import TextIO

from perfkit.errors import PerfError

# Long enough for a full build or the soak scenario; a hung command still ends.
RUN_TIMEOUT_S = 4 * 3600
CAPTURE_TIMEOUT_S = 300


class CommandRunner:
    """Prints each command, runs it and turns a failure into a PerfError.

    What the command prints goes to stderr, so stdout carries only the result table.
    """

    def __init__(self, log: TextIO = sys.stderr):
        self._log = log

    def run(self, args: list[str], cwd: Path | None = None, env: Mapping[str, str] | None = None,
            allowed: tuple[int, ...] = (0,), timeout: float = RUN_TIMEOUT_S) -> int:
        """Runs `args`; returns its exit code, which must be one of `allowed`."""
        print("+ " + " ".join(args), file=self._log, flush=True)
        try:
            done = subprocess.run(args, cwd=cwd, env=env, check=False, stdout=sys.stderr,  # nosec B603
                                  timeout=timeout)
        except FileNotFoundError as error:
            raise PerfError(f"{args[0]} is not installed") from error
        except subprocess.TimeoutExpired as error:
            raise PerfError(f"{Path(args[0]).name} did not finish in {timeout:g} s") from error
        if done.returncode not in allowed:
            raise PerfError(f"{Path(args[0]).name} failed with exit code {done.returncode}")
        return done.returncode

    def capture(self, args: list[str], env: Mapping[str, str] | None = None,
                timeout: float = CAPTURE_TIMEOUT_S) -> str:
        """Runs `args` and returns what it printed; a failure is a PerfError."""
        try:
            done = subprocess.run(args, env=env, check=True, capture_output=True, text=True,  # nosec B603
                                  timeout=timeout)
        except FileNotFoundError as error:
            raise PerfError(f"{args[0]} is not installed") from error
        except subprocess.TimeoutExpired as error:
            raise PerfError(f"{Path(args[0]).name} did not finish in {timeout:g} s") from error
        except subprocess.CalledProcessError as error:
            raise PerfError(f"{Path(args[0]).name} failed with exit code {error.returncode}: "
                            f"{error.stderr.strip()[-500:]}") from error
        return done.stdout
