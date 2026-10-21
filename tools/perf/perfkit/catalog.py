"""Finds, checks and indexes the descriptions and extensions under one catalog folder.

Every kind is defined by a schema in tools/perf/schemas whose "x-perfkit" block says where its
items live, whether they are descriptions or extensions, which fields refer to other items and
which fields name files that must exist. An item is a folder <kind folder>/<id>/ with a
manifest.json; its id must equal the folder name.
"""
import hashlib
import json
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from perfkit.errors import PerfError, Problem, SchemaError
from perfkit.schema import SCHEMA_SUFFIX, SchemaStore, SchemaValidator
from perfkit.version import Version

MANIFEST_NAME = "manifest.json"
GROUPS = ("description", "extension")
# Never part of an item's content: Python caches and editor or OS litter.
_IGNORED_DIRS = frozenset({"__pycache__", ".pytest_cache", ".mypy_cache"})
_IGNORED_SUFFIXES = (".pyc", ".pyo", ".swp", "~")
_IGNORED_NAMES = frozenset({".DS_Store", "Thumbs.db"})
_HASH_CHUNK_BYTES = 1 << 20


@dataclass(frozen=True)
class Reference:
    """A field whose value names another item, optionally with a file in it (ID:FILE)."""

    path: str
    kind: str
    with_file: bool = False


@dataclass(frozen=True)
class FileRule:
    """A field whose value is a file in the item's folder; `before` cuts off a suffix (FILE:Class)."""

    path: str
    before: str = ""


@dataclass(frozen=True)
class KindSpec:
    """One kind of catalog item, read from the x-perfkit block of its schema."""

    name: str
    folder: str
    group: str
    schema: str
    references: tuple[Reference, ...] = ()
    files: tuple[FileRule, ...] = ()
    required_files: tuple[str, ...] = ()


class KindRegistry:
    """The kinds the toolchain knows, one per schema file with an x-perfkit block."""

    def __init__(self, store: SchemaStore, kinds: list[KindSpec]):
        self.store = store
        self._kinds = {kind.name: kind for kind in kinds}

    @classmethod
    def load(cls, schemas_dir: Path) -> "KindRegistry":
        store = SchemaStore.load(schemas_dir)
        kinds = [cls._spec(name, store.get(name)) for name in store.names() if "x-perfkit" in store.get(name)]
        folders = [kind.folder for kind in kinds]
        if len(set(folders)) != len(folders):
            raise SchemaError("two kinds share one folder")
        return cls(store, kinds)

    @staticmethod
    def _spec(file_name: str, schema: dict) -> KindSpec:
        block = schema["x-perfkit"]
        name = file_name.removesuffix(SCHEMA_SUFFIX)
        try:
            spec = KindSpec(
                name=name,
                folder=block["folder"],
                group=block["group"],
                schema=file_name,
                references=tuple(Reference(r["path"], r["kind"], r.get("withFile", False))
                                 for r in block.get("references", [])),
                files=tuple(FileRule(r["path"], r.get("before", "")) for r in block.get("files", [])),
                required_files=tuple(block.get("requiredFiles", [])),
            )
        except (KeyError, TypeError) as error:
            raise SchemaError(f"{file_name}: bad x-perfkit block ({error})") from error
        if spec.group not in GROUPS:
            raise SchemaError(f"{file_name}: group must be one of {', '.join(GROUPS)}")
        if Path(spec.folder).is_absolute() or ".." in Path(spec.folder).parts:
            raise SchemaError(f"{file_name}: folder must stay inside the catalog")
        return spec

    def kinds(self) -> list[KindSpec]:
        return sorted(self._kinds.values(), key=lambda kind: kind.folder)

    def get(self, name: str) -> KindSpec | None:
        return self._kinds.get(name)

    def validator(self, kind: KindSpec) -> SchemaValidator:
        return self.store.validator(kind.schema)


@dataclass(frozen=True)
class Item:
    """One description or extension: its manifest and the folder it lives in."""

    kind: KindSpec
    id: str
    version: Version
    folder: Path
    manifest: dict = field(compare=False, repr=False)

    @property
    def key(self) -> str:
        return f"{self.kind.name}/{self.id}"


class ContentHasher:
    """SHA-256 of everything in an item's folder: relative paths and bytes, in a fixed order."""

    @staticmethod
    def files(folder: Path) -> list[Path]:
        found = []
        # Sorted by the POSIX form: Path order ignores case on Windows, so the hash would differ.
        for path in sorted(folder.rglob("*"), key=lambda found_path: found_path.relative_to(folder).as_posix()):
            relative = path.relative_to(folder)
            if any(part in _IGNORED_DIRS for part in relative.parts):
                continue
            if path.name in _IGNORED_NAMES or path.name.endswith(_IGNORED_SUFFIXES):
                continue
            if path.is_file():
                found.append(path)
        return found

    @classmethod
    def folder(cls, folder: Path) -> str:
        digest = hashlib.sha256()
        for path in cls.files(folder):
            digest.update(path.relative_to(folder).as_posix().encode("utf-8") + b"\0")
            digest.update(cls._file(path).encode("ascii") + b"\n")
        return digest.hexdigest()

    @staticmethod
    def _file(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(_HASH_CHUNK_BYTES), b""):
                digest.update(chunk)
        return digest.hexdigest()


class FieldPath:
    """Values at a dotted path: `a.b` walks objects, `a[]` every list item, `*` every object value."""

    @classmethod
    def values(cls, data: Any, path: str) -> Iterator[Any]:
        yield from cls._walk(data, path.split("."))

    @classmethod
    def _walk(cls, data: Any, parts: list[str]) -> Iterator[Any]:
        if not parts:
            yield data
            return
        head, rest = parts[0], parts[1:]
        if head == "*":
            children = list(data.values()) if isinstance(data, dict) else []
        else:
            name = head.removesuffix("[]")
            child = data.get(name) if isinstance(data, dict) else None
            if head.endswith("[]"):
                children = child if isinstance(child, list) else []
            else:
                children = [] if child is None else [child]
        for child in children:
            yield from cls._walk(child, rest)


class Catalog:
    """Every item found under one root folder, indexed by kind and id."""

    def __init__(self, root: Path, items: list[Item]):
        self.root = root
        self._items = {item.key: item for item in items}

    def items(self, kind: str | None = None) -> list[Item]:
        chosen = [item for item in self._items.values() if kind is None or item.kind.name == kind]
        return sorted(chosen, key=lambda item: (item.kind.folder, item.id))

    def get(self, kind: str, item_id: str) -> Item | None:
        return self._items.get(f"{kind}/{item_id}")


class CatalogLoader:
    """Reads a catalog folder and lists every problem in it, not only the first one."""

    def __init__(self, registry: KindRegistry):
        self._registry = registry

    def load(self, root: Path) -> tuple[Catalog, list[Problem]]:
        if not root.is_dir():
            raise PerfError(f"catalog folder {root} does not exist")
        problems: list[Problem] = []
        items: list[Item] = []
        for kind in self._registry.kinds():
            items.extend(self._load_kind(root, kind, problems))
        catalog = Catalog(root, items)
        for item in catalog.items():
            problems.extend(self._check_files(item))
            problems.extend(self._check_references(catalog, item))
        return catalog, problems

    def _load_kind(self, root: Path, kind: KindSpec, problems: list[Problem]) -> list[Item]:
        folder = root / kind.folder
        if not folder.is_dir():
            return []
        items = []
        for child in sorted(folder.iterdir()):
            if not child.is_dir() or child.name in _IGNORED_DIRS:
                continue
            item = self._load_item(child, kind, problems)
            if item is not None:
                items.append(item)
        return items

    def _load_item(self, folder: Path, kind: KindSpec, problems: list[Problem]) -> Item | None:
        path = folder / MANIFEST_NAME
        if not path.is_file():
            problems.append(Problem(folder, f"no {MANIFEST_NAME}"))
            return None
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            problems.append(Problem(path, f"not valid JSON ({error})"))
            return None
        errors = self._registry.validator(kind).errors(manifest)
        problems.extend(Problem(path, message) for message in errors)
        if errors:
            return None
        if manifest["id"] != folder.name:
            problems.append(Problem(path, f"id '{manifest['id']}' must equal the folder name '{folder.name}'"))
            return None
        return Item(kind, manifest["id"], Version.parse(manifest["version"]), folder, manifest)

    def _check_files(self, item: Item) -> list[Problem]:
        manifest_path = item.folder / MANIFEST_NAME
        names = list(item.kind.required_files)
        for rule in item.kind.files:
            for value in FieldPath.values(item.manifest, rule.path):
                if isinstance(value, str):
                    names.append(value.split(rule.before, 1)[0] if rule.before else value)
        return [Problem(manifest_path, message) for message in
                (self.missing_file(item.folder, name) for name in names) if message]

    def _check_references(self, catalog: Catalog, item: Item) -> list[Problem]:
        manifest_path = item.folder / MANIFEST_NAME
        problems = []
        for reference in item.kind.references:
            for value in FieldPath.values(item.manifest, reference.path):
                if not isinstance(value, str):
                    continue
                message = self._reference_problem(catalog, reference, value)
                if message:
                    problems.append(Problem(manifest_path, f"{reference.path}: {message}"))
        return problems

    def _reference_problem(self, catalog: Catalog, reference: Reference, value: str) -> str:
        target_id, _, file_name = value.partition(":") if reference.with_file else (value, "", "")
        target = catalog.get(reference.kind, target_id)
        if target is None:
            return f"no {reference.kind} '{target_id}'"
        if reference.with_file:
            return self.missing_file(target.folder, file_name)
        return ""

    @staticmethod
    def missing_file(folder: Path, name: str) -> str:
        """Why `name` is not a usable file inside `folder`, or an empty string when it is."""
        relative = Path(name)
        if not name or relative.is_absolute() or ".." in relative.parts:
            return f"'{name}' must be a relative path inside {folder.name}/"
        path = folder / relative
        if not path.resolve().is_relative_to(folder.resolve()):
            return f"'{name}' leads outside {folder.name}/"
        if not path.is_file():
            return f"file '{name}' does not exist in {folder.name}/"
        return ""
