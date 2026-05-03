#!/usr/bin/env bash
# Lightweight libcow microbenchmarks for repeated tuning.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="${LIB:-$HERE/libcow.so}"
ROUNDS="${ROUNDS:-1}"

if [[ ! -f "$LIB" ]]; then
  echo "libcow not found: $LIB" >&2
  echo "Run: make -C $HERE all" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/cowbench.py" <<'PY'
import json
import os
import shutil
import sys
import tempfile
import time

mode = sys.argv[1]
root = tempfile.mkdtemp(prefix="cowbench-")
try:
    lower = os.path.join(root, "lower")
    upper = os.path.join(root, "upper")
    os.mkdir(lower)
    os.mkdir(upper)
    if mode == "readonly-open":
        path = os.path.join(upper, "static.txt")
        with open(path, "wb") as f:
            f.write(b"x" * 64)
        t0 = time.perf_counter()
        for _ in range(30000):
            fd = os.open(path, os.O_RDONLY)
            os.read(fd, 1)
            os.close(fd)
        ops = 30000
    elif mode == "exclusive-create":
        t0 = time.perf_counter()
        for i in range(10000):
            fd = os.open(os.path.join(upper, f"tmp{i}"),
                         os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
            os.write(fd, b"x")
            os.close(fd)
        ops = 10000
    elif mode == "copy-up":
        payload = b"a" * (1024 * 1024)
        for i in range(64):
            lower_path = os.path.join(lower, f"f{i}")
            upper_path = os.path.join(upper, f"f{i}")
            with open(lower_path, "wb") as f:
                f.write(payload)
            os.link(lower_path, upper_path)
        t0 = time.perf_counter()
        for i in range(64):
            fd = os.open(os.path.join(upper, f"f{i}"), os.O_WRONLY)
            os.write(fd, b"Z")
            os.close(fd)
        ops = 64
    elif mode == "truncate-copy-up":
        payload = b"a" * (1024 * 1024)
        for i in range(64):
            lower_path = os.path.join(lower, f"t{i}")
            upper_path = os.path.join(upper, f"t{i}")
            with open(lower_path, "wb") as f:
                f.write(payload)
            os.link(lower_path, upper_path)
        t0 = time.perf_counter()
        for i in range(64):
            fd = os.open(os.path.join(upper, f"t{i}"),
                         os.O_WRONLY | os.O_TRUNC)
            os.write(fd, b"Z")
            os.close(fd)
        ops = 64
    else:
        raise SystemExit(f"unknown mode: {mode}")
    elapsed = time.perf_counter() - t0
    print(json.dumps({
        "mode": mode,
        "ops": ops,
        "seconds": round(elapsed, 6),
        "ops_per_second": round(ops / elapsed, 2),
    }, sort_keys=True))
finally:
    shutil.rmtree(root, ignore_errors=True)
PY

for round in $(seq 1 "$ROUNDS"); do
  for mode in readonly-open exclusive-create copy-up truncate-copy-up; do
    printf '{"round":%s,' "$round"
    LD_PRELOAD="$LIB" python3 "$TMP/cowbench.py" "$mode" | sed 's/^{//'
  done
done
