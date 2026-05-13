#!/system/bin/sh
# Device-side placeholder for the interrupted image-pull crash-safety scenario.
# It records the intended phases but intentionally does not claim pass/fail success
# until pdocker Android service controls and evidence paths are wired here.
set -eu
OUT_DIR=/sdcard/pdocker/image-pull-crash-safety
PHASE=unset
IMAGE=busybox:latest
PACKAGE=com.pdocker.android
while [ "$#" -gt 0 ]; do
  case "$1" in
    --phase) PHASE="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --package) PACKAGE="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done
mkdir -p "$OUT_DIR"
{
  echo "scenario_id=image.pull.interrupted-kill-restart"
  echo "phase=$PHASE"
  echo "package=$PACKAGE"
  echo "image=$IMAGE"
  echo "status=planned-gap"
  echo "success=false"
  date 2>/dev/null || true
  echo "Device-side automation is not wired yet; host artifact must remain planned-gap."
} > "$OUT_DIR/$PHASE.txt"
exit 0
