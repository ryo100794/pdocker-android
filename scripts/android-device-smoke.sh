#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="${PDOCKER_PACKAGE:-io.github.ryo100794.pdocker}"
APK="${PDOCKER_APK:-$ROOT/app/build/outputs/apk/debug/app-debug.apk}"
PROJECT="device-smoke"
MODE="full"

usage() {
  cat <<EOF
Usage: $0 [--quick] [--no-install]

Runs a repeatable pdocker Android device smoke through adb + run-as.

Environment:
  ADB               adb executable (default: adb)
  PDOCKER_PACKAGE   Android package (default: $PKG)
  PDOCKER_APK       debug APK path (default: $APK)

Modes:
  --quick       only install/start pdockerd and run docker version
  --no-install  skip adb install; useful when the same debug APK is present
EOF
}

ADB="${ADB:-adb}"
INSTALL=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) MODE="quick" ;;
    --no-install) INSTALL=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

run_adb() {
  "$ADB" "$@"
}

run_as() {
  run_adb shell run-as "$PKG" sh -c "$1"
}

wait_for_socket() {
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

docker_cmd() {
  local cmd="$1"
  run_as "cd files && export PATH=\"\$PWD/pdocker-runtime/docker-bin:\$PATH\" DOCKER_HOST=\"unix://\$PWD/pdocker/pdockerd.sock\" DOCKER_BUILDKIT=0 COMPOSE_DOCKER_CLI_BUILD=0 BUILDKIT_PROGRESS=plain COMPOSE_PROGRESS=plain COMPOSE_MENU=false && $cmd"
}

echo "[pdocker smoke] device: $(run_adb get-serialno)"

if [[ "$INSTALL" -eq 1 ]]; then
  if [[ ! -f "$APK" ]]; then
    echo "APK not found: $APK" >&2
    echo "Run ./gradlew :app:assembleDebug first." >&2
    exit 1
  fi
  echo "[pdocker smoke] installing $APK"
  run_adb install -r "$APK" >/dev/null
fi

run_adb shell pm grant "$PKG" android.permission.POST_NOTIFICATIONS >/dev/null 2>&1 || true
run_adb shell am start -n "$PKG/.MainActivity" >/dev/null
run_adb shell am start-foreground-service \
  -n "$PKG/.PdockerdService" \
  -a "$PKG.action.START" >/dev/null || \
run_adb shell am startservice \
  -n "$PKG/.PdockerdService" \
  -a "$PKG.action.START" >/dev/null

wait_for_socket

echo "[pdocker smoke] docker version"
docker_cmd 'docker version'

if [[ "$MODE" == "quick" ]]; then
  echo "[pdocker smoke] quick mode passed"
  exit 0
fi

echo "[pdocker smoke] creating tiny project"
run_as "mkdir -p files/pdocker/projects/$PROJECT && cat > files/pdocker/projects/$PROJECT/Dockerfile <<'EOF'
FROM ubuntu:22.04
RUN printf 'pdocker-smoke-build\n' > /pdocker-smoke.txt
CMD [\"/bin/sh\", \"-lc\", \"cat /pdocker-smoke.txt && sleep 2\"]
EOF
cat > files/pdocker/projects/$PROJECT/compose.yaml <<'EOF'
services:
  app:
    build: .
    command: [\"/bin/sh\", \"-lc\", \"cat /pdocker-smoke.txt && sleep 2\"]
EOF"

echo "[pdocker smoke] docker build"
docker_cmd "cd pdocker/projects/$PROJECT && docker build -t local/pdocker-device-smoke:latest ."

echo "[pdocker smoke] compose up/down"
docker_cmd "cd pdocker/projects/$PROJECT && docker compose up --detach --build && docker compose ps && docker compose logs --tail=50 && docker compose down"

echo "[pdocker smoke] checking UI-visible job state path"
run_as 'ls -l files/pdocker/jobs.json >/dev/null 2>&1 || true; ls -ld files/pdocker/projects/device-smoke files/pdocker-runtime'

echo "[pdocker smoke] passed"
