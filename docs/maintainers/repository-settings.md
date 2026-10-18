# Repository settings for maintainers

The workflows assume these GitHub settings. They cannot be set from a pull request, so a
maintainer with admin rights applies them once in the repository settings. Together they make
every change, from a branch or a fork, pass the same checks and a review before it reaches `main`,
without giving pull request code any secret or write access.

## How pull requests are checked

| Workflow | Trigger | Runs pull request code | Token | Secrets |
|---|---|---|---|---|
| `ci.yml`, fork or Dependabot | `pull_request` | yes | read-only | none |
| `ci.yml`, comment job, branches of this repository | `pull_request` | **no**: posts the finished report as text with `gh` | `pull-requests: write` | none |
| `pr-title.yml` | `pull_request` | yes (commitlint config) | read-only | none |
| `ci-report.yml`, forks | `workflow_run` after CI | **no**: main's code, PR artifacts read as data | `pull-requests: write` | none |
| CodeQL, SonarCloud, Gitar | GitHub apps | analyse only | app's own | app's own |

The rule behind it: a job that runs pull request code never gets write access or secrets, and a
job that has write access never checks out or runs pull request code. `pull_request_target` is
not used.

## Actions → General

- **Actions permissions:** *Allow Rufics, and select non-Rufics, actions and reusable workflows*.
  Tick *Allow actions created by GitHub* and allow `docker/*`. Tick
  *Require actions to be pinned to a full-length commit SHA*; every workflow already pins its
  actions that way, and Dependabot keeps the pins current.
- **Fork pull request workflows from outside collaborators:** *Require approval for all external
  contributors*. A maintainer reads the diff, mainly any change under `.github/`, before the first
  run of every outside pull request.
- **Workflow permissions:** *Read repository contents and packages permissions*. Untick *Allow
  GitHub Actions to create and approve pull requests*. Each workflow asks for the write scopes it
  needs itself.

## Rules → Rulesets

### Branch ruleset `main`

Target: the default branch. Enforcement: *Active*.

- **Bypass list:** *Repository admin*, set to *For pull requests only*. A sole maintainer cannot
  approve their own pull request, so this lets them merge their own after the checks pass, but
  never push straight to `main`.
- **Restrict deletions** and **Block force pushes**.
- **Require linear history** (pull requests are squash-merged).
- **Require a pull request before merging:**
  - Required approvals: 1.
  - Dismiss stale pull request approvals when new commits are pushed.
  - Require review from Code Owners (`.github/CODEOWNERS`).
  - Require approval of the most recent reviewable push.
  - Require conversation resolution before merging.
  - Allowed merge methods: Squash.
- **Require status checks to pass:**
  - Require branches to be up to date before merging.
  - Checks: `Test report` (the CI gate: red when any CI job failed, green when the jobs a change
    does not affect were skipped), `Conventional Commits title`, `SonarCloud Code Analysis`.
- **Require code scanning results:** CodeQL, *Security alerts: High or higher*, *Alerts: Errors*.

`C++ static analysis`, `C++ build and tests`, `C++ coverage` and `Web build and tests` are not
required one by one: they are skipped when a change cannot affect them, and `Test report` already
fails when any of them fails. `C++ static analysis` (clang-tidy, checks in `.clang-tidy`) runs
first: when it fails, the C++ build, tests and coverage are skipped.

### Tag ruleset `v*`

Target: tags matching `v*`. **Restrict updates** and **Restrict deletions**, no bypass. The release
workflow creates new tags; nobody can move or delete a released one.

## Code security

- Dependabot alerts and Dependabot security updates: on.
- Secret scanning and **push protection**: on.
- Private vulnerability reporting: on (`SECURITY.md` links to it).
- CodeQL: default setup, for C/C++, JavaScript/TypeScript, Python and GitHub Actions.

## General → Pull requests

- Allow squash merging only, with the pull request title as the commit message.
- Always suggest updating pull request branches.
- Automatically delete head branches.

## Environments: the release signing key

The release signing key (`RFMAP_OWNER_SECRET_KEY`) must live in the `release` environment, not in
the repository secrets. A repository secret is readable by any workflow on any branch of this
repository, including a pull request from a branch; an environment secret only by a job that
enters the environment, and only from the branches the environment allows. Only the `release`
job of `release.yml` enters it.

1. Settings → Environments → `release` (GitHub creates it on the first release run; create it
   first to set the rules): *Deployment branches and tags*: selected branches, `main` only.
   *Required reviewers*: a maintainer. *Prevent self-review* off for a sole maintainer.
2. Add `RFMAP_OWNER_SECRET_KEY` as an environment secret of `release`.
3. Delete `RFMAP_OWNER_SECRET_KEY` from the repository secrets.

Until step 3, the key stays readable outside the environment.
