"""Core of the perf toolchain: the catalog of descriptions and extensions, and its lock file.

Descriptions (fixtures, personas, scenarios) are JSON only; extensions (collectors, reporters,
benchmarks) also carry code. Each item is a folder with a manifest.json. The kinds themselves are
defined by the JSON Schemas in tools/perf/schemas, so a new kind is a new schema file.
"""
