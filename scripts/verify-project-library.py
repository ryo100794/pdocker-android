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
    compare_script = read(ROOT / "scripts" / "android-llama-gpu-compare.sh")
    compare_doc = read(ROOT / "docs" / "test" / "LLAMA_BENCHMARKS.md")
    compare_todo = read(ROOT / "docs" / "plan" / "TODO.md")
    compare_expectations = {
        "compare script schema": "pdocker.llama.gpu.compare.v1" in compare_script,
        "compare script leaves llama.cpp unmodified": '"llama_cpp_modified": False' in compare_script,
        "compare script records 10x target": '"target_speedup": 10.0' in compare_script and "target_tps = cpu_tps * 10.0" in compare_script,
        "compare script captures Vulkan allocation trace": "PDOCKER_VULKAN_ICD_TRACE_ALLOC=1" in compare_script and "allocation_trace_bytes" in compare_script,
        "compare script uses standard Vulkan entry": "standard Vulkan loader through pdocker-vulkan-icd.so" in compare_script,
        "compare script classifies dispatch blocker": "queue_submit_blocker" in compare_script and "vk::Queue::submit: ErrorFeatureNotPresent" in compare_script,
        "compare script restores CPU server": "restore CPU server" in compare_script and "start_cpu" in compare_script,
        "compare docs record latest report": "llama-gpu-compare-latest.json" in compare_doc,
        "compare todo records 10x task list": "llama.cpp Container GPU 10x Task List" in compare_todo,
        "compare todo preserves no llama patch policy": "llama.cpp source must remain unmodified" in compare_todo,
    }
    for name, passed in compare_expectations.items():
        if not passed:
            fail(name)
    ok("llama gpu 10x comparison scenario is recorded")

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
    dev_workspace_extensions = read(dev_root / "workspace" / ".vscode" / "extensions.json")
    for token in (
        "code-server",
        "Continue.continue",
        "@openai/codex",
        "OpenAI.chatgpt",
        "@anthropic-ai/claude-code",
        "Anthropic.claude-code",
        "ANTHROPIC_API_KEY",
        "gpus: all",
        "18080:18080",
        "# pdocker.service-url: 18080=VS Code",
        "# pdocker.auto-open: VS Code",
    ):
        if token not in dev_compose + dev_dockerfile + dev_workspace_extensions:
            fail(f"dev-workspace missing {token}")
    ok("dev-workspace includes code-server, Continue, Codex, Claude Code, and GPU request")

    llama = templates["llama-cpp-gpu"]
    llama_root = ASSETS / llama["assetPath"]
    llama_compose = read(llama_root / llama["compose"])
    llama_dockerfile = read(llama_root / llama["dockerfile"])
    profile = read(llama_root / "scripts" / "pdocker-gpu-profile.sh")
    start = read(llama_root / "scripts" / "start-llama-server.sh")

    expectations = {
        "compose gpus all": "gpus: all" in llama_compose,
        "compose exposes build parallelism": "LLAMA_CPP_BUILD_JOBS" in llama_compose,
        "compose model volume": "./models:/models" in llama_compose,
        "compose model url syntax": re.search(r'\$\{LLAMA_MODEL_URL:-[^}]+\}', llama_compose) is not None,
        "Dockerfile modern Vulkan headers": "FROM ubuntu:24.04" in llama_dockerfile,
        "Dockerfile llama.cpp source": "ggml-org/llama.cpp" in llama_dockerfile,
        "Dockerfile keeps llama engine local": "-DGGML_RPC=ON" not in llama_dockerfile and "LLAMA_ARG_RPC" not in llama_compose + start,
        "Dockerfile Vulkan build": "-DGGML_VULKAN=ON" in llama_dockerfile,
        "Dockerfile Vulkan shader compiler": "glslc" in llama_dockerfile,
        "Dockerfile SPIR-V headers": "spirv-headers" in llama_dockerfile and "spirv-tools" in llama_dockerfile,
        "Dockerfile OpenBLAS build": "-DGGML_BLAS=ON" in llama_dockerfile,
        "Dockerfile server-only build target": "--target llama-server" in llama_dockerfile and "--parallel" in llama_dockerfile,
        "Dockerfile bounded build jobs": "ARG LLAMA_CPP_BUILD_JOBS=2" in llama_dockerfile and 'jobs="${LLAMA_CPP_BUILD_JOBS:-2}"' in llama_dockerfile,
        "Dockerfile log directory": "/workspace/logs" in llama_dockerfile and "/var/log/pdocker" in llama_dockerfile,
        "Dockerfile llama healthcheck": "HEALTHCHECK" in llama_dockerfile and "/health" in llama_dockerfile and "/v1/models" in llama_dockerfile,
        "profile Vulkan detection": "PDOCKER_VULKAN_PASSTHROUGH" in profile,
        "profile CUDA compat detection": "PDOCKER_CUDA_COMPAT" in profile,
        "profile gates unfinished pdocker Vulkan before CUDA compat": profile.find('pdocker_vulkan_icd_signal" = "true"') < profile.find('mode" = "cuda"'),
        "profile CPU fallback": re.search(r'backend="cpu"', profile) is not None,
        "profile diagnostics json": "-diagnostics.json" in profile and '"signals"' in profile and "json_escape" in profile,
        "profile quotes extra args for source": "shell_quote" in profile and 'LLAMA_EXTRA_ARGS=$(shell_quote "$extra")' in profile,
        "start sources profile": "source \"$profile\"" in start,
        "start shows diagnostics": "LLAMA_GPU_DIAGNOSTICS" in start and "llama.cpp gpu diagnostics" in start,
        "start hides gpu env during cpu fallback": 'LLAMA_GPU_BACKEND:-cpu' in start and 'export GGML_VK_VISIBLE_DEVICES=""' in start and "unset VK_ICD_FILENAMES" in start and "unset OCL_ICD_VENDORS" in start,
        "start passes gpu layers": "--n-gpu-layers" in start,
        "llama default port offset": "18081:18081" in llama_compose and "18081" in start,
        "llama service shortcut comment": "# pdocker.service-url: 18081=llama.cpp" in llama_compose,
        "llama default 8b model": "Qwen/Qwen3-8B-GGUF" in llama_compose and "Qwen3-8B-Q4_K_M.gguf" in llama_compose,
        "llama optional model download": "LLAMA_MODEL_URL" in llama_compose and "curl -fL" in start and "-C -" in start,
        "llama default chat template": "--jinja" in start,
        "llama docker logs stream": "LLAMA_LOG_FILE" in llama_compose and "tee -a \"$log_file\"" in start and "stdbuf -oL -eL" in start,
        "llama missing-model status page": "http.server" in start and "waiting for a GGUF model" in start,
    }
    for name, passed in expectations.items():
        if not passed:
            fail(name)
    ok("llama-cpp-gpu template has compose, Dockerfile, GPU profile, and server entrypoint")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
