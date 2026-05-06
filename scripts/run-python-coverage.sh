#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${PDOCKER_PYTHON_COVERAGE_OUT:-$ROOT/docs/test/python-coverage-latest.json}"
HTML_DIR="${PDOCKER_PYTHON_COVERAGE_HTML:-}"
DATA_FILE="${PDOCKER_PYTHON_COVERAGE_DATA:-$ROOT/.coverage.pdocker-python}"

export COVERAGE_FILE="$DATA_FILE"
rm -f "$DATA_FILE"

python3 -m coverage erase

run_cov() {
  python3 -m coverage run --branch --parallel-mode "$@"
}

run_cov -m unittest tests.direct_syscall.test_container_probe_assets
run_cov -m unittest tests.direct_syscall.test_path_boundary_matrix
run_cov -m unittest tests.direct_syscall.test_scenario_manifest
run_cov -m unittest tests.direct_syscall.test_memory_guard_contract
run_cov -m unittest tests.storage_metrics.test_verify_storage_metrics
run_cov -m unittest tests.metadata_index.test_verify_metadata_index
run_cov scripts/verify-build-profile.py
run_cov scripts/verify-project-library.py
run_cov scripts/verify_direct_syscall_contracts.py

python3 -m coverage combine
mkdir -p "$(dirname "$OUT")"
python3 -m coverage json --pretty-print -o "$OUT"
if [[ -n "$HTML_DIR" ]]; then
  python3 -m coverage html -d "$HTML_DIR"
fi

python3 - "$OUT" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
totals = data.get("totals", {})
covered_lines = int(totals.get("covered_lines") or 0)
num_statements = int(totals.get("num_statements") or 0)
covered_branches = int(totals.get("covered_branches") or 0)
num_branches = int(totals.get("num_branches") or 0)
percent = float(totals.get("percent_covered") or 0.0)
data["pdocker"] = {
    "schema": "pdocker.python.coverage.v1",
    "coverage_items": covered_lines + covered_branches,
    "covered_lines": covered_lines,
    "num_statements": num_statements,
    "covered_branches": covered_branches,
    "num_branches": num_branches,
    "percent_covered": percent,
    "source": "coverage.py --branch",
}
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
print(
    "python coverage: "
    f"lines={covered_lines}/{num_statements} "
    f"branches={covered_branches}/{num_branches} "
    f"items={covered_lines + covered_branches} "
    f"percent={percent:.2f}"
)
print(f"coverage artifact: {path}")
PY
