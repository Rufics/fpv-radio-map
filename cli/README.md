# rfmap

The command-line client of the server's HTTP API, for scripts, CI and other services. It runs
link, route and coverage calculations on the server and reads its data (library, forecast,
offline region packages), printing JSON on stdout. Exit statuses tell a failed check from a
server that refused or could not be reached.

The user guide is [docs/guide/cli.md](../docs/guide/cli.md).

## Layout

| File | What it does |
|---|---|
| `src/arguments.*` | The command line: positional words, `--flag VALUE`, switches, coordinates and numbers |
| `src/api_client.*` | HTTP to `<server>/api/v1`: the token, retries after 429/503 and failed reads, streamed downloads with SHA-256 |
| `src/checks.*` | The CI checks (`--fail-on`, `--min-margin`) and the one-line-per-leg summaries |
| `src/digest.*` | SHA-256 in pieces (downloads, tokens) and hex |
| `src/service_token.*` | `rfmap token new`: a random token and the SHA-256 the server keeps |
| `src/cli.*` | One method per command, the exit statuses |
| `src/main.cpp` | The process: the version, the `RFMAP_*` environment, the streams |

Tests: `tests/cli_test.cpp` runs the commands against a stand-in server in the same process.
The server's integration test (`server/tests/integration_test.cpp`) runs them against a real
server with a service token.
