#!/usr/bin/env bash
# Initialize local Git metadata and hooks for one pdocker development machine.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
cd "$ROOT"

git_dir="$(git rev-parse --git-dir)"
machine_file="$git_dir/info/pdocker-machine-id"
hook_src="$ROOT/scripts/git-hooks/prepare-commit-msg"
hook_dst="$git_dir/hooks/prepare-commit-msg"

sanitize() {
    tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9._-' '-' | sed 's/^-//; s/-$//'
}

if [[ -n "${PDOCKER_MACHINE_ID:-}" ]]; then
    machine_id="$(printf "%s" "$PDOCKER_MACHINE_ID" | sanitize)"
elif [[ -f "$machine_file" ]]; then
    machine_id="$(tr -d '\r\n' < "$machine_file")"
else
    host="$(hostname 2>/dev/null | sanitize)"
    [[ -n "$host" ]] || host="dev"
    suffix="$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')"
    machine_id="pdocker-${host}-${suffix}"
fi

printf "%s\n" "$machine_id" > "$machine_file"
git config pdocker.machineId "$machine_id"

if [[ -f "$hook_dst" ]] && ! cmp -s "$hook_src" "$hook_dst"; then
    backup="$hook_dst.pdocker-backup-$(date -u +%Y%m%dT%H%M%SZ)"
    cp "$hook_dst" "$backup"
    echo "backed up existing prepare-commit-msg hook to $backup"
fi
cp "$hook_src" "$hook_dst"
chmod +x "$hook_dst"

echo "pdocker Git worktree prepared"
echo "  machine: $machine_id"
echo "  hook:    $hook_dst"
echo
echo "Run scripts/git-preflight.sh before starting a shared work session."

