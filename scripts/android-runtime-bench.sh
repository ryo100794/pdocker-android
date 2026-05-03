#!/usr/bin/env bash
set -euo pipefail

PKG="${PDOCKER_ANDROID_PACKAGE:-io.github.ryo100794.pdocker.compat}"
ADB="${ADB:-adb}"
PROJECT_ROOT="files"

usage() {
  cat <<'EOF'
Usage: scripts/android-runtime-bench.sh [--apt-update] [--trace-mode MODE] [--proot-cmd CMD]

Runs repeatable Android-side runtime benchmarks without installing external
runtime code. The direct backend benchmark uses the app's staged
pdocker-direct helper and reports syscall stop counts when supported.

Options:
  --apt-update       also run slow apt-get update wall-clock benchmark
  --trace-mode MODE  direct backend mode: syscall (default) or seccomp
  --proot-cmd CMD    optional existing proot-compatible command to compare;
                    the script does not download or bundle proot
EOF
}

RUN_APT_UPDATE=0
TRACE_MODE="${PDOCKER_DIRECT_TRACE_MODE:-seccomp}"
PROOT_CMD=""
while (($#)); do
  case "$1" in
    --apt-update) RUN_APT_UPDATE=1 ;;
    --trace-mode)
      shift
      TRACE_MODE="${1:-}"
      ;;
    --proot-cmd)
      shift
      PROOT_CMD="${1:-}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

run_as() {
  "$ADB" shell "run-as $PKG sh -c '$*'"
}

bench_direct() {
  local body="$1"
  run_as "cd $PROJECT_ROOT; R=\$(cd pdocker/containers && ls -td build_*/rootfs 2>/dev/null | head -1); test -n \"\$R\"; export PDOCKER_DIRECT_EXPERIMENTAL_PROCESS_EXEC=1 PDOCKER_DIRECT_TRACE_SYSCALLS=0 PDOCKER_DIRECT_TRACE_VERBOSE=0 PDOCKER_DIRECT_TRACE_PATHS=0 PDOCKER_DIRECT_STATS=1 PDOCKER_DIRECT_TRACE_MODE='$TRACE_MODE'; /system/bin/time -p pdocker-runtime/docker-bin/pdocker-direct run --mode bench --rootfs \"pdocker/containers/\$R\" --workdir / --env HOME=/root --env DEBIAN_FRONTEND=noninteractive --env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin -- /bin/sh -lc \"$body\""
}

echo "[pdocker bench] direct apt-cache (trace-mode=$TRACE_MODE)"
bench_direct "apt-cache policy nodejs >/tmp/pdocker-bench-apt-cache.log; head -5 /tmp/pdocker-bench-apt-cache.log"

if [[ "$RUN_APT_UPDATE" == "1" ]]; then
  echo
  echo "[pdocker bench] direct apt-get update"
  bench_direct "apt-get update >/tmp/pdocker-bench-apt-update.log; tail -1 /tmp/pdocker-bench-apt-update.log"
fi

if [[ -n "$PROOT_CMD" ]]; then
  echo
  echo "[pdocker bench] external proot command"
  echo "command: $PROOT_CMD"
  "$ADB" shell "$PROOT_CMD"
fi
