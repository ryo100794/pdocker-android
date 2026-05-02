#!/usr/bin/env python3
"""Offline checks for bundled pdocker project-library templates."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "app" / "src" / "main" / "assets"
LIBRARY = ASSETS / "project-library" / "library.json"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def ok(msg: str) -> None:
    print(f"ok: {msg}")


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"missing {path.relative_to(ROOT)}")
    return path.read_text()


def main() -> int:
    data = json.loads(read(LIBRARY))
    templates = {item["id"]: item for item in data.get("templates", [])}
    for tid in ("dev-workspace", "llama-cpp-gpu"):
        if tid not in templates:
            fail(f"template {tid} absent from library.json")
    ok("required templates listed")

    dev = templates["dev-workspace"]
    dev_root = ASSETS / dev["assetPath"]
    dev_compose = read(dev_root / dev["compose"])
    dev_dockerfile = read(dev_root / dev["dockerfile"])
    for token in ("code-server", "Continue.continue", "@openai/codex", "gpus: all", "18080:18080"):
        if token not in dev_compose + dev_dockerfile:
            fail(f"dev-workspace missing {token}")
    ok("dev-workspace includes code-server, Continue, Codex, and GPU request")

    llama = templates["llama-cpp-gpu"]
    llama_root = ASSETS / llama["assetPath"]
    llama_compose = read(llama_root / llama["compose"])
    llama_dockerfile = read(llama_root / llama["dockerfile"])
    profile = read(llama_root / "scripts" / "pdocker-gpu-profile.sh")
    start = read(llama_root / "scripts" / "start-llama-server.sh")

    expectations = {
        "compose gpus all": "gpus: all" in llama_compose,
        "compose model volume": "./models:/models" in llama_compose,
        "Dockerfile llama.cpp source": "ggml-org/llama.cpp" in llama_dockerfile,
        "Dockerfile Vulkan build": "-DGGML_VULKAN=ON" in llama_dockerfile,
        "Dockerfile OpenBLAS build": "-DGGML_BLAS=ON" in llama_dockerfile,
        "profile Vulkan detection": "PDOCKER_VULKAN_PASSTHROUGH" in profile,
        "profile CUDA compat detection": "PDOCKER_CUDA_COMPAT" in profile,
        "profile CPU fallback": re.search(r'backend="cpu"', profile) is not None,
        "start sources profile": "source \"$profile\"" in start,
        "start passes gpu layers": "--n-gpu-layers" in start,
        "llama default port offset": "18081:18081" in llama_compose and "18081" in start,
        "llama default gpt-oss model": "ggml-org/gpt-oss-20b-GGUF" in llama_compose and "gpt-oss-20b-mxfp4.gguf" in llama_compose,
        "llama optional model download": "LLAMA_MODEL_URL" in llama_compose and "curl -fL" in start and "-C -" in start,
        "llama default chat template": "--jinja" in start,
        "llama missing-model status page": "python3 -m http.server" in start and "waiting for a GGUF model" in start,
    }
    for name, passed in expectations.items():
        if not passed:
            fail(name)
    ok("llama-cpp-gpu template has compose, Dockerfile, GPU profile, and server entrypoint")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
