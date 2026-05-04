#!/usr/bin/env bash
# Capture comparable host/container CPU/GPU timings plus bridge overhead.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="${ADB:-adb}"
PKG="${PDOCKER_PACKAGE:-io.github.ryo100794.pdocker.compat}"
RUNS="${1:-${PDOCKER_GPU_COMPARE_RUNS:-5}}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_JSON="${PDOCKER_GPU_COMPARE_OUT:-$ROOT/docs/test/gpu-host-container-comparison-latest.json}"
OUT_MD="${PDOCKER_GPU_COMPARE_MD:-$ROOT/docs/test/gpu-host-container-comparison-latest.md}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

remote_quote() {
  printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

run_as() {
  "$ADB" shell "run-as $PKG sh -c $(remote_quote "$1")"
}

stage_test_cli() {
  local docker_bin="$ROOT/docker-proot-setup/docker-bin/docker"
  local compose_bin="$ROOT/vendor/lib/docker-compose"
  if [[ ! -x "$docker_bin" || ! -x "$compose_bin" ]]; then
    echo "test Docker CLI/Compose binaries missing; run backend fetch/build first" >&2
    return 1
  fi
  "$ADB" push "$docker_bin" /data/local/tmp/pdocker-test-docker >/dev/null
  "$ADB" push "$compose_bin" /data/local/tmp/pdocker-test-docker-compose >/dev/null
  run_as "mkdir -p files/pdocker-runtime/docker-bin/cli-plugins && cp /data/local/tmp/pdocker-test-docker files/pdocker-runtime/docker-bin/docker && cp /data/local/tmp/pdocker-test-docker-compose files/pdocker-runtime/docker-bin/cli-plugins/docker-compose && chmod 755 files/pdocker-runtime/docker-bin/docker files/pdocker-runtime/docker-bin/cli-plugins/docker-compose"
}

docker_cmd() {
  local cmd="$1"
  run_as "cd files && export PATH=\"\$PWD/pdocker-runtime/docker-bin:\$PATH\" DOCKER_CONFIG=\"\$PWD/pdocker-runtime/docker-bin\" DOCKER_HOST=\"unix://\$PWD/pdocker/pdockerd.sock\" DOCKER_BUILDKIT=0 COMPOSE_DOCKER_CLI_BUILD=0 BUILDKIT_PROGRESS=plain COMPOSE_PROGRESS=plain COMPOSE_MENU=false && $cmd"
}

wait_for_runtime() {
  local i
  for i in $(seq 1 45); do
    if run_as 'test -S files/pdocker/pdockerd.sock && test -S files/pdocker-runtime/gpu/pdocker-gpu.sock' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "pdockerd/gpu executor sockets did not appear" >&2
  return 1
}

pick_container() {
  if [[ -n "${PDOCKER_GPU_COMPARE_CONTAINER:-}" ]]; then
    printf '%s\n' "$PDOCKER_GPU_COMPARE_CONTAINER"
    return 0
  fi
  docker_cmd "docker ps --format '{{.Names}}'" | awk 'NF {print; exit}'
}

container_rootfs() {
  local container="$1"
  local inspect="$TMP/container-inspect.json"
  docker_cmd "docker inspect $(printf "%q" "$container")" >"$inspect"
  python3 - "$inspect" <<'PY'
import json
import sys
doc = json.load(open(sys.argv[1]))
if not doc:
    raise SystemExit(1)
cid = doc[0].get("Id") or ""
rootfs = ((doc[0].get("Storage") or {}).get("Rootfs") or "")
if not rootfs and cid:
    rootfs = f"pdocker/containers/{cid}/rootfs"
if not rootfs:
    raise SystemExit(1)
print(rootfs)
PY
}

direct_container_cmd() {
  local rootfs="$1"
  local argv="$2"
  run_as "cd files && ROOTFS=$(remote_quote "$rootfs") && RUNTIME=\$PWD/pdocker-runtime && export PDOCKER_DIRECT_EXPERIMENTAL_PROCESS_EXEC=1 PDOCKER_DIRECT_TRACE_MODE=seccomp PDOCKER_DIRECT_TRACE_SYSCALLS=0 PDOCKER_DIRECT_TRACE_VERBOSE=0; pdocker-runtime/docker-bin/pdocker-direct run --mode bench --rootfs \"\$ROOTFS\" --workdir / --env HOME=/root --env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin --env PDOCKER_GPU_QUEUE_SOCKET=\"\$RUNTIME/gpu/pdocker-gpu.sock\" --bind \"\$RUNTIME/gpu:/run/pdocker-gpu\" -- /bin/sh -lc \"/usr/local/bin/pdocker-gpu-shim $argv\""
}

install_container_helpers() {
  local rootfs="$1"
  run_as "cd files && ROOTFS=$(remote_quote "$rootfs") && mkdir -p \"\$ROOTFS/usr/local/bin\" && cp pdocker-runtime/lib/pdocker-gpu-shim \"\$ROOTFS/usr/local/bin/pdocker-gpu-shim\" && chmod 755 \"\$ROOTFS/usr/local/bin/pdocker-gpu-shim\""
}

mkdir -p "$(dirname "$OUT_JSON")"
"$ADB" shell monkey -p "$PKG" 1 >/dev/null
wait_for_runtime
stage_test_cli

CONTAINER="$(pick_container || true)"
if [[ -z "$CONTAINER" ]]; then
  echo "no running container found; start llama/dev compose first or set PDOCKER_GPU_COMPARE_CONTAINER" >&2
  exit 2
fi
ROOTFS="$(container_rootfs "$CONTAINER")"
install_container_helpers "$ROOTFS"

echo "[pdocker gpu compare] container=$CONTAINER rootfs=$ROOTFS runs=$RUNS"

measure() {
  local label="$1"; shift
  local start end rc=0
  start="$(date +%s%N)"
  "$@" >"$TMP/$label.jsonl" 2>"$TMP/$label.err" || rc=$?
  end="$(date +%s%N)"
  python3 - "$label" "$start" "$end" "$RUNS" "$rc" >>"$TMP/wall.tsv" <<'PY'
import sys
label, start, end, runs, rc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
ms = (end - start) / 1_000_000.0
print(f"{label}\t{ms:.6f}\t{ms / runs:.6f}\t{rc}")
PY
  cat "$TMP/$label.err" >&2
  return "$rc"
}

measure host_cpu run_as "files/pdocker-runtime/gpu/pdocker-gpu-executor --bench-cpu-vector-add '$RUNS'"
measure host_gpu_vulkan run_as "files/pdocker-runtime/gpu/pdocker-gpu-executor --bench-vulkan-vector-add '$RUNS'"
measure container_cpu direct_container_cmd "$ROOTFS" "--bench-cpu-vector-add $(printf "%q" "$RUNS")"
measure bridge_noop direct_container_cmd "$ROOTFS" "--bench-noop-persistent $(printf "%q" "$RUNS")"
measure container_gpu_vulkan direct_container_cmd "$ROOTFS" "--bench-vulkan-vector-add-3fd-persistent $(printf "%q" "$RUNS")"

python3 - "$TMP" "$OUT_JSON" "$OUT_MD" "$RUNS" "$CONTAINER" "$STAMP" <<'PY'
import json
import statistics
import sys
from pathlib import Path

tmp = Path(sys.argv[1])
out_json = Path(sys.argv[2])
out_md = Path(sys.argv[3])
runs = int(sys.argv[4])
container = sys.argv[5]
stamp = sys.argv[6]
labels = ["host_cpu", "host_gpu_vulkan", "container_cpu", "bridge_noop", "container_gpu_vulkan"]

def load(label):
    rows = []
    path = tmp / f"{label}.jsonl"
    if not path.exists():
        return rows
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or not line.startswith("{"):
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return rows

def summary(rows):
    valid = [r for r in rows if r.get("valid") is True]
    totals = [float(r.get("total_ms", 0.0)) for r in valid]
    dispatch = [float(r.get("dispatch_ms", 0.0)) for r in valid]
    warm = totals[1:] if len(totals) > 1 else totals
    return {
        "samples": len(rows),
        "valid_samples": len(valid),
        "backend_impl": next((r.get("backend_impl") for r in rows if r.get("backend_impl")), None),
        "transport": next((r.get("transport") for r in rows if r.get("transport")), None),
        "cached_samples": sum(1 for r in rows if r.get("backend_cached") is True),
        "total_ms_mean": statistics.fmean(totals) if totals else None,
        "total_ms_min": min(totals) if totals else None,
        "warm_total_ms_mean": statistics.fmean(warm) if warm else None,
        "dispatch_ms_mean": statistics.fmean(dispatch) if dispatch else None,
    }

wall = {}
wall_path = tmp / "wall.tsv"
if wall_path.exists():
    for line in wall_path.read_text().splitlines():
        label, total, per_run, rc = line.split("\t")
        wall[label] = {"wall_ms": float(total), "wall_ms_per_run": float(per_run), "rc": int(rc)}

rows = {label: load(label) for label in labels}
summaries = {label: summary(rows[label]) for label in labels}
host_gpu = summaries["host_gpu_vulkan"].get("warm_total_ms_mean")
container_gpu = summaries["container_gpu_vulkan"].get("warm_total_ms_mean")
host_cpu = summaries["host_cpu"].get("warm_total_ms_mean")
container_cpu = summaries["container_cpu"].get("warm_total_ms_mean")
bridge_noop = wall.get("bridge_noop", {}).get("wall_ms_per_run")
bridge_noop_roundtrip = summaries["bridge_noop"].get("warm_total_ms_mean")

doc = {
    "timestamp_utc": stamp,
    "runs_requested": runs,
    "container": container,
    "summaries": summaries,
    "wall": wall,
    "ratios": {
        "container_gpu_over_host_gpu_warm_total": (container_gpu / host_gpu) if host_gpu and container_gpu else None,
        "container_cpu_over_host_cpu_warm_total": (container_cpu / host_cpu) if host_cpu and container_cpu else None,
        "bridge_noop_roundtrip_warm_total_ms": bridge_noop_roundtrip,
        "direct_executor_bridge_noop_wall_ms_per_call": bridge_noop,
    },
    "samples": rows,
}
out_json.write_text(json.dumps(doc, indent=2) + "\n")

def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4f}"
    return str(v)

table = [
    ("Host CPU", "host_cpu"),
    ("Host GPU Vulkan", "host_gpu_vulkan"),
    ("Container CPU", "container_cpu"),
    ("Bridge NOOP", "bridge_noop"),
    ("Container GPU Vulkan bridge", "container_gpu_vulkan"),
]
lines = [
    "# GPU Host/Container Comparison",
    "",
    f"- Date: {stamp} UTC.",
    f"- Container: `{container}`.",
    f"- Runs: {runs}.",
    "",
    "| Scope | Backend | Valid | Warm total mean ms | Dispatch mean ms | Wall ms/call | Transport |",
    "| --- | --- | ---: | ---: | ---: | ---: | --- |",
]
for title, label in table:
    s = summaries[label]
    w = wall.get(label, {})
    lines.append(
        f"| {title} | {s.get('backend_impl') or '-'} | {s.get('valid_samples')}/{s.get('samples')} | "
        f"{fmt(s.get('warm_total_ms_mean'))} | {fmt(s.get('dispatch_ms_mean'))} | "
        f"{fmt(w.get('wall_ms_per_run'))} | {s.get('transport') or '-'} |"
    )
lines += [
    "",
    "## Ratios",
    "",
    f"- Container GPU / host GPU warm total: {fmt(doc['ratios']['container_gpu_over_host_gpu_warm_total'])}x.",
    f"- Container CPU / host CPU warm total: {fmt(doc['ratios']['container_cpu_over_host_cpu_warm_total'])}x.",
    f"- Bridge NOOP round trip inside container process: {fmt(doc['ratios']['bridge_noop_roundtrip_warm_total_ms'])} ms/call.",
    f"- Direct-executor wall time for the bridge NOOP measurement: {fmt(doc['ratios']['direct_executor_bridge_noop_wall_ms_per_call'])} ms/call.",
    "",
    "The direct-executor wall time includes starting and tracing the benchmark process; use the bridge NOOP round-trip row for the command-queue crossing cost.",
]
out_md.write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "[pdocker gpu compare] json: $OUT_JSON"
echo "[pdocker gpu compare] markdown: $OUT_MD"
