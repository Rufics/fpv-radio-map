#!/usr/bin/env python3
"""Publishes docs/ to the GitHub wiki checked out in wiki/ at the repository root.

    git clone https://github.com/Rufics/fpv-radio-map.wiki.git wiki
    GITHUB_REPOSITORY=Rufics/fpv-radio-map python3 tools/wiki/publish.py

The script takes no arguments: both folders are fixed relative to the repository, so it can read
and write nothing else. GITHUB_REPOSITORY (set by GitHub Actions) names the repository for links.

The wiki becomes a mirror of docs/: every Markdown file is one page, named after its first
heading (docs/README.md is Home, docs/_Sidebar.md is the sidebar). Links between docs point to
the wiki pages, images under docs/ are copied, and every other relative link points to the file
on main. Pages that no longer exist in docs/ are removed.
"""
import os
import re
import shutil
import sys
from pathlib import Path, PurePosixPath

SPECIAL = {"README.md": "Home", "_Sidebar.md": "_Sidebar"}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}
# ](target) in Markdown and src="target" in inline HTML.
LINK = re.compile(r'(\]\(|src=")([^)"\s]+)([)"])')
REPO_ROOT = Path(__file__).resolve().parents[2]
EXTERNAL = re.compile(r"^[a-z][a-z0-9+.-]*:")
REPO_NAME = re.compile(r"^[\w.-]+/[\w.-]+$")


class PublishError(Exception):
    pass


def page_name(rel: PurePosixPath, text: str) -> str:
    if str(rel) in SPECIAL:
        return SPECIAL[str(rel)]
    heading = next((line[2:].strip() for line in text.splitlines() if line.startswith("# ")), rel.stem)
    words = re.sub(r"[^\w\s-]", " ", heading).split()
    return "-".join(words)


def resolve(base: PurePosixPath, target: str) -> PurePosixPath:
    parts: list[str] = []
    for part in (base.parent / target).parts:
        if part == ".." and parts and parts[-1] != "..":
            parts.pop()
        elif part != ".":
            parts.append(part)
    return PurePosixPath(*parts)


def inside(root: Path, path: Path) -> Path:
    """The real path of `path`, which must lie under `root` (symlinks followed)."""
    real = path.resolve()
    if not real.is_relative_to(root):
        raise PublishError(f"{path} is outside {root}")
    return real


class Publisher:
    def __init__(self, docs: Path, wiki: Path, repo: str):
        if not REPO_NAME.match(repo):
            raise PublishError(f"not an owner/name repository: {repo}")
        self.docs = docs.resolve()
        self.wiki = wiki.resolve()
        if not self.wiki.is_dir():
            raise PublishError(f"no wiki checkout at {self.wiki}; clone the wiki repository there first")
        self.repo_root = self.docs.parent
        self.blob = f"https://github.com/{repo}/blob/main"
        self.tree = f"https://github.com/{repo}/tree/main"
        self.sources = {PurePosixPath(p.relative_to(self.docs).as_posix()): p.read_text(encoding="utf-8")
                        for p in sorted(self.docs.rglob("*.md"))}
        self.names = {rel: page_name(rel, text) for rel, text in self.sources.items()}
        clashes = sorted({n for n in self.names.values() if list(self.names.values()).count(n) > 1})
        if clashes:
            raise PublishError(f"two docs would publish to the same wiki page: {clashes}")

    def clear(self) -> None:
        for old in self.wiki.iterdir():
            if old.name.startswith("."):
                continue
            shutil.rmtree(old) if old.is_dir() else old.unlink()

    def copy_image(self, resolved: PurePosixPath) -> str:
        source = inside(self.docs, self.docs / resolved)
        dest = inside(self.wiki, self.wiki / "assets" / resolved)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        return f"assets/{resolved}"

    def target_for(self, rel: PurePosixPath, target: str) -> str:
        if EXTERNAL.match(target) or target.startswith(("#", "/")):
            return target
        path, _, anchor = target.partition("#")
        anchor = f"#{anchor}" if anchor else ""
        resolved = resolve(rel, path)
        if resolved in self.names:
            return self.names[resolved] + anchor
        if resolved.parts and resolved.parts[0] != ".." and resolved.suffix.lower() in IMAGE_SUFFIXES:
            return self.copy_image(resolved)
        in_repo = resolve(PurePosixPath("docs/index"), str(resolved))
        base = self.tree if (self.repo_root / in_repo).is_dir() else self.blob
        return f"{base}/{in_repo}{anchor}"

    def write(self, name: str, text: str) -> None:
        inside(self.wiki, self.wiki / f"{name}.md").write_text(text, encoding="utf-8")

    def publish(self) -> int:
        self.clear()
        for rel, text in self.sources.items():
            body = LINK.sub(lambda m, rel=rel: m.group(1) + self.target_for(rel, m.group(2)) + m.group(3), text)
            self.write(self.names[rel], body)
        self.write("_Footer", f"This wiki is generated from the [`docs/` folder]({self.tree}/docs). "
                              "Edit the files there with a pull request; changes made here are overwritten.\n")
        return len(self.sources)


def main() -> int:
    if len(sys.argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        count = Publisher(REPO_ROOT / "docs", REPO_ROOT / "wiki", os.environ.get("GITHUB_REPOSITORY", "")).publish()
    except PublishError as e:
        print(e, file=sys.stderr)
        return 1
    print(f"published {count} pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
