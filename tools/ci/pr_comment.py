#!/usr/bin/env python3
"""GitHub API steps of the CI report workflow (.github/workflows/ci-report.yml).

    python3 -I tools/ci/pr_comment.py find    # in the data folder: writes jobs.json, prints outputs
    python3 -I tools/ci/pr_comment.py post    # in the data folder: posts report.md

`find` looks up the open pull request whose head is the CI run's head commit, writes the run's
jobs to jobs.json and prints `number=`, `coverage=`, `coverage_url=`, `web_coverage=` and
`web_coverage_url=` lines for $GITHUB_OUTPUT.
It prints nothing when no open pull request has that head any more. `post` creates or updates the
one report comment on pull request PR_NUMBER.

Both read and write only fixed names in the current directory. The pull request is found from
the run's metadata, never from an artifact, because the artifacts are written by pull request code.

Environment: GITHUB_TOKEN, GITHUB_REPOSITORY, GITHUB_SERVER_URL; for `find` RUN_ID, HEAD_SHA,
HEAD_BRANCH and HEAD_REPO (owner/name of the head repository); for `post` PR_NUMBER.
"""
import json
import os
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://api.github.com"
MARKER = "<!-- rfmap-ci-report -->"
BOT = "github-actions[bot]"
COMMENT_LIMIT = 65000
REPO = re.compile(r"[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}")
NUMBER = re.compile(r"[1-9]\d{0,15}")
SHA = re.compile(r"[0-9a-f]{40}")
NEXT = re.compile(r'<(https://api\.github\.com/[^>]+)>;\s*rel="next"')


class ApiError(Exception):
    pass


def env(name, pattern=None):
    value = os.environ.get(name, "")
    if not value or (pattern and not pattern.fullmatch(value)):
        raise ApiError(f"{name} is missing or malformed")
    return value


def request(method, url, body=None):
    """One API call; returns (parsed JSON, URL of the next page or None)."""
    if not url.startswith(API + "/"):
        raise ApiError(f"refusing to call {url}")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {env('GITHUB_TOKEN')}",
        "X-GitHub-Api-Version": "2022-11-28",
        "Content-Type": "application/json",
        "User-Agent": "rfmap-ci-report",
    })
    with urllib.request.urlopen(req, timeout=30) as resp:
        nxt = NEXT.search(resp.headers.get("Link", ""))
        return json.loads(resp.read() or b"null"), nxt.group(1) if nxt else None


def paginate(path, params, key=None):
    url = f"{API}{path}?{urllib.parse.urlencode({**params, 'per_page': 100})}"
    items = []
    while url:
        page, url = request("GET", url)
        items += page[key] if key else page
    return items


def find():
    repo = env("GITHUB_REPOSITORY", REPO)
    run_id = env("RUN_ID", NUMBER)
    head_sha = env("HEAD_SHA", SHA)
    head_repo = env("HEAD_REPO", REPO)
    head = f"{head_repo.split('/')[0]}:{env('HEAD_BRANCH')}"
    prs = paginate(f"/repos/{repo}/pulls", {"state": "open", "head": head})
    matches = [p for p in prs if p["head"]["sha"] == head_sha
               and (p["head"].get("repo") or {}).get("full_name") == head_repo]
    # Exactly one pull request, or none: the same head opened against two base branches is
    # ambiguous, and a report must never land on a pull request it was not built for.
    if len(matches) != 1:
        print(f"::notice::{len(matches)} open pull requests have {head_sha} as their head; nothing to report.",
              file=sys.stderr)
        return
    pr = matches[0]
    jobs = paginate(f"/repos/{repo}/actions/runs/{run_id}/jobs", {"filter": "latest"}, key="jobs")
    Path("jobs.json").write_text(json.dumps(
        [{"name": j["name"], "result": j.get("conclusion") or j.get("status"), "url": j.get("html_url")} for j in jobs]))
    artifacts = [a for a in paginate(f"/repos/{repo}/actions/runs/{run_id}/artifacts", {}, key="artifacts")
                 if not a.get("expired")]
    run_url = f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/{repo}/actions/runs/{run_id}"
    print(f"number={int(pr['number'])}")
    for output, name in (("coverage", "coverage-cpp"), ("web_coverage", "coverage-web")):
        present, url = coverage_artifacts(artifacts, name, run_url)
        print(f"{output}={'true' if present else 'false'}")
        print(f"{output}_url={url}")


def coverage_artifacts(artifacts, name, run_url):
    """Whether the run has the coverage data artifact `name`, and the link to its HTML report."""
    html = next((a for a in artifacts if a["name"] == f"{name}-html"), None)
    present = any(a["name"] == name for a in artifacts)
    return present, f"{run_url}/artifacts/{int(html['id'])}" if html else ""


def post():
    repo = env("GITHUB_REPOSITORY", REPO)
    number = env("PR_NUMBER", NUMBER)
    body = Path("report.md").read_text(encoding="utf-8")
    if not body.startswith(MARKER):
        raise ApiError("the report does not start with its marker")
    if len(body) > COMMENT_LIMIT:
        body = body[:COMMENT_LIMIT] + "\n\n… (cut; the full report is in the run summary)\n"
    comments = paginate(f"/repos/{repo}/issues/{number}/comments", {})
    mine = next((c for c in comments if (c.get("user") or {}).get("login") == BOT
                 and (c.get("body") or "").startswith(MARKER)), None)
    if mine:
        request("PATCH", f"{API}/repos/{repo}/issues/comments/{int(mine['id'])}", {"body": body})
    else:
        request("POST", f"{API}/repos/{repo}/issues/{number}/comments", {"body": body})


def main():
    commands = {"find": find, "post": post}
    if len(sys.argv) != 2 or sys.argv[1] not in commands:
        sys.exit("usage: pr_comment.py find|post")
    try:
        commands[sys.argv[1]]()
    except (ApiError, OSError, KeyError, ValueError) as e:
        sys.exit(f"::error::{e}")


if __name__ == "__main__":
    main()
