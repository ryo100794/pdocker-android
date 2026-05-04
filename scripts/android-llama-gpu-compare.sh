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
OP_ID="llama-gpu-compare-$(date -u +%Y%m%dT%H%M%SZ)-$$"

usage() {
  cat <<EOF
Usage: $0 [--out PATH] [--gpu-layers N] [--gpu-ctx N] [--cpu-ctx N] [--predict N] [--repeat N] [--no-restore]

Runs a repeatable Android llama.cpp CPU/GPU comparison scenario without
modifying llama.cpp:
  1. start the project-library llama container in CPU mode and benchmark HTTP;
  2. start the same image in forced Vulkan mode and capture model-load/serve status;
  3. write a JSON report with CPU speed, GPU load evidence, and the 10x gap;
  4. restore the CPU server unless --no-restore is passed.

This script drives pdockerd through the Docker-compatible Engine HTTP API over
the app Unix socket. It does not require staging the upstream Docker CLI into
the APK runtime.
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
CURRENT_STAGE="initializing"

cleanup() {
  local status="$?"
  if [[ "$status" -ne 0 ]]; then
    operation_notify "failed" "$CURRENT_STAGE failed with exit code $status" 1 >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP"
  "$ADB" forward --remove "tcp:$LOCAL_PORT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

remote_quote() {
  printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

run_as() {
  "$ADB" shell "run-as $PKG sh -c $(remote_quote "$1")"
}

operation_notify() {
  local status="$1"
  local detail="$2"
  local finished="${3:-0}"
  local json len
  json="$(python3 - "$OP_ID" "$status" "$detail" "$finished" <<'PY'
import json
import sys

op_id, status, detail, finished = sys.argv[1:5]
print(json.dumps({
    "Id": op_id,
    "Kind": "llama-gpu-compare",
    "Title": "llama.cpp GPU compare",
    "Status": status,
    "Detail": detail,
    "Finished": finished == "1",
}, separators=(",", ":")))
PY
)"
  len="$(printf "%s" "$json" | wc -c | tr -d ' ')"
  run_as "cd files && { printf 'POST /system/operations HTTP/1.1\r\nHost: pdocker\r\nContent-Type: application/json\r\nContent-Length: $len\r\nConnection: close\r\n\r\n'; printf %s $(remote_quote "$json"); } | toybox nc -U pdocker/pdockerd.sock >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
}

wait_for_engine() {
  local i
  for i in $(seq 1 45); do
    if run_as 'test -S files/pdocker/pdockerd.sock' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "pdockerd socket did not appear" >&2
  return 1
}

urlencode() {
  python3 - "$1" <<'PY'
import sys
import urllib.parse
print(urllib.parse.quote(sys.argv[1], safe=""))
PY
}

http_body() {
  python3 -c 'import sys
data = sys.stdin.buffer.read()
split = data.find(b"\r\n\r\n")
if split < 0:
    split = data.find(b"\n\n")
    offset = 2
else:
    offset = 4
body = data[split + offset:] if split >= 0 else data
sys.stdout.buffer.write(body)'
}

engine_request() {
  local method="$1"
  local path="$2"
  local body="${3-}"
  local len=0
  if [[ $# -ge 3 ]]; then
    len="$(printf "%s" "$body" | wc -c | tr -d ' ')"
    run_as "cd files && { printf '%s %s HTTP/1.1\r\nHost: pdocker\r\nContent-Type: application/json\r\nContent-Length: %s\r\nConnection: close\r\n\r\n' $(remote_quote "$method") $(remote_quote "$path") $(remote_quote "$len"); printf %s $(remote_quote "$body"); } | toybox nc -U pdocker/pdockerd.sock"
  else
    run_as "cd files && { printf '%s %s HTTP/1.1\r\nHost: pdocker\r\nConnection: close\r\n\r\n' $(remote_quote "$method") $(remote_quote "$path"); } | toybox nc -U pdocker/pdockerd.sock"
  fi
}

engine_body() {
  engine_request "$@" | http_body
}

decode_engine_logs() {
  python3 -c 'import sys
data = sys.stdin.buffer.read()
split = data.find(b"\r\n\r\n")
if split >= 0:
    data = data[split + 4:]
out = bytearray()
idx = 0
while idx + 8 <= len(data):
    size = int.from_bytes(data[idx + 4:idx + 8], "big")
    idx += 8
    if size <= 0 or idx + size > len(data):
        idx -= 8
        break
    out.extend(data[idx:idx + size])
    idx += size
if idx < len(data):
    out.extend(data[idx:])
sys.stdout.buffer.write(out)'
}

container_payload() {
  local mode="$1"
  local ctx="$2"
  local gpu_layers="${3:-}"
  python3 - "$IMAGE" "$DEVICE_PROJECT" "$mode" "$ctx" "$gpu_layers" "$REMOTE_PORT" <<'PY'
import json
import sys

image, project, mode, ctx, gpu_layers, port = sys.argv[1:7]
env = [
    "PDOCKER_GPU=auto",
    "PDOCKER_GPU_AUTO=1",
    f"PDOCKER_GPU_MODE={mode}",
    "LLAMA_ARG_MODEL=/models/model.gguf",
    f"LLAMA_ARG_CTX={ctx}",
    f"LLAMA_ARG_PORT={port}",
    "LLAMA_LOG_FILE=/workspace/logs/llama-server.log",
]
if mode == "vulkan-raw":
    env.extend([
        "PDOCKER_VULKAN_ICD_TRACE_ALLOC=1",
        "PDOCKER_VULKAN_MAX_BUFFER_BYTES=536870912",
        "GGML_VK_FORCE_MAX_BUFFER_SIZE=536870912",
        "GGML_VK_FORCE_MAX_ALLOCATION_SIZE=536870912",
        "GGML_VK_SUBALLOCATION_BLOCK_SIZE=536870912",
        f"LLAMA_ARG_N_GPU_LAYERS={gpu_layers}",
    ])
port_key = f"{port}/tcp"
payload = {
    "Image": image,
    "Env": env,
    "ExposedPorts": {port_key: {}},
    "Labels": {
        "io.pdocker.project": "llama-cpp-gpu",
        "io.pdocker.role": "llama-gpu-compare",
    },
    "HostConfig": {
        "Binds": [
            f"{project}/models:/models",
            f"{project}/workspace:/workspace",
            f"{project}/profiles:/profiles",
        ],
        "PortBindings": {
            port_key: [{"HostIp": "127.0.0.1", "HostPort": str(port)}],
        },
        "DeviceRequests": [
            {
                "Driver": "",
                "Count": -1,
                "DeviceIDs": None,
                "Capabilities": [["gpu"]],
                "Options": {},
            },
        ],
    },
}
print(json.dumps(payload, separators=(",", ":")))
PY
}

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
  engine_request GET "/containers/$(urlencode "$CONTAINER")/logs?stdout=1&stderr=1&tail=320" | decode_engine_logs || true
}

container_state() {
  engine_body GET "/containers/$(urlencode "$CONTAINER")/json" || true
}

remove_container() {
  engine_request DELETE "/containers/$(urlencode "$CONTAINER")?force=true" >/dev/null || true
}

start_container_mode() {
  local mode="$1"
  local ctx="$2"
  local gpu_layers="${3:-}"
  local payload cid
  remove_container
  payload="$(container_payload "$mode" "$ctx" "$gpu_layers")"
  cid="$(engine_body POST "/containers/create?name=$(urlencode "$CONTAINER")" "$payload" | python3 -c 'import json,sys; print(json.load(sys.stdin)["Id"])')"
  engine_request POST "/containers/$cid/start" "" >/dev/null
  printf "%s\n" "$cid"
}

start_cpu() {
  start_container_mode "cpu" "$CPU_CTX"
}

start_gpu() {
  start_container_mode "vulkan-raw" "$GPU_CTX" "$GPU_LAYERS"
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
CURRENT_STAGE="CPU baseline"
operation_notify "running" "CPU baseline: starting"
wait_for_engine
DEVICE_PROJECT="$(run_as "cd $(remote_quote "$PROJECT") && pwd" | tr -d '\r')"
start_cpu >/dev/null
if ! wait_server 90; then
  operation_notify "failed" "CPU server did not become reachable" 1
  echo "CPU server did not become reachable" >&2
  container_state >&2
  container_logs >&2
  exit 1
fi
CPU_JSON="$TMP/cpu.json"
bench_http "cpu-baseline" "$CPU_JSON" >/dev/null

echo "[pdocker llama compare] start forced Vulkan run"
CURRENT_STAGE="forced Vulkan"
operation_notify "running" "CPU baseline complete; forced Vulkan model load starting"
start_gpu >/dev/null
GPU_LOG="$TMP/gpu.log"
GPU_STATE="$TMP/gpu-state.txt"
GPU_JSON="$TMP/gpu.json"
if wait_server 120; then
  operation_notify "running" "Forced Vulkan served; recording HTTP benchmark"
  bench_http "vulkan-forced-ngl-$GPU_LAYERS" "$GPU_JSON" >/dev/null || true
  gpu_served=1
else
  operation_notify "running" "Forced Vulkan did not serve; collecting container logs"
  gpu_served=0
fi
container_state > "$GPU_STATE"
container_logs > "$GPU_LOG"

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

def json_string_field_seen(name, value):
    return re.search(rf'"{re.escape(name)}"\s*:\s*"{re.escape(value)}"', log) is not None

def json_bool_field_seen(name, value):
    return re.search(rf'"{re.escape(name)}"\s*:\s*{str(value).lower()}\b', log) is not None

executor_backends = sorted(set(re.findall(r'"backend_impl"\s*:\s*"([^"]+)"', log)))
executor_errors = sorted(set(re.findall(r'"error"\s*:\s*"([^"]+)"', log)))
spirv_hashes = sorted(set(re.findall(r'"spirv_hash"\s*:\s*"([^"]+)"', log)))
generic_spirv_attempted = (
    json_string_field_seen("kernel", "generic_spirv")
    or "generic dispatch response:" in log
    or "generic SPIR-V dispatch failed" in log
)
executor_submit_generic_dispatch_error = json_string_field_seen("error", "submit-generic-dispatch")
generic_spirv_dispatch_failed = "generic SPIR-V dispatch failed" in log
queue_submit_blocker = "vk::Queue::submit: ErrorFeatureNotPresent" in log
android_vulkan_dispatch_blocker = (
    json_string_field_seen("backend_impl", "android_vulkan")
    and executor_submit_generic_dispatch_error
)
generic_spirv_dispatch_blocker = (
    generic_spirv_attempted
    and (
        executor_submit_generic_dispatch_error
        or generic_spirv_dispatch_failed
        or queue_submit_blocker
    )
)
evidence = {
    "vulkan_device_seen": "Vulkan0 (pdocker Vulkan bridge" in log or "Vulkan0 model buffer size" in log,
    "offload_seen": "offloading" in log,
    "model_loaded": "main: model loaded" in log,
    "serve_reachable": bool(int(gpu_served_s)),
    "buffer_allocation_blocker": "unable to allocate Vulkan0 buffer" in log,
    "assert_blocker": "GGML_ASSERT" in log,
    "buffer_range_assert_blocker": "ggml_backend_buffer_get_alloc_size" in log,
    "gpu_model_buffer_seen": "Vulkan0 model buffer size" in log,
    "generic_dispatch_response_seen": "generic dispatch response:" in log,
    "generic_spirv_dispatch_attempted": generic_spirv_attempted,
    "generic_spirv_dispatch_seen": json_string_field_seen("kernel", "generic_spirv") and json_bool_field_seen("valid", True),
    "generic_spirv_dispatch_failed": generic_spirv_dispatch_failed,
    "executor_spirv_trace_seen": "pdocker-gpu-executor: SPIR-V trace" in log,
    "executor_feature_trace_seen": "pdocker-gpu-executor: Android Vulkan features" in log,
    "android_vulkan_dispatch_blocker": android_vulkan_dispatch_blocker,
    "executor_submit_generic_dispatch_error": executor_submit_generic_dispatch_error,
    "executor_fallback_dispatch_blocker": (
        json_string_field_seen("backend_affinity", "fallback")
        and executor_submit_generic_dispatch_error
    ),
    "queue_submit_blocker": queue_submit_blocker,
    "spirv_dispatch_blocker": (
        "real SPIR-V dispatch is not lowered yet" in log
        or queue_submit_blocker
        or generic_spirv_dispatch_blocker
    ),
}
if evidence["buffer_range_assert_blocker"]:
    blocker_class = "vulkan_buffer_range_accounting"
    blocker_detail = "scheduler warmup hit ggml_backend_buffer_get_alloc_size"
elif evidence["buffer_allocation_blocker"] or evidence["assert_blocker"]:
    blocker_class = "vulkan_buffer_allocation"
    blocker_detail = "Vulkan buffer allocation/assertion failed before dispatch"
elif generic_spirv_dispatch_blocker:
    blocker_class = "vulkan_generic_spirv_dispatch"
    blocker_detail = "generic SPIR-V dispatch reached submit-generic-dispatch / queue submit failure"
elif evidence["generic_spirv_dispatch_seen"] and bool(int(gpu_served_s)):
    blocker_class = "bridge_dispatch_performance"
    blocker_detail = "generic SPIR-V dispatch served; benchmark throughput is the remaining gap"
else:
    blocker_class = "vulkan_device_discovery"
    blocker_detail = "Vulkan offload evidence was not sufficient to classify a later blocker"
allocations = [
    int(m.group(1))
    for m in re.finditer(r"pdocker-vulkan-icd: allocate ([0-9]+) bytes", log)
]
dispatch_upload_ms = [float(m.group(1)) for m in re.finditer(r'"upload_ms":([0-9.]+)', log)]
dispatch_ms = [float(m.group(1)) for m in re.finditer(r'"dispatch_ms":([0-9.]+)', log)]
dispatch_download_ms = [float(m.group(1)) for m in re.finditer(r'"download_ms":([0-9.]+)', log)]
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
        "bridge_dispatch_profile": {
            "samples": len(dispatch_ms),
            "upload_ms_mean": (sum(dispatch_upload_ms) / len(dispatch_upload_ms)) if dispatch_upload_ms else 0.0,
            "dispatch_ms_mean": (sum(dispatch_ms) / len(dispatch_ms)) if dispatch_ms else 0.0,
            "download_ms_mean": (sum(dispatch_download_ms) / len(dispatch_download_ms)) if dispatch_download_ms else 0.0,
        },
        "diagnostics": {
            "blocker_class": blocker_class,
            "blocker_detail": blocker_detail,
            "executor_backends": executor_backends,
            "executor_errors": executor_errors,
            "spirv_hashes": spirv_hashes[-4:],
        },
    },
    "comparison": {
        "speedup": (gpu_tps / cpu_tps) if cpu_tps and gpu_tps else 0.0,
        "target_tokens_per_second": target_tps,
        "target_met": bool(cpu_tps and gpu_tps >= target_tps),
    },
    "operation": {
        "kind": "llama-gpu-compare",
        "ui_surface": "Overview daemon operation/progress card",
        "container_surface": "pdocker-llama-cpp remains the container shown by Engine container listing",
        "cleanup": "remove adb port forward, mark failed operation on nonzero exit, restore CPU server unless --no-restore is passed",
    },
    "next_blocker": (
        "fix Vulkan buffer base/range accounting for scheduler warmup"
        if evidence["buffer_range_assert_blocker"]
        else
        "split 4GiB+ Vulkan buffers / pinned host-buffer path"
        if evidence["buffer_allocation_blocker"] or evidence["assert_blocker"]
        else "lower generic SPIR-V dispatch into the Android Vulkan executor or clamp advertised capabilities"
        if blocker_class == "vulkan_generic_spirv_dispatch"
        else "inspect traced Android Vulkan feature/SPIR-V mismatch"
        if evidence["android_vulkan_dispatch_blocker"] and evidence["executor_spirv_trace_seen"]
        else "lower llama.cpp SPIR-V dispatch into the Android GPU executor"
        if evidence["spirv_dispatch_blocker"] or evidence["queue_submit_blocker"]
        else "reduce bridge upload/copy overhead and benchmark with larger n_predict"
        if evidence["generic_spirv_dispatch_seen"] and bool(int(gpu_served_s))
        else "make Vulkan device discovery reliable"
    ),
}
Path(out_path).write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(json.dumps(result["comparison"], indent=2))
print("next_blocker:", result["next_blocker"])
PY

SUMMARY="$(python3 - "$OUT" <<'PY'
import json
import sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
print(
    f"CPU {d['cpu']['tokens_per_second']:.3f} tok/s; "
    f"GPU {d['gpu']['tokens_per_second']:.3f} tok/s; "
    f"GPU served={str(d['gpu']['served']).lower()}; "
    f"speedup {d['comparison']['speedup']:.2f}x; "
    f"target_met={str(d['comparison']['target_met']).lower()}; "
    f"gpu_layers={d['settings']['gpu_layers']}; "
    f"next: {d['next_blocker']}"
)
PY
)"
operation_notify "running" "$SUMMARY"

DEVICE_NAME="$(basename "$OUT")"
"$ADB" push "$OUT" "/data/local/tmp/$DEVICE_NAME" >/dev/null
run_as "mkdir -p files/pdocker/bench && cp /data/local/tmp/$DEVICE_NAME files/pdocker/bench/$DEVICE_NAME"

if [[ "$RESTORE_CPU" -eq 1 ]]; then
  echo "[pdocker llama compare] restore CPU server"
  CURRENT_STAGE="restore CPU server"
  operation_notify "running" "Restoring CPU llama server"
  start_cpu >/dev/null
  wait_server 90 >/dev/null || true
fi

CURRENT_STAGE="complete"
if [[ "$RESTORE_CPU" -eq 1 ]]; then
  operation_notify "done" "$SUMMARY; CPU server restored" 1
else
  operation_notify "done" "$SUMMARY; CPU server left in last compare mode (--no-restore)" 1
fi
echo "[pdocker llama compare] local: $OUT"
echo "[pdocker llama compare] device: files/pdocker/bench/$DEVICE_NAME"
