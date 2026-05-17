#!/usr/bin/env python3
"""Validate the top-level scripts inventory.

The inventory is deliberately separate from any future directory reshuffle:
top-level script paths remain stable public entrypoints until a wrapper/shim
exists and all references are migrated.
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
MANIFEST = SCRIPTS / "script-inventory.json"
README = SCRIPTS / "README.md"
IGNORED_TOP_LEVEL = {
    "README.md",
    "script-inventory.json",
    "__pycache__",
}
REQUIRED_CATEGORIES = {
    "runtime-package-needed",
    "build-developer",
    "test-verification",
    "generated-maintenance",
    "obsolete-suspect",
}
REQUIRED_STABLE_ENTRYPOINTS = {
    "scripts/build-all.sh",
    "scripts/build-apk.sh",
    "scripts/verify-fast.sh",
    "scripts/verify-heavy.sh",
    "scripts/pdocker-test-driver.py",
    "scripts/android-selfdebug.sh",
}
KNOWN_OBSOLETE_SUSPECTS = {
    "scripts/android-terminal-it-repro.sh",
    "scripts/verify-llama-startup-logging.py",
    "scripts/wrap-ndk-box64.sh",
}


def load_manifest() -> dict[str, Any]:
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SystemExit(f"missing manifest: {MANIFEST}")
    if data.get("schema") != "pdocker.script-inventory.v1":
        raise SystemExit("script inventory schema mismatch")
    return data


def top_level_script_paths() -> set[str]:
    result: set[str] = set()
    for path in SCRIPTS.iterdir():
        if path.name in IGNORED_TOP_LEVEL or path.is_dir():
            continue
        result.add(f"scripts/{path.name}")
    return result


def fail(message: str) -> None:
    raise SystemExit(f"verify-script-inventory: FAIL: {message}")


def main() -> int:
    data = load_manifest()
    entries = data.get("entries")
    if not isinstance(entries, list):
        fail("entries must be a list")

    paths: list[str] = []
    categories: Counter[str] = Counter()
    stable_paths: set[str] = set()
    obsolete_paths: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            fail(f"entry {index} is not an object")
        path = entry.get("path")
        category = entry.get("category")
        stability = entry.get("stability")
        role = entry.get("role")
        if not isinstance(path, str) or not path.startswith("scripts/"):
            fail(f"entry {index} has invalid path {path!r}")
        if category not in REQUIRED_CATEGORIES:
            fail(f"{path} has unknown category {category!r}")
        if not isinstance(stability, str) or not stability:
            fail(f"{path} has missing stability")
        if not isinstance(role, str) or not role:
            fail(f"{path} has missing role")
        paths.append(path)
        categories[category] += 1
        if stability == "stable-entrypoint":
            stable_paths.add(path)
        if category == "obsolete-suspect":
            obsolete_paths.add(path)

    path_set = set(paths)
    if len(path_set) != len(paths):
        duplicates = sorted(path for path in path_set if paths.count(path) > 1)
        fail(f"duplicate entries: {duplicates}")

    actual = top_level_script_paths()
    missing = sorted(actual - path_set)
    stale = sorted(path_set - actual)
    if missing or stale:
        fail(f"inventory drift missing={missing} stale={stale}")

    missing_stable = sorted(REQUIRED_STABLE_ENTRYPOINTS - stable_paths)
    if missing_stable:
        fail(f"stable entrypoints not marked stable: {missing_stable}")

    if obsolete_paths != KNOWN_OBSOLETE_SUSPECTS:
        fail(
            "obsolete-suspect set changed without verifier update: "
            f"expected={sorted(KNOWN_OBSOLETE_SUSPECTS)} observed={sorted(obsolete_paths)}"
        )

    if "runtime-package-needed" not in categories:
        fail("runtime package staging category is empty")

    readme = README.read_text(encoding="utf-8")
    for category, count in categories.items():
        row = f"| `{category}` | {count} |"
        if row not in readme:
            fail(f"README category count is stale for {category}: expected row prefix {row!r}")
    for path in REQUIRED_STABLE_ENTRYPOINTS:
        if f"`{path}`" not in readme:
            fail(f"README omits stable entrypoint {path}")

    print("verify-script-inventory: PASS")
    for category in sorted(REQUIRED_CATEGORIES):
        print(f"ok: {category} = {categories.get(category, 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
