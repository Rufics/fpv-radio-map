# SonarQube Cloud

The code is analysed by [SonarQube Cloud](https://sonarcloud.io/project/overview?id=Rufics_fpv-radio-map)
on every pull request from a branch of this repository and on every push to `main`. The analysis
is the `SonarQube Cloud analysis` job of the CI workflow (`.github/workflows/ci.yml`), not
SonarQube Cloud's automatic analysis, because only a CI run can hand it the C++ compile database
and the coverage reports. It starts once the C++ coverage and web jobs of the same run are done and
reads their coverage, so nothing is built or tested twice.

## What is analysed

The settings are in [`sonar-project.properties`](../../sonar-project.properties):

- **Sources and tests.** Everything under the repository root; files in a `tests/` folder,
  `*_test.py` and `*.spec.ts` are test code. Dependencies, build output, generated code
  (`*.generated.ts`) and the fuzz targets are left out.
- **C++.** The `C++ coverage` job builds and tests the instrumented Debug build
  (`-DRFMAP_COVERAGE=ON`) and writes the coverage for SonarQube Cloud with `gcovr`, using the same
  filters as the CI report. The analysis job configures the same build only to get
  `build-sonar/compile_commands.json` and the generated headers the analyser reads, which takes
  seconds.
- **Python.** Analysed as Python 3.14, the version on Ubuntu 26.04 that both CI and the
  tiles-builder image use. Coverage comes from the `tools/ci` unit tests.
- **TypeScript.** Coverage of the `ui-kit` and `fpv-app` unit tests of the `Web build and tests`
  job, merged into one LCOV report by `tools/ci/sonar_lcov.py`, which makes the paths relative to
  the repository root.

Code that no CI test can reach is not counted for coverage: the example module, the fuzz harness,
the sandboxed module worker, the operator tools that run in their own images and the deploy files.

## Pull requests: only the changed parts

A pull request is analysed for the files it adds or modifies only. Before anything is built,
[`tools/ci/sonar_scope.py`](../../tools/ci/sonar_scope.py) compares the pull request with its base
and sorts the changed files into three parts:

| Part | Files | Coverage from |
| --- | --- | --- |
| C++ | C/C++ sources and headers, CMake files, `core/`, `platform/`, `modules/`, `server/`, `cli/`, `examples/`, `fuzz/`, `schemas/`, `data/`, the C++ setup actions | the `C++ coverage` job |
| Python | `*.py` | the `tools/ci`, `tools/perf` and `tools/blackbox` unit tests, run with coverage in the analysis job |
| Web | `web/`, `schemas/` | the `Web build and tests` job |

Only the parts that changed are analysed with coverage. When the CI job that writes a part's
coverage did not run, as for a pull request that changes only the analysis settings, the analysis
job builds and tests that part itself (`.github/actions/sonar-cpp` with `build: "true"`,
`.github/actions/sonar-web`). The scanner gets `sonar.inclusions` set to the
changed files and `sonar.test.inclusions` set to the changed test files, so a web-only pull
request runs neither the C++ build nor the Python tests. The C/C++ analyser reads a header only
through the sources that include it, so a changed header adds every C/C++ file, tests included, to
the scan. For a part that is skipped the scope also switches its coverage report off, and without
the C++ part the C/C++ analyser is turned off (`sonar.c.file.suffixes=-` and so on), since it would
need the compile database of the skipped build.

The job runs for every pull request that changes more than docs, Markdown and the licence, and
its scan always runs, so the quality gate reports on every analysed pull request. It is not part
of the `Test report` gate; the quality gate is the `SonarCloud Code Analysis` check. SonarQube Cloud judges a pull request on its new code only, so leaving unchanged files out
does not change the gate.

The whole repository is analysed instead on every push to `main` that runs the `Main` workflow, and
on a pull request that changes the analysis itself (`sonar-project.properties`, `.github/workflows/ci.yml`,
the `.github/actions/sonar-*` actions, `tools/ci/sonar_scope.py`, `tools/ci/sonar_lcov.py`), changes
more than 500 files, only deletes files, or has a file name that a Sonar pattern cannot carry
(anything but letters, digits and `_./@+-`).

The job reads in three blocks: the scope step, one block per part that fetches its coverage from
the CI jobs or, failing that, builds and tests it (`.github/actions/sonar-cpp`, `sonar-python`,
`sonar-web`), and the scan.

## One-time setup

1. In SonarQube Cloud, open the project, then **Administration → Analysis Method** and turn
   **Automatic Analysis** off. A CI analysis fails while it is on.
2. On the same page choose **GitHub Actions** and copy the token it shows (or create one under
   **My Account → Security**).
3. In GitHub, add it as the repository secret `SONAR_TOKEN`
   (**Settings → Secrets and variables → Actions → New repository secret**).

Until the secret exists the job skips the analysis instead of failing. `.github/workflows/main.yml`
passes the secret to the CI workflow it calls; the release workflow does not, so a release run
does not analyse.

Pull requests from forks and from Dependabot get no secrets, so they are not analysed; the CI
workflow still tests them.

## Running it locally

The scanner is not needed to work on the code: the CI report and `clang-tidy` cover the same
limits (see [Contributing](../../CONTRIBUTING.md#code-style)). To reproduce an analysis, run the
steps of the `sonar` job in `.github/workflows/ci.yml` (with `build: "true"`) and then
`sonar-scanner` with a personal token in
`SONAR_TOKEN`.
