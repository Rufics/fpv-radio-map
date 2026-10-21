# Contributing

Thank you for helping. Code, verified equipment data with sources, modules, documentation and bug reports are all
welcome. By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Workflow

1. For anything bigger than a small fix, open an issue first so we can agree on the approach.
2. Fork the repository and create a branch from `main`.
3. Make the change with tests. Radio maths changes need a test against an exact or published value, not a
   snapshot of the current output.
4. Open a pull request with a Conventional Commits title. CI must be green and a maintainer must approve.
5. New features that fit as a module are better as a module: see [the developer guide](docs/modules/developer-guide.md).

## Language

All documentation, UI text, code comments, commit messages, issues and pull requests are in English.

## Code style

C++ is written object-oriented: behaviour lives in classes with one clear responsibility, and
state that several steps share is a class's members, not parameters passed from function to
function. Keep the cognitive load low, within the limits SonarCloud enforces on every pull request:

- cognitive complexity at most 25 per function;
- at most 3 nested `if`, `for`, `do`, `while` and `switch` statements.

A function that grows past them is split into named steps, usually methods of a small class.
Locally, `clang-tidy` reports the complexity of a build configured with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`:

```
clang-tidy -p build --checks='-*,readability-function-cognitive-complexity' \
  --config='{CheckOptions: {readability-function-cognitive-complexity.Threshold: 25}}' <files>
```

## Documentation

Documentation lives in `docs/`. After every change on `main` it is published to the
[wiki](https://github.com/Rufics/fpv-radio-map/wiki) by `tools/wiki/publish.py`: each file becomes
the page named after its first heading, `docs/README.md` becomes Home and `docs/_Sidebar.md` the
sidebar. Edit `docs/`, not the wiki; wiki edits are overwritten. Link to other docs with relative
paths, and the publisher turns them into wiki links.

## Commit messages: Conventional Commits

Releases are made by [semantic-release](https://semantic-release.gitbook.io) from the commit
history, so commit messages follow [Conventional Commits 1.0](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>(<scope>): <what changes, in the imperative>

<optional body: why>

<optional footer: BREAKING CHANGE: ..., Refs #12>
```

| Type | Use it for | Version bump |
|---|---|---|
| `feat` | a new capability a user or module author can see | minor (0.**2**.0) |
| `fix` | a bug fix | patch (0.1.**1**) |
| `perf` | faster, same results | patch |
| `feat!`, `fix!` or a `BREAKING CHANGE:` footer | an incompatible change: API, mission or library schema, module ABI or host contract | major |
| `docs`, `test`, `refactor`, `build`, `ci`, `chore`, `style`, `revert` | everything else | none |

Scopes: `rf-core`, `rf-ext`, `rfmod`, `fpv`, `server`, `web`, `tiles`, `deploy`, `data`,
`schemas`, `docs`, `ci`, `perf`, `deps`. The scope is optional.

Examples:

```
feat(fpv): suggest a relay point inside the drawn area
fix(rf-core): use the exact J(ν) minimum for ν < 0
feat(server)!: require If-Match on mission updates

BREAKING CHANGE: PUT /missions/{id} without If-Match now answers 428.
```

A breaking change raises the host version's major number. Module manifests declare a host range
(`"host": ">=0.1.0 <1.0.0"`), so a major release also needs the modules' ranges updated.

Pull requests are **squash-merged**: the pull request title becomes the commit on `main`, and CI
checks that title with commitlint. Commits inside a pull request may be informal.

To check messages locally:

```
cd tools/ci && npm ci --ignore-scripts
git config core.hooksPath .githooks      # once per clone
```

## Checks

Every pull request runs the CI workflow (`.github/workflows/ci.yml`) and gets a CI report comment:

- C++: everything under `ctest` on the Release build, including the server integration test against
  PostgreSQL 16 + PostGIS and the signed-package upload checks of every FPV module;
- C++ coverage: the same tests on an instrumented build, reported per area (core, FPV modules,
  platform, server) and for the lines the pull request changes;
- web: both production builds and the unit tests of `ui-kit` and `fpv-app`;
- fuzzing: once the C++ build passed, every fuzz target replays its corpus and is fuzzed for a
  minute (longer every night in `.github/workflows/fuzz.yml`; see [Fuzz testing](docs/dev/fuzzing.md));
- the pull request title against Conventional Commits (its own check, re-run when the title is edited).

A separate workflow (`.github/workflows/sonar.yml`) runs the SonarQube Cloud analysis with C++,
Python and TypeScript coverage; its quality gate shows on the pull request. See
[SonarQube Cloud](docs/dev/sonarcloud.md).

### The CI report

The comment on the pull request is kept up to date on every push. It starts with one line: passed
or failed, the number of tests, the C++ and web line coverage, and how much of the changed lines the
tests run. Below it:

- failed tests, open at the top, when there are any: each folds out to the reason (the failed
  assertion of GoogleTest, the message and stack of Vitest or Python) and a link to the line it
  points at, so you rarely need to open the job log;
- a table of the checks, each linked to its log, with the test counts;
- the test counts per module, folded away: each C++ module (`rf-core`, `rf-ext`, `rfmod`, every
  `fpv.<module>`, `server`, the example module), both web projects and the Python CI tools;
- C++ line and function coverage for core (`rf-core`, `rf-ext`), the FPV modules, the platform
  (`rfmod`) and the server, with a per-component breakdown folded away;
- web line and function coverage for `ui-kit` and `fpv-app` from the Vitest (V8) run, with a
  breakdown per folder folded away;
- the changed lines that no test runs, linked to the exact lines of the commit. This list is the
  one to look at before asking for a review;
- links to the workflow run, whose summary lists every test of the run in a folded table per
  module, and to the browsable HTML coverage reports of C++ and web (kept for 7 days).

Coverage is reported, not enforced: 🟢 is 80% or more, 🟡 60–80%, 🔴 below 60%. Test code, the
example module and the sandboxed module worker are not counted. The same report is in the run
summary of the `Test report` job.

The test details come from the JUnit files of the runs: `ctest --output-junit`, the Vitest junit
reporter and `tools/ci/unittest_junit.py` for the Python tools. A C++ test belongs to the module
named by its first CTest label, so a new test target needs `PROPERTIES LABELS <module>` on its
`gtest_discover_tests()` (or `LABELS` on its `add_test()`); `ctest -L <module>` runs one module
locally.

To see coverage locally:

```
sudo apt-get install -y gcovr
cmake -S . -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRFMAP_COVERAGE=ON
cmake --build build-cov && ctest --test-dir build-cov
gcovr --root . --filter 'core/' --filter 'platform/' --filter 'modules/' --filter 'server/' \
  --exclude '.*/tests/.*' --exclude-directories '.*/_deps$' --exclude-directories '.*/rfmod-worker\.dir$' \
  --html-details build-cov/coverage.html build-cov
```

### Pull requests from forks

Fork pull requests run exactly the same checks before they can be merged, with no secrets and a
read-only token:

- A maintainer approves the workflow runs of an outside contributor's pull request before they
  start. Until then the checks show as waiting; this is expected.
- `ci.yml` runs your code with a read-only token and only uploads its results. The `CI report`
  workflow (`.github/workflows/ci-report.yml`) then runs from `main`, reads those results as data
  and posts the comment. Nothing from a fork runs with write access, and changing either workflow
  in a fork's pull request has no effect on that pull request's report. Pull requests from
  branches of this repository get the comment from a job in `ci.yml` that only posts the
  finished report and runs none of the pull request's code.
- Changes to `.github/`, `tools/ci/`, `deploy/` and the signing keys need a review by a code owner
  (`.github/CODEOWNERS`).

Only the parts a pull request can affect are built: changes to `web/` run the web job, changes to
the C++ tree run the C++ job, and changes to docs alone run neither. A file the rules in `ci.yml`
do not know builds everything. Pushes to `main` that only touch docs skip the image build.

Run the same locally. On Ubuntu 24.04 the build needs:

```
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libgeographiclib-dev nlohmann-json3-dev libarchive-dev libsodium-dev \
  libpqxx-dev libgdal-dev gdal-bin libcpp-httplib-dev libgtest-dev \
  postgresql-16 postgresql-16-postgis-3
```

Then:

```
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
cd web && npm ci --ignore-scripts && npm run build && npm test
```

npm dependencies are always installed with `--ignore-scripts`, so no package runs code at install
time. The native parts the build needs (esbuild and the like) come as prebuilt platform packages.

Every push to `main` runs the CI again, builds the server and web Docker images, starts them with
PostGIS and checks the stack end to end (`deploy/ci/smoke.sh`), then publishes them to GHCR as
`:edge`. The previous `:edge` is deleted, so only `:edge` and the release tags take package storage.
CI test results are kept as workflow artifacts for 7 days.

## Releases

Actions → **Release** → Run workflow, on `main`. Tick *dry run* to only see the next version and its
notes. A release:

1. computes the version from the commits since the last tag;
2. runs the full CI;
3. builds with that version and the owner key, signs every FPV module (`.rfmod`) and runs its
   upload checks and self-test;
4. tags `vX.Y.Z` and publishes the GitHub release with generated notes, the signed modules,
   their `SHA256SUMS` and the `rfmod` CLI;
5. publishes `fpv-radio-map-server` and `fpv-radio-map-web` images as `X.Y.Z`, `X.Y` and `latest`.

It needs the repository variable `RFMAP_OWNER_PUBKEY` (contents of `owner.pub`) and the secret
`RFMAP_OWNER_SECRET_KEY` (contents of `owner.secret`) in the `release` environment, both made with
`rfmod keygen --out owner`. Only the release job enters that environment, from `main`, so no pull
request or other workflow can read the key
([setup](docs/maintainers/repository-settings.md#environments-the-release-signing-key)). A release
never signs with the public development key.

The first release counts from a `v0.1.0` tag that the workflow puts on the first commit when the
repository has no tag yet, so a `feat` gives 0.2.0, not semantic-release's default 1.0.0. That keeps
the host version inside the FPV modules' `<1.0.0` range.
