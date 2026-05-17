#!/usr/bin/env python3
"""Exercise the llama-cpp-gpu entrypoint startup logging contract.

The test stubs pdocker-gpu-profile, forces a missing model so the script stops
at its status page, and verifies early tee logging plus llama-startup.json
content without building llama.cpp or downloading a model.
"""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
START = ROOT / "app/src/main/assets/project-library/llama-cpp-gpu/scripts/start-llama-server.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pdocker-llama-startup-") as td:
        tmp = Path(td)
        fakebin = tmp / "bin"
        profiles = tmp / "profiles"
        logs = tmp / "logs"
        fakebin.mkdir()
        profiles.mkdir()
        logs.mkdir()

        profiler = fakebin / "pdocker-gpu-profile"
        profiler.write_text(
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            "out=\"$1\"\n"
            "diag=\"${LLAMA_GPU_DIAGNOSTICS:?}\"\n"
            "mkdir -p \"$(dirname \"$out\")\" \"$(dirname \"$diag\")\"\n"
            "echo PROFILE_STDOUT_SENTINEL\n"
            "echo PROFILE_STDERR_SENTINEL >&2\n"
            "cat >\"$out\" <<'ENV'\n"
            "export LLAMA_GPU_BACKEND=vulkan\n"
            "export LLAMA_ARG_N_GPU_LAYERS=3\n"
            "export LLAMA_ARG_CTX=2048\n"
            "export LLAMA_ARG_THREADS=7\n"
            "export VK_ICD_FILENAMES=/tmp/fake-vulkan.json\n"
            "export PDOCKER_GPU_QUEUE_SOCKET=/tmp/pdocker-gpu.sock\n"
            "export PDOCKER_VULKAN_ICD_KIND=pdocker-adreno\n"
            "export PDOCKER_VULKAN_ICD_READY=0\n"
            "ENV\n"
            "printf '{\"backend\":\"vulkan\"}\n' >\"$diag\"\n",
            encoding="utf-8",
        )
        profiler.chmod(0o755)

        log_file = logs / "llama-server.log"
        startup_json = logs / "llama-startup.json"
        env = os.environ.copy()
        env.update(
            {
                "PATH": f"{fakebin}:{env.get('PATH', '')}",
                "LLAMA_LOG_FILE": str(log_file),
                "LLAMA_STARTUP_JSON": str(startup_json),
                "LLAMA_GPU_PROFILE": str(profiles / "pdocker-gpu.env"),
                "LLAMA_GPU_DIAGNOSTICS": str(profiles / "pdocker-gpu-diagnostics.json"),
                "LLAMA_GPU_PROFILE_REFRESH": "always",
                "LLAMA_ARG_MODEL": str(tmp / "missing.gguf"),
                "LLAMA_MODEL_URL": "",
                "LLAMA_ARG_PORT": "0",
                "LLAMA_EXTRA_ARGS": "--jinja",
            }
        )
        proc = subprocess.run(
            ["timeout", "8", "bash", str(START)],
            cwd=tmp,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        require(proc.returncode in {0, 124}, f"entrypoint exited unexpectedly: rc={proc.returncode}\n{proc.stdout}")
        require(log_file.exists(), "LLAMA_LOG_FILE was not created")
        log_text = log_file.read_text(encoding="utf-8", errors="replace")
        combined_log = proc.stdout + "\n" + log_text
        require("PROFILE_STDOUT_SENTINEL" in combined_log, "early tee missed profile stdout")
        require("PROFILE_STDERR_SENTINEL" in combined_log, "early tee missed profile stderr")
        require(startup_json.exists(), "llama-startup.json was not written")

        report = json.loads(startup_json.read_text(encoding="utf-8"))
        require(report["profile_path"] == env["LLAMA_GPU_PROFILE"], "startup JSON profile path mismatch")
        require(report["profile_refresh_rc"] == 0, "startup JSON profile refresh rc missing/incorrect")
        resolved = report.get("resolved", {})
        for key, expected in {
            "LLAMA_GPU_BACKEND": "vulkan",
            "LLAMA_ARG_N_GPU_LAYERS": "3",
            "LLAMA_ARG_CTX": "2048",
            "LLAMA_ARG_THREADS": "7",
            "VK_ICD_FILENAMES": "/tmp/fake-vulkan.json",
            "PDOCKER_GPU_QUEUE_SOCKET": "/tmp/pdocker-gpu.sock",
            "PDOCKER_VULKAN_ICD_KIND": "pdocker-adreno",
            "PDOCKER_VULKAN_ICD_READY": "0",
        }.items():
            require(resolved.get(key) == expected, f"resolved {key} mismatch: {resolved.get(key)!r}")
        require(report.get("memory", {}).get("MemAvailable"), "MemAvailable missing from startup JSON")
        require("SwapFree" in report.get("memory", {}), "SwapFree missing from startup JSON")
        argv = report.get("llama_server_argv", [])
        require("--no-kv-offload" in argv, "llama-server argv missing KV offload guard")
        guard = report.get("kv_offload_guard", {})
        require(guard.get("active") is True, "KV offload guard state not recorded as active")
        require(guard.get("added_arg") is True, "KV offload guard added_arg not recorded")
        require(guard.get("disabled_effective") is True, "KV offload disabled_effective not recorded")

    print("ok: llama startup logging captures profile generation and startup JSON contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
