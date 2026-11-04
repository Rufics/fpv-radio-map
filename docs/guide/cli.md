# Command-line client (rfmap)

`rfmap` is the command-line client of the server's HTTP API, for scripts, CI and other services.
It runs the same calculations the web app does: link paths, whole routes and coverage grids, on
the server's DEM, library and modules. It also reads the server's data: the equipment library,
the forecast, offline region packages and the map configuration.

- Results go to stdout as JSON, so you can pipe them into `jq` or save them as artifacts.
- Errors, summaries and hints go to stderr.
- The exit status tells a failed check from a server that refused the request or could not be
  reached, so CI can gate on the result.

## Install

- **Release:** download `rfmap-linux-x86_64` from the
  [releases page](https://github.com/Rufics/fpv-radio-map/releases) and put it on the `PATH`
  as `rfmap`.
- **Server image:** the image has it as `/usr/bin/rfmap`. For example:
  `docker compose exec rfmap-server rfmap --server http://127.0.0.1:8080 health`.
- **From source:** `cmake --build build --target rfmap`. The binary is `build/cli/rfmap`. The
  build needs the same packages as the server (see [CONTRIBUTING](../../CONTRIBUTING.md#checks)).

## Connect

```sh
export RFMAP_SERVER=https://maps.example.org   # the API is at <server>/api/v1
export RFMAP_TOKEN=rfmap_svc_...               # a service token, for calculations
rfmap health
```

- `--server URL` sets the server for one command, and `RFMAP_SERVER` sets it for all of them. A
  URL with a path works: `https://example.org/maps` reaches `https://example.org/maps/api/v1`.
- **Only `https://`.** `https://` servers are checked against the system's CA certificates.
  Plain `http://` works only for a server on the same machine (`localhost`, `127.0.0.1`,
  `[::1]`), as inside the server image, so a token never crosses a network unencrypted. Any
  other `http://` server exits with 64.
- `RFMAP_TOKEN` sets the token. You can also use `--token-file FILE` or `RFMAP_TOKEN_FILE`.
  `--token TOKEN` works too, but process lists show it.
- **Calculations need a service token:** `calc path`, `calc route`, `calc coverage` and
  `flightlog decode`. The rest of the data API needs no token.

### Service tokens

The server operator creates a service token for each service that calculates, and the server
stores only its SHA-256.

1. Make the token on any machine. No server is needed:

   ```sh
   rfmap token new --name nightly-ci
   # {"name": "nightly-ci", "token": "rfmap_svc_...", "sha256": "4f0c..."}
   ```

2. Give the service the token, for example as a CI secret named `RFMAP_TOKEN`.
3. Add `{"name": "nightly-ci", "sha256": "4f0c..."}` to the server's service tokens:
   - with the Docker stack, in `deploy/.env`:
     `RFMAP_SERVICE_TOKENS=[{"name":"nightly-ci","sha256":"4f0c..."}]`;
   - otherwise in `services.tokens` of the server config
     ([Configuration](../admin/deploy.md#3-configuration)), or from the environment as
     `RFMAP__SERVICES__TOKENS`.
4. Restart the server (`docker compose up -d` picks up a changed `.env`).

A service token can run calculations and their coverage jobs. It reaches no mission: the mission
API answers it with 403, and it cannot see other callers' coverage jobs.

- **Rate limits.** A token's calculations and reads count in the `service` rate class, per
  token, whatever address it calls from (`rateLimits.service`, 300 a minute by default).
- **Revoke a token:** remove its entry from `services.tokens` and restart the server.
- **Rotate a token:** add the new entry, move the service to the new token, then remove the old
  entry.

## Commands

```
rfmap health                                     exit 1 unless the server says ok
rfmap version                                    this client's version and the server's
rfmap calc path --from LAT,LON,AGL --to LAT,LON,AGL LINK [--time UTC] [--modules FILE]
rfmap calc route MISSION.json
rfmap calc coverage MISSION.json [--out-dir DIR] [--no-wait] [--poll S] [--wait S]
rfmap elevation --from LAT,LON --to LAT,LON
rfmap library list KIND | get KIND ID | history KIND ID
rfmap weather point --lat LAT --lon LON [--time UTC] | weather horizon
rfmap modules | map
rfmap regions list | get ID | download ID --out FILE.zip
rfmap flightlog decode FILE [--name NAME]
rfmap api GET|POST|PUT|DELETE PATH [--data FILE]
rfmap token new [--name NAME]
```

`rfmap --help` lists every option. A file of `-` reads stdin. `--output FILE` writes the result
to a file instead of stdout, and `--compact` prints it on one line.

### One link

`LINK` is one of these:

- `--link FILE`: a linkSetup from
  [the mission schema](../../schemas/mission.schema.json), for the link `--kind` names
  (`control` by default).
- `--links FILE`: both links as `{control, video}`, with `--evaluate both|control|video`.
- The link written out in flags:

  ```sh
  rfmap calc path --from 48.50,35.20,1.5 --to 48.52,35.22,120 \
      --radio elrs-915-50hz --tx-power 27 --ground-antenna dipole-ground --air-antenna dipole-air \
      --required-margin 10 --summary
  ```

Heights are above ground. Radios and antennas are library ids, and `rfmap library list antennas`
shows them.

### A whole route

`calc route` takes a mission file, the same document the web app saves (`mission/v1`), and
answers every leg with its worst sample per link.

```sh
rfmap calc route mission.json --summary > result.json
```

`--summary` prints one line per leg on stderr:

```
leg 1 (Pilot -> WP1)                 3.70 km  ok            control +41.8 dB  video +12.3 dB
leg 2 (WP1 -> WP2)                   3.30 km  blocked       control -5.2 dB
```

### Coverage

`calc coverage` starts a coverage job for the mission's `coverage` settings and waits for it.
It polls every `--poll` seconds (2 by default) and gives up after `--wait` seconds (1800 by
default), cancelling the job.

- Without `--out-dir`, the result holds the job and every finished grid.
- `--out-dir DIR` writes `DIR/control.json` and `DIR/video.json` instead.
- `--no-wait` prints the started job and returns. Poll it later with `rfmap api GET /jobs/<id>`.

### Offline region packages

```sh
rfmap regions download east --out east.zip
```

This downloads the current package of a region and checks it against the SHA-256 the catalog
lists. A package that does not match exits with 1.

### Anything else

`rfmap api` sends any other call to the API, with your token: for example
`rfmap api GET /admin/cache` with the admin token as `RFMAP_TOKEN`.

## Checks for CI

`calc path` and `calc route` can fail the run when the result falls short. The result is printed
either way.

| Option | Fails when |
|---|---|
| `--fail-on blocked` | a leg is `blocked` or `missing-data`; for `calc path`, a link that is not reachable |
| `--fail-on warn` | a leg is anything but `ok` |
| `--min-margin DB` | an evaluated link's margin at its worst sample is below `DB` |

Each check that fails prints `check failed: ...` on stderr.

```yaml
# .github/workflows/link-check.yml
on: [pull_request]
jobs:
  links:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v5
      - name: Install rfmap
        run: |
          curl -fsSL -o rfmap https://github.com/Rufics/fpv-radio-map/releases/latest/download/rfmap-linux-x86_64
          chmod +x rfmap
      - name: Every planned route keeps 6 dB on both links
        env:
          RFMAP_SERVER: https://maps.example.org
          RFMAP_TOKEN: ${{ secrets.RFMAP_TOKEN }}
        run: |
          mkdir -p results
          for m in missions/*.json; do
            ./rfmap calc route "$m" --fail-on blocked --min-margin 6 --summary --output "results/$(basename "$m")"
          done
```

## Exit status

| Status | Meaning |
|---|---|
| 0 | Done, and every check passed |
| 1 | A check failed: `--fail-on` or `--min-margin`, health not ok, a coverage job failed or ran out of time, a package that does not match its SHA-256 |
| 2 | The server answered an error (4xx or 5xx). stderr has its message and the schema issues |
| 3 | The server could not be reached: refused, DNS, TLS or timeout |
| 64 | The command line is wrong, or the server URL is not `https://` (see [Connect](#connect)) |
| 66 | An input file cannot be read, or is not JSON |
| 73 | An output file cannot be written |

### Retries

- **Busy server.** On 429 or 503, `rfmap` waits for the time the server asks for (at most 60
  s), then tries again.
- **Connection failures.** Reads are also retried after a failed connection, waiting 1, 2, 4 s,
  and so on.
- **No repeated calculations.** A calculation is not repeated after a failed connection, because
  it may already have run.
- `--retries N` sets how many times to try again (3 by default, 0 to 10).
- `--timeout SECONDS` limits each request (300 by default).
