#!/usr/bin/env python3
"""Command line of the perf toolchain.

    python3 -I tools/perf/perf.py [--catalog-dir DIR] list [--kind KIND]
    python3 -I tools/perf/perf.py [--catalog-dir DIR] catalog check
    python3 -I tools/perf/perf.py [--catalog-dir DIR] catalog lock
    python3 -I tools/perf/perf.py [--catalog-dir DIR] run --suite micro [--only ID] [--repetitions N]
        [--build-dir DIR] [--results-dir DIR] [--window N] [--no-gate] [--no-report]
    python3 -I tools/perf/perf.py [--catalog-dir DIR] run --scenario ID --dataset DIR [--port N]
        [--server-image IMAGE] [--no-build] [--keep-stand] [--no-monitoring] [--prometheus-port N]
        [--grafana-port N] [--results-dir DIR] [--window N] [--no-gate] [--no-report]
    python3 -I tools/perf/perf.py [--catalog-dir DIR] report RUN_DIR [--reporter ID] [--results-dir DIR]

The catalog is one folder (default: tools/perf) with fixtures/, personas/, scenarios/ and
extensions/{collectors,reporters,benchmarks,dashboards}/, one subfolder with a manifest.json per item.
`list` shows what was found. `catalog check` validates every manifest against its schema, checks
that referenced items and files exist, and that catalog.lock.json matches the catalog, so no item
changed without a new version. `catalog lock` rewrites catalog.lock.json after versions were
raised. `run` builds and runs a suite or an HTTP scenario on a
Docker stand, records it in the history and compares it with the baseline (README.md, "Micro
suite" and "HTTP scenarios") and writes the reports; `report` writes them again for a run
folder (README.md, "Reports").

The exit code is 0 on success, 1 when the catalog has problems, a run fails or a gated measurement
regressed, 2 on a usage error.
"""
import sys
from pathlib import Path

if __name__ == "__main__":
    # Run with -I, Python does not put this folder on sys.path; perfkit lives next to this file.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from perfkit.cli import PerfCli

    sys.exit(PerfCli().main())
