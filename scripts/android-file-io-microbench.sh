#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ADB="${ADB:-adb}"
PKG="${PDOCKER_ANDROID_PACKAGE:-io.github.ryo100794.pdocker.compat}"
FILES="${PDOCKER_FILE_IO_MICRO_FILES:-128}"
BLOCKS="${PDOCKER_FILE_IO_MICRO_BLOCKS:-64}"
BLOCK_SIZE="${PDOCKER_FILE_IO_MICRO_BLOCK_SIZE:-4096}"
FSYNC="${PDOCKER_FILE_IO_MICRO_FSYNC:-0}"
TRACE_MODE="${PDOCKER_DIRECT_TRACE_MODE:-seccomp}"
OUT="${PDOCKER_FILE_IO_MICRO_OUT:-$ROOT/docs/test/file-io-microbench-latest.json}"
MD_OUT="${PDOCKER_FILE_IO_MICRO_MD:-$ROOT/docs/test/file-io-microbench-latest.md}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_BENCH_DIR="files/pdocker/bench"
RAW="$(mktemp "${TMPDIR:-/tmp}/pdocker-file-io-micro.XXXXXX.log")"
BIN="/tmp/pdocker-fileio-microbench-$STAMP"
DIRECT="/tmp/pdocker-direct-microbench-$STAMP"
trap 'rm -f "$RAW" "$BIN" "$DIRECT"' EXIT

usage() {
  cat <<EOF
Usage: scripts/android-file-io-microbench.sh [--files N] [--blocks N] [--block-size N] [--fsync 0|1] [--out PATH]

Runs the same static AArch64 file-I/O microbenchmark binary directly in the
APK domain and through pdocker-direct against the same rootfs backing tree.
This is the performance benchmark for direct-executor file syscall overhead;
the shell-based file-io bench is diagnostic only.
EOF
}

while (($#)); do
  case "$1" in
    --files) shift; FILES="${1:?--files requires a value}" ;;
    --blocks) shift; BLOCKS="${1:?--blocks requires a value}" ;;
    --block-size) shift; BLOCK_SIZE="${1:?--block-size requires a value}" ;;
    --fsync) shift; FSYNC="${1:?--fsync requires a value}" ;;
    --out) shift; OUT="${1:?--out requires a value}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

remote_quote() {
  printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

adb_run_as() {
  "$ADB" shell "run-as $PKG sh -c $(remote_quote "$1")"
}

echo "[pdocker file-io microbench] building static workload"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static \
  -o "$BIN" tools/pdocker_fileio_microbench.c

echo "[pdocker file-io microbench] building direct executor"
bash scripts/build-native-termux.sh >/dev/null
cp app/src/main/jniLibs/arm64-v8a/libpdockerdirect.so "$DIRECT"

"$ADB" push "$BIN" "/data/local/tmp/$(basename "$BIN")" >/dev/null
"$ADB" push "$DIRECT" "/data/local/tmp/$(basename "$DIRECT")" >/dev/null

device_serial="$("$ADB" get-serialno 2>&1 || true)"
cat >"$RAW" <<EOF
__PDIO_MICRO_CONTEXT__:device=$device_serial;files=$FILES;blocks=$BLOCKS;block_size=$BLOCK_SIZE;fsync=$FSYNC;trace_mode=$TRACE_MODE
EOF

set +e
adb_run_as "
set +e
cd files || exit 1
R=\$(find pdocker/containers -mindepth 2 -maxdepth 2 -type d -name rootfs 2>/dev/null | head -1)
if test -z \"\$R\"; then
  R=\$(find pdocker/images -mindepth 2 -maxdepth 3 -type d -name rootfs 2>/dev/null | head -1)
fi
if test -z \"\$R\"; then
  echo '__PDIO_MICRO_ERROR__:no-rootfs'
  exit 2
fi
mkdir -p pdocker/bench \"\$R/tmp\" || exit 1
WORKLOAD=\"\$R/tmp/pdocker_fileio_microbench\"
DIRECT=\"pdocker/bench/pdocker-direct-microbench\"
cp '/data/local/tmp/$(basename "$BIN")' \"\$WORKLOAD\" && chmod 755 \"\$WORKLOAD\"
cp '/data/local/tmp/$(basename "$DIRECT")' \"\$DIRECT\" && chmod 755 \"\$DIRECT\"
echo \"__PDIO_MICRO_CONTEXT__:rootfs=\$R;workload=\$WORKLOAD;direct=\$DIRECT\"
echo '__PDIO_MICRO_BEGIN__:native_rootfs'
\"\$WORKLOAD\" \"\$R/tmp/pdocker-fileio-micro-native-$STAMP\" '$FILES' '$BLOCKS' '$BLOCK_SIZE' '$FSYNC'
native_rc=\$?
echo \"__PDIO_MICRO_END__:native_rootfs:rc=\$native_rc\"
echo '__PDIO_MICRO_BEGIN__:container_rootfs'
export PDOCKER_DIRECT_EXPERIMENTAL_PROCESS_EXEC=1
export PDOCKER_DIRECT_TRACE_MODE='$TRACE_MODE'
export PDOCKER_DIRECT_TRACE_SYSCALLS=0
export PDOCKER_DIRECT_TRACE_VERBOSE=0
export PDOCKER_DIRECT_TRACE_PATHS=0
export PDOCKER_DIRECT_STATS=1
\"\$DIRECT\" run --mode bench --rootfs \"\$R\" --workdir / -- /tmp/pdocker_fileio_microbench \"/tmp/pdocker-fileio-micro-container-$STAMP\" '$FILES' '$BLOCKS' '$BLOCK_SIZE' '$FSYNC'
container_rc=\$?
echo \"__PDIO_MICRO_END__:container_rootfs:rc=\$container_rc\"
rm -rf \"\$R/tmp/pdocker-fileio-micro-native-$STAMP\" \"\$R/tmp/pdocker-fileio-micro-container-$STAMP\"
exit \$((native_rc != 0 || container_rc != 0))
" 2>&1 | tee -a "$RAW"
run_rc=${PIPESTATUS[0]}
set -e

mkdir -p "$(dirname "$OUT")" "$(dirname "$MD_OUT")"
python3 - "$RAW" "$OUT" "$MD_OUT" "$FILES" "$BLOCKS" "$BLOCK_SIZE" "$FSYNC" "$TRACE_MODE" "$device_serial" "$run_rc" <<'PY'
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

raw_path, out_path, md_path, files, blocks, block_size, fsync, trace_mode, device_serial, run_rc = sys.argv[1:11]
lines = Path(raw_path).read_text(errors="replace").splitlines()
contexts = []
events = []
current = None
for line in lines:
    if line.startswith("__PDIO_MICRO_CONTEXT__:"):
        contexts.append(line.split(":", 1)[1])
        continue
    m = re.match(r"__PDIO_MICRO_BEGIN__:(native_rootfs|container_rootfs)$", line)
    if m:
        current = {"scope": m.group(1), "results": [], "raw_tail": []}
        continue
    m = re.match(r"__PDIO_MICRO_END__:(native_rootfs|container_rootfs):rc=([0-9]+)$", line)
    if m and current:
        current["rc"] = int(m.group(2))
        events.append(current)
        current = None
        continue
    if current is not None:
        if line.startswith("{"):
            try:
                current["results"].append(json.loads(line))
            except json.JSONDecodeError:
                current["raw_tail"].append(line)
        else:
            current["raw_tail"].append(line)
for event in events:
    stats = "\n".join(event["raw_tail"])
    m = re.search(r"stops=([0-9]+)", stats)
    if m:
        event["direct_stops"] = int(m.group(1))
    top = []
    for rank, nr, name, count in re.findall(r"#([0-9]+) nr=([0-9]+)\(([^)]+)\) count=([0-9]+)", stats):
        top.append({"rank": int(rank), "nr": int(nr), "name": name, "count": int(count)})
    if top:
        event["top_syscalls"] = top
    event["raw_tail"] = event["raw_tail"][-20:]

by_scope = {e["scope"]: {r["label"]: r for r in e["results"]} for e in events}
comparisons = []
for label in ["seq_write", "seq_read", "small_create", "small_stat", "small_read", "unlink"]:
    n = by_scope.get("native_rootfs", {}).get(label)
    c = by_scope.get("container_rootfs", {}).get(label)
    row = {"label": label}
    if n:
        row["native_ms"] = n["elapsed_ms"]
        row["native_ops"] = n["ops"]
        row["native_bytes"] = n["bytes"]
    if c:
        row["container_ms"] = c["elapsed_ms"]
        row["container_ops"] = c["ops"]
        row["container_bytes"] = c["bytes"]
    if n and c and n["elapsed_ms"] > 0:
        row["overhead_ms"] = c["elapsed_ms"] - n["elapsed_ms"]
        row["ratio"] = c["elapsed_ms"] / n["elapsed_ms"]
        row["target_met"] = row["overhead_ms"] <= 10.0
    comparisons.append(row)

artifact = {
    "schema": 1,
    "kind": "pdocker.file-io-microbench",
    "git_commit": subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip(),
    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    "device_serial": device_serial,
    "parameters": {
        "files": int(files),
        "blocks": int(blocks),
        "block_size": int(block_size),
        "fsync": int(fsync),
        "trace_mode": trace_mode,
    },
    "run_rc": int(run_rc),
    "contexts": contexts,
    "events": events,
    "comparisons": comparisons,
    "summary": {
        "all_rc_zero": int(run_rc) == 0 and all(e.get("rc") == 0 for e in events),
        "max_overhead_ms": max((r.get("overhead_ms", 0.0) for r in comparisons), default=0.0),
        "target_overhead_ms": 10.0,
        "target_met": all(r.get("target_met", False) for r in comparisons),
    },
}
Path(out_path).write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")

md = [
    "# pdocker File I/O Microbenchmark",
    "",
    f"- Commit: `{artifact['git_commit']}`",
    f"- Timestamp: `{artifact['timestamp_utc']}`",
    f"- Device: `{device_serial}`",
    f"- Workload: files={files}, blocks={blocks}, block_size={block_size}, fsync={fsync}",
    f"- Target: direct executor overhead <= 10 ms per operation group",
    f"- Result: {'PASS' if artifact['summary']['target_met'] else 'FAIL'}; max overhead {artifact['summary']['max_overhead_ms']:.3f} ms",
    "",
    "| operation | native rootfs ms | container rootfs ms | overhead ms | ratio | target |",
    "|---|---:|---:|---:|---:|---|",
]
for row in comparisons:
    md.append(
        "| {label} | {native:.3f} | {container:.3f} | {overhead:.3f} | {ratio:.2f} | {target} |".format(
            label=row["label"],
            native=row.get("native_ms", 0.0),
            container=row.get("container_ms", 0.0),
            overhead=row.get("overhead_ms", 0.0),
            ratio=row.get("ratio", 0.0),
            target="PASS" if row.get("target_met") else "FAIL",
        )
    )
md.extend([
    "",
    "## Method",
    "",
    "- Both paths run the same static AArch64 benchmark binary stored in the rootfs.",
    "- `native rootfs` executes that binary directly in the APK domain against the rootfs backing path.",
    "- `container rootfs` executes the same binary through `pdocker-direct`, so ptrace/seccomp path mediation remains active.",
    "- Timing is measured inside the benchmark process around direct file syscall loops, not around shell command startup.",
])
Path(md_path).write_text("\n".join(md) + "\n")
print(f"[pdocker file-io microbench] wrote {out_path}")
print(f"[pdocker file-io microbench] wrote {md_path}")
PY

"$ADB" push "$OUT" "/data/local/tmp/file-io-microbench-$STAMP.json" >/dev/null || true
adb_run_as "mkdir -p '$REMOTE_BENCH_DIR' && cp '/data/local/tmp/file-io-microbench-$STAMP.json' '$REMOTE_BENCH_DIR/file-io-microbench-$STAMP.json' && cp '$REMOTE_BENCH_DIR/file-io-microbench-$STAMP.json' '$REMOTE_BENCH_DIR/file-io-microbench-latest.json'" >/dev/null || true

exit "$run_rc"
