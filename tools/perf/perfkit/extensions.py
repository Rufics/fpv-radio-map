"""Loads the Python class of an extension from its folder ("entry": "FILE.py:Class")."""
import importlib.util
import sys

from perfkit.catalog import Item
from perfkit.errors import PerfError


class ExtensionLoader:
    """Imports an extension's entry file once and checks that its class has the right base."""

    def __init__(self):
        self._classes: dict[str, type] = {}

    def load(self, item: Item, base: type) -> type:
        if item.key not in self._classes:
            self._classes[item.key] = self._import(item)
        found = self._classes[item.key]
        if not (isinstance(found, type) and issubclass(found, base)):
            raise PerfError(f"{item.key}: {item.manifest['entry']} is not a {base.__name__}")
        return found

    @staticmethod
    def _import(item: Item) -> type:
        file_name, class_name = item.manifest["entry"].split(":", 1)
        path = item.folder / file_name
        # Ids may hold dashes; the module name only has to be unique and stable.
        module_name = f"perfkit_ext.{item.kind.name}.{item.id.replace('-', '_')}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise PerfError(f"{item.key}: cannot import {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        try:
            spec.loader.exec_module(module)
        except (ImportError, SyntaxError) as error:
            del sys.modules[module_name]
            raise PerfError(f"{item.key}: cannot import {path} ({error})") from error
        if not hasattr(module, class_name):
            raise PerfError(f"{item.key}: {file_name} has no class {class_name}")
        return getattr(module, class_name)
