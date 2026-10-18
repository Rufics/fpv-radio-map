# Repository settings for maintainers

The workflows assume these GitHub settings. They cannot be set from a pull request, so a
maintainer with admin rights applies them once in the repository settings. Together they make
every change, from a branch or a fork, pass the same checks and a review before it reaches `main`,
without giving pull request code any secret or write access.

## How pull requests are checked

| Workflow | Trigger | Runs pull request code | Token | Secrets |
|---|---|---|---|---|
| `ci.yml`, fork or Dependabot | `pull_request` | yes | read-only | none |
| `ci.yml`, branch of this repository | `pull_request` | yes, by someone who can push here | `pull-requests: write` in the report job | none |
| `pr-title.yml` | `pull_request` | yes (commitlint config) | read-only | none |
| `ci-report.yml`, forks and Dependabot | `workflow_run` after CI | **no**: main's code, PR artifacts read as data | `pull-requests: write` | none |
| CodeQL, SonarCloud, Gitar | GitHub apps | analyse only | app's own | app's own |

The rule behind it: code from a fork never runs with write access or secrets, and a workflow
that has write access for a fork never runs that fork's code. A branch of this repository can
only be pushed by someone who can already change the workflows, so its report job may comment,
and nothing gets secrets. `pull_request_target` is not used.

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

`C++ build and tests`, `C++ coverage` and `Web build and tests` are not required one by one: they
are skipped when a change cannot affect them, and `Test report` already fails when any of them
fails.

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

## Recommended next step: a protected release environment

The release signing key (`RFMAP_OWNER_SECRET_KEY`) is a repository secret today, so any workflow
on any branch of this repository could read it. Moving it into an environment closes that:

1. Settings → Environments → New environment `release`: deployment branches *main* only, required
   reviewer: a maintainer.
2. Move `RFMAP_OWNER_SECRET_KEY` from the repository secrets to that environment.
3. Add `environment: release` to the `plan` and `release` jobs of `release.yml`.

Do the three together: the release fails its key check while the secret and the workflow disagree.
