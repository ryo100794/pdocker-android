#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="${ADB:-adb}"
PKG="${PDOCKER_PACKAGE:-io.github.ryo100794.pdocker.compat}"
CONTAINER="${PDOCKER_LLAMA_CONTAINER:-pdocker-llama-cpp}"
OUT="${PDOCKER_LLAMA_READINESS_OUT:-$ROOT/docs/test/llama-gpu-device-readiness-latest.json}"
MIN_AVAILABLE_MB="${PDOCKER_LLAMA_MIN_FREE_MB:-512}"
MIN_SWAP_FREE_MB="${PDOCKER_LLAMA_MIN_SWAP_FREE_MB:-1024}"

usage() {
  cat <<EOF
Usage: $0 [--out PATH]

Writes a JSON readiness report for the llama GPU bridge device run.  This is a
low-impact preflight: it does not start pdockerd, does not start containers, and
does not force-stop the browser or VS Code session.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT="$2"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$(dirname "$OUT")"

RAW_MEM="$("$ADB" shell 'cat /proc/meminfo 2>/dev/null' 2>/dev/null || true)"
RAW_PS="$("$ADB" shell "ps -A | grep -E 'pdocker|llama|chrome' || true" 2>/dev/null || true)"
SOCKET_STATE="$("$ADB" shell "run-as $PKG sh -c 'cd files 2>/dev/null && if test -S pdocker/pdockerd.sock; then echo present; else echo absent; fi' 2>/dev/null" 2>/dev/null | tr -d '\r' || true)"

python3 - "$OUT" "$MIN_AVAILABLE_MB" "$MIN_SWAP_FREE_MB" "$CONTAINER" "$RAW_MEM" "$RAW_PS" "$SOCKET_STATE" <<'PY'
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

out, min_available, min_swap, container, raw_mem, raw_ps, socket_state = sys.argv[1:8]
min_available = int(min_available)
min_swap = int(min_swap)

def kb_to_mb(value):
    return int(value) // 1024

memory = {
    "mem_total_mb": 0,
    "mem_free_mb": 0,
    "mem_available_mb": 0,
    "swap_total_mb": 0,
    "swap_free_mb": 0,
    "swap_cached_mb": 0,
}
for line in raw_mem.splitlines():
    m = re.match(r"^([A-Za-z_()]+):\s+([0-9]+)\s+kB$", line.strip())
    if not m:
        continue
    key, value = m.group(1), int(m.group(2))
    if key == "MemTotal":
        memory["mem_total_mb"] = kb_to_mb(value)
    elif key == "MemFree":
        memory["mem_free_mb"] = kb_to_mb(value)
    elif key == "MemAvailable":
        memory["mem_available_mb"] = kb_to_mb(value)
    elif key == "SwapTotal":
        memory["swap_total_mb"] = kb_to_mb(value)
    elif key == "SwapFree":
        memory["swap_free_mb"] = kb_to_mb(value)
    elif key == "SwapCached":
        memory["swap_cached_mb"] = kb_to_mb(value)

process_lines = [line for line in raw_ps.splitlines() if line.strip()]
stale_target_hint = any(container in line or "llama" in line.lower() for line in process_lines)
pdocker_process_hint = any("pdocker" in line for line in process_lines)
browser_hint = any("chrome" in line.lower() for line in process_lines)
ready = (
    memory["mem_available_mb"] >= min_available
    and memory["swap_free_mb"] >= min_swap
)
actions = []
if not ready:
    actions.append("Do not start the llama GPU compare/benchmark; readiness=false is a hard GPU-run stop.")
    actions.append("Do not classify compare, correctness, or benchmark claims from a run started while readiness=false.")
    if stale_target_hint:
        actions.append("Stop the pdocker llama container from the UI or Engine, then re-check readiness.")
    actions.append("Wait for Android reclaim or reboot the test device if SwapFree remains low.")
    actions.append("Do not force-stop the browser/VS Code session from automation.")
else:
    actions.append("Run scripts/android-llama-gpu-compare.sh with PDOCKER_GPU_CPU_ORACLE=1 and the Q6_K workgroup artifact path.")

report = {
    "schema": "pdocker.llama.gpu.device-readiness.v1",
    "timestamp_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "ready": ready,
    "gpu_run_allowed": ready,
    "claim_policy": {
        "readiness_false_blocks_gpu_run": True,
        "executor_marker_required_for_compare_claim": True,
        "cpu_comparison_required_for_benchmark_claim": True,
    },
    "required": {
        "mem_available_mb": min_available,
        "swap_free_mb": min_swap,
    },
    "memory": memory,
    "pdockerd_socket": socket_state.strip() or "unknown",
    "process_hints": {
        "pdocker_process_seen": pdocker_process_hint,
        "stale_target_hint": stale_target_hint,
        "browser_hint": browser_hint,
        "sample": process_lines[:24],
    },
    "device_actions": actions,
}
Path(out).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(json.dumps(report, indent=2, sort_keys=True))
raise SystemExit(0 if ready else 20)
PY
