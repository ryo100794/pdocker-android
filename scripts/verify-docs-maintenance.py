#!/usr/bin/env python3
"""Verify documentation maintenance inventory and local Markdown links."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
BACKLOG = ROOT / "docs" / "maintenance" / "DOCUMENTATION_DEDUP_BACKLOG.md"

GROUP_RE = re.compile(r"^###\s+(\d+)\.\s+(.+?)\s*$", re.MULTILINE)
INLINE_LINK_RE = re.compile(r"!?\[[^\]\n]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
REFERENCE_LINK_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s+(\S+)", re.MULTILINE)


class CheckFailure(Exception):
    pass


@dataclass(frozen=True)
class LinkIssue:
    path: Path
    line: int
    target: str
    message: str


def rel(path: Path, root: Path = ROOT) -> str:
    return path.relative_to(root).as_posix()


def fail(message: str) -> None:
    raise CheckFailure(message)


def read_text(path: Path, root: Path = ROOT) -> str:
    if not path.is_file():
        fail(f"missing required file: {rel(path, root)}")
    return path.read_text(encoding="utf-8")


def check_backlog(root: Path = ROOT) -> None:
    backlog = root / "docs" / "maintenance" / "DOCUMENTATION_DEDUP_BACKLOG.md"
    text = read_text(backlog, root)
    groups = list(GROUP_RE.finditer(text))
    numbers = [int(match.group(1)) for match in groups]
    if numbers != list(range(1, 9)):
        fail(
            f"{rel(backlog, root)} must contain exactly 8 numbered backlog groups "
            f"(found {numbers or 'none'})"
        )

    for index, match in enumerate(groups):
        start = match.end()
        end = groups[index + 1].start() if index + 1 < len(groups) else len(text)
        section = text[start:end]
        if "Canonical owners:" not in section:
            fail(
                f"{rel(backlog, root)} group {match.group(1)} "
                f"({match.group(2)}) is missing a 'Canonical owners:' section"
            )
        owner_block = section.split("Canonical owners:", 1)[1].split("Backlog:", 1)[0]
        owner_lines = [line for line in owner_block.splitlines() if line.startswith("- ")]
        if not owner_lines:
            fail(
                f"{rel(backlog, root)} group {match.group(1)} "
                "must list at least one canonical owner"
            )

    if "There are 8 active deduplication backlog groups" not in text:
        fail(f"{rel(backlog, root)} must keep the open backlog count at 8")


def is_external_or_nonlocal(target: str) -> bool:
    parsed = urlparse(target)
    return bool(parsed.scheme) or target.startswith("//")


def normalize_target(raw: str) -> str:
    return raw.strip().strip("<>")


def iter_markdown_link_targets(text: str) -> list[tuple[int, str]]:
    targets: list[tuple[int, str]] = []
    for regex in (INLINE_LINK_RE, REFERENCE_LINK_RE):
        for match in regex.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            targets.append((line, normalize_target(match.group(1))))
    return targets


def check_local_markdown_links(root: Path = ROOT) -> list[LinkIssue]:
    docs = root / "docs"
    issues: list[LinkIssue] = []
    for path in sorted(docs.rglob("*.md")):
        text = path.read_text(encoding="utf-8")
        for line, target in iter_markdown_link_targets(text):
            if not target or target.startswith("#") or is_external_or_nonlocal(target):
                continue
            path_part = unquote(target.split("#", 1)[0])
            if not path_part:
                continue
            candidate = (path.parent / path_part).resolve()
            try:
                candidate.relative_to(root.resolve())
            except ValueError:
                issues.append(LinkIssue(path, line, target, "escapes repository root"))
                continue
            if not candidate.exists():
                issues.append(LinkIssue(path, line, target, "target does not exist"))
    return issues


def check_links(root: Path = ROOT) -> None:
    issues = check_local_markdown_links(root)
    if issues:
        rendered = "; ".join(
            f"{rel(issue.path, root)}:{issue.line}: {issue.target} ({issue.message})"
            for issue in issues[:20]
        )
        suffix = "" if len(issues) <= 20 else f"; ... and {len(issues) - 20} more"
        fail(f"local markdown link check failed: {rendered}{suffix}")


def run(root: Path = ROOT) -> None:
    check_backlog(root)
    check_links(root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        run(root)
    except CheckFailure as exc:
        print(f"verify-docs-maintenance: FAIL: {exc}", file=sys.stderr)
        return 1
    print("verify-docs-maintenance: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
