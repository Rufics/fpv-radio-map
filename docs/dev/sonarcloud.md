# SonarQube Cloud

The code is analysed by [SonarQube Cloud](https://sonarcloud.io/project/overview?id=rufics_fpv-radio-map)
on every pull request from a branch of this repository and on every push to `main`. The analysis
runs in GitHub Actions (`.github/workflows/sonar.yml`), not as SonarQube Cloud's automatic
analysis, because only a CI run can hand it the C++ compile database and the coverage reports.

## What is analysed

The settings are in [`sonar-project.properties`](../../sonar-project.properties):

- **Sources and tests.** Everything under the repository root; files in a `tests/` folder,
  `*_test.py` and `*.spec.ts` are test code. Dependencies, build output, generated code
  (`*.generated.ts`) and the fuzz targets are left out.
- **C++.** An instrumented Debug build (`-DRFMAP_COVERAGE=ON`) provides
  `build-sonar/compile_commands.json` for the analyser and the coverage, collected with `gcovr`
  using the same filters as the CI report.
- **Python.** Analysed as Python 3.12, the version on Ubuntu 24.04. Coverage comes from the
  `tools/ci` unit tests.
- **TypeScript.** Coverage of the `ui-kit` and `fpv-app` unit tests, merged into one LCOV report by
  `tools/ci/sonar_lcov.py`, which makes the paths relative to the repository root.

Code that no CI test can reach is not counted for coverage: the example module, the fuzz harness,
the sandboxed module worker, the operator tools that run in their own images and the deploy files.

## One-time setup

1. In SonarQube Cloud, open the project, then **Administration → Analysis Method** and turn
   **Automatic Analysis** off. A CI analysis fails while it is on.
2. On the same page choose **GitHub Actions** and copy the token it shows (or create one under
   **My Account → Security**).
3. In GitHub, add it as the repository secret `SONAR_TOKEN`
   (**Settings → Secrets and variables → Actions → New repository secret**).

Until the secret exists the workflow skips the analysis instead of failing.

Pull requests from forks and from Dependabot get no secrets, so they are not analysed; the CI
workflow still tests them.

## Running it locally

The scanner is not needed to work on the code: the CI report and `clang-tidy` cover the same
limits (see [Contributing](../../CONTRIBUTING.md#code-style)). To reproduce an analysis, run the
steps of `.github/workflows/sonar.yml` and then `sonar-scanner` with a personal token in
`SONAR_TOKEN`.
