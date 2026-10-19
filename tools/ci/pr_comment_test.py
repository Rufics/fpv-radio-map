"""Tests of the CI report's GitHub API steps: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import io
import json
import os
import runpy
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import pr_comment
from pr_comment import API, BOT, COMMENT_LIMIT, MARKER, ApiError

REPO = "rufics/fpv-radio-map"
FORK = "someone/fpv-radio-map"
SHA = "a" * 40


class FakeResponse:
    """The part of an HTTP response pr_comment.request() reads."""

    def __init__(self, payload, link=""):
        self._body = b"" if payload is None else json.dumps(payload).encode()
        self.headers = {"Link": link} if link else {}

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeGitHub:
    """Answers urlopen() from a table of URL prefixes and records every request it gets."""

    def __init__(self, routes):
        self.routes = routes
        self.calls = []

    def urlopen(self, req, timeout):
        body = json.loads(req.data) if req.data else None
        self.calls.append((req.get_method(), req.full_url, body, dict(req.header_items())))
        for prefix, response in self.routes:
            if req.full_url.startswith(prefix):
                return response() if callable(response) else response
        raise AssertionError(f"unexpected request to {req.full_url}")

    def methods(self):
        return [(method, url.split("?")[0]) for method, url, _, _ in self.calls]


class ApiTestCase(unittest.TestCase):
    """Runs each test in an empty folder with a clean, valid environment."""

    BASE_ENV = {"GITHUB_TOKEN": "t0ken", "GITHUB_REPOSITORY": REPO}

    def setUp(self):
        self._cwd = os.getcwd()
        self._dir = tempfile.TemporaryDirectory()
        os.chdir(self._dir.name)
        env = mock.patch.dict(os.environ, self.BASE_ENV, clear=True)
        env.start()
        self.addCleanup(env.stop)

    def tearDown(self):
        os.chdir(self._cwd)
        self._dir.cleanup()

    def serve(self, routes):
        github = FakeGitHub(routes)
        patcher = mock.patch.object(pr_comment.urllib.request, "urlopen", github.urlopen)
        patcher.start()
        self.addCleanup(patcher.stop)
        return github


class EnvTest(ApiTestCase):
    def test_returns_a_value_that_matches(self):
        self.assertEqual(pr_comment.env("GITHUB_REPOSITORY", pr_comment.REPO), REPO)

    def test_missing_and_malformed_values_are_refused(self):
        with self.assertRaisesRegex(ApiError, "RUN_ID is missing"):
            pr_comment.env("RUN_ID")
        os.environ["RUN_ID"] = "12; rm -rf /"
        with self.assertRaises(ApiError):
            pr_comment.env("RUN_ID", pr_comment.NUMBER)
        os.environ["HEAD_REPO"] = "../../etc"
        with self.assertRaises(ApiError):
            pr_comment.env("HEAD_REPO", pr_comment.REPO)


class RequestTest(ApiTestCase):
    def test_sends_the_token_and_json_body(self):
        github = self.serve([(API, FakeResponse({"ok": True}))])
        data, nxt = pr_comment.request("POST", f"{API}/x", {"body": "hi"})
        self.assertEqual((data, nxt), ({"ok": True}, None))
        method, _, body, headers = github.calls[0]
        self.assertEqual((method, body), ("POST", {"body": "hi"}))
        self.assertEqual(headers["Authorization"], "Bearer t0ken")

    def test_an_empty_answer_is_none(self):
        self.serve([(API, FakeResponse(None))])
        self.assertEqual(pr_comment.request("PATCH", f"{API}/x"), (None, None))

    def test_refuses_urls_outside_the_api(self):
        for url in ("https://evil.example/x", "https://api.github.com.evil.example/x", API):
            with self.subTest(url=url), self.assertRaises(ApiError):
                pr_comment.request("GET", url)

    def test_paginate_follows_next_links(self):
        second = f"{API}/items?page=2"
        github = self.serve([
            (second, FakeResponse({"jobs": [3]})),
            (f"{API}/items", FakeResponse({"jobs": [1, 2]}, link=f'<{second}>; rel="next", <{API}/x>; rel="last"')),
        ])
        self.assertEqual(pr_comment.paginate("/items", {"a": "b"}, key="jobs"), [1, 2, 3])
        self.assertIn("per_page=100", github.calls[0][1])
        self.assertIn("a=b", github.calls[0][1])


class FindTest(ApiTestCase):
    BASE_ENV = {**ApiTestCase.BASE_ENV, "RUN_ID": "77", "HEAD_SHA": SHA, "HEAD_BRANCH": "feature/x",
                "HEAD_REPO": FORK, "GITHUB_SERVER_URL": "https://github.example"}
    RUN = f"{API}/repos/{REPO}/actions/runs/77"

    @staticmethod
    def pr(number, sha=SHA, repo=FORK):
        return {"number": number, "head": {"sha": sha, "repo": {"full_name": repo} if repo else None}}

    def run_find(self, prs, artifacts=()):
        github = self.serve([
            (f"{API}/repos/{REPO}/pulls", FakeResponse(prs)),
            (f"{self.RUN}/jobs", FakeResponse({"jobs": [
                {"name": "Build", "conclusion": "success", "html_url": "https://github.com/j/1"},
                {"name": "Fuzz", "conclusion": None, "status": "in_progress"},
            ]})),
            (f"{self.RUN}/artifacts", FakeResponse({"artifacts": list(artifacts)})),
        ])
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            pr_comment.find()
        return github, out.getvalue(), err.getvalue()

    def test_writes_jobs_and_outputs_for_the_one_matching_pull_request(self):
        artifacts = [{"name": "coverage-cpp", "id": 5}, {"name": "coverage-cpp-html", "id": 6},
                     {"name": "old", "id": 7, "expired": True}, {"name": "coverage-web", "id": 8},
                     {"name": "coverage-web-html", "id": 9}]
        github, out, _ = self.run_find([self.pr(12, sha="b" * 40), self.pr(34)], artifacts)
        self.assertEqual(out.splitlines(), [
            "number=34", "coverage=true",
            f"coverage_url=https://github.example/{REPO}/actions/runs/77/artifacts/6",
            "web_coverage=true",
            f"web_coverage_url=https://github.example/{REPO}/actions/runs/77/artifacts/9"])
        self.assertIn("head=someone%3Afeature%2Fx", github.calls[0][1])
        self.assertEqual(json.loads(Path("jobs.json").read_text()), [
            {"name": "Build", "result": "success", "url": "https://github.com/j/1"},
            {"name": "Fuzz", "result": "in_progress", "url": None}])

    def test_without_coverage_artifacts_the_outputs_say_so(self):
        _, out, _ = self.run_find([self.pr(34)], [{"name": "coverage-cpp-html", "id": 6, "expired": True}])
        self.assertEqual(out.splitlines()[1:], ["coverage=false", "coverage_url=", "web_coverage=false",
                                                "web_coverage_url="])

    def test_no_or_several_matches_report_nothing(self):
        cases = {
            "none": [],
            "other head repo": [self.pr(1, repo=REPO)],
            "deleted head repo": [self.pr(1, repo=None)],
            "two base branches": [self.pr(1), self.pr(2)],
        }
        for name, prs in cases.items():
            with self.subTest(name):
                github, out, err = self.run_find(prs)
                self.assertEqual(out, "")
                self.assertIn("nothing to report", err)
                self.assertEqual(len(github.calls), 1)
                self.assertFalse(Path("jobs.json").exists())

    def test_a_malformed_head_sha_is_refused_before_any_call(self):
        os.environ["HEAD_SHA"] = "main"
        github = self.serve([])
        with self.assertRaises(ApiError):
            pr_comment.find()
        self.assertEqual(github.calls, [])


class PostTest(ApiTestCase):
    BASE_ENV = {**ApiTestCase.BASE_ENV, "PR_NUMBER": "34"}
    COMMENTS = f"{API}/repos/{REPO}/issues/34/comments"

    def run_post(self, report, comments):
        Path("report.md").write_text(report, encoding="utf-8")
        github = self.serve([
            (f"{API}/repos/{REPO}/issues/comments/", FakeResponse({})),
            (self.COMMENTS, lambda: FakeResponse({}) if github.calls[-1][0] == "POST" else FakeResponse(comments)),
        ])
        pr_comment.post()
        return github

    def test_updates_the_bot_comment_that_carries_the_marker(self):
        comments = [
            {"id": 1, "user": {"login": "someone"}, "body": MARKER + " fake"},
            {"id": 2, "user": {"login": BOT}, "body": "another bot note"},
            {"id": 3, "user": None, "body": None},
            {"id": 4, "user": {"login": BOT}, "body": MARKER + "\nold"},
        ]
        github = self.run_post(MARKER + "\nnew", comments)
        self.assertEqual(github.methods(), [
            ("GET", self.COMMENTS), ("PATCH", f"{API}/repos/{REPO}/issues/comments/4")])
        self.assertEqual(github.calls[-1][2], {"body": MARKER + "\nnew"})

    def test_creates_a_comment_when_there_is_none(self):
        github = self.run_post(MARKER + "\nnew", [{"id": 1, "user": {"login": "someone"}, "body": "hi"}])
        self.assertEqual(github.methods(), [("GET", self.COMMENTS), ("POST", self.COMMENTS)])

    def test_a_long_report_is_cut(self):
        github = self.run_post(MARKER + "x" * (COMMENT_LIMIT * 2), [])
        body = github.calls[-1][2]["body"]
        self.assertTrue(body.startswith(MARKER))
        self.assertTrue(body.endswith("in the run summary)\n"))
        self.assertLess(len(body), COMMENT_LIMIT + 100)

    def test_a_report_without_its_marker_is_refused(self):
        Path("report.md").write_text("# not a report", encoding="utf-8")
        github = self.serve([])
        with self.assertRaisesRegex(ApiError, "marker"):
            pr_comment.post()
        self.assertEqual(github.calls, [])


class MainTest(ApiTestCase):
    def run_main(self, *args):
        with mock.patch.object(pr_comment.sys, "argv", ["pr_comment.py", *args]):
            pr_comment.main()

    def test_usage_errors_exit(self):
        for args in ((), ("delete",), ("find", "post")):
            with self.subTest(args=args), self.assertRaisesRegex(SystemExit, "usage"):
                self.run_main(*args)

    def test_failures_become_workflow_errors(self):
        with self.assertRaisesRegex(SystemExit, r"::error::PR_NUMBER is missing"):
            self.run_main("post")

    def test_dispatches_the_command(self):
        with mock.patch.object(pr_comment, "find") as find:
            self.run_main("find")
        find.assert_called_once_with()

    def test_runs_as_a_script(self):
        with mock.patch.object(pr_comment.sys, "argv", ["pr_comment.py"]), \
                self.assertRaisesRegex(SystemExit, "usage"):
            runpy.run_path(pr_comment.__file__, run_name="__main__")


if __name__ == "__main__":
    unittest.main()
