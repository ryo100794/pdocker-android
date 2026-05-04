#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export PYTHONDONTWRITEBYTECODE=1
export PDOCKER_ANDROID_FLAVOR="${PDOCKER_ANDROID_FLAVOR:-compat}"
case "$PDOCKER_ANDROID_FLAVOR" in
  compat|modern) ;;
  *)
    echo "verify-fast: PDOCKER_ANDROID_FLAVOR must be 'compat' or 'modern' (got '$PDOCKER_ANDROID_FLAVOR')" >&2
    exit 2
    ;;
esac

run() {
  printf '\n==> %s\n' "$*"
  "$@"
}

run python3 -m py_compile \
  scripts/compat-audit.py \
  scripts/verify-build-profile.py \
  scripts/verify-dockerfile-standard.py \
  scripts/verify_direct_syscall_contracts.py \
  scripts/run_direct_syscall_scenarios.py \
  scripts/verify-project-library.py \
  scripts/verify-storage-metrics.py \
  scripts/verify-ui-actions.py \
  scripts/verify_terminal_editor_contracts.py \
  scripts/update-showcase.py \
  docker-proot-setup/scripts/verify_runtime_contract.py

run python3 docker-proot-setup/scripts/verify_runtime_contract.py
run python3 scripts/verify_direct_syscall_contracts.py
run python3 scripts/run_direct_syscall_scenarios.py --lane local
run cmp -s docker-proot-setup/bin/pdockerd app/src/main/assets/pdockerd/pdockerd
run python3 scripts/verify-build-profile.py
run python3 scripts/verify-dockerfile-standard.py
run python3 scripts/verify-storage-metrics.py
run python3 scripts/verify_terminal_editor_contracts.py
run bash scripts/smoke-vulkan-llama-init.sh
run bash scripts/smoke-vulkan-icd-bridge.sh
run python3 scripts/compat-audit.py
run python3 scripts/update-showcase.py --check

printf '\nverify-fast: PASS\n'
