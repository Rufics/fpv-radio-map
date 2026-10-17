# Contributing

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
`schemas`, `docs`, `ci`, `deps`. The scope is optional.

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
cd tools/ci && npm ci
git config core.hooksPath .githooks      # once per clone
```

## Checks

Every pull request runs the CI workflow (`.github/workflows/ci.yml`) and gets a test report comment:

- C++: everything under `ctest`, including the server integration test against PostgreSQL 16 + PostGIS
  and the signed-package upload checks of every FPV module;
- web: both production builds and the unit tests of `ui-kit` and `fpv-app`;
- the pull request title against Conventional Commits.

Run the same locally:

```
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
cd web && npm ci && npm run build && npm test
```

Every push to `main` runs the CI again, builds the server and web Docker images, starts them with
PostGIS and checks the stack end to end (`deploy/ci/smoke.sh`), then publishes them to GHCR as
`:edge` and `:sha-<commit>`.

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
`RFMAP_OWNER_SECRET_KEY` (contents of `owner.secret`), both made with `rfmod keygen --out owner`.
A release never signs with the public development key.
