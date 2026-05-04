#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export PYTHONDONTWRITEBYTECODE=1

run() {
  printf '\n==> %s\n' "$*"
  "$@"
}

run python3 -m py_compile \
  scripts/compat-audit.py \
  scripts/verify-build-profile.py \
  scripts/verify-dockerfile-standard.py \
  scripts/verify-project-library.py \
  scripts/verify-ui-actions.py \
  scripts/verify_terminal_editor_contracts.py \
  docker-proot-setup/scripts/verify_runtime_contract.py

run python3 docker-proot-setup/scripts/verify_runtime_contract.py
run cmp -s docker-proot-setup/bin/pdockerd app/src/main/assets/pdockerd/pdockerd
run python3 scripts/verify-build-profile.py
run python3 scripts/verify-dockerfile-standard.py
run python3 scripts/verify_terminal_editor_contracts.py
run python3 scripts/compat-audit.py

printf '\nverify-fast: PASS\n'
