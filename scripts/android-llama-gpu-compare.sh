#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="${ADB:-adb}"
PKG="${PDOCKER_PACKAGE:-io.github.ryo100794.pdocker.compat}"
CONTAINER="${PDOCKER_LLAMA_CONTAINER:-pdocker-llama-cpp}"
IMAGE="${PDOCKER_LLAMA_IMAGE:-pdocker/llama-cpp-gpu:latest}"
PROJECT="${PDOCKER_LLAMA_PROJECT:-files/pdocker/projects/llama-cpp-gpu}"
LOCAL_PORT="${PDOCKER_LLAMA_LOCAL_PORT:-28081}"
REMOTE_PORT="${PDOCKER_LLAMA_REMOTE_PORT:-18081}"
CPU_CTX="${PDOCKER_LLAMA_CPU_CTX:-2048}"
GPU_CTX="${PDOCKER_LLAMA_GPU_CTX:-512}"
GPU_LAYERS="${PDOCKER_LLAMA_GPU_LAYERS:-1}"
PREDICT="${PDOCKER_LLAMA_BENCH_PREDICT:-4}"
REPEAT="${PDOCKER_LLAMA_BENCH_REPEAT:-1}"
OUT="${PDOCKER_LLAMA_COMPARE_OUT:-$ROOT/docs/test/llama-gpu-compare-latest.json}"
RESTORE_CPU=1

usage() {
  cat <<EOF
Usage: $0 [--out PATH] [--gpu-layers N] [--gpu-ctx N] [--cpu-ctx N] [--predict N] [--repeat N] [--no-restore]

Runs a repeatable Android llama.cpp CPU/GPU comparison scenario without
modifying llama.cpp:
  1. start the project-library llama container in CPU mode and benchmark HTTP;
  2. start the same image in forced Vulkan mode and capture model-load/serve status;
  3. write a JSON report with CPU speed, GPU load evidence, and the 10x gap;
  4. restore the CPU server unless --no-restore is passed.

This script uses the test-staged Docker CLI only as a diagnostic harness. The
APK product path remains the engine/UI route.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT="$2"; shift ;;
    --gpu-layers) GPU_LAYERS="$2"; shift ;;
    --gpu-ctx) GPU_CTX="$2"; shift ;;
    --cpu-ctx) CPU_CTX="$2"; shift ;;
    --predict) PREDICT="$2"; shift ;;
    --repeat) REPEAT="$2"; shift ;;
    --no-restore) RESTORE_CPU=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$(dirname "$OUT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; "$ADB" forward --remove "tcp:'"$LOCAL_PORT"'" >/dev/null 2>&1 || true' EXIT

remote_quote() {
  printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

run_as() {
  "$ADB" shell "run-as $PKG sh -c $(remote_quote "$1")"
}

docker_prefix='cd files && export PATH="$PWD/pdocker-runtime/docker-bin:$PATH" DOCKER_CONFIG="$PWD/pdocker-runtime/docker-bin" DOCKER_HOST="unix://$PWD/pdocker/pdockerd.sock" DOCKER_BUILDKIT=0'
project_prefix='cd '"$PROJECT"' && export PATH="$PWD/../../../pdocker-runtime/docker-bin:$PATH" DOCKER_CONFIG="$PWD/../../../pdocker-runtime/docker-bin" DOCKER_HOST="unix://$PWD/../../pdockerd.sock" DOCKER_BUILDKIT=0'

wait_server() {
  local seconds="$1"
  "$ADB" forward --remove "tcp:$LOCAL_PORT" >/dev/null 2>&1 || true
  "$ADB" forward "tcp:$LOCAL_PORT" "tcp:$REMOTE_PORT" >/dev/null
  for _ in $(seq 1 "$seconds"); do
    if curl -fsS --max-time 2 "http://127.0.0.1:$LOCAL_PORT/v1/models" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

container_logs() {
  run_as "$docker_prefix; docker logs --tail 320 $(printf "%q" "$CONTAINER") 2>&1 || true"
}

container_state() {
  run_as "$docker_prefix; docker ps -a --filter name=$(printf "%q" "$CONTAINER") 2>&1 || true"
}

start_cpu() {
  run_as "$project_prefix; docker rm -f $(printf "%q" "$CONTAINER") >/dev/null 2>&1 || true; docker run -d --name $(printf "%q" "$CONTAINER") --gpus all -p $REMOTE_PORT:$REMOTE_PORT -v \"\$PWD/models:/models\" -v \"\$PWD/workspace:/workspace\" -v \"\$PWD/profiles:/profiles\" -e PDOCKER_GPU=auto -e PDOCKER_GPU_AUTO=1 -e PDOCKER_GPU_MODE=cpu -e LLAMA_ARG_MODEL=/models/model.gguf -e LLAMA_ARG_CTX=$CPU_CTX -e LLAMA_ARG_PORT=$REMOTE_PORT -e LLAMA_LOG_FILE=/workspace/logs/llama-server.log $(printf "%q" "$IMAGE")"
}

start_gpu() {
  run_as "$project_prefix; docker rm -f $(printf "%q" "$CONTAINER") >/dev/null 2>&1 || true; docker run -d --name $(printf "%q" "$CONTAINER") --gpus all -p $REMOTE_PORT:$REMOTE_PORT -v \"\$PWD/models:/models\" -v \"\$PWD/workspace:/workspace\" -v \"\$PWD/profiles:/profiles\" -e PDOCKER_GPU=auto -e PDOCKER_GPU_AUTO=1 -e PDOCKER_GPU_MODE=vulkan-raw -e PDOCKER_VULKAN_ICD_TRACE_ALLOC=1 -e GGML_VK_FORCE_MAX_BUFFER_SIZE=8589934592 -e GGML_VK_FORCE_MAX_ALLOCATION_SIZE=8589934592 -e LLAMA_ARG_MODEL=/models/model.gguf -e LLAMA_ARG_CTX=$GPU_CTX -e LLAMA_ARG_N_GPU_LAYERS=$GPU_LAYERS -e LLAMA_ARG_PORT=$REMOTE_PORT -e LLAMA_LOG_FILE=/workspace/logs/llama-server.log $(printf "%q" "$IMAGE")"
}

bench_http() {
  local mode="$1"
  local out="$2"
  PDOCKER_LLAMA_LOCAL_PORT="$LOCAL_PORT" \
    PDOCKER_LLAMA_REMOTE_PORT="$REMOTE_PORT" \
    PDOCKER_LLAMA_BENCH_PREDICT="$PREDICT" \
    PDOCKER_LLAMA_BENCH_REPEAT="$REPEAT" \
    PDOCKER_LLAMA_BENCH_MODE="$mode" \
    PDOCKER_LLAMA_BENCH_OUT="$out" \
    "$ROOT/scripts/android-llama-bench.sh"
}

echo "[pdocker llama compare] start CPU baseline"
start_cpu >/dev/null
if ! wait_server 90; then
  echo "CPU server did not become reachable" >&2
  container_state >&2
  container_logs >&2
  exit 1
fi
CPU_JSON="$TMP/cpu.json"
bench_http "cpu-baseline" "$CPU_JSON" >/dev/null

echo "[pdocker llama compare] start forced Vulkan run"
start_gpu >/dev/null
GPU_LOG="$TMP/gpu.log"
GPU_STATE="$TMP/gpu-state.txt"
GPU_JSON="$TMP/gpu.json"
if wait_server 120; then
  bench_http "vulkan-forced-ngl-$GPU_LAYERS" "$GPU_JSON" >/dev/null || true
  gpu_served=1
else
  gpu_served=0
  container_state > "$GPU_STATE"
  container_logs > "$GPU_LOG"
fi

python3 - "$CPU_JSON" "$GPU_JSON" "$GPU_LOG" "$GPU_STATE" "$OUT" "$gpu_served" "$GPU_LAYERS" "$GPU_CTX" <<'PY'
import json
import re
import sys
import time
from pathlib import Path

cpu_path, gpu_path, gpu_log_path, gpu_state_path, out_path, gpu_served_s, gpu_layers, gpu_ctx = sys.argv[1:9]
cpu = json.load(open(cpu_path, encoding="utf-8"))
gpu = {}
if Path(gpu_path).is_file() and Path(gpu_path).stat().st_size:
    try:
        gpu = json.load(open(gpu_path, encoding="utf-8"))
    except Exception:
        gpu = {}
log = Path(gpu_log_path).read_text(encoding="utf-8", errors="replace") if Path(gpu_log_path).is_file() else ""
state = Path(gpu_state_path).read_text(encoding="utf-8", errors="replace") if Path(gpu_state_path).is_file() else ""
cpu_tps = float(cpu.get("summary", {}).get("predicted_tokens_per_second_mean") or 0.0)
gpu_tps = float(gpu.get("summary", {}).get("predicted_tokens_per_second_mean") or 0.0)
target_tps = cpu_tps * 10.0
evidence = {
    "vulkan_device_seen": "Vulkan0 (pdocker Vulkan bridge" in log,
    "offload_seen": "offloading" in log,
    "model_loaded": "main: model loaded" in log,
    "serve_reachable": bool(int(gpu_served_s)),
    "buffer_allocation_blocker": "unable to allocate Vulkan0 buffer" in log,
    "assert_blocker": "GGML_ASSERT" in log,
    "gpu_model_buffer_seen": "Vulkan0 model buffer size" in log,
    "queue_submit_blocker": "vk::Queue::submit: ErrorFeatureNotPresent" in log,
    "spirv_dispatch_blocker": "real SPIR-V dispatch is not lowered yet" in log or "vk::Queue::submit: ErrorFeatureNotPresent" in log,
}
allocations = [
    int(m.group(1))
    for m in re.finditer(r"pdocker-vulkan-icd: allocate ([0-9]+) bytes", log)
]
result = {
    "schema": "pdocker.llama.gpu.compare.v1",
    "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "policy": {
        "llama_cpp_modified": False,
        "gpu_entry": "standard Vulkan loader through pdocker-vulkan-icd.so",
        "target_speedup": 10.0,
    },
    "settings": {
        "gpu_layers": int(gpu_layers),
        "gpu_ctx": int(gpu_ctx),
    },
    "cpu": {
        "tokens_per_second": cpu_tps,
        "summary": cpu.get("summary", {}),
    },
    "gpu": {
        "tokens_per_second": gpu_tps,
        "summary": gpu.get("summary", {}),
        "served": bool(int(gpu_served_s)),
        "state_excerpt": state[:2000],
        "log_excerpt": log[-12000:],
        "evidence": evidence,
        "allocation_trace_bytes": allocations[-32:],
    },
    "comparison": {
        "speedup": (gpu_tps / cpu_tps) if cpu_tps and gpu_tps else 0.0,
        "target_tokens_per_second": target_tps,
        "target_met": bool(cpu_tps and gpu_tps >= target_tps),
    },
    "next_blocker": (
        "split 4GiB+ Vulkan buffers / pinned host-buffer path"
        if evidence["buffer_allocation_blocker"] or evidence["assert_blocker"]
        else "lower llama.cpp SPIR-V dispatch into the Android GPU executor"
        if evidence["vulkan_device_seen"] or evidence["queue_submit_blocker"]
        else "make Vulkan device discovery reliable"
    ),
}
Path(out_path).write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(json.dumps(result["comparison"], indent=2))
print("next_blocker:", result["next_blocker"])
PY

DEVICE_NAME="$(basename "$OUT")"
"$ADB" push "$OUT" "/data/local/tmp/$DEVICE_NAME" >/dev/null
run_as "mkdir -p files/pdocker/bench && cp /data/local/tmp/$DEVICE_NAME files/pdocker/bench/$DEVICE_NAME"

if [[ "$RESTORE_CPU" -eq 1 ]]; then
  echo "[pdocker llama compare] restore CPU server"
  start_cpu >/dev/null
  wait_server 90 >/dev/null || true
fi

echo "[pdocker llama compare] local: $OUT"
echo "[pdocker llama compare] device: files/pdocker/bench/$DEVICE_NAME"
