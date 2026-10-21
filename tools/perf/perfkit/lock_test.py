import unittest

from perfkit.catalog_test import CatalogTestCase
from perfkit.errors import PerfError
from perfkit.lock import LOCK_NAME, LockComparer, LockFile, Mode
from perfkit.version import Version


class LockTest(CatalogTestCase):
    def comparer(self):
        catalog, problems = self.load()
        self.assertEqual(problems, [])
        lock_file = LockFile(self.sample.root)
        return LockComparer(catalog, lock_file.read(), lock_file.path), lock_file

    def lock(self):
        comparer, lock_file = self.comparer()
        lock_file.write(comparer.current())

    def messages(self, mode):
        comparer, _ = self.comparer()
        return [problem.describe(self.sample.root) for problem in comparer.problems(mode)]

    def test_new_items_need_a_lock_only_for_check(self):
        self.assertEqual(len(self.messages(Mode.CHECK)), 7)
        self.assertEqual(self.messages(Mode.LOCK), [])

    def test_locked_catalog_is_consistent(self):
        self.lock()
        self.assertEqual(self.messages(Mode.CHECK), [])
        self.assertEqual(LockFile(self.sample.root).read()["persona/link-editor"].version, Version(1, 0, 0))

    def test_change_without_new_version(self):
        self.lock()
        (self.sample.root / "fixtures/warsaw-points/points.json").write_text("[]", encoding="utf-8")
        expected = [("fixtures/warsaw-points: content changed but version stayed 1.0.0; raise the version in "
                     "manifest.json")]
        self.assertEqual(self.messages(Mode.CHECK), expected)
        self.assertEqual(self.messages(Mode.LOCK), expected)

    def test_new_version_must_be_locked(self):
        self.lock()
        self.sample.change("personas/link-editor", version="1.1.0")
        self.assertEqual(self.messages(Mode.CHECK), [
            "personas/link-editor: version 1.1.0 is not locked yet (locked 1.0.0); run perf.py catalog lock"])
        self.assertEqual(self.messages(Mode.LOCK), [])

    def test_version_going_down(self):
        self.sample.change("personas/link-editor", version="2.0.0")
        self.lock()
        self.sample.change("personas/link-editor", version="1.5.0")
        self.assertEqual(self.messages(Mode.LOCK), ["personas/link-editor: version went down from 2.0.0 to 1.5.0"])

    def test_removed_item(self):
        self.lock()
        for path in sorted((self.sample.root / "extensions/reporters/html").iterdir()):
            path.unlink()
        (self.sample.root / "extensions/reporters/html").rmdir()
        self.sample.change("scenarios/peak", version="1.0.1", reporters=[])
        self.assertIn(f"{LOCK_NAME}: reporter/html is locked but no longer exists; run perf.py catalog lock",
                      self.messages(Mode.CHECK))
        self.assertEqual(self.messages(Mode.LOCK), [])

    def test_unreadable_lock(self):
        path = self.sample.root / LOCK_NAME
        for content in ("{", '{"format": 2, "items": {}}', '{"format": 1, "items": {"a/b": {}}}', "[]"):
            path.write_text(content, encoding="utf-8")
            with self.subTest(content=content), self.assertRaises(PerfError):
                LockFile(self.sample.root).read()


class VersionTest(unittest.TestCase):
    def test_parse_and_order(self):
        self.assertLess(Version.parse("1.9.0"), Version.parse("1.10.0"))
        self.assertEqual(str(Version.parse("2.0.13")), "2.0.13")
        for text in ("1.0", "01.0.0", "1.0.0-rc1", ""):
            with self.subTest(text=text), self.assertRaises(PerfError):
                Version.parse(text)


if __name__ == "__main__":
    unittest.main()
