#!/usr/bin/env python3
"""Fuzzes every target in a fuzz build for a while, several at once.

    fuzz/run.py BUILD_DIR SECONDS_PER_TARGET [--work DIR] [--merge] [TARGET ...]

New inputs go to WORK/corpus/<target> (default WORK: BUILD_DIR/fuzz-work), crashes to
WORK/crashes/<target>, logs to WORK/<target>.log. The checked-in seeds under fuzz/corpus are read,
never written. --merge minimises each target's corpus afterwards, which keeps the CI cache small.
Exits 1 when any target crashed and lists the inputs that did it.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parent

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


def fuzz_one(binary: Path, target: str, seconds: int, work: Path, merge: bool) -> bool:
    corpus = work / "corpus" / target
    crashes = work / "crashes" / target
    corpus.mkdir(parents=True, exist_ok=True)
    crashes.mkdir(parents=True, exist_ok=True)
    log = work / f"{target}.log"
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
        with (work / f"{target}.merge.log").open("w") as out:
            subprocess.run([str(binary), *flags, "-merge=1", str(merged), str(corpus)],
                           stdout=out, stderr=subprocess.STDOUT, check=True)
        shutil.rmtree(corpus)
        merged.rename(corpus)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", type=Path)
    parser.add_argument("seconds", type=int)
    parser.add_argument("targets", nargs="*")
    parser.add_argument("--work", type=Path)
    parser.add_argument("--merge", action="store_true")
    args = parser.parse_args()

    work = (args.work or args.build / "fuzz-work").resolve()
    work.mkdir(parents=True, exist_ok=True)
    binaries = {p.name.removesuffix("_fuzzer"): p for p in sorted((args.build / "fuzz").glob("*_fuzzer"))}
    targets = args.targets or list(binaries)
    unknown = [t for t in targets if t not in binaries]
    if unknown:
        parser.error("no such fuzz target in the build: " + ", ".join(unknown))

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        results = list(pool.map(lambda t: fuzz_one(binaries[t], t, args.seconds, work, args.merge), targets))
    if all(results):
        return 0
    print("Crashing inputs:")
    for f in sorted((work / "crashes").rglob("*")):
        if f.is_file():
            print(f"  {f}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
