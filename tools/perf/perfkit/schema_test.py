import json
import tempfile
import unittest
from pathlib import Path

from perfkit.errors import SchemaError
from perfkit.schema import SchemaStore

SCHEMA = {
    "type": "object",
    "required": ["name"],
    "additionalProperties": False,
    "properties": {
        "name": {"type": "string", "minLength": 2, "maxLength": 4, "pattern": "^[a-z]+$"},
        "count": {"type": "integer", "minimum": 1, "maximum": 3},
        "ratio": {"type": "number", "exclusiveMinimum": 0},
        "kind": {"const": "x"},
        "mode": {"enum": ["a", "b"]},
        "tags": {"type": "array", "minItems": 1, "uniqueItems": True, "items": {"$ref": "#/$defs/tag"}},
        "map": {"type": "object", "minProperties": 1, "additionalProperties": {"type": "boolean"}},
        "shared": {"$ref": "other.schema.json#/$defs/flag"},
        "either": {"type": ["string", "null"]},
    },
    "$defs": {"tag": {"type": "string"}},
}
OTHER = {"$defs": {"flag": {"type": "boolean"}}}


class SchemaValidatorTest(unittest.TestCase):
    def setUp(self):
        self.validator = SchemaStore({"main.schema.json": SCHEMA, "other.schema.json": OTHER}).validator(
            "main.schema.json")

    def test_valid_value(self):
        value = {"name": "abc", "count": 2, "ratio": 0.5, "kind": "x", "mode": "a", "tags": ["t"],
                 "map": {"k": True}, "shared": False, "either": None}
        self.assertEqual(self.validator.errors(value), [])

    def test_every_keyword_reports(self):
        value = {"count": 9, "ratio": 0, "kind": "y", "mode": "c", "tags": [], "map": {}, "shared": 1,
                 "either": 3, "extra": 1}
        self.assertEqual(sorted(self.validator.errors(value)), sorted([
            "$: missing 'name'",
            "$: unknown field 'extra'",
            "$.count: must be at most 3",
            "$.ratio: must be greater than 0",
            '$.kind: must be "x"',
            '$.mode: must be one of "a", "b"',
            "$.tags: needs at least 1 item(s)",
            "$.map: needs at least 1 field(s)",
            "$.shared: expected boolean",
            "$.either: expected string or null",
        ]))

    def test_nested_and_string_rules(self):
        errors = self.validator.errors({"name": "A", "count": 0, "tags": [1, "a", "a"], "map": {"k": "no"}})
        self.assertEqual(sorted(errors), sorted([
            "$.name: must have at least 2 character(s)",
            "$.name: 'A' does not match ^[a-z]+$",
            "$.count: must be at least 1",
            "$.tags[0]: expected string",
            "$.tags: items must be unique",
            "$.map.k: expected boolean",
        ]))
        self.assertEqual(self.validator.errors({"name": "abcde"}), ["$.name: must have at most 4 character(s)"])

    def test_booleans_are_not_numbers(self):
        self.assertEqual(self.validator.errors({"name": "ab", "count": True}), ["$.count: expected integer"])

    def test_wrong_top_level_type(self):
        self.assertEqual(self.validator.errors([]), ["$: expected object"])


class SchemaStoreTest(unittest.TestCase):
    def test_unsupported_keyword_and_type(self):
        for schema in ({"oneOf": []}, {"type": "date"}):
            validator = SchemaStore({"s.schema.json": schema}).validator("s.schema.json")
            with self.subTest(schema=schema), self.assertRaises(SchemaError):
                validator.errors({})

    def test_bad_references(self):
        for reference in ("#/$defs/none", "missing.schema.json#/x", "#/$defs/leaf"):
            schema = {"$ref": reference, "$defs": {"leaf": 1}}
            validator = SchemaStore({"s.schema.json": schema}).validator("s.schema.json")
            with self.subTest(reference=reference), self.assertRaises(SchemaError):
                validator.errors({})

    def test_load_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "a.schema.json").write_text(json.dumps({"type": "object"}), encoding="utf-8")
            self.assertEqual(SchemaStore.load(Path(tmp)).names(), ["a.schema.json"])
            for content in ("{", "[]"):
                Path(tmp, "b.schema.json").write_text(content, encoding="utf-8")
                with self.subTest(content=content), self.assertRaises(SchemaError):
                    SchemaStore.load(Path(tmp))


if __name__ == "__main__":
    unittest.main()
