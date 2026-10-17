#!/usr/bin/env python3
"""Publishes docs/ to the GitHub wiki checkout given as the second argument.

    python3 tools/wiki/publish.py docs path/to/repo.wiki Rufics/fpv-radio-map

The wiki becomes a mirror of docs/: every Markdown file is one page, named after its first
heading (docs/README.md is Home, docs/_Sidebar.md is the sidebar). Links between docs point to
the wiki pages, images under docs/ are copied, and every other relative link points to the file
on main. Pages that no longer exist in docs/ are removed.
"""
import re
import shutil
import sys
from pathlib import Path, PurePosixPath

SPECIAL = {"README.md": "Home", "_Sidebar.md": "_Sidebar"}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}
# ](target) in Markdown and src="target" in inline HTML.
LINK = re.compile(r'(\]\(|src=")([^)"\s]+)([)"])')


def page_name(rel: PurePosixPath, text: str) -> str:
    if str(rel) in SPECIAL:
        return SPECIAL[str(rel)]
    heading = next((line[2:].strip() for line in text.splitlines() if line.startswith("# ")), rel.stem)
    words = re.sub(r"[^\w\s-]", " ", heading).split()
    return "-".join(words)


def resolve(base: PurePosixPath, target: str) -> PurePosixPath:
    parts: list[str] = []
    for part in (base.parent / target).parts:
        if part == "..":
            if parts:
                parts.pop()
            else:
                parts.append("..")
        elif part != ".":
            parts.append(part)
    return PurePosixPath(*parts)


def main() -> int:
    docs, wiki, repo = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    blob = f"https://github.com/{repo}/blob/main"
    tree = f"https://github.com/{repo}/tree/main"
    repo_root = docs.resolve().parent

    sources = {PurePosixPath(p.relative_to(docs).as_posix()): p.read_text(encoding="utf-8")
               for p in sorted(docs.rglob("*.md"))}
    names = {rel: page_name(rel, text) for rel, text in sources.items()}
    clashes = {n for n in names.values() if list(names.values()).count(n) > 1}
    if clashes:
        print(f"two docs would publish to the same wiki page: {sorted(clashes)}", file=sys.stderr)
        return 1

    for old in wiki.iterdir():
        if old.name.startswith("."):
            continue
        shutil.rmtree(old) if old.is_dir() else old.unlink()

    def rewrite(rel: PurePosixPath, match: re.Match) -> str:
        opening, target, closing = match.groups()
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith(("#", "/")):
            return match.group(0)
        path, _, anchor = target.partition("#")
        anchor = f"#{anchor}" if anchor else ""
        resolved = resolve(rel, path)
        if resolved in names:
            return f"{opening}{names[resolved]}{anchor}{closing}"
        if resolved.parts and resolved.parts[0] != ".." and resolved.suffix.lower() in IMAGE_SUFFIXES:
            dest = wiki / "assets" / resolved
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(docs / resolved, dest)
            return f"{opening}assets/{resolved}{closing}"
        in_repo = resolve(PurePosixPath("docs/x"), str(resolved))
        base = tree if (repo_root / in_repo).is_dir() else blob
        return f"{opening}{base}/{in_repo}{anchor}{closing}"

    for rel, text in sources.items():
        body = LINK.sub(lambda m: rewrite(rel, m), text)
        (wiki / f"{names[rel]}.md").write_text(body, encoding="utf-8")
    (wiki / "_Footer.md").write_text(
        "This wiki is generated from the [`docs/` folder](" + f"{tree}/docs" + "). "
        "Edit the files there with a pull request; changes made here are overwritten.\n",
        encoding="utf-8")
    print(f"published {len(sources)} pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
