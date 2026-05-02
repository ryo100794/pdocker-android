#!/usr/bin/env python3
"""Ensure bundled Dockerfiles stay on Docker's standard instruction surface."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {
    "ADD",
    "ARG",
    "CMD",
    "COPY",
    "ENTRYPOINT",
    "ENV",
    "EXPOSE",
    "FROM",
    "HEALTHCHECK",
    "LABEL",
    "MAINTAINER",
    "ONBUILD",
    "RUN",
    "SHELL",
    "STOPSIGNAL",
    "USER",
    "VOLUME",
    "WORKDIR",
}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def logical_lines(path: Path) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    current = ""
    start_line = 0
    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not current:
            start_line = lineno
        if raw.rstrip().endswith("\\"):
            current += raw.rstrip()[:-1] + " "
            continue
        current += raw
        out.append((start_line, current.strip()))
        current = ""
    if current:
        out.append((start_line, current.strip()))
    return out


def main() -> int:
    dockerfiles = sorted((ROOT / "app" / "src" / "main" / "assets").rglob("Dockerfile"))
    if not dockerfiles:
        fail("no bundled Dockerfiles found")
    for path in dockerfiles:
        rel = path.relative_to(ROOT)
        for lineno, line in logical_lines(path):
            instr = line.split(None, 1)[0].upper()
            if instr not in ALLOWED:
                fail(f"{rel}:{lineno} uses non-standard Dockerfile instruction {instr!r}")
            if instr.startswith("PDOCKER"):
                fail(f"{rel}:{lineno} uses pdocker-specific Dockerfile syntax")
    print("ok: bundled Dockerfiles use standard Dockerfile instructions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
