"""A small JSON Schema validator for the catalog manifests, using only the standard library.

It knows the subset of draft 2020-12 the schemas in tools/perf/schemas use. A keyword it does not
know is a SchemaError rather than being skipped, so a schema never checks less than it seems to.
References may point into the same file (#/$defs/x) or into another schema in the same folder
(common.schema.json#/$defs/x).
"""
import json
import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from perfkit.errors import SchemaError

SCHEMA_SUFFIX = ".schema.json"
_ANNOTATIONS = frozenset({"$schema", "$id", "$defs", "title", "description", "x-perfkit"})
_TYPES: dict[str, Callable[[Any], bool]] = {
    "object": lambda value: isinstance(value, dict),
    "array": lambda value: isinstance(value, list),
    "string": lambda value: isinstance(value, str),
    "integer": lambda value: isinstance(value, int) and not isinstance(value, bool),
    "number": lambda value: isinstance(value, (int, float)) and not isinstance(value, bool),
    "boolean": lambda value: isinstance(value, bool),
    "null": lambda value: value is None,
}


class SchemaStore:
    """All schema files of one folder, by file name, so references between them resolve."""

    def __init__(self, schemas: dict[str, dict]):
        self._schemas = schemas

    @classmethod
    def load(cls, folder: Path) -> "SchemaStore":
        schemas = {}
        for path in sorted(folder.glob(f"*{SCHEMA_SUFFIX}")):
            try:
                schema = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                raise SchemaError(f"cannot read {path}: {error}") from error
            if not isinstance(schema, dict):
                raise SchemaError(f"{path} is not a JSON object")
            schemas[path.name] = schema
        return cls(schemas)

    def names(self) -> list[str]:
        return list(self._schemas)

    def get(self, name: str) -> dict:
        try:
            return self._schemas[name]
        except KeyError:
            raise SchemaError(f"no schema {name}") from None

    def resolve(self, reference: str, base: str) -> tuple[dict, str]:
        """The schema a $ref names, and the file it lives in (the base for its own references)."""
        file_name, _, pointer = reference.partition("#")
        file_name = file_name or base
        node: Any = self.get(file_name)
        for part in filter(None, pointer.split("/")):
            if not isinstance(node, dict) or part not in node:
                raise SchemaError(f"$ref {reference} from {base} does not resolve")
            node = node[part]
        if not isinstance(node, dict):
            raise SchemaError(f"$ref {reference} from {base} is not a schema")
        return node, file_name

    def validator(self, name: str) -> "SchemaValidator":
        return SchemaValidator(self, name)


@dataclass(frozen=True)
class _Node:
    """Where the validator is: the schema in force, the file it came from and the JSON path."""

    schema: dict
    base: str
    path: str


class SchemaValidator:
    """Checks one JSON value against one schema and lists every mismatch with its JSON path."""

    def __init__(self, store: SchemaStore, name: str):
        self._store = store
        self._name = name
        self._found: list[str] = []
        self._keywords: dict[str, Callable[[_Node, Any, Any], None]] = {
            "const": self._check_const,
            "enum": self._check_enum,
            "properties": self._check_properties,
            "required": self._check_required,
            "additionalProperties": self._check_additional,
            "minProperties": self._check_min_properties,
            "items": self._check_items,
            "minItems": self._check_min_items,
            "uniqueItems": self._check_unique,
            "pattern": self._check_pattern,
            "minLength": self._check_min_length,
            "maxLength": self._check_max_length,
            "minimum": self._check_minimum,
            "maximum": self._check_maximum,
            "exclusiveMinimum": self._check_exclusive_minimum,
        }

    def errors(self, value: Any) -> list[str]:
        self._found = []
        self._check(_Node(self._store.get(self._name), self._name, "$"), value)
        return self._found

    def _report(self, node: _Node, message: str) -> None:
        self._found.append(f"{node.path}: {message}")

    def _check(self, node: _Node, value: Any) -> None:
        schema = node.schema
        if "$ref" in schema:
            target, target_base = self._store.resolve(schema["$ref"], node.base)
            self._check(_Node(target, target_base, node.path), value)
        if "type" in schema and not self._type_matches(schema["type"], value):
            self._report(node, f"expected {self._type_name(schema['type'])}")
            return
        for keyword, argument in schema.items():
            if keyword in _ANNOTATIONS or keyword in ("$ref", "type"):
                continue
            check = self._keywords.get(keyword)
            if check is None:
                raise SchemaError(f"{node.base}: keyword '{keyword}' is not supported")
            check(node, argument, value)

    def _child(self, node: _Node, schema: dict, suffix: str, value: Any) -> None:
        self._check(_Node(schema, node.base, node.path + suffix), value)

    @staticmethod
    def _type_matches(expected: Any, value: Any) -> bool:
        names = expected if isinstance(expected, list) else [expected]
        unknown = [name for name in names if name not in _TYPES]
        if unknown:
            raise SchemaError(f"unknown type '{unknown[0]}'")
        return any(_TYPES[name](value) for name in names)

    @staticmethod
    def _type_name(expected: Any) -> str:
        return " or ".join(expected) if isinstance(expected, list) else str(expected)

    def _check_const(self, node: _Node, argument: Any, value: Any) -> None:
        if value != argument:
            self._report(node, f"must be {json.dumps(argument)}")

    def _check_enum(self, node: _Node, argument: Any, value: Any) -> None:
        if value not in argument:
            self._report(node, f"must be one of {', '.join(json.dumps(item) for item in argument)}")

    def _check_properties(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, dict):
            for name, schema in argument.items():
                if name in value:
                    self._child(node, schema, f".{name}", value[name])

    def _check_required(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, dict):
            for name in argument:
                if name not in value:
                    self._report(node, f"missing '{name}'")

    def _check_additional(self, node: _Node, argument: Any, value: Any) -> None:
        if not isinstance(value, dict):
            return
        declared = node.schema.get("properties", {})
        for name, item in value.items():
            if name in declared:
                continue
            if argument is False:
                self._report(node, f"unknown field '{name}'")
            elif isinstance(argument, dict):
                self._child(node, argument, f".{name}", item)

    def _check_min_properties(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, dict) and len(value) < argument:
            self._report(node, f"needs at least {argument} field(s)")

    def _check_items(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, list):
            for index, item in enumerate(value):
                self._child(node, argument, f"[{index}]", item)

    def _check_min_items(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, list) and len(value) < argument:
            self._report(node, f"needs at least {argument} item(s)")

    def _check_unique(self, node: _Node, argument: Any, value: Any) -> None:
        if argument and isinstance(value, list):
            seen = [json.dumps(item, sort_keys=True) for item in value]
            if len(set(seen)) != len(seen):
                self._report(node, "items must be unique")

    def _check_pattern(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, str) and not re.search(argument, value):
            self._report(node, f"'{value}' does not match {argument}")

    def _check_min_length(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, str) and len(value) < argument:
            self._report(node, f"must have at least {argument} character(s)")

    def _check_max_length(self, node: _Node, argument: Any, value: Any) -> None:
        if isinstance(value, str) and len(value) > argument:
            self._report(node, f"must have at most {argument} character(s)")

    def _check_minimum(self, node: _Node, argument: Any, value: Any) -> None:
        if _TYPES["number"](value) and value < argument:
            self._report(node, f"must be at least {argument}")

    def _check_maximum(self, node: _Node, argument: Any, value: Any) -> None:
        if _TYPES["number"](value) and value > argument:
            self._report(node, f"must be at most {argument}")

    def _check_exclusive_minimum(self, node: _Node, argument: Any, value: Any) -> None:
        if _TYPES["number"](value) and value <= argument:
            self._report(node, f"must be greater than {argument}")
