"""Reads JUnit XML test results for the CI report (report.py).

The files come from CTest (`ctest --output-junit`), the Vitest junit reporter and
unittest_junit.py. Every file may be written by the code of a fork's pull request, so the XML is
read with bounded regular expressions, never with an XML parser that could expand entities, and
nothing in it is trusted beyond being text to show.

CTest writes one <testcase> per test, with the labels of the test in a `cmake_labels` property
and the whole output of the test in <system-out>; the reason of a failure is in that output.
Vitest and unittest_junit.py write the message and the stack trace inside <failure> or <error>.
"""
import re
from dataclasses import dataclass

# The excerpt of one failure: enough to see the assertion, never a whole log.
MAX_REASON_LINES = 40
MAX_REASON_CHARS = 3000
# Top folders of the repository; absolute paths of the runner are cut back to them.
REPO_ROOTS = "core|platform|modules|server|examples|fuzz|web|tools|schemas|data"


@dataclass(frozen=True)
class TestCase:
    """One test of a JUnit file."""

    name: str
    outcome: str  # "passed", "failed" or "skipped"
    module: str | None = None
    seconds: float = 0.0
    reason: str = ""

    @property
    def failed(self):
        return self.outcome == "failed"

    @property
    def skipped(self):
        return self.outcome == "skipped"


@dataclass(frozen=True)
class SuiteFile:
    """The test cases of one JUnit file and the total its runner declared."""

    cases: tuple
    declared_total: int = 0


class XmlText:
    """Small helpers for the escaped text of a JUnit file."""

    ENTITIES = (("&quot;", '"'), ("&apos;", "'"), ("&lt;", "<"), ("&gt;", ">"), ("&#10;", "\n"),
                ("&#13;", ""), ("&#9;", "\t"), ("&amp;", "&"))
    CDATA = re.compile(r"<!\[CDATA\[(.*?)\]\]>", re.DOTALL)

    @classmethod
    def unescape(cls, text):
        if text is None:
            return ""
        text = cls.CDATA.sub(lambda m: m.group(1).replace("&", "&amp;").replace("<", "&lt;"), text)
        for entity, char in cls.ENTITIES:
            text = text.replace(entity, char)
        return text

    @staticmethod
    def attribute(tag, name):
        m = re.search(rf'\s{name}="([^"]*)"', tag)
        return XmlText.unescape(m.group(1)) if m else None

    @staticmethod
    def element(body, name):
        """(attributes, text) of the first <name> element of a body, or None."""
        m = re.search(rf"<{name}\b([^>]*?)(?:/>|>(.*?)</{name}>)", body, re.DOTALL)
        return (m.group(1), m.group(2) or "") if m else None


class GtestOutput:
    """The parts of a GoogleTest log that explain a failure."""

    RUN = "[ RUN      ]"
    FAILED = "[  FAILED  ]"
    PASSED = "[       OK ]"

    @classmethod
    def failed_blocks(cls, output):
        """The lines of each failed test, from its RUN line to its FAILED line."""
        blocks, current = [], None
        for line in output.splitlines():
            if line.startswith(cls.RUN):
                current = []
            elif current is not None and line.startswith(cls.FAILED):
                blocks.append(current)
                current = None
            elif current is not None and line.startswith(cls.PASSED):
                current = None
            elif current is not None:
                current.append(line)
        return ["\n".join(b).strip() for b in blocks if any(s.strip() for s in b)]


class FailureReason:
    """The text that says why a test failed, cut to a readable excerpt."""

    ABSOLUTE = re.compile(rf"(?<![\w.])/[\w./+-]{{0,300}}?/(?=(?:{REPO_ROOTS})/)")
    ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
    CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
    GENERIC_MESSAGES = frozenset({"", "Failed", "failed"})

    @classmethod
    def of(cls, body):
        """The reason of a failed <testcase> body."""
        parts = []
        failure = XmlText.element(body, "failure") or XmlText.element(body, "error")
        if failure:
            parts += cls._failure_parts(*failure)
        output = XmlText.element(body, "system-out")
        if output and not parts[1:]:
            parts.append(cls._output_excerpt(XmlText.unescape(output[1])))
        return cls.excerpt("\n".join(p for p in parts if p))

    @classmethod
    def _failure_parts(cls, attrs, text):
        message = XmlText.attribute(attrs, "message") or ""
        detail = XmlText.unescape(text).strip()
        message = message.strip()
        # unittest_junit.py and Vitest repeat the message inside the stack trace.
        if message in cls.GENERIC_MESSAGES or message in detail:
            return [detail] if detail else []
        return [message, detail] if detail else [message]

    @classmethod
    def _output_excerpt(cls, output):
        """The failed GoogleTest blocks of a log, or its last lines for other programs."""
        blocks = GtestOutput.failed_blocks(output)
        if blocks:
            return "\n\n".join(blocks)
        return "\n".join(output.strip().splitlines()[-MAX_REASON_LINES:])

    @classmethod
    def excerpt(cls, text):
        """Readable text: no colours or control characters, repository paths, bounded size."""
        text = cls.CONTROL.sub("", cls.ANSI.sub("", text.replace("\r\n", "\n")))
        lines = text.strip().splitlines()
        if len(lines) > MAX_REASON_LINES:
            lines = lines[:MAX_REASON_LINES] + [f"… {len(lines) - MAX_REASON_LINES} more lines"]
        text = "\n".join(lines)
        if len(text) > MAX_REASON_CHARS:
            text = text[: MAX_REASON_CHARS - 1] + "…"
        # Cut last, on bounded text: the paths only get shorter.
        return cls.ABSOLUTE.sub("", text)


class SourceLocation:
    """The first repository file and line a failure points at, such as `core/a.cpp:12`."""

    PATH = rf"(?<![\w/.-])((?:{REPO_ROOTS}|projects)/[\w./+-]{{1,300}}?\.[A-Za-z]{{1,5}})"
    # `path:12` from compilers, GoogleTest and Vitest; `File "path", line 12` from Python.
    PATTERNS = (re.compile(rf"{PATH}:(\d{{1,7}})\b"), re.compile(rf'File "{PATH}", line (\d{{1,7}})\b'))

    @classmethod
    def find(cls, text):
        """(path, line) of the earliest match or None. The path holds no `..` segment."""
        found = [m for p in cls.PATTERNS for m in p.finditer(text) if ".." not in m.group(1).split("/")]
        if not found:
            return None
        first = min(found, key=lambda m: m.start())
        path = first.group(1)
        # Vitest runs in web/ and names its files from there.
        return ("web/" + path if path.startswith("projects/") else path), int(first.group(2))


class JUnitParser:
    """Test cases of one JUnit file."""

    TESTCASE = re.compile(r"<testcase\b([^>]*?)(/>|>(.*?)</testcase>)", re.DOTALL)
    ROOT = re.compile(r"<testsuites?\b[^>]*>")
    FAILED_BODY = re.compile(r"<(failure|error)\b")
    FAILED_STATUS = re.compile(r'\sstatus="fail(ed)?"')
    SKIPPED_BODY = re.compile(r"<skipped\b")
    SKIPPED_STATUS = re.compile(r'\sstatus="(notrun|disabled|skipped)"')
    LABELS = re.compile(r'<property\s+name="cmake_labels"\s+value="([^"]*)"')

    def parse(self, xml):
        cases = tuple(self._case(attrs, body) for attrs, _, body in self.TESTCASE.findall(xml))
        return SuiteFile(cases=cases, declared_total=self._declared_total(xml))

    def _case(self, attrs, body):
        outcome = self._outcome(attrs, body)
        return TestCase(
            name=self._name(attrs),
            outcome=outcome,
            module=self._module(body),
            seconds=self._seconds(attrs),
            reason=FailureReason.of(body) if outcome == "failed" else "",
        )

    def _outcome(self, attrs, body):
        if self.FAILED_BODY.search(body) or self.FAILED_STATUS.search(attrs):
            return "failed"
        if self.SKIPPED_BODY.search(body) or self.SKIPPED_STATUS.search(attrs):
            return "skipped"
        return "passed"

    @staticmethod
    def _name(attrs):
        name = XmlText.attribute(attrs, "name") or "?"
        cls = XmlText.attribute(attrs, "classname") or ""
        return f"{cls} › {name}" if cls and cls != name else name

    def _module(self, body):
        """The first CTest label of the test: the module it belongs to."""
        m = self.LABELS.search(body)
        if not m:
            return None
        first = XmlText.unescape(m.group(1)).split(";")[0].strip()
        return first or None

    @staticmethod
    def _seconds(attrs):
        try:
            seconds = float(XmlText.attribute(attrs, "time") or 0)
        except ValueError:
            return 0.0
        return seconds if 0 <= seconds < 1e7 else 0.0

    def _declared_total(self, xml):
        root = self.ROOT.search(xml)
        try:
            return max(int(XmlText.attribute(root.group(0), "tests") or 0), 0) if root else 0
        except ValueError:
            return 0
