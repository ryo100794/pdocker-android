#!/usr/bin/env bash
# Compare APK-side executor direct GPU work with container-facing shim bridge.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNS="${1:-8}"
OUT="${2:-$ROOT/docs/test/gpu-bridge-bench-latest.json}"
EXECUTOR="$ROOT/app/src/main/jniLibs/arm64-v8a/libpdockergpuexecutor.so"
SHIM="$ROOT/app/src/main/jniLibs/arm64-v8a/libpdockergpushim.so"
SOCK="${TMPDIR:-/tmp}/pdocker-gpu-bridge-$$.sock"
TMP="$(mktemp -d)"
trap '[[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || true; rm -rf "$TMP" "$SOCK"' EXIT

mkdir -p "$(dirname "$OUT")"

"$EXECUTOR" --serve-socket "$SOCK" >"$TMP/executor.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCK" ]] && break
    sleep 0.05
done
[[ -S "$SOCK" ]] || { cat "$TMP/executor.log" >&2; echo "executor socket did not appear" >&2; exit 1; }

measure() {
    local label="$1"; shift
    local start end
    start="$(date +%s%N)"
    "$@"
    end="$(date +%s%N)"
    python3 - "$label" "$start" "$end" "$RUNS" >>"$TMP/wall.tsv" <<'PY'
import sys
label, start, end, runs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
ms = (end - start) / 1_000_000.0
print(f"{label}\t{ms:.6f}\t{ms / runs:.6f}")
PY
}

measure direct_vector "$EXECUTOR" --bench-vector-add "$RUNS" >"$TMP/direct.jsonl"
measure bridge_vector env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-vector-add "$RUNS" >"$TMP/bridge.jsonl"
measure bridge_vector_persistent env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-vector-add-persistent "$RUNS" >"$TMP/bridge-persistent.jsonl"
measure bridge_vector_fd env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-vector-add-fd "$RUNS" >"$TMP/bridge-fd.jsonl"
measure bridge_vector_fd_persistent env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-vector-add-fd-persistent "$RUNS" >"$TMP/bridge-fd-persistent.jsonl"
measure bridge_vector_registered env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-vector-add-registered "$RUNS" >"$TMP/bridge-registered.jsonl"
measure direct_noop "$EXECUTOR" --bench-noop "$RUNS" >"$TMP/direct-noop.jsonl"
measure bridge_noop env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-noop "$RUNS" >"$TMP/bridge-noop.jsonl"
measure bridge_noop_persistent env PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$SHIM" --bench-noop-persistent "$RUNS" >"$TMP/bridge-noop-persistent.jsonl"

python3 - "$TMP/direct.jsonl" "$TMP/bridge.jsonl" "$TMP/bridge-persistent.jsonl" "$TMP/bridge-fd.jsonl" "$TMP/bridge-fd-persistent.jsonl" "$TMP/bridge-registered.jsonl" "$TMP/direct-noop.jsonl" "$TMP/bridge-noop.jsonl" "$TMP/bridge-noop-persistent.jsonl" "$TMP/wall.tsv" "$OUT" "$RUNS" <<'PY'
import json, statistics, sys, time
direct_path, bridge_path, bridge_persistent_path, bridge_fd_path, bridge_fd_persistent_path, bridge_registered_path, direct_noop_path, bridge_noop_path, bridge_noop_persistent_path, wall_path, out_path, runs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6], sys.argv[7], sys.argv[8], sys.argv[9], sys.argv[10], sys.argv[11], int(sys.argv[12])

def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows

def summary(rows):
    totals = [float(r["total_ms"]) for r in rows if r.get("valid")]
    dispatch = [float(r.get("dispatch_ms", 0.0)) for r in rows if r.get("valid")]
    warm_totals = totals[1:] if len(totals) > 1 else totals
    warm_dispatch = dispatch[1:] if len(dispatch) > 1 else dispatch
    return {
        "count": len(totals),
        "total_ms_mean": statistics.fmean(totals) if totals else None,
        "total_ms_min": min(totals) if totals else None,
        "total_ms_max": max(totals) if totals else None,
        "dispatch_ms_mean": statistics.fmean(dispatch) if dispatch else None,
        "warm_count": len(warm_totals),
        "warm_total_ms_mean": statistics.fmean(warm_totals) if warm_totals else None,
        "warm_total_ms_min": min(warm_totals) if warm_totals else None,
        "warm_total_ms_max": max(warm_totals) if warm_totals else None,
        "warm_dispatch_ms_mean": statistics.fmean(warm_dispatch) if warm_dispatch else None,
    }

direct = load(direct_path)
bridge = load(bridge_path)
bridge_persistent = load(bridge_persistent_path)
bridge_fd = load(bridge_fd_path)
bridge_fd_persistent = load(bridge_fd_persistent_path)
bridge_registered = load(bridge_registered_path)
direct_noop = load(direct_noop_path)
bridge_noop = load(bridge_noop_path)
bridge_noop_persistent = load(bridge_noop_persistent_path)
wall = {}
with open(wall_path) as f:
    for line in f:
        label, total, per_run = line.rstrip("\n").split("\t")
        wall[label] = {"wall_ms": float(total), "wall_ms_per_run": float(per_run)}
direct_s = summary(direct)
bridge_s = summary(bridge)
bridge_persistent_s = summary(bridge_persistent)
bridge_fd_s = summary(bridge_fd)
bridge_fd_persistent_s = summary(bridge_fd_persistent)
bridge_registered_s = summary(bridge_registered)
ratio = None
if direct_s["total_ms_mean"] and bridge_s["total_ms_mean"]:
    ratio = bridge_s["total_ms_mean"] / direct_s["total_ms_mean"]
warm_ratio = None
if direct_s["warm_total_ms_mean"] and bridge_s["warm_total_ms_mean"]:
    warm_ratio = bridge_s["warm_total_ms_mean"] / direct_s["warm_total_ms_mean"]
persistent_ratio = None
if direct_s["warm_total_ms_mean"] and bridge_persistent_s["warm_total_ms_mean"]:
    persistent_ratio = bridge_persistent_s["warm_total_ms_mean"] / direct_s["warm_total_ms_mean"]
fd_ratio = None
if direct_s["warm_total_ms_mean"] and bridge_fd_s["warm_total_ms_mean"]:
    fd_ratio = bridge_fd_s["warm_total_ms_mean"] / direct_s["warm_total_ms_mean"]
fd_persistent_ratio = None
if direct_s["warm_total_ms_mean"] and bridge_fd_persistent_s["warm_total_ms_mean"]:
    fd_persistent_ratio = bridge_fd_persistent_s["warm_total_ms_mean"] / direct_s["warm_total_ms_mean"]
registered_ratio = None
if direct_s["warm_total_ms_mean"] and bridge_registered_s["warm_total_ms_mean"]:
    registered_ratio = bridge_registered_s["warm_total_ms_mean"] / direct_s["warm_total_ms_mean"]
doc = {
    "timestamp_unix": int(time.time()),
    "runs_requested": runs,
    "direct_executor": direct_s,
    "shim_bridge": bridge_s,
    "shim_bridge_persistent": bridge_persistent_s,
    "shim_bridge_fd_shared_buffer": bridge_fd_s,
    "shim_bridge_fd_shared_buffer_persistent": bridge_fd_persistent_s,
    "shim_bridge_registered_shared_buffer": bridge_registered_s,
    "noop": {
        "direct": summary(direct_noop),
        "bridge": summary(bridge_noop),
        "bridge_persistent": summary(bridge_noop_persistent),
    },
    "wall": wall,
    "bridge_over_direct_total_ratio": ratio,
    "bridge_over_direct_warm_total_ratio": warm_ratio,
    "persistent_bridge_over_direct_warm_total_ratio": persistent_ratio,
    "fd_shared_buffer_bridge_over_direct_warm_total_ratio": fd_ratio,
    "persistent_fd_shared_buffer_bridge_over_direct_warm_total_ratio": fd_persistent_ratio,
    "registered_shared_buffer_bridge_over_direct_warm_total_ratio": registered_ratio,
    "direct_samples": direct,
    "bridge_samples": bridge,
    "bridge_persistent_samples": bridge_persistent,
    "bridge_fd_shared_buffer_samples": bridge_fd,
    "bridge_fd_shared_buffer_persistent_samples": bridge_fd_persistent,
    "bridge_registered_shared_buffer_samples": bridge_registered,
    "direct_noop_samples": direct_noop,
    "bridge_noop_samples": bridge_noop,
    "bridge_noop_persistent_samples": bridge_noop_persistent,
}
with open(out_path, "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print(json.dumps({
    "direct_total_ms_mean": direct_s["total_ms_mean"],
    "bridge_total_ms_mean": bridge_s["total_ms_mean"],
    "bridge_over_direct_total_ratio": ratio,
    "direct_warm_total_ms_mean": direct_s["warm_total_ms_mean"],
    "bridge_warm_total_ms_mean": bridge_s["warm_total_ms_mean"],
    "bridge_over_direct_warm_total_ratio": warm_ratio,
    "bridge_persistent_warm_total_ms_mean": bridge_persistent_s["warm_total_ms_mean"],
    "persistent_bridge_over_direct_warm_total_ratio": persistent_ratio,
    "bridge_fd_shared_buffer_warm_total_ms_mean": bridge_fd_s["warm_total_ms_mean"],
    "fd_shared_buffer_bridge_over_direct_warm_total_ratio": fd_ratio,
    "bridge_fd_shared_buffer_persistent_warm_total_ms_mean": bridge_fd_persistent_s["warm_total_ms_mean"],
    "persistent_fd_shared_buffer_bridge_over_direct_warm_total_ratio": fd_persistent_ratio,
    "bridge_registered_shared_buffer_warm_total_ms_mean": bridge_registered_s["warm_total_ms_mean"],
    "registered_shared_buffer_bridge_over_direct_warm_total_ratio": registered_ratio,
    "noop_wall_ms_per_run": {
        "direct": wall.get("direct_noop", {}).get("wall_ms_per_run"),
        "bridge": wall.get("bridge_noop", {}).get("wall_ms_per_run"),
        "bridge_persistent": wall.get("bridge_noop_persistent", {}).get("wall_ms_per_run"),
    },
    "vector_wall_ms_per_run": {
        "direct": wall.get("direct_vector", {}).get("wall_ms_per_run"),
        "bridge": wall.get("bridge_vector", {}).get("wall_ms_per_run"),
        "bridge_persistent": wall.get("bridge_vector_persistent", {}).get("wall_ms_per_run"),
        "bridge_fd_shared_buffer": wall.get("bridge_vector_fd", {}).get("wall_ms_per_run"),
        "bridge_fd_shared_buffer_persistent": wall.get("bridge_vector_fd_persistent", {}).get("wall_ms_per_run"),
        "bridge_registered_shared_buffer": wall.get("bridge_vector_registered", {}).get("wall_ms_per_run"),
    },
    "out": out_path,
}, indent=2))
PY
