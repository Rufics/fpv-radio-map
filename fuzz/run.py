#!/usr/bin/env python3
"""Fuzzes every target in the fuzz build for a while, several at once.

    FUZZ_SECONDS=300 fuzz/run.py

Settings come from the environment: FUZZ_SECONDS is the time per target (default 60), FUZZ_MERGE=1
minimises each target's corpus afterwards, which keeps the CI cache small, and FUZZ_TARGETS names
the targets to run, separated by spaces (default: all of them).

The fuzz build is build-fuzz/ at the repository root (docs/dev/fuzzing.md). New inputs go to
build-fuzz/fuzz-work/corpus/<target>, crashes to build-fuzz/fuzz-work/crashes/<target>, logs to
build-fuzz/fuzz-work/<target>.log. The checked-in seeds under fuzz/corpus are read, never written.
Exits 1 when any target crashed and lists the inputs that did it.

The script takes no command-line arguments and its paths are fixed, so nothing passed to it can
point it at other files or reach the fuzzer's command line.
"""

import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parent
BUILD = SRC.parent / "build-fuzz"
WORK = BUILD / "fuzz-work"

# Extra libFuzzer flags per target: dictionaries, and room for the long inputs that found bugs.
OPTIONS = {
    "semver": ["-dict=" + str(SRC / "dict/semver.dict")],
    "manifest": ["-dict=" + str(SRC / "dict/json.dict"), "-max_len=65536"],
    "mission_schema": ["-dict=" + str(SRC / "dict/json.dict"), "-max_len=65536"],
    "weather": ["-dict=" + str(SRC / "dict/json.dict")],
    "signed_package": ["-dict=" + str(SRC / "dict/json.dict")],
    "iso_time": ["-max_len=65536"],
    "package": ["-max_len=65536"],
}


def fuzz_one(binary: Path, target: str, seconds: int, merge: bool) -> bool:
    corpus = WORK / "corpus" / target
    crashes = WORK / "crashes" / target
    corpus.mkdir(parents=True, exist_ok=True)
    crashes.mkdir(parents=True, exist_ok=True)
    log = WORK / f"{target}.log"
    flags = OPTIONS.get(target, [])
    with log.open("w") as out:
        rc = subprocess.run(
            [str(binary), *flags, f"-max_total_time={seconds}", "-timeout=25", "-rss_limit_mb=2560",
             "-print_final_stats=1", f"-artifact_prefix={crashes}/", str(corpus), str(SRC / "corpus" / target)],
            stdout=out, stderr=subprocess.STDOUT).returncode
    text = log.read_text(errors="replace")
    if rc != 0:
        print(f"{target}: FAILED, see {log}\n" + "\n".join(text.splitlines()[-60:]), flush=True)
        return False
    runs = re.search(r"stat::number_of_executed_units:\s*(\d+)", text)
    print(f"{target}: ok ({runs.group(1) if runs else '?'} runs)", flush=True)
    if merge:
        merged = corpus.with_name(target + ".merged")
        shutil.rmtree(merged, ignore_errors=True)
        merged.mkdir()
        with (WORK / f"{target}.merge.log").open("w") as out:
            subprocess.run([str(binary), *flags, "-merge=1", str(merged), str(corpus)],
                           stdout=out, stderr=subprocess.STDOUT, check=True)
        shutil.rmtree(corpus)
        merged.rename(corpus)
    return True


def setting_seconds() -> int:
    text = os.environ.get("FUZZ_SECONDS", "60")
    if not re.fullmatch(r"[0-9]{1,6}", text) or int(text) == 0:
        sys.exit(f"FUZZ_SECONDS must be a whole number of seconds from 1 to 999999, not {text!r}")
    return int(text)


def main() -> int:
    if len(sys.argv) > 1:
        sys.exit(__doc__)
    seconds = setting_seconds()
    merge = os.environ.get("FUZZ_MERGE") == "1"
    wanted = os.environ.get("FUZZ_TARGETS", "").split()

    binaries = {p.name.removesuffix("_fuzzer"): p for p in sorted((BUILD / "fuzz").glob("*_fuzzer"))}
    if not binaries:
        sys.exit(f"no fuzz targets in {BUILD}; build them first (docs/dev/fuzzing.md)")
    unknown = sorted(set(wanted) - set(binaries))
    if unknown:
        sys.exit("no such fuzz target: " + ", ".join(unknown))
    # Names come from the build, never from the settings.
    targets = [name for name in binaries if not wanted or name in wanted]

    WORK.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        results = list(pool.map(lambda t: fuzz_one(binaries[t], t, seconds, merge), targets))
    if all(results):
        return 0
    print("Crashing inputs:")
    for f in sorted((WORK / "crashes").rglob("*")):
        if f.is_file():
            print(f"  {f}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
